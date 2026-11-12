// Round Radar - time keeping (clock chip PCF85063), shared text helpers, and
// the clock screen.
#include "App.h"
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

// ------------------------------------------------------------------ helpers --
// The built-in font is 6x8 pixels per size step.
void UI_TextL(int x, int y, const char* s, int size, uint16_t col) {
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(x, y);
  gfx->print(s);
}
void UI_Text(int cx, int y, const char* s, int size, uint16_t col) {
  UI_TextL(cx - (int)strlen(s) * 3 * size, y, s, size, col);
}
void UI_TextR(int xRight, int y, const char* s, int size, uint16_t col) {
  UI_TextL(xRight - (int)strlen(s) * 6 * size, y, s, size, col);
}
// ------------------------------------------------------------------- theme --
uint16_t C_GREEN = 0x3FEF, C_MID = 0x252A, C_DIM = 0x1305, C_AMBER = 0xFE49, C_GREY = 0x6B8D;
static bool s_red = false;
bool Theme_Red() { return s_red; }
void Theme_Set(bool red) {
  if (red == s_red) return;
  s_red = red;
  if (red) { C_GREEN = 0xF943; C_MID = 0xA0C2; C_DIM = 0x6061; C_AMBER = 0xFB65; C_GREY = 0x6986; }
  else     { C_GREEN = 0x3FEF; C_MID = 0x252A; C_DIM = 0x1305; C_AMBER = 0xFE49; C_GREY = 0x6B8D; }
  Scope_ThemeChanged();
}

void UI_Pill(int x, int y, int w, int h, const char* s, bool on) {
  if (on) gfx->fillRoundRect(x, y, w, h, 9, C_GREEN);
  else    gfx->drawRoundRect(x, y, w, h, 9, C_MID);
  UI_Text(x + w / 2, y + (h - 16) / 2, s, 2, on ? C_BLACK : C_GREEN);
}
const char* UI_Compass(float deg) {
  static const char* P[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                               "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
  int i = (int)floorf(fmodf(deg + 360.0f, 360.0f) / 22.5f + 0.5f) & 15;
  return P[i];
}
void Geo_DistBearing(float lat1, float lon1, float lat2, float lon2, float* km, float* brg) {
  const float dx = (lon2 - lon1) * 111.32f * cosf(lat1 * 0.0174532925f);
  const float dy = (lat2 - lat1) * 110.57f;
  if (km) *km = sqrtf(dx * dx + dy * dy);
  if (brg) { float b = atan2f(dx, dy) * 57.29578f; if (b < 0) b += 360.0f; *brg = b; }
}

// --------------------------------------------------------------- clock chip --
#define RTC_ADDR 0x51
static bool s_rtcOk = false;

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Days since 1970-01-01 for a calendar date (UTC), so the chip can hold UTC
// without depending on the C library's local-time conversions.
static long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

bool Time_Valid() { return time(nullptr) > 1750000000; }   // after mid-2025

void Time_Begin() {
  setenv("TZ", TZ_UK, 1);
  tzset();

  Wire.beginTransmission(RTC_ADDR);
  s_rtcOk = (Wire.endTransmission() == 0);
  if (!s_rtcOk) { Serial.println("RTC: clock chip not found"); return; }

  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(RTC_ADDR, 7) != 7) return;
  uint8_t r[7];
  for (int i = 0; i < 7; i++) r[i] = Wire.read();
  if (r[0] & 0x80) { Serial.println("RTC: time lost (no backup power), waiting for the internet"); return; }
  const int year = 2000 + bcd2bin(r[6]);
  if (year < 2026) return;
  const long days = daysFromCivil(year, bcd2bin(r[5] & 0x1F), bcd2bin(r[3] & 0x3F));
  struct timeval tv;
  tv.tv_sec = days * 86400L + bcd2bin(r[2] & 0x3F) * 3600L + bcd2bin(r[1] & 0x7F) * 60L + bcd2bin(r[0] & 0x7F);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  Serial.println("RTC: time restored from the clock chip");
}

void Time_SaveToChip() {
  if (!s_rtcOk || !Time_Valid()) return;
  time_t t = time(nullptr);
  struct tm g;
  gmtime_r(&t, &g);
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  Wire.write(bin2bcd(g.tm_sec));          // also clears the "time lost" flag
  Wire.write(bin2bcd(g.tm_min));
  Wire.write(bin2bcd(g.tm_hour));
  Wire.write(bin2bcd(g.tm_mday));
  Wire.write(g.tm_wday);
  Wire.write(bin2bcd(g.tm_mon + 1));
  Wire.write(bin2bcd(g.tm_year - 100));
  Wire.endTransmission();
}

