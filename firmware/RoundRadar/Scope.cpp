// Round Radar - the radar scope and the aircraft detail screen.
//
// How the sweep works. Two byte-per-pixel layers live in PSRAM:
//   s_static  how bright the fixed picture is at each pixel (coast 255, rings 110)
//   s_angle   the bearing of each pixel from the centre, as 0..255 clockwise from north
// Each frame, every pixel inside the circle looks up how long ago the sweep
// passed its bearing ("age") and takes the static brightness faded by that age,
// or the glow of the wedge trailing the sweep line, whichever is brighter.
// The static layer is only rebuilt when the range or the home position changes.
#include "App.h"
#include "EuMapData.h"        // Natural Earth outlines, from MeteoPlaneRadar (include once)
#include "esp_heap_caps.h"
#include <WiFi.h>
#include <math.h>
extern "C" {
#include "qrcode.h"
}

#define CX 240
#define CY 240
#define R_SCOPE 230           // px that the selected range maps onto
#define R_EDGE 238            // last drawn pixel radius
#define PX (LCD_WIDTH * LCD_HEIGHT)

const float RANGES_KM[RANGE_COUNT] = { 10.0f, 25.0f, 50.0f, 100.0f };

static uint8_t* s_static = nullptr;
static uint8_t* s_angle = nullptr;
static int16_t  s_halfW[LCD_HEIGHT];     // half width of the circle on each row, -1 = none
static uint16_t s_pal[256];              // brightness -> shade of scope green
static uint8_t  s_fade[256];             // age -> brightness of the fixed picture (x/256)
static uint8_t  s_glow[256];             // age -> brightness of the trailing wedge
static bool     s_dirty = true;

// Screen positions from the last frame, for tap-to-select.
static int16_t s_px[AC_MAX], s_py[AC_MAX];
static char    s_phex[AC_MAX][8];
static int     s_pn = 0;

// Towns in view, found when the static layer is built. Inland there is no coast
// to draw, so towns are what make the scope recognisable (Bradford has no
// outline in view until 100 km).
struct Town { int16_t x, y; const char* label; bool big; };   // big: 300,000 people or more
static Town s_towns[48];
static int  s_townN = 0;

// Airports, entered by hand: the map data has none. Drawn in amber as a ring
// with a bar along the runway, so the place most local traffic comes from shows.
struct Airport { float lat, lon, runwayDeg; const char* label; };
static const Airport AIRPORTS[] = {
  { 53.8659f, -1.6606f, 140.0f, "LBA" },   // Leeds Bradford, runway 14/32
  { 53.3537f, -2.2750f,  51.0f, "MAN" },   // Manchester, 05/23
  { 53.3336f, -2.8497f,  86.0f, "LPL" },   // Liverpool, 09/27
  { 53.7717f, -3.0286f,  97.0f, "BLK" },   // Blackpool, 10/28
  { 54.5092f, -1.4294f,  49.0f, "MME" },   // Teesside, 05/23
  { 53.5744f, -0.3508f,  24.0f, "HUY" },   // Humberside, 02/20
};
static const int AIRPORT_N = sizeof(AIRPORTS) / sizeof(AIRPORTS[0]);
static int16_t s_apX[AIRPORT_N], s_apY[AIRPORT_N];
static bool    s_apOn[AIRPORT_N];

static uint16_t shade(uint8_t v) {
  if (Theme_Red()) return (uint16_t)(((31 * v / 255) << 11) | ((9 * v / 255) << 5) | (3 * v / 255));
  return (uint16_t)(((7 * v / 255) << 11) | ((63 * v / 255) << 5) | (15 * v / 255));
}
void Scope_ThemeChanged() { for (int i = 0; i < 256; i++) s_pal[i] = shade((uint8_t)i); }

void Scope_Invalidate() { s_dirty = true; }

