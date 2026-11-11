// Round Radar - start-up, gestures, and the loop that owns the display.
//
// Core 1 (this file) draws every frame and reads the touch panel.
// Core 0 (Net.cpp) fetches aircraft, so the sweep never waits on a download.
#include "App.h"
#include "TCA9554.h"
#include "Board.h"
#include "Display_ST7701.h"
#include "Touch.h"
#include "Watchdog.h"
#include <Wire.h>
#include <WiFi.h>
#include <time.h>

Canvas16* gfx = nullptr;

enum Screen { SC_BOOT, SC_SCOPE, SC_DETAIL, SC_CLOCK, SC_SETTINGS, SC_NIGHT, SC_PLACE, SC_PORTAL };
static Screen s_scr = SC_BOOT;
static Screen s_settingsReturn = SC_SCOPE;
static unsigned long s_bootStart = 0;

static Aircraft s_snap[AC_MAX];
static int      s_snapN = 0;
static uint32_t s_snapVer = 0;

// The aircraft whose detail is open, remembered by ICAO address, with the last
// known values kept so the page survives the feed dropping it for a poll or two.
static char     s_selHex[8] = "";
static Aircraft s_selCache;
static int      s_selMiss = 0;

static unsigned long s_buzzOffAt = 0;
static char s_pinged[16][8];
static int  s_pingedNext = 0;

// ----------------------------------------------------------------- gestures --
enum Gesture { G_NONE, G_TAP, G_LEFT, G_RIGHT, G_UP, G_DOWN, G_LONG };

static Gesture pollTouch(int* gx, int* gy) {
  static bool down = false, longFired = false;
  static int sx, sy, lx, ly;
  static unsigned long t0, lastSeen;
  TouchData t;
  Touch_Read(&t);
  const unsigned long now = millis();
  if (t.points) {
    if (!down) { down = true; longFired = false; sx = t.x; sy = t.y; t0 = now; }
    lx = t.x; ly = t.y; lastSeen = now;
    if (!longFired && now - t0 >= LONG_PRESS_MS && abs(lx - sx) < SWIPE_PX / 2 && abs(ly - sy) < SWIPE_PX / 2) {
      longFired = true;
      *gx = sx; *gy = sy;
      return G_LONG;
    }
    return G_NONE;
  }
  if (down && now - lastSeen >= TOUCH_RELEASE_MS) {
    down = false;
    if (longFired) return G_NONE;
    const int dx = lx - sx, dy = ly - sy;
    if (abs(dx) >= SWIPE_PX || abs(dy) >= SWIPE_PX)
      return abs(dx) > abs(dy) ? (dx < 0 ? G_LEFT : G_RIGHT) : (dy < 0 ? G_UP : G_DOWN);
    *gx = sx; *gy = sy;
    return G_TAP;
  }
  return G_NONE;
}

// ---------------------------------------------------------------- backlight --
static bool nightNow() {
  if (!cfg.nightOn || !Time_Valid() || cfg.nightFrom == cfg.nightTo) return false;
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  const int h = lt.tm_hour;
  return cfg.nightFrom < cfg.nightTo ? (h >= cfg.nightFrom && h < cfg.nightTo)
                                     : (h >= cfg.nightFrom || h < cfg.nightTo);
}

static void applyBacklight() {
  static int last = -1;
  int b = cfg.bright;
  // Settings stay at the chosen brightness so they can be read and changed at night.
  if (nightNow() && s_scr != SC_SETTINGS && s_scr != SC_NIGHT && s_scr != SC_PLACE && b > NIGHT_BRIGHTNESS) b = NIGHT_BRIGHTNESS;
  if (b != last) { Set_Backlight((uint8_t)b); last = b; }
}

// -------------------------------------------------------------------- setup --
static void goScreen(Screen s) {
  s_scr = s;
  Net_SetScopeActive(s == SC_SCOPE || s == SC_DETAIL);
}

static void showSaved() { goScreen(cfg.screen == 1 ? SC_CLOCK : SC_SCOPE); }

