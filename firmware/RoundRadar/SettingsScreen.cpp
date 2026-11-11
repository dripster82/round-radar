// Round Radar - the settings screen and its night-dimming page.
// Layout follows mockups.html. Every button is 40 px tall (about 4.5 mm) and
// taps are accepted a few pixels outside it.
#include "App.h"
#include <WiFi.h>

struct Btn { int16_t x, y, w, h; };
static bool hit(const Btn& b, int x, int y) {
  const int m = 8;
  return x >= b.x - m && x < b.x + b.w + m && y >= b.y - m && y < b.y + b.h + m;
}
static void pill(const Btn& b, const char* s, bool on) { UI_Pill(b.x, b.y, b.w, b.h, s, on); }

// ----------------------------------------------------------------- settings --
static const Btn B_BR_MINUS = { 252, 98, 42, 40 },  B_BR_PLUS = { 358, 98, 42, 40 };
static const Btn B_12H      = { 252, 152, 72, 40 }, B_24H     = { 328, 152, 72, 40 };
static const Btn B_NIGHT    = { 252, 206, 148, 40 };
static const Btn B_PING_OFF = { 252, 260, 72, 40 }, B_PING_ON = { 328, 260, 72, 40 };
static const Btn B_PLACE    = { 96, 322, 140, 40 },  B_WIFI    = { 244, 322, 140, 40 };
static const Btn B_DONE     = { 180, 382, 120, 40 };

void SettingsScreen_Draw() {
  gfx->fillScreen(C_BLACK);
  UI_Text(240, 52, "SETTINGS", 3, C_GREEN);
  char buf[16];

  UI_TextL(84, 110, "Brightness", 2, C_GREEN);
  pill(B_BR_MINUS, "-", false);
  snprintf(buf, sizeof(buf), "%d%%", cfg.bright);
  UI_Text(326, 110, buf, 2, C_GREEN);
  pill(B_BR_PLUS, "+", false);

  UI_TextL(84, 164, "Clock", 2, C_GREEN);
  pill(B_12H, "12h", !cfg.h24);
  pill(B_24H, "24h", cfg.h24);

  UI_TextL(84, 218, "Night dim", 2, C_GREEN);
  if (cfg.nightOn) snprintf(buf, sizeof(buf), "%02d-%02d >", cfg.nightFrom, cfg.nightTo);
  else             snprintf(buf, sizeof(buf), "Off >");
  pill(B_NIGHT, buf, false);

  UI_TextL(84, 272, "Ping", 2, C_GREEN);
  pill(B_PING_OFF, "Off", !cfg.ping);
  pill(B_PING_ON, "On", cfg.ping);

  pill(B_PLACE, "Postcode", false);
  pill(B_WIFI, "Wi-Fi setup", false);
  pill(B_DONE, "Done", true);
}

SettingsAction SettingsScreen_Tap(int x, int y) {
  if (hit(B_DONE, x, y)) return SA_DONE;
  if (hit(B_PLACE, x, y)) return SA_OPEN_PLACE;
  if (hit(B_WIFI, x, y)) return SA_WIFI;
  if (hit(B_NIGHT, x, y)) return SA_OPEN_NIGHT;
  if (hit(B_BR_MINUS, x, y)) { if (cfg.bright > 10) cfg.bright -= 10; return SA_CHANGED; }
  if (hit(B_BR_PLUS, x, y))  { if (cfg.bright < 100) cfg.bright += 10; return SA_CHANGED; }
  if (hit(B_12H, x, y)) { cfg.h24 = false; return SA_CHANGED; }
  if (hit(B_24H, x, y)) { cfg.h24 = true;  return SA_CHANGED; }
  if (hit(B_PING_OFF, x, y)) { cfg.ping = false; return SA_CHANGED; }
  if (hit(B_PING_ON, x, y))  { cfg.ping = true;  return SA_CHANGED; }
  return SA_NONE;
}