void Scope_Begin() {
  s_static = (uint8_t*)heap_caps_malloc(PX, MALLOC_CAP_SPIRAM);
  s_angle  = (uint8_t*)heap_caps_malloc(PX, MALLOC_CAP_SPIRAM);
  if (!s_static || !s_angle) { Serial.println("SCOPE: no memory for the layers"); return; }

  for (int y = 0; y < LCD_HEIGHT; y++) {
    const int dy = y - CY;
    const int d2 = R_EDGE * R_EDGE - dy * dy;
    s_halfW[y] = d2 >= 0 ? (int16_t)sqrtf((float)d2) : -1;
    for (int x = 0; x < LCD_WIDTH; x++) {
      float a = atan2f((float)(x - CX), (float)-dy);          // 0 = north, clockwise
      if (a < 0) a += 6.2831853f;
      s_angle[y * LCD_WIDTH + x] = (uint8_t)(a * (256.0f / 6.2831853f));
    }
  }
  for (int i = 0; i < 256; i++) {
    s_pal[i] = shade((uint8_t)i);
    // Fixed picture: full brightness under the line, settling to a 30% floor.
    s_fade[i] = (uint8_t)(77.0f + 178.0f * expf(-(float)i / 45.0f));
    // Wedge: about 110 degrees of glow behind the line.
    const float w = 1.0f - (float)i / 78.0f;
    s_glow[i] = w > 0 ? (uint8_t)(72.0f * powf(w, 1.6f)) : 0;
  }
}

// ------------------------------------------------------------ static layer --
static inline void plot(int x, int y, uint8_t v) {
  if ((unsigned)x < LCD_WIDTH && (unsigned)y < LCD_HEIGHT) {
    uint8_t* p = &s_static[y * LCD_WIDTH + x];
    if (v > *p) *p = v;
  }
}

static int outcode(float x, float y) {
  return (x < 0 ? 1 : 0) | (x > LCD_WIDTH - 1 ? 2 : 0) | (y < 0 ? 4 : 0) | (y > LCD_HEIGHT - 1 ? 8 : 0);
}

// Line into the static layer, clipped to the screen first (Cohen-Sutherland),
// because at short range a border point can project tens of thousands of pixels away.
static void lineClipped(float x0, float y0, float x1, float y1, uint8_t v) {
  int c0 = outcode(x0, y0), c1 = outcode(x1, y1);
  for (int guard = 0; guard < 8; guard++) {
    if (!(c0 | c1)) break;
    if (c0 & c1) return;
    const int c = c0 ? c0 : c1;
    float x, y;
    const float W = LCD_WIDTH - 1, H = LCD_HEIGHT - 1;
    if (c & 8)      { x = x0 + (x1 - x0) * (H - y0) / (y1 - y0); y = H; }
    else if (c & 4) { x = x0 + (x1 - x0) * (0 - y0) / (y1 - y0); y = 0; }
    else if (c & 2) { y = y0 + (y1 - y0) * (W - x0) / (x1 - x0); x = W; }
    else            { y = y0 + (y1 - y0) * (0 - x0) / (x1 - x0); x = 0; }
    if (c == c0) { x0 = x; y0 = y; c0 = outcode(x0, y0); }
    else         { x1 = x; y1 = y; c1 = outcode(x1, y1); }
  }
  if (c0 | c1) return;
  int ax = (int)x0, ay = (int)y0, bx = (int)x1, by = (int)y1;
  const int dx = abs(bx - ax), dy = -abs(by - ay), sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
  int e = dx + dy;
  for (;;) {
    plot(ax, ay, v); plot(ax + 1, ay, v); plot(ax, ay + 1, v);   // a little weight
    if (ax == bx && ay == by) break;
    const int e2 = 2 * e;
    if (e2 >= dy) { e += dy; ax += sx; }
    if (e2 <= dx) { e += dx; ay += sy; }
  }
}

static void ring(int r, uint8_t v) {
  const int steps = r * 8;
  for (int i = 0; i < steps; i++) {
    const float a = i * 6.2831853f / steps;
    plot(CX + (int)lroundf(r * sinf(a)), CY - (int)lroundf(r * cosf(a)), v);
  }
}

static inline float ptLon(uint16_t v) { return EU_LON_ORIGIN + v * EU_COORD_SCALE; }
static inline float ptLat(uint16_t v) { return EU_LAT_ORIGIN + v * EU_COORD_SCALE; }

static void project(float lat, float lon, float* sx, float* sy) {
  const float scale = (float)R_SCOPE / RANGES_KM[cfg.rangeIdx];
  const float dx = (lon - cfg.lon) * 111.32f * cosf(cfg.lat * 0.0174532925f);
  const float dy = (lat - cfg.lat) * 110.57f;
  *sx = CX + dx * scale;
  *sy = CY - dy * scale;
}