static void enterPortal() {
  Net_SetPaused(true);
  s_scr = SC_PORTAL;
  Watchdog_Feed();
  Portal_Begin(cfg.ssid[0] != '\0');
  Watchdog_Feed();
}

static void failScreen(const char* title, const char* what, const char* l1, const char* l2) {
  for (int s = 10; s > 0; s--) {
    Setup_DrawFail(title, what, l1, l2, s);
    Watchdog_Feed();
    delay(1000);
  }
}

// Try the details from the phone form. Saved settings are only replaced once
// the new Wi-Fi has connected and the postcode has been found.
static bool tryNewDetails(const PortalResult& r) {
  Portal_End();
  WiFi.begin(r.ssid, r.pass);
  const unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_JOIN_TIMEOUT_MS) {
    Setup_DrawConnecting(r.ssid, 0, r.postcode);
    Watchdog_Feed();
    delay(150);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("SETUP: could not join");
    WiFi.disconnect(true);
    failScreen("COULDN'T JOIN", r.ssid, "Check the password", "and try again");
    return false;
  }
  Setup_DrawConnecting(r.ssid, 1, r.postcode);
  Watchdog_Feed();
  float lat = 0, lon = 0;
  bool found = Net_LookupPostcode(r.postcode, &lat, &lon);
  Watchdog_Feed();
  if (!found) { delay(1500); found = Net_LookupPostcode(r.postcode, &lat, &lon); Watchdog_Feed(); }   // one retry
  if (!found) {
    Serial.println("SETUP: postcode not found");
    WiFi.disconnect(true);
    failScreen("PLACE NOT FOUND", r.postcode, "Check the postcode", "and try again");
    return false;
  }
  strncpy(cfg.ssid, r.ssid, sizeof(cfg.ssid) - 1);       cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
  strncpy(cfg.pass, r.pass, sizeof(cfg.pass) - 1);       cfg.pass[sizeof(cfg.pass) - 1] = '\0';
  strncpy(cfg.postcode, r.postcode, sizeof(cfg.postcode) - 1); cfg.postcode[sizeof(cfg.postcode) - 1] = '\0';
  for (char* p = cfg.postcode; *p; p++) *p = toupper((unsigned char)*p);
  cfg.lat = lat; cfg.lon = lon; cfg.hasPos = true;
  Settings_Save();
  Serial.printf("SETUP: saved, position %.4f, %.4f\n", lat, lon);
  return true;
}

// The postcode keypad's OK: look the postcode up over the Wi-Fi already joined.
// The saved place is only replaced once the new one has been found.
static void tryNewPostcode() {
  if (WiFi.status() != WL_CONNECTED) { PlaceScreen_SetStatus("No Wi-Fi", C_RED); return; }
  PlaceScreen_SetStatus("Looking up", C_AMBER);
  PlaceScreen_Draw(); gfx->flush();
  Watchdog_Feed();
  float lat = 0, lon = 0;
  bool found = Net_LookupPostcode(PlaceScreen_Text(), &lat, &lon);
  Watchdog_Feed();
  if (!found) { delay(1500); found = Net_LookupPostcode(PlaceScreen_Text(), &lat, &lon); Watchdog_Feed(); }   // one retry
  if (!found) {
    Serial.println("PLACE: postcode not found");
    PlaceScreen_SetStatus("Not found", C_RED);
    return;
  }
  // Stored the usual way, with a space before the last three characters.
  const char* t = PlaceScreen_Text();
  const int n = (int)strlen(t);
  snprintf(cfg.postcode, sizeof(cfg.postcode), "%.*s %s", n - 3, t, t + n - 3);
  cfg.lat = lat; cfg.lon = lon; cfg.hasPos = true;
  Settings_Save();
  Serial.printf("PLACE: saved %s, position %.4f, %.4f\n", cfg.postcode, lat, lon);
  Scope_Invalidate();
  Net_Kick();
  goScreen(SC_SETTINGS);
}

static void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid, cfg.pass);
}