// ---------------------------------------------------------------- night dim --
static const Btn N_OFF        = { 252, 88, 72, 40 },  N_ON        = { 328, 88, 72, 40 };
static const Btn N_FROM_MINUS = { 214, 138, 42, 40 }, N_FROM_PLUS = { 358, 138, 42, 40 };
static const Btn N_TO_MINUS   = { 214, 188, 42, 40 }, N_TO_PLUS   = { 358, 188, 42, 40 };
static const Btn N_LV_MINUS   = { 214, 238, 42, 40 }, N_LV_PLUS   = { 358, 238, 42, 40 };
static const Btn N_GREEN      = { 252, 288, 72, 40 }, N_RED       = { 328, 288, 72, 40 };
static const Btn N_BACK       = { 180, 382, 120, 40 };
static const uint8_t N_LEVELS[] = { 1, 2, 5, 10, 15, 20, 30 };   // night brightness steps, percent
static const int N_LEVEL_N = sizeof(N_LEVELS);

static void stepLevel(int dir) {
  int i = 0;
  while (i < N_LEVEL_N - 1 && N_LEVELS[i] < cfg.nightLevel) i++;
  i = constrain(i + dir, 0, N_LEVEL_N - 1);
  cfg.nightLevel = N_LEVELS[i];
}

void NightScreen_Draw() {
  gfx->fillScreen(C_BLACK);
  UI_Text(240, 46, "NIGHT", 3, C_GREEN);
  char buf[16];

  UI_TextL(84, 100, "Night dim", 2, C_GREEN);
  pill(N_OFF, "Off", !cfg.nightOn);
  pill(N_ON, "On", cfg.nightOn);

  const uint16_t col = cfg.nightOn ? C_GREEN : C_GREY;
  UI_TextL(84, 150, "From", 2, col);
  pill(N_FROM_MINUS, "-", false);
  snprintf(buf, sizeof(buf), "%02d:00", cfg.nightFrom);
  UI_Text(307, 150, buf, 2, col);
  pill(N_FROM_PLUS, "+", false);

  UI_TextL(84, 200, "Until", 2, col);
  pill(N_TO_MINUS, "-", false);
  snprintf(buf, sizeof(buf), "%02d:00", cfg.nightTo);
  UI_Text(307, 200, buf, 2, col);
  pill(N_TO_PLUS, "+", false);

  UI_TextL(84, 250, "Dim to", 2, col);
  pill(N_LV_MINUS, "-", false);
  snprintf(buf, sizeof(buf), "%d%%", cfg.nightLevel);
  UI_Text(307, 250, buf, 2, col);
  pill(N_LV_PLUS, "+", false);

  UI_TextL(84, 300, "Colour", 2, col);
  pill(N_GREEN, "Green", !cfg.nightRed);
  pill(N_RED, "Red", cfg.nightRed);

  UI_Text(240, 346, "Also rests aircraft data", 2, C_MID);
  pill(N_BACK, "Back", true);
}

SettingsAction NightScreen_Tap(int x, int y) {
  if (hit(N_BACK, x, y)) return SA_CLOSE_NIGHT;
  if (hit(N_OFF, x, y)) { cfg.nightOn = false; return SA_CHANGED; }
  if (hit(N_ON, x, y))  { cfg.nightOn = true;  return SA_CHANGED; }
  if (hit(N_FROM_MINUS, x, y)) { cfg.nightFrom = (cfg.nightFrom + 23) % 24; return SA_CHANGED; }
  if (hit(N_FROM_PLUS, x, y))  { cfg.nightFrom = (cfg.nightFrom + 1) % 24;  return SA_CHANGED; }
  if (hit(N_TO_MINUS, x, y))   { cfg.nightTo = (cfg.nightTo + 23) % 24;     return SA_CHANGED; }
  if (hit(N_TO_PLUS, x, y))    { cfg.nightTo = (cfg.nightTo + 1) % 24;      return SA_CHANGED; }
  if (hit(N_LV_MINUS, x, y))   { stepLevel(-1); return SA_CHANGED; }
  if (hit(N_LV_PLUS, x, y))    { stepLevel(1);  return SA_CHANGED; }
  if (hit(N_GREEN, x, y)) { cfg.nightRed = false; return SA_CHANGED; }
  if (hit(N_RED, x, y))   { cfg.nightRed = true;  return SA_CHANGED; }
  return SA_NONE;
}