static void buildStatic() {
  memset(s_static, 0, PX);
  ring(R_SCOPE, 110);
  ring(R_SCOPE / 2, 110);

  // Only outlines whose bounding box reaches the view are projected at all.
  const float km = RANGES_KM[cfg.rangeIdx] * 1.3f;
  const float dLat = km / 110.57f, dLon = km / (111.32f * cosf(cfg.lat * 0.0174532925f));
  const float lat0 = cfg.lat - dLat, lat1 = cfg.lat + dLat, lon0 = cfg.lon - dLon, lon1 = cfg.lon + dLon;

  for (int r = 0; r < EU_RING_COUNT; r++) {
    const uint16_t start = EU_RING_OFFSETS[r], end = EU_RING_OFFSETS[r + 1];
    if (end - start < 2) continue;
    uint16_t xmin = 0xFFFF, xmax = 0, ymin = 0xFFFF, ymax = 0;
    for (uint16_t i = start; i < end; i++) {
      const uint16_t x = EU_BORDER_PTS[i][0], y = EU_BORDER_PTS[i][1];
      if (x < xmin) xmin = x;
      if (x > xmax) xmax = x;
      if (y < ymin) ymin = y;
      if (y > ymax) ymax = y;
    }
    if (ptLon(xmax) < lon0 || ptLon(xmin) > lon1 || ptLat(ymax) < lat0 || ptLat(ymin) > lat1) continue;

    float px = 0, py = 0;
    for (uint16_t i = start; i < end; i++) {
      float sx, sy;
      project(ptLat(EU_BORDER_PTS[i][1]), ptLon(EU_BORDER_PTS[i][0]), &sx, &sy);
      if (i > start) lineClipped(px, py, sx, sy, 255);
      px = sx; py = sy;
    }
  }

  // Places. Cities (300,000 people or more) are always marked. Towns (50,000 or more)
  // fill the gaps: one is added when it is at least 20 km from every city, or when at
  // most one city is in view at all. Larger towns are placed first, and none is placed
  // within 60 px of a marker already there, so they never bunch up.
  // Names are full up to 50 km, four letters at 100 km.
  const float range = RANGES_KM[cfg.rangeIdx];
  s_townN = 0;
  int cities = 0;               // in view, including the home city when the centre cross hides its marker
  float cityLat[16], cityLon[16];
  int cityN = 0;                // every city near the view, for the 20 km rule
  auto addPlaces = [&](uint8_t tier) {
    for (int i = 0; i < EU_CITY_COUNT && s_townN < (int)(sizeof(s_towns) / sizeof(s_towns[0])); i++) {
      const EuCity& c = EU_CITIES[i];
      if (c.tier != tier || c.lat < lat0 || c.lat > lat1 || c.lon < lon0 || c.lon > lon1) continue;
      if (tier == 1 && cityN < 16) { cityLat[cityN] = c.lat; cityLon[cityN] = c.lon; cityN++; }
      float fx, fy;
      project(c.lat, c.lon, &fx, &fy);
      const int x = (int)fx, y = (int)fy, dx = x - CX, dy = y - CY;
      const int d2 = dx * dx + dy * dy;
      if (d2 > (R_SCOPE - 8) * (R_SCOPE - 8)) continue;                   // off the scope
      if (tier == 1) cities++;
      if (d2 < 14 * 14) continue;                                         // under the centre cross
      if (tier > 1) {
        bool ok = true;
        if (cities > 1) {
          for (int k = 0; k < cityN && ok; k++) {
            float km;
            Geo_DistBearing(c.lat, c.lon, cityLat[k], cityLon[k], &km, nullptr);
            if (km < 20.0f) ok = false;
          }
        }
        for (int k = 0; k < s_townN && ok; k++) {
          const int ex = s_towns[k].x - x, ey = s_towns[k].y - y;
          if (ex * ex + ey * ey < 60 * 60) ok = false;
        }
        if (!ok) continue;
      }
      for (int k = -3; k <= 3; k++) {                                     // small hollow square
        plot(x + k, y - 3, 200); plot(x + k, y + 3, 200); plot(x - 3, y + k, 200); plot(x + 3, y + k, 200);
      }
      s_towns[s_townN++] = { (int16_t)x, (int16_t)y, range > 50.0f ? c.abbr : c.name, tier == 1 };
    }
  };
  addPlaces(1);
  addPlaces(2);
  addPlaces(3);
  for (int i = 0; i < AIRPORT_N; i++) {
    float fx, fy;
    project(AIRPORTS[i].lat, AIRPORTS[i].lon, &fx, &fy);
    const float dx = fx - CX, dy = fy - CY;
    s_apOn[i] = dx * dx + dy * dy <= (float)(R_SCOPE - 14) * (R_SCOPE - 14);
    s_apX[i] = (int16_t)fx; s_apY[i] = (int16_t)fy;
  }
  s_dirty = false;
}

