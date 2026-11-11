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

enum Screen { SC_BOOT, SC_SCOPE, SC_DETAIL, SC_CLOCK, SC_SETTINGS, SC_NIGHT, SC_PLACE, SC_WIFI, SC_PASS, SC_PORTAL };
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
static int      s_detailPage = 0;      // 0 = figures, 1 = photo and route, 2 = link to the photo's page

static unsigned long s_buzzOffAt = 0;
static char s_pinged[16][8];
static int  s_pingedNext = 0;

// The battery's voltage, averaged over a few readings to steady it. With no battery
// fitted this is whatever the charger leaves on the empty socket, not a battery.
static float batteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BATTERY);
  return mv / 8 * 3 / 1000.0f;
}

// Sound the buzzer; the loop switches it off again.
static void buzz(unsigned long ms) {
  TCA9554_SetPin(EXIO_BUZZER, true);
  s_buzzOffAt = millis() + ms;
  if (s_buzzOffAt == 0) s_buzzOffAt = 1;
}

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
    int tx = t.x, ty = t.y;
    gfx->turnPoint(&tx, &ty);                 // the picture may be turned on the glass
    t.x = tx; t.y = ty;
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

static unsigned long s_lastTouch = 0;

// True while the screen is dimmed for the night. Settings stay at the chosen brightness
// so they can be read and changed at night, and a touch brightens the screen for 30 seconds.
static bool dimmedNow() {
  return nightNow() && s_scr < SC_SETTINGS && millis() - s_lastTouch > NIGHT_BRIGHT_MS && cfg.bright > cfg.nightLevel;
}

static int  s_blLast = -1;
static bool s_blDark = false;           // held dark while storage is written

static void applyBacklight() {
  int b = cfg.bright;
  const bool dim = dimmedNow();
  if (dim) b = cfg.nightLevel;
  if (!s_blDark && b != s_blLast) { Set_Backlight((uint8_t)b); s_blLast = b; }
  // Red, if chosen, for the whole of the night hours, brightened by a touch or not.
  // The night page always shows the choice, so it can be seen when picking it by day.
  Theme_Set(cfg.nightRed && (nightNow() || s_scr == SC_NIGHT));
}

// Writing to the storage starves the screen of its picture for a moment, which shows
// as a burst of garbage. So what the photo page looked up is written as that page
// closes, with the light off; it comes back with the next picture drawn.
static void saveLookups() {
  if (!Info_SaveWaiting()) return;
  s_blDark = true;
  Set_Backlight(0);
  const unsigned long t0 = millis();
  Info_FlushSaves();
  Serial.printf("STORE: written in %lu ms\n", millis() - t0);
}
static void frameShown() {
  if (!s_blDark) return;
  s_blDark = false;
  s_blLast = -1;
  applyBacklight();
}

// -------------------------------------------------------------------- setup --
static void goScreen(Screen s) {
  if (s_scr == SC_DETAIL && s != SC_DETAIL) saveLookups();
  s_scr = s;
  Net_SetScopeActive(s == SC_SCOPE || s == SC_DETAIL);
}

static void showSaved() { goScreen(cfg.screen == 1 ? SC_CLOCK : SC_SCOPE); }

static void enterPortal() {
  s_scr = SC_PORTAL;
  Watchdog_Feed();
  Portal_Begin(true);
  Watchdog_Feed();
}

static void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid, cfg.pass);
}

// A new network could not be joined: return to the saved one at once.
static void rejoinSaved() {
  WiFi.disconnect();
  if (cfg.ssid[0]) { delay(100); startWifi(); }
}

// Wi-Fi setup on the device itself: list the networks nearby. Looking is free: the
// network already joined stays joined and aircraft keep arriving. Only Join (or the
// phone form's Save) leaves it, and a failed attempt goes straight back to it.
static void enterWifi() {
  Setup_DrawSplash("Looking for Wi-Fi");
  Watchdog_Feed();
  WiFi.mode(WIFI_STA);
  if (WiFi.status() != WL_CONNECTED) { WiFi.disconnect(); delay(100); }   // a join in progress blocks the search
  WifiScreen_Scan(cfg.ssid[0] != '\0' && cfg.hasPos);
  Watchdog_Feed();
  s_scr = SC_WIFI;
}