// ------------------------------------------------------------------- keypad --
// One QWERTY keypad, used for the postcode and for a Wi-Fi password. Ten keys
// across is the most the round screen takes: each key is 39 px wide (about
// 4.3 mm) and 52 px tall, and the rows sit across the middle, where the circle
// is widest. Everything is at least 15 px inside the rim.
//   typed text and a status line above, keypad in the middle, three buttons below.
// The postcode needs capitals and digits only. A password needs every printable
// character, so it adds two keys to the bottom row: \x01 switches capitals and
// \x02 switches to symbols. A run of spaces in a row is drawn as one space bar.
static const char* const K_POSTCODE[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
static const char* const K_LOWER[4]    = { "1234567890", "qwertyuiop", "asdfghjkl", "\x01zxcvbnm\x02" };
static const char* const K_UPPER[4]    = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "\x01ZXCVBNM\x02" };
static const char* const K_SYMBOL[4]   = { "!\"#$%&'()*", "+,-./:;<=>", "?@[\\]^_`{", "\x01|}~    \x02" };
static const int P_Y0 = 136, P_KW = 39, P_KH = 52;
static const Btn P_BACK = { 105, 360, 84, 40 }, P_DEL = { 198, 360, 84, 40 }, P_OK = { 291, 360, 84, 40 };
static bool     s_kbPass = false;             // false = postcode, true = Wi-Fi password
static bool     s_kbCaps = false, s_kbSym = false;
static char     s_kb[65] = "";                // what has been typed
static char     s_kbIdle[24] = "";            // status line while typing
static char     s_kbStatus[24] = "";
static uint16_t s_kbStatusCol = C_MID;

static const char* const* kbRows() {
  if (!s_kbPass) return K_POSTCODE;
  return s_kbSym ? K_SYMBOL : (s_kbCaps ? K_UPPER : K_LOWER);
}
static size_t kbMax() { return s_kbPass ? 63 : 7; }
static size_t kbMin() { return s_kbPass ? 8 : 5; }     // shortest worth trying
static int rowX0(const char* row) { return 240 - (int)strlen(row) * P_KW / 2; }   // each row centred

void Keypad_SetStatus(const char* s, uint16_t col) {
  strncpy(s_kbStatus, s, sizeof(s_kbStatus) - 1);
  s_kbStatus[sizeof(s_kbStatus) - 1] = '\0';
  s_kbStatusCol = col;
}
static void kbIdle() { Keypad_SetStatus(s_kbIdle, C_MID); }

void Keypad_OpenPostcode() {
  s_kbPass = false;
  int n = 0;
  for (const char* p = cfg.postcode; *p && n < 7; p++) if (isalnum((unsigned char)*p)) s_kb[n++] = *p;
  s_kb[n] = '\0';
  strcpy(s_kbIdle, "Postcode");
  kbIdle();
}
void Keypad_OpenPassword(const char* ssid) {
  s_kbPass = true; s_kbCaps = false; s_kbSym = false;
  s_kb[0] = '\0';
  snprintf(s_kbIdle, sizeof(s_kbIdle), "%.22s", ssid);
  kbIdle();
}
const char* Keypad_Text() { return s_kb; }