// -------------------------------------------------------------- per frame ---
static void composite(uint16_t* fb, uint8_t sweep, bool sweeping) {
  for (int y = 0; y < LCD_HEIGHT; y++) {
    uint16_t* row = fb + y * LCD_WIDTH;
    const int hw = s_halfW[y];
    if (hw < 0) { memset(row, 0, LCD_WIDTH * 2); continue; }
    const int x0 = CX - hw, x1 = CX + hw;
    memset(row, 0, x0 * 2);
    memset(row + x1 + 1, 0, (LCD_WIDTH - 1 - x1) * 2);
    const uint8_t* st = s_static + y * LCD_WIDTH;
    const uint8_t* an = s_angle + y * LCD_WIDTH;
    if (sweeping) {
      for (int x = x0; x <= x1; x++) {
        const uint8_t age = (uint8_t)(sweep - an[x]);
        uint8_t b = s_glow[age];
        const uint8_t v = st[x];
        if (v) { const uint8_t t = (uint8_t)((v * s_fade[age]) >> 8); if (t > b) b = t; }
        row[x] = s_pal[b];
      }
    } else {
      for (int x = x0; x <= x1; x++) row[x] = s_pal[st[x] >> 1];   // sweep stopped: steady, dim
    }
  }
}

static uint16_t planeColour(int x, int y, uint8_t sweep, bool sweeping, bool stale) {
  if (stale) return C_GREY;
  if (!sweeping) return shade(180);
  const uint8_t age = (uint8_t)(sweep - s_angle[constrain(y, 0, LCD_HEIGHT - 1) * LCD_WIDTH + constrain(x, 0, LCD_WIDTH - 1)]);
  return shade((uint8_t)(110 + ((145 * (255 - age)) >> 8)));       // 43%..100%
}

static void drawPlane(int x, int y, float trackDeg, bool hasTrack, uint16_t col) {
  if (!hasTrack) { gfx->fillCircle(x, y, 5, col); return; }
  const float t = trackDeg * 0.0174532925f, s = sinf(t), c = cosf(t);
  // Arrowhead pointing along the track: nose, right wing, tail notch, left wing.
  auto rx = [&](float px, float py) { return x + (int)lroundf(px * c - py * s); };
  auto ry = [&](float px, float py) { return y + (int)lroundf(px * s + py * c); };
  const int nx = rx(0, -11), ny = ry(0, -11), wx = rx(7, 8), wy = ry(7, 8);
  const int tx = rx(0, 4), ty = ry(0, 4), lx = rx(-7, 8), ly = ry(-7, 8);
  gfx->fillTriangle(nx, ny, wx, wy, tx, ty, col);
  gfx->fillTriangle(nx, ny, lx, ly, tx, ty, col);
}

// Labels claim their rectangle so that two never print through each other.
struct Rect { int16_t x, y, w, h; };
static Rect s_claims[AC_MAX * 2 + 60];
static int  s_claimN = 0;
static bool claim(int x, int y, int w, int h) {
  // Must sit inside the circle.
  const int corners[4][2] = { { x, y }, { x + w, y }, { x, y + h }, { x + w, y + h } };
  for (auto& k : corners) {
    const int dx = k[0] - CX, dy = k[1] - CY;
    if (dx * dx + dy * dy > 228 * 228) return false;
  }
  for (int i = 0; i < s_claimN; i++) {
    const Rect& r = s_claims[i];
    if (x < r.x + r.w && x + w > r.x && y < r.y + r.h && y + h > r.y) return false;
  }
  if (s_claimN < (int)(sizeof(s_claims) / sizeof(s_claims[0]))) s_claims[s_claimN++] = { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h };
  return true;
}