// Leave Wi-Fi setup without changing anything: back to the saved network.
static void leaveWifi() {
  if (WiFi.status() != WL_CONNECTED) startWifi();
  Net_SetPaused(false);
  goScreen(SC_SETTINGS);
}

// Join the network picked on the device. The saved network is only replaced once
// the new one has connected.
static bool tryJoin(const char* ssid, const char* pass) {
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, pass);
  const unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_JOIN_TIMEOUT_MS) {
    Setup_DrawConnecting(ssid, 0, "");
    Watchdog_Feed();
    delay(150);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WIFI: could not join");
    rejoinSaved();
    return false;
  }
  snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", ssid);
  snprintf(cfg.pass, sizeof(cfg.pass), "%s", pass);
  Settings_Save();
  WiFi.setAutoReconnect(true);
  Serial.println("WIFI: joined and saved");
  return true;
}

// After joining on the device: a new device still needs its postcode.
static void afterJoin() {
  if (!cfg.hasPos) { Keypad_OpenPostcode(); s_scr = SC_PLACE; return; }
  Net_SetPaused(false);
  Net_Kick();
  goScreen(SC_SETTINGS);
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
  WiFi.disconnect();                   // leave the saved network, or it would count as joined
  delay(100);
  WiFi.begin(r.ssid, r.pass);
  const unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_JOIN_TIMEOUT_MS) {
    Setup_DrawConnecting(r.ssid, 0, r.postcode);
    Watchdog_Feed();
    delay(150);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("SETUP: could not join");
    rejoinSaved();
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
    rejoinSaved();
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
  if (WiFi.status() != WL_CONNECTED) { Keypad_SetStatus("No Wi-Fi", C_RED); return; }
  const bool firstTime = !cfg.hasPos;
  Keypad_SetStatus("Looking up", C_AMBER);
  Keypad_Draw(); gfx->flush();
  Watchdog_Feed();
  float lat = 0, lon = 0;
  bool found = Net_LookupPostcode(Keypad_Text(), &lat, &lon);
  Watchdog_Feed();
  if (!found) { delay(1500); found = Net_LookupPostcode(Keypad_Text(), &lat, &lon); Watchdog_Feed(); }   // one retry
  if (!found) {
    Serial.println("PLACE: postcode not found");
    Keypad_SetStatus("Not found", C_RED);
    return;
  }
  // Stored the usual way, with a space before the last three characters.
  const char* t = Keypad_Text();
  const int n = (int)strlen(t);
  snprintf(cfg.postcode, sizeof(cfg.postcode), "%.*s %s", n - 3, t, t + n - 3);
  cfg.lat = lat; cfg.lon = lon; cfg.hasPos = true;
  Settings_Save();
  Serial.printf("PLACE: saved %s, position %.4f, %.4f\n", cfg.postcode, lat, lon);
  Scope_Invalidate();
  Net_Kick();
  if (firstTime) { Net_SetPaused(false); showSaved(); }   // setup finished: start showing aircraft
  else goScreen(SC_SETTINGS);
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
  gfx->setTurn(cfg.rotate);
  gfx->fillScreen(C_BLACK); gfx->flush();
  gfx->fillScreen(C_BLACK); gfx->flush();     // both buffers start black
  applyBacklight();

  pinMode(PIN_BOOT, INPUT_PULLUP);
  if (!Touch_Init()) Serial.println("WARNING: touch did not start");
  Time_Begin();
  Scope_Begin();
  Net_Begin();
  Watchdog_Begin();

  if (cfg.ssid[0] == '\0') {
    enterWifi();
  } else if (!cfg.hasPos) {                    // Wi-Fi saved, setup stopped before the postcode
    startWifi();
    Keypad_OpenPostcode();
    s_scr = SC_PLACE;
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
          s_detailPage = 0;
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
      else if (g == G_LEFT || g == G_RIGHT) {
        // Left for the next page, right for the one before; round and round.
        s_detailPage = (s_detailPage + (g == G_LEFT ? 1 : DETAIL_PAGES - 1)) % DETAIL_PAGES;
        if (s_detailPage != 0) Info_Request(s_selCache.hex, s_selCache.callsign, s_selCache.type);   // fetched only when looked at
      }
      break;
    case SC_CLOCK:
      if (g == G_UP || g == G_DOWN) { cfg.screen = 0; Settings_Save(); goScreen(SC_SCOPE); }
      else if (g == G_LONG) { s_settingsReturn = SC_CLOCK; goScreen(SC_SETTINGS); }
      break;
    case SC_SETTINGS:
    case SC_NIGHT:
      if (g == G_TAP) {
        const bool pingWas = cfg.ping;
        const SettingsAction a = s_scr == SC_SETTINGS ? SettingsScreen_Tap(x, y) : NightScreen_Tap(x, y);
        if (a == SA_CHANGED) {
          gfx->setTurn(cfg.rotate);
          Settings_Save(); applyBacklight();
          if (cfg.ping && !pingWas) { Serial.println("PING: test beep"); buzz(PING_MS); }   // lets the sound be heard when choosing it
        }
        else if (a == SA_DONE) goScreen(s_settingsReturn);
        else if (a == SA_OPEN_NIGHT) goScreen(SC_NIGHT);
        else if (a == SA_CLOSE_NIGHT) goScreen(SC_SETTINGS);
        else if (a == SA_OPEN_PLACE) { Keypad_OpenPostcode(); goScreen(SC_PLACE); }
        else if (a == SA_WIFI) enterWifi();
      }
      break;
    case SC_PLACE:
      if (g == G_TAP) {
        const SettingsAction a = Keypad_Tap(x, y);
        if (a == SA_KEYS_BACK) { if (cfg.hasPos) goScreen(SC_SETTINGS); else enterWifi(); }
        else if (a == SA_KEYS_OK) tryNewPostcode();
      }
      break;
    case SC_WIFI:
      if (g == G_TAP) {
        const SettingsAction a = WifiScreen_Tap(x, y);
        if (a == SA_WIFI_BACK) leaveWifi();
        else if (a == SA_WIFI_RESCAN) enterWifi();
        else if (a == SA_WIFI_PHONE) enterPortal();
        else if (a == SA_WIFI_PICK) {
          if (!WifiScreen_Open()) { Keypad_OpenPassword(WifiScreen_Ssid()); s_scr = SC_PASS; }
          else if (tryJoin(WifiScreen_Ssid(), "")) afterJoin();
          else failScreen("COULDN'T JOIN", WifiScreen_Ssid(), "Try again, or pick", "another network");
        }
      }
      break;
    case SC_PASS:
      if (g == G_TAP) {
        const SettingsAction a = Keypad_Tap(x, y);
        if (a == SA_KEYS_BACK) s_scr = SC_WIFI;
        else if (a == SA_KEYS_OK) {
          if (tryJoin(WifiScreen_Ssid(), Keypad_Text())) afterJoin();
          else Keypad_SetStatus("Couldn't join", C_RED);   // password kept, to correct it
        }
      }
      break;
    case SC_PORTAL:
      // A long press goes back to setting up on the device.
      if (g == G_LONG) {
        Portal_End();
        enterWifi();
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
      Serial.printf("PING: %s at %.1f km\n", s_snap[i].hex, km);
      buzz(PING_MS);
    }
  }
}