void Keypad_Draw() {
  gfx->fillScreen(C_BLACK);
  // A long password shows its last 14 characters, so the newest is always in view.
  const size_t n = strlen(s_kb);
  char shown[20];
  if (n > 16) snprintf(shown, sizeof(shown), "..%s", s_kb + n - 14);
  else        snprintf(shown, sizeof(shown), "%s", n ? s_kb : (s_kbPass ? "password" : "-"));
  UI_Text(240, 62, shown, 3, n ? C_GREEN : C_DIM);
  UI_Text(240, 98, s_kbStatus, 2, s_kbStatusCol);
  const char* const* rows = kbRows();
  char k[2] = "";
  for (int r = 0; r < 4; r++) {
    const int x0 = rowX0(rows[r]), y = P_Y0 + r * P_KH;
    for (int i = 0; rows[r][i]; i++) {
      const char c = rows[r][i];
      int x = x0 + i * P_KW, w = P_KW;
      const char* label = k;
      bool lit = false;
      if (c == '\x01')      { label = "Aa"; lit = s_kbCaps && !s_kbSym; }
      else if (c == '\x02') { label = s_kbSym ? "ab" : "#?"; }
      else if (c == ' ')    { while (rows[r][i + 1] == ' ') { i++; w += P_KW; } label = "space"; }
      else k[0] = c;
      if (lit) gfx->fillRoundRect(x + 2, y + 2, w - 4, P_KH - 4, 7, C_GREEN);
      else     gfx->drawRoundRect(x + 2, y + 2, w - 4, P_KH - 4, 7, C_MID);
      UI_Text(x + w / 2, y + P_KH / 2 - 8, label, 2, lit ? C_BLACK : (label == k ? C_GREEN : C_AMBER));
    }
  }
  pill(P_BACK, "Back", false);
  pill(P_DEL, "Del", false);
  pill(P_OK, s_kbPass ? "Join" : "OK", n >= kbMin());   // lit once it is long enough to try
}

SettingsAction Keypad_Tap(int x, int y) {
  const size_t n = strlen(s_kb);
  // The keypad first: keys in a row touch, so a tap lands on exactly one. A tap just
  // past the end of a shorter row (within half a key) counts as its end key.
  if (y >= P_Y0 && y < P_Y0 + 4 * P_KH) {
    const char* row = kbRows()[(y - P_Y0) / P_KH];
    const int len = (int)strlen(row), x0 = rowX0(row);
    if (x < x0 - P_KW / 2 || x >= x0 + len * P_KW + P_KW / 2) return SA_NONE;
    int i = (x - x0) / P_KW;
    if (x < x0) i = 0;
    if (i >= len) i = len - 1;
    const char c = row[i];
    if (c == '\x01')      { s_kbCaps = !s_kbCaps; s_kbSym = false; }
    else if (c == '\x02') { s_kbSym = !s_kbSym; }
    else if (n < kbMax()) {
      s_kb[n] = c;
      s_kb[n + 1] = '\0';
      kbIdle();
    }
    return SA_NONE;
  }
  if (hit(P_BACK, x, y)) return SA_KEYS_BACK;
  if (hit(P_DEL, x, y)) { if (n) s_kb[n - 1] = '\0'; kbIdle(); return SA_NONE; }
  if (hit(P_OK, x, y) && n >= kbMin()) return SA_KEYS_OK;
  return SA_NONE;
}

// -------------------------------------------------------------- Wi-Fi list --
// Nearby networks, strongest first, five to a page. Tapping one leads to the
// password keypad (or straight to joining, if it has no password).
struct Found { char ssid[33]; int8_t rssi; bool open; };
static Found s_nets[20];
static int   s_netN = 0, s_netPage = 0, s_netPick = -1;
static bool  s_netCanBack = false;
static const int W_ROWS = 5, W_Y0 = 92, W_PITCH = 46;
static const Btn W_ROW  = { 70, W_Y0, 340, 40 };
static const Btn W_PREV = { 110, 328, 56, 40 }, W_NEXT = { 314, 328, 56, 40 };
static const Btn W_BACK = { 88, 380, 96, 40 }, W_SCAN = { 192, 380, 96, 40 }, W_PHONE = { 296, 380, 96, 40 };

void WifiScreen_Scan(bool canBack) {
  s_netCanBack = canBack;
  s_netN = 0; s_netPage = 0; s_netPick = -1;
  const int n = WiFi.scanNetworks();
  for (int i = 0; i < n && s_netN < (int)(sizeof(s_nets) / sizeof(s_nets[0])); i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0 || ssid.length() > 32) continue;   // hidden networks: use the phone
    bool dup = false;
    for (int k = 0; k < s_netN; k++) if (ssid == s_nets[k].ssid) { dup = true; break; }
    if (dup) continue;                                        // several access points, one name
    Found& f = s_nets[s_netN++];
    strcpy(f.ssid, ssid.c_str());
    f.rssi = (int8_t)constrain(WiFi.RSSI(i), -127, 0);
    f.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  Serial.printf("WIFI: %d networks found\n", s_netN);
}
const char* WifiScreen_Ssid() { return s_netPick >= 0 ? s_nets[s_netPick].ssid : ""; }
bool WifiScreen_Open()        { return s_netPick >= 0 && s_nets[s_netPick].open; }