// Finds room for a place name beside its marker: the side facing the middle of the
// screen first, then the other side, then above and below. False if none is free.
static bool placeName(int x, int y, int gap, int tw, int* tx, int* ty) {
  const int near = x > CX + 60 ? x - gap - tw : x + gap;
  const int far  = x > CX + 60 ? x + gap : x - gap - tw;
  const int spots[4][2] = { { near, y - 7 }, { far, y - 7 }, { x - tw / 2, y - gap - 18 }, { x - tw / 2, y + gap + 2 } };
  for (auto& k : spots) {
    if (claim(k[0] - 2, k[1] - 2, tw + 4, 20)) { *tx = k[0]; *ty = k[1]; return true; }
  }
  return false;
}

void Scope_Draw(const Aircraft* list, int n, bool wifi) {
  if (!s_static || !s_angle) { gfx->fillScreen(C_BLACK); UI_Text(CX, 232, "Out of memory", 2, C_RED); return; }
  if (s_dirty) buildStatic();

  const bool sweeping = wifi;
  const unsigned long ms = millis() % SWEEP_PERIOD_MS;
  const uint8_t sweep = (uint8_t)(ms * 256UL / SWEEP_PERIOD_MS);
  composite(gfx->getFramebuffer(), sweep, sweeping);

  // Centre cross and the sweep line.
  gfx->drawFastHLine(CX - 8, CY, 17, C_GREEN);
  gfx->drawFastVLine(CX, CY - 8, 17, C_GREEN);
  if (sweeping) {
    const float a = ms * 6.2831853f / SWEEP_PERIOD_MS;
    const int ex = CX + (int)(R_EDGE * sinf(a)), ey = CY - (int)(R_EDGE * cosf(a));
    gfx->drawLine(CX, CY, ex, ey, C_GREEN);
    gfx->drawLine(CX + 1, CY, ex + 1, ey, C_GREEN);
  }

  // Reserve the fixed text first, so aircraft labels have to give way to it.
  s_claimN = 0;
  s_claims[s_claimN++] = { 90, 50, 300, 28 };     // top line
  s_claims[s_claimN++] = { 170, 408, 140, 60 };   // range and dots
  s_claims[s_claimN++] = { 244, 112, 52, 20 };    // half-range label

  char buf[32];
  snprintf(buf, sizeof(buf), "%g", RANGES_KM[cfg.rangeIdx] / 2);   // 12.5 at the 25 km range
  UI_TextL(247, 114, buf, 2, C_MID);

  // Airports: under the aircraft, and aircraft callsigns keep off them.
  for (int i = 0; i < AIRPORT_N; i++) {
    if (!s_apOn[i]) continue;
    const int x = s_apX[i], y = s_apY[i];
    const float t = AIRPORTS[i].runwayDeg * 0.0174532925f;
    const int rx = (int)lroundf(12 * sinf(t)), ry = (int)lroundf(-12 * cosf(t));
    gfx->drawCircle(x, y, 6, C_AMBER);
    gfx->drawLine(x - rx, y - ry, x + rx, y + ry, C_AMBER);
    gfx->drawLine(x - rx + 1, y - ry, x + rx + 1, y + ry, C_AMBER);
    s_claims[s_claimN++] = { (int16_t)(x - 13), (int16_t)(y - 13), 26, 26 };
  }

  // Fixed landmarks get their names before anything that moves: airports, then the
  // big cities. Aircraft callsigns then have to keep off them, not the other way round.
  for (int i = 0; i < AIRPORT_N; i++) {
    if (!s_apOn[i]) continue;
    int tx, ty;
    if (placeName(s_apX[i], s_apY[i], 16, (int)strlen(AIRPORTS[i].label) * 12, &tx, &ty))
      UI_TextL(tx, ty, AIRPORTS[i].label, 2, C_AMBER);
  }
  auto townNames = [&](bool big) {
    for (int i = 0; i < s_townN; i++) {
      const Town& t = s_towns[i];
      if (t.big != big) continue;
      int tx, ty;
      if (!placeName(t.x, t.y, 8, (int)strlen(t.label) * 12, &tx, &ty)) continue;
      uint8_t b = 90;
      if (sweeping) {
        const uint8_t ag = (uint8_t)(sweep - s_angle[t.y * LCD_WIDTH + t.x]);
        b = (uint8_t)(70 + ((90 * (255 - ag)) >> 8));
      }
      UI_TextL(tx, ty, t.label, 2, shade(b));
    }
  };
  townNames(true);

  // Aircraft.
  const uint32_t age = Net_DataAgeMs();
  const bool stale = age > DATA_STALE_MS;
  const bool hidden = !Net_EverOk() || age > DATA_HIDE_MS;
  const float scale = (float)R_SCOPE / RANGES_KM[cfg.rangeIdx];
  int shown = 0;
  s_pn = 0;
  // Between fetches each aircraft is moved on along its track at its own speed, so
  // the picture stays smooth although downloads are kept few (to spare mobile data).
  const float sinceS = stale ? 0.0f : age / 1000.0f;
  if (!hidden && wifi) {
    for (int i = 0; i < n; i++) {
      const Aircraft& a = list[i];
      float lat = a.lat, lon = a.lon;
      if (a.hasTrack && sinceS > 0) {
        const float km = a.gsKt * 0.000514444f * sinceS, tr = a.track * 0.0174532925f;
        lat += km * cosf(tr) / 110.57f;
        lon += km * sinf(tr) / (111.32f * cosf(lat * 0.0174532925f));
      }
      float fx, fy;
      project(lat, lon, &fx, &fy);
      const int x = (int)fx, y = (int)fy;
      const int dx = x - CX, dy = y - CY;
      if (dx * dx + dy * dy > R_SCOPE * R_SCOPE) continue;
      const uint16_t col = planeColour(x, y, sweep, sweeping, stale);

      // Trail: where it was on earlier polls, dimmer with age.
      for (int k = 0; k < a.trailN; k++) {
        float tx, ty;
        project(a.trailLat[k], a.trailLon[k], &tx, &ty);
        gfx->fillCircle((int)tx, (int)ty, 2, stale ? C_GREY : shade((uint8_t)(150 - k * 30)));
      }
      drawPlane(x, y, a.track, a.hasTrack, col);

      const char* label = a.callsign[0] ? a.callsign : a.hex;
      const int tw = (int)strlen(label) * 12;
      if (claim(x - tw / 2 - 2, y + 15, tw + 4, 20)) UI_TextL(x - tw / 2, y + 17, label, 2, col);
      if (s_claimN < (int)(sizeof(s_claims) / sizeof(s_claims[0])))     // keep town names off the icon
        s_claims[s_claimN++] = { (int16_t)(x - 12), (int16_t)(y - 12), 24, 24 };

      s_px[s_pn] = x; s_py[s_pn] = y;
      strncpy(s_phex[s_pn], a.hex, sizeof(s_phex[0]) - 1);
      s_phex[s_pn][sizeof(s_phex[0]) - 1] = '\0';
      s_pn++;
      shown++;
    }
  }
  (void)scale;

  // The remaining town names take only the space the aircraft left over.
  townNames(false);

  // Top line.
  if (!wifi) {
    gfx->fillRoundRect(90, 203, 300, 76, 10, C_BLACK);
    gfx->drawRoundRect(90, 203, 300, 76, 10, C_AMBER);
    UI_Text(CX, 216, "No Wi-Fi", 3, C_AMBER);
    UI_Text(CX, 252, "trying again", 2, C_AMBER);
  } else if (!Net_EverOk()) {
    UI_Text(CX, 56, "Getting aircraft", 2, C_MID);
  } else if (Net_Resting()) {
    UI_Text(CX, 56, "Resting: touch to wake", 2, C_MID);
  } else if (age > DATA_HIDE_MS) {
    UI_Text(CX, 56, "No aircraft data", 2, C_AMBER);
  } else if (stale) {
    snprintf(buf, sizeof(buf), "Data %lu min old", (unsigned long)(age / 60000UL));
    UI_Text(CX, 56, buf, 2, C_AMBER);
  } else if (shown == 0) {
    UI_Text(CX, 56, "No aircraft in range", 2, C_MID);
  } else {
    snprintf(buf, sizeof(buf), "%d aircraft", shown);
    UI_Text(CX, 56, buf, 2, C_GREEN);
  }

  // Range and its four dots.
  snprintf(buf, sizeof(buf), "%.0f km", RANGES_KM[cfg.rangeIdx]);
  UI_Text(CX, 414, buf, 3, C_AMBER);
  for (int i = 0; i < RANGE_COUNT; i++) {
    const int x = 204 + i * 24;
    if (i == cfg.rangeIdx) gfx->fillCircle(x, 455, 5, C_AMBER);
    else                   gfx->drawCircle(x, 455, 5, C_GREY);
  }
}