// ------------------------------------------------------------ factory reset --
// Holding the BOOT button on the side for 8 seconds forgets the Wi-Fi, postcode and
// every setting, then restarts into first-time setup. It works from any screen, so a
// device that cannot be operated by touch can still be cleared. Letting go early
// cancels. Returns true while the warning owns the screen.
static bool factoryResetHeld() {
  static unsigned long downAt = 0;
  if (digitalRead(PIN_BOOT) != LOW) { downAt = 0; return false; }
  const unsigned long now = millis();
  if (!downAt) { downAt = now ? now : 1; Serial.println("BOOT button down"); }
  const unsigned long held = now - downAt;
  if (held < FACTORY_WARN_MS) return false;
  if (held >= FACTORY_HOLD_MS) {
    Serial.println("FACTORY RESET: erasing saved settings");
    gfx->fillScreen(C_BLACK);
    UI_Text(240, 220, "ERASED", 4, C_RED);
    UI_Text(240, 280, "Restarting", 2, C_MID);
    gfx->flush();
    Settings_Erase();
    Info_EraseStore();
    WiFi.disconnect(true, true);       // also the copy the Wi-Fi driver keeps
    delay(800);
    ESP.restart();
  }
  char buf[24];
  gfx->fillScreen(C_BLACK);
  UI_Text(240, 120, "FACTORY RESET", 3, C_RED);
  snprintf(buf, sizeof(buf), "%lu", (FACTORY_HOLD_MS - held) / 1000 + 1);
  UI_Text(240, 190, buf, 8, C_AMBER);
  UI_Text(240, 290, "Keep holding to erase", 2, C_AMBER);
  UI_Text(240, 316, "Wi-Fi, postcode, settings", 2, C_MID);
  UI_Text(240, 366, "Let go to cancel", 2, C_GREEN);
  gfx->flush();
  return true;
}