// ------------------------------------------------------------- clock screen --
// Seven-segment digit: which of segments a..g are lit for 0..9.
static const uint8_t SEG[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
#define C_GHOST (Theme_Red() ? 0x1800 : 0x00E1)   // unlit segment, barely visible

static void drawDigit(int x, int y, int w, int h, int d) {
  const int t = 15, v = h / 2 - (3 * t) / 2;
  const uint8_t on = (d >= 0 && d <= 9) ? SEG[d] : 0;
  const int r[7][4] = {
    { x + t, y, w - 2 * t, t },                       // a top
    { x + w - t, y + t, t, v },                       // b top right
    { x + w - t, y + h / 2 + t / 2, t, v },           // c bottom right
    { x + t, y + h - t, w - 2 * t, t },               // d bottom
    { x, y + h / 2 + t / 2, t, v },                   // e bottom left
    { x, y + t, t, v },                               // f top left
    { x + t, y + h / 2 - t / 2, w - 2 * t, t },       // g middle
  };
  for (int i = 0; i < 7; i++)
    gfx->fillRoundRect(r[i][0], r[i][1], r[i][2], r[i][3], 5, (on >> i) & 1 ? C_GREEN : C_GHOST);
}

// Battery outline filled to its level, the percentage beside it, and a lightning bolt
// across the battery while power is coming in.
static void drawBattery(int cx, int y) {
  const int pct = Power_Percent();
  const bool low = pct <= 15 && !Power_Charging();
  const uint16_t col = low ? C_AMBER : C_GREEN;
  char txt[6];
  snprintf(txt, sizeof(txt), "%d%%", pct);
  const int bw = 40, bh = 20, gap = 10, tw = (int)strlen(txt) * 12;
  const int x = cx - (bw + 4 + gap + tw) / 2;
  gfx->drawRoundRect(x, y, bw, bh, 4, col);
  gfx->drawRoundRect(x + 1, y + 1, bw - 2, bh - 2, 3, col);
  gfx->fillRect(x + bw, y + 6, 4, 8, col);                           // the cap
  const int fill = (bw - 8) * pct / 100;
  if (fill > 0) gfx->fillRect(x + 4, y + 4, fill, bh - 8, low ? C_AMBER : C_MID);
  UI_TextL(x + bw + 4 + gap, y + 2, txt, 2, col);
  if (Power_Charging()) {
    const int bx = x + bw / 2;
    for (int pass = 0; pass < 2; pass++) {                           // a black edge first, so it stands out
      const int g = pass ? 0 : 2;
      const uint16_t c = pass ? C_AMBER : C_BLACK;
      gfx->fillTriangle(bx + 4 + g, y - 5 - g, bx - 7 - g, y + 12, bx + 1, y + 12 + g, c);
      gfx->fillTriangle(bx - 4 - g, y + 25 + g, bx + 7 + g, y + 8, bx - 1, y + 8 - g, c);
    }
  }
}

void Clock_Draw(int aircraft, bool wifi, bool dataFresh) {
  gfx->fillScreen(C_BLACK);

  const bool valid = Time_Valid();
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);

  // Seconds ring: 60 marks, lit up to the current second.
  for (int i = 0; i < 60; i++) {
    const float a = i * 6 * 0.0174532925f, s = sinf(a), c = cosf(a);
    const int r0 = (i % 5) ? 222 : 214, r1 = 232;
    const uint16_t col = (valid && i <= lt.tm_sec) ? C_GREEN : (Theme_Red() ? 0x2000 : 0x01E1);
    const int x0 = 240 + (int)(r0 * s), y0 = 240 - (int)(r0 * c);
    const int x1 = 240 + (int)(r1 * s), y1 = 240 - (int)(r1 * c);
    gfx->drawLine(x0, y0, x1, y1, col);
    gfx->drawLine(x0 + 1, y0, x1 + 1, y1, col);
    gfx->drawLine(x0, y0 + 1, x1, y1 + 1, col);
  }

  int hour = lt.tm_hour;
  const bool pm = hour >= 12;
  if (!cfg.h24) { hour %= 12; if (hour == 0) hour = 12; }
  const int y = 165, h = 130, w = 70;
  if (valid) {
    drawDigit(60, y, w, h, (hour >= 10 || cfg.h24) ? hour / 10 : -1);   // no leading zero in 12 hour
    drawDigit(146, y, w, h, hour % 10);
    drawDigit(264, y, w, h, lt.tm_min / 10);
    drawDigit(350, y, w, h, lt.tm_min % 10);
  } else {
    for (int x : { 60, 146, 264, 350 }) drawDigit(x, y, w, h, -1);
  }
  gfx->fillRoundRect(232, y + 36, 16, 16, 4, C_GREEN);
  gfx->fillRoundRect(232, y + 80, 16, 16, 4, C_GREEN);

  if (valid) {
    if (!cfg.h24) UI_Text(240, 122, pm ? "PM" : "AM", 3, C_GREEN);
    static const char* DAY[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    static const char* MON[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    char date[16];
    snprintf(date, sizeof(date), "%s %d %s", DAY[lt.tm_wday], lt.tm_mday, MON[lt.tm_mon]);
    UI_Text(240, 326, date, 3, C_GREEN);
  } else {
    UI_Text(240, 326, "SETTING TIME", 3, C_MID);
  }

  drawBattery(240, 78);

  char foot[28];
  if (!wifi) {
    UI_Text(240, 382, "No Wi-Fi", 2, C_AMBER);
  } else if (dataFresh) {
    snprintf(foot, sizeof(foot), "%d aircraft in range", aircraft);
    UI_Text(240, 382, foot, 2, C_MID);
  }
}