void WifiScreen_Draw() {
  gfx->fillScreen(C_BLACK);
  UI_Text(240, 46, "WI-FI", 3, C_GREEN);
  const int pages = max(1, (s_netN + W_ROWS - 1) / W_ROWS);
  if (s_netN == 0) {
    UI_Text(240, 180, "No networks found", 2, C_AMBER);
    UI_Text(240, 210, "Move closer to the router", 2, C_MID);
  }
  char buf[24];
  for (int r = 0; r < W_ROWS; r++) {
    const int i = s_netPage * W_ROWS + r;
    if (i >= s_netN) break;
    const Found& f = s_nets[i];
    const int y = W_Y0 + r * W_PITCH;
    gfx->drawRoundRect(W_ROW.x, y, W_ROW.w, W_ROW.h, 9, C_MID);
    if (strlen(f.ssid) > 20) snprintf(buf, sizeof(buf), "%.18s..", f.ssid);
    else                     snprintf(buf, sizeof(buf), "%s", f.ssid);
    UI_TextL(W_ROW.x + 14, y + 12, buf, 2, C_GREEN);
    // Signal: four bars, lit by strength.
    const int bars = f.rssi >= -55 ? 4 : f.rssi >= -67 ? 3 : f.rssi >= -78 ? 2 : 1;
    for (int b = 0; b < 4; b++) {
      const int h = 5 + b * 4, bx = W_ROW.x + W_ROW.w - 38 + b * 6;
      gfx->fillRect(bx, y + 29 - h, 4, h, b < bars ? C_GREEN : C_DIM);
    }
    // Padlock on networks that need a password.
    if (!f.open) {
      const int lx = W_ROW.x + W_ROW.w - 62, ly = y + 18;
      gfx->drawRoundRect(lx + 2, ly - 8, 8, 12, 4, C_AMBER);
      gfx->fillRect(lx, ly, 12, 10, C_AMBER);
    }
  }
  if (pages > 1) {
    pill(W_PREV, "<", false);
    snprintf(buf, sizeof(buf), "%d of %d", s_netPage + 1, pages);
    UI_Text(240, 340, buf, 2, C_MID);
    pill(W_NEXT, ">", false);
  }
  if (s_netCanBack) pill(W_BACK, "Back", false);
  pill(W_SCAN, "Rescan", false);
  pill(W_PHONE, "QR code", false);
}

SettingsAction WifiScreen_Tap(int x, int y) {
  const int pages = max(1, (s_netN + W_ROWS - 1) / W_ROWS);
  if (y >= W_Y0 - 3 && y < W_Y0 + W_ROWS * W_PITCH - 3 && x >= W_ROW.x - 8 && x < W_ROW.x + W_ROW.w + 8) {
    const int i = s_netPage * W_ROWS + (y - (W_Y0 - 3)) / W_PITCH;
    if (i >= s_netN) return SA_NONE;
    s_netPick = i;
    return SA_WIFI_PICK;
  }
  if (pages > 1 && hit(W_PREV, x, y)) { s_netPage = (s_netPage + pages - 1) % pages; return SA_NONE; }
  if (pages > 1 && hit(W_NEXT, x, y)) { s_netPage = (s_netPage + 1) % pages; return SA_NONE; }
  if (s_netCanBack && hit(W_BACK, x, y)) return SA_WIFI_BACK;
  if (hit(W_SCAN, x, y)) return SA_WIFI_RESCAN;
  if (hit(W_PHONE, x, y)) return SA_WIFI_PHONE;
  return SA_NONE;
}