// --------------------------------------------------------------------- loop --
void App_Loop() {
  Watchdog_Feed();
  // A line typed over USB. "wipe card" empties the memory card (for a card of unknown
  // history, when there is no reader to hand).
  static char cmd[16]; static int cmdN = 0;
  while (Serial.available()) {
    const char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      cmd[cmdN] = '\0';
      if (!strcmp(cmd, "wipe card")) Info_WipeCard();
      else if (!strcmp(cmd, "list card")) Info_ListCard();
      else if (!strncmp(cmd, "rotate ", 7)) { cfg.rotate = (int16_t)(((atoi(cmd + 7) % 360) + 360) % 360); gfx->setTurn(cfg.rotate); Settings_Save(); Serial.printf("ROTATE: %d\n", cfg.rotate); }
      else if (!strcmp(cmd, "battery")) Serial.printf("BATT: %.2f V\n", batteryVolts());
      else if (!strcmp(cmd, "clear photos")) Info_EraseStore();
      cmdN = 0;
    } else if (cmdN < (int)sizeof(cmd) - 1) cmd[cmdN++] = ch;
  }
  static bool warned = false;
  if (factoryResetHeld()) { warned = true; delay(20); return; }
  if (warned) {                        // let go early: the QR screen is drawn once, so put it back
    warned = false;
    if (s_scr == SC_PORTAL) Portal_Redraw();
  }
  const unsigned long now = millis();

  if (s_buzzOffAt && (long)(now - s_buzzOffAt) >= 0) { TCA9554_SetPin(EXIO_BUZZER, false); s_buzzOffAt = 0; }

  // Fault finding: the longest each part of a pass took, reported in the SCOPE log line.
  static unsigned long slowTouch = 0, slowKeep = 0, slowData = 0, slowLog = 0, lastFrame = 0;
  int gx = 0, gy = 0;
  unsigned long ta = millis();
  const Gesture g = pollTouch(&gx, &gy);
  if (g != G_NONE) {
    // A touch on a dimmed screen only brightens it; the next one acts.
    const bool wakeOnly = dimmedNow();
    s_lastTouch = millis();
    if (!wakeOnly) handleGesture(g, gx, gy);
    applyBacklight();
  }
  // In the night hours nobody is watching: stop downloading until the screen is touched.
  Net_SetResting(nightNow() && millis() - s_lastTouch > NIGHT_AWAKE_MS);
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
    frameShown();
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
      char st[200];
      Net_Status(st, sizeof(st));
      Serial.printf("NET: %s\n", st);
      Serial.printf("BATT: %.2f V\n", batteryVolts());
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
    case SC_DETAIL:   Detail_Draw(s_selCache, s_selMiss > 0, s_detailPage); break;
    case SC_CLOCK:    Clock_Draw(s_snapN, wifi, Net_EverOk() && Net_DataAgeMs() < FETCH_BACKGROUND_MS + DATA_STALE_MS); break;
    case SC_SETTINGS: SettingsScreen_Draw(); break;
    case SC_NIGHT:    NightScreen_Draw(); break;
    case SC_PLACE:
    case SC_PASS:     Keypad_Draw(); break;
    case SC_WIFI:     WifiScreen_Draw(); break;
    default: break;
  }
  gfx->flush();
  frameShown();
}