bool Scope_HitTest(int x, int y, char* hexOut) {
  long best = (long)TAP_RADIUS_PX * TAP_RADIUS_PX;
  int hit = -1;
  for (int i = 0; i < s_pn; i++) {
    const long dx = s_px[i] - x, dy = s_py[i] - y, d = dx * dx + dy * dy;
    if (d < best) { best = d; hit = i; }
  }
  if (hit < 0) return false;
  strcpy(hexOut, s_phex[hit]);
  return true;
}

// ------------------------------------------------------------------ detail --
static void withCommas(char* out, size_t cap, long v) {
  if (labs(v) >= 1000) snprintf(out, cap, "%ld,%03ld", v / 1000, labs(v) % 1000);
  else                 snprintf(out, cap, "%ld", v);
}

// The photo, drawn straight into the frame. In the red night colours it keeps its own
// colours but is drawn 30% transparent, so it is not the brightest thing in a dark room.
static void drawPhoto(const AcInfo& in, int top) {
  const int x0 = CX - in.pw / 2, y0 = top + (PHOTO_H - in.ph) / 2;
  uint16_t* fb = gfx->getFramebuffer();
  if (!fb || x0 < 0 || y0 < 0 || x0 + in.pw > 480 || y0 + in.ph > 480) return;
  const bool red = Theme_Red();
  for (int y = 0; y < in.ph; y++) {
    const uint16_t* s = in.photo + y * in.pw;
    uint16_t* d = fb + (y0 + y) * 480 + x0;
    if (!red) { memcpy(d, s, in.pw * 2); continue; }
    for (int x = 0; x < in.pw; x++) {
      const uint16_t p = s[x];
      d[x] = (uint16_t)(((((p >> 11) & 0x1F) * 179 >> 8) << 11) | ((((p >> 5) & 0x3F) * 179 >> 8) << 5) | ((p & 0x1F) * 179 >> 8));   // 70%
    }
  }
}