void App_Setup() {
  Serial.begin(115200);
  // Log lines must never hold up the picture. Over USB a line can otherwise wait for the
  // computer to read it, which stops the sweep until it gives up.
  Serial.setTxTimeoutMs(0);
  delay(300);
  Serial.printf("\n=== Round Radar v%s ===\n", FW_VERSION);
  Serial.printf("Reset reason: %d, free memory: %u B, PSRAM: %u B\n",
                (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

  Settings_Load();

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(50);
  TCA9554_Init();
  TCA9554_SetPin(EXIO_BUZZER, false);
  delay(10);
  Board_Detect();
  Serial.printf("Board: %s%s\n", Board_Name(), Board_Detected() ? "" : " (fallback)");

  Backlight_Init();
  if (!ST7701_Init()) {
    Serial.println("Display failed to start, restarting in 5 s");
    delay(5000);
    ESP.restart();
  }
  gfx = new Canvas16(LCD_WIDTH, LCD_HEIGHT);
  uint16_t* fb1 = LCD_FrameBuffer(1);
  if (fb1) gfx->useExternalBuffer(fb1, 1);
  else if (!gfx->begin()) { Serial.println("FATAL: no memory for the screen (check PSRAM is set to OPI)"); while (true) delay(1000); }
  gfx->setTextWrap(false);
  gfx->fillScreen(C_BLACK); gfx->flush();
  gfx->fillScreen(C_BLACK); gfx->flush();     // both buffers start black
  applyBacklight();

  if (!Touch_Init()) Serial.println("WARNING: touch did not start");
  Time_Begin();
  Scope_Begin();
  Net_Begin();
  Watchdog_Begin();

  if (cfg.ssid[0] == '\0' || !cfg.hasPos) {
    enterPortal();
  } else {
    startWifi();
    Net_SetPaused(false);
    s_scr = SC_BOOT;
    s_bootStart = millis();
  }
  Serial.println("Setup done");
}

// ------------------------------------------------------------------ gestures --
static void handleGesture(Gesture g, int x, int y) {
  switch (s_scr) {
    case SC_SCOPE:
      if (g == G_TAP) {
        if (Scope_HitTest(x, y, s_selHex)) {
          s_selMiss = 0;
          for (int i = 0; i < s_snapN; i++) if (!strcmp(s_snap[i].hex, s_selHex)) s_selCache = s_snap[i];
          goScreen(SC_DETAIL);
        }
      } else if (g == G_LEFT || g == G_RIGHT) {
        // Swipe left for a wider view, right for a closer one; stops at each end.
        const int next = (int)cfg.rangeIdx + (g == G_LEFT ? 1 : -1);
        if (next >= 0 && next < RANGE_COUNT) {
          cfg.rangeIdx = (uint8_t)next;
          Scope_Invalidate();
          Net_Kick();
          Settings_Save();
          Serial.printf("Range: %.0f km\n", RANGES_KM[cfg.rangeIdx]);
        }
      } else if (g == G_UP || g == G_DOWN) {
        cfg.screen = 1; Settings_Save(); goScreen(SC_CLOCK);
      } else if (g == G_LONG) {
        s_settingsReturn = SC_SCOPE; goScreen(SC_SETTINGS);
      }
      break;
    case SC_DETAIL:
      if (g == G_TAP) { s_selHex[0] = '\0'; goScreen(SC_SCOPE); }
      break;
    case SC_CLOCK:
      if (g == G_UP || g == G_DOWN) { cfg.screen = 0; Settings_Save(); goScreen(SC_SCOPE); }
      else if (g == G_LONG) { s_settingsReturn = SC_CLOCK; goScreen(SC_SETTINGS); }
      break;
    case SC_SETTINGS:
    case SC_NIGHT:
      if (g == G_TAP) {
        const SettingsAction a = s_scr == SC_SETTINGS ? SettingsScreen_Tap(x, y) : NightScreen_Tap(x, y);
        if (a == SA_CHANGED) { Settings_Save(); applyBacklight(); }
        else if (a == SA_DONE) goScreen(s_settingsReturn);
        else if (a == SA_OPEN_NIGHT) goScreen(SC_NIGHT);
        else if (a == SA_CLOSE_NIGHT) goScreen(SC_SETTINGS);
        else if (a == SA_OPEN_PLACE) { PlaceScreen_Open(); goScreen(SC_PLACE); }
        else if (a == SA_WIFI) enterPortal();
      }
      break;
    case SC_PLACE:
      if (g == G_TAP) {
        const SettingsAction a = PlaceScreen_Tap(x, y);
        if (a == SA_CLOSE_PLACE) goScreen(SC_SETTINGS);
        else if (a == SA_PLACE_SUBMIT) tryNewPostcode();
      }
      break;
    case SC_PORTAL:
      // Entered from settings by mistake: a long press goes back to the saved network.
      if (g == G_LONG && cfg.ssid[0] && cfg.hasPos) {
        Portal_End();
        startWifi();
        Net_SetPaused(false);
        showSaved();
      }
      break;
    default: break;
  }
}

// ------------------------------------------------------------- aircraft data --
static void onNewData() {
  s_snapN = Net_Snapshot(s_snap);

  if (s_selHex[0]) {
    bool found = false;
    for (int i = 0; i < s_snapN; i++)
      if (!strcmp(s_snap[i].hex, s_selHex)) { s_selCache = s_snap[i]; found = true; break; }
    if (found) s_selMiss = 0;
    else if (++s_selMiss > DETAIL_GRACE_POLLS && s_scr == SC_DETAIL) { s_selHex[0] = '\0'; goScreen(SC_SCOPE); }
  }

  if (cfg.ping) {
    for (int i = 0; i < s_snapN; i++) {
      float km;
      Geo_DistBearing(cfg.lat, cfg.lon, s_snap[i].lat, s_snap[i].lon, &km, nullptr);
      if (km > PING_KM) continue;
      bool seen = false;
      for (auto& h : s_pinged) if (!strcmp(h, s_snap[i].hex)) { seen = true; break; }
      if (seen) continue;
      strcpy(s_pinged[s_pingedNext], s_snap[i].hex);
      s_pingedNext = (s_pingedNext + 1) % 16;
      TCA9554_SetPin(EXIO_BUZZER, true);
      s_buzzOffAt = millis() + PING_MS;
      if (s_buzzOffAt == 0) s_buzzOffAt = 1;
    }
  }
}

// --------------------------------------------------------------------- loop --
void App_Loop() {
  Watchdog_Feed();
  const unsigned long now = millis();

  if (s_buzzOffAt && (long)(now - s_buzzOffAt) >= 0) { TCA9554_SetPin(EXIO_BUZZER, false); s_buzzOffAt = 0; }

  // Fault finding: the longest each part of a pass took, reported in the SCOPE log line.
  static unsigned long slowTouch = 0, slowKeep = 0, slowData = 0, slowLog = 0, lastFrame = 0;
  int gx = 0, gy = 0;
  unsigned long ta = millis();
  const Gesture g = pollTouch(&gx, &gy);
  if (g != G_NONE) handleGesture(g, gx, gy);
  if (millis() - ta > slowTouch) slowTouch = millis() - ta;

  if (s_scr == SC_PORTAL) {
    PortalResult r;
    if (Portal_Loop(&r)) {
      if (tryNewDetails(r)) {
        WiFi.setAutoReconnect(true);
        Scope_Invalidate();
        Net_SetPaused(false);
        Net_Kick();
        showSaved();
      } else {
        enterPortal();
      }
    }
    delay(5);
    return;
  }

  // Slow housekeeping, once a second.
  static unsigned long lastSlow = 0;
  ta = millis();
  if (now - lastSlow >= 1000) {
    lastSlow = now;
    applyBacklight();
    if (Net_TakeTimeSynced()) { Time_SaveToChip(); Serial.println("TIME: set from the internet"); }
    static uint8_t tick = 0;
    if (++tick % 5 == 0) TCA9554_Verify();           // the expander holds the display's reset line
  }

  if (millis() - ta > slowKeep) slowKeep = millis() - ta;
  ta = millis();
  if (Net_Version() != s_snapVer) { s_snapVer = Net_Version(); onNewData(); }
  if (millis() - ta > slowData) slowData = millis() - ta;
  if (s_scr != SC_SCOPE) lastFrame = 0;           // time away from the scope is not a freeze

  const bool wifi = WiFi.status() == WL_CONNECTED;

  if (s_scr == SC_BOOT) {
    static unsigned long lastDraw = 0;
    if (now - lastDraw >= 250) { lastDraw = now; Setup_DrawSplash("Joining Wi-Fi"); }
    // Do not wait for ever: after a power cut the router may take minutes, and
    // the clock still works meanwhile. Wi-Fi keeps retrying in the background.
    if (wifi || now - s_bootStart > 12000) showSaved();
    delay(5);
    return;
  }

  // The scope animates, so it draws every pass (the flush waits for the panel's
  // refresh). The other screens only need a few frames a second.
  static unsigned long lastDraw = 0, fpsT0 = 0;
  static int frames = 0;
  if (s_scr == SC_SCOPE) {
    // Fault finding for "the sweep freezes": every 10 s the log says how long the
    // longest wait between two pictures was, how many the panel really showed, and
    // for how many of ours Wi-Fi was down (the sweep is stopped on purpose then).
    static unsigned long maxGap = 0, slowDraw = 0, slowFlush = 0;
    static uint32_t panel0 = 0;
    static int wifiDown = 0;
    static bool wasWifi = true;
    if (wifi != wasWifi) { Serial.printf("WIFI: %s (status %d)\n", wifi ? "back" : "lost", (int)WiFi.status()); wasWifi = wifi; }
    if (!wifi) wifiDown++;
    const unsigned long t0 = millis();
    if (lastFrame && t0 - lastFrame > maxGap) maxGap = t0 - lastFrame;
    lastFrame = t0;
    Scope_Draw(s_snap, s_snapN, wifi);
    const unsigned long t1 = millis();
    gfx->flush();
    const unsigned long t2 = millis();
    if (t1 - t0 > slowDraw) slowDraw = t1 - t0;
    if (t2 - t1 > slowFlush) slowFlush = t2 - t1;
    frames++;
    if (now - fpsT0 >= 10000) {
      const unsigned long tl = millis();
      Serial.printf("SCOPE: %.1f frames/s, %d aircraft, free memory %u B, longest gap %lu ms (draw %lu, flush %lu, touch %lu, upkeep %lu, data %lu, log %lu), panel showed %u, Wi-Fi down %d frames, signal %d\n",
                    frames * 1000.0f / (now - fpsT0), s_snapN, (unsigned)ESP.getFreeHeap(),
                    maxGap, slowDraw, slowFlush, slowTouch, slowKeep, slowData, slowLog,
                    (unsigned)(LCD_VsyncCount() - panel0), wifiDown, (int)WiFi.RSSI());
      slowLog = millis() - tl;                    // shown in the next line
      fpsT0 = now; frames = 0; maxGap = 0; slowDraw = 0; slowFlush = 0; slowTouch = 0; slowKeep = 0; slowData = 0;
      wifiDown = 0; panel0 = LCD_VsyncCount();
    }
    return;
  }
  if (now - lastDraw < 100) { delay(5); return; }
  lastDraw = now;
  fpsT0 = now; frames = 0;
  switch (s_scr) {
    case SC_DETAIL:   Detail_Draw(s_selCache, s_selMiss > 0); break;
    case SC_CLOCK:    Clock_Draw(s_snapN, wifi, Net_EverOk() && Net_DataAgeMs() < DATA_STALE_MS); break;
    case SC_SETTINGS: SettingsScreen_Draw(); break;
    case SC_NIGHT:    NightScreen_Draw(); break;
    case SC_PLACE:    PlaceScreen_Draw(); break;
    default: break;
  }
  gfx->flush();
}