// Which of the detail pages this is: a row of dots at the foot.
static void pageDots(int page) {
  for (int i = 0; i < DETAIL_PAGES; i++) {
    const int x = CX + (i * 2 - (DETAIL_PAGES - 1)) * 10;
    if (i == page) gfx->fillCircle(x, 436, 4, C_AMBER);
    else           gfx->drawCircle(x, 436, 4, C_GREY);
  }
}

// Third details page: a QR code that opens the photo's own page on the site it came
// from, so the photographer and the full-size picture are one scan away.
static void detailLink(const Aircraft& a) {
  AcInfo in;
  Info_Get(&in);
  const bool mine = strcmp(in.hex, a.hex) == 0;
  const size_t len = mine ? strlen(in.link) : 0;
  if (!len) {
    UI_Text(CX, 38, a.callsign[0] ? a.callsign : a.hex, 3, C_AMBER);
    UI_Text(CX, 212, (!mine || in.loading) ? "Getting photo" : "No photo page", 2, C_MID);
    if (mine && !in.loading) UI_Text(CX, 236, "for this aircraft", 2, C_MID);
    return;
  }
  // The smallest code that holds the address (bytes at the lowest error correction).
  static const uint16_t CAP[] = { 0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271 };
  uint8_t version = 3;
  while (version < 10 && CAP[version] < len) version++;
  static uint8_t data[420];                    // enough for version 10
  QRCode qr;
  if (qrcode_initText(&qr, data, version, ECC_LOW, in.link) < 0) return;
  const int m = 256 / (qr.size + 2);           // a clear margin of one module all round
  const int box = (qr.size + 2) * m, x0 = CX - box / 2, y0 = 202 - box / 2;
  UI_Text(CX, 44, "Scan for the photo", 2, C_MID);
  // Light squares in the theme's brightest colour: white by day, red in the night hours.
  gfx->fillRect(x0, y0, box, box, Theme_Red() ? C_GREEN : C_WHITE);
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y)) gfx->fillRect(x0 + (x + 1) * m, y0 + (y + 1) * m, m, m, C_BLACK);
  char buf[40];
  snprintf(buf, sizeof(buf), "Photo: %s", in.credit);
  UI_Text(CX, 352, buf, 2, C_GREEN);
  UI_Text(CX, 378, in.site, 2, C_MID);
}

// Second details page: a photo of this very aircraft, where it is flying from and to,
// and who operates it.
static void detailMore(const Aircraft& a) {
  AcInfo in;
  Info_Get(&in);
  const bool mine = strcmp(in.hex, a.hex) == 0;
  char buf[48];

  UI_Text(CX, 38, a.callsign[0] ? a.callsign : a.hex, 3, C_AMBER);

  const int top = 72;
  if (mine && in.photo) {
    drawPhoto(in, top);
    snprintf(buf, sizeof(buf), "Photo: %s", in.credit);
    UI_Text(CX, 278, buf, 2, C_MID);
  } else {
    gfx->drawRect(CX - PHOTO_W / 2, top, PHOTO_W, PHOTO_H, C_DIM);
    UI_Text(CX, top + PHOTO_H / 2 - 8, (!mine || in.loading) ? "Getting photo" : "No photo found", 2, C_MID);
  }

  if (mine && in.from[0]) {
    snprintf(buf, sizeof(buf), "%s > %s", in.from, in.to);
    UI_Text(CX, 304, buf, 3, C_AMBER);
    snprintf(buf, sizeof(buf), "%.13s to %.13s", in.fromCity, in.toCity);
    UI_Text(CX, 336, buf, 2, C_GREEN);
  } else {
    UI_Text(CX, 320, (!mine || in.loading) ? "Looking up route" : "Route not known", 2, C_MID);
  }

  const char* who = (mine && in.airline[0]) ? in.airline : a.op;
  if (who[0]) UI_Text(CX, 360, who, 2, C_GREEN);

  buf[0] = '\0';
  auto add = [&](const char* s) {
    if (!s[0]) return;
    if (buf[0]) strlcat(buf, "  ", sizeof(buf));
    strlcat(buf, s, sizeof(buf));
  };
  add(a.type); add(a.reg);
  if (a.squawk[0]) { char sq[12]; snprintf(sq, sizeof(sq), "sq %s", a.squawk); add(sq); }
  UI_Text(CX, 384, buf, 2, C_MID);
}

void Detail_Draw(const Aircraft& a, bool lost, int page) {
  gfx->fillScreen(C_BLACK);
  char buf[40], num[16];
  pageDots(page);
  if (page == 1) { detailMore(a); return; }
  if (page == 2) { detailLink(a); return; }

  UI_Text(CX, 62, a.callsign[0] ? a.callsign : a.hex, 5, C_AMBER);
  // The type in words where the feed has it, otherwise its code and the registration.
  if (a.desc[0]) snprintf(buf, sizeof(buf), "%.30s", a.desc);
  else snprintf(buf, sizeof(buf), "%s%s%s", a.type[0] ? a.type : "", (a.type[0] && a.reg[0]) ? "  " : "", a.reg);
  UI_Text(CX, 116, buf, 2, C_GREEN);
  gfx->drawFastHLine(90, 146, 300, C_DIM);

  const int x0 = 86, x1 = 394;
  int y = 166;
  auto row = [&](const char* label, const char* value) {
    UI_TextL(x0, y + 4, label, 2, C_MID);
    UI_TextR(x1, y, value, 3, C_GREEN);
    y += 40;
  };

  withCommas(num, sizeof(num), lroundf(a.altFt / 25.0f) * 25);
  snprintf(buf, sizeof(buf), "%s ft", num);
  row("Altitude", buf);

  snprintf(buf, sizeof(buf), "%.0f kt", a.gsKt);
  row("Speed", buf);

  if (a.hasTrack) snprintf(buf, sizeof(buf), "%03.0f %s", a.track, UI_Compass(a.track));
  else            snprintf(buf, sizeof(buf), "unknown");
  row("Heading", buf);

  if (fabsf(a.baroRate) < 100) snprintf(buf, sizeof(buf), "level");
  else {
    withCommas(num, sizeof(num), labs(lroundf(a.baroRate / 50.0f) * 50));
    snprintf(buf, sizeof(buf), "%s%s fpm", a.baroRate > 0 ? "+" : "-", num);
  }
  row("Climb", buf);

  float km, brg;
  Geo_DistBearing(cfg.lat, cfg.lon, a.lat, a.lon, &km, &brg);
  snprintf(buf, sizeof(buf), km < 10 ? "%.1f km %s" : "%.0f km %s", km, UI_Compass(brg));
  row("Distance", buf);

  if (lost) {
    UI_Text(CX, 380, "Signal lost", 2, C_AMBER);
    UI_Text(CX, 404, "last known values", 2, C_AMBER);
  } else {
    UI_Text(CX, 380, "Tap to close", 2, C_MID);
    UI_Text(CX, 402, "Swipe for photo", 2, C_MID);
  }
}
