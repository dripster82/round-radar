// Round Radar - the settings screen and its night-dimming page.
// Layout follows mockups.html. Every button is 40 px tall (about 4.5 mm) and
// taps are accepted a few pixels outside it.
#include "App.h"

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
static const Btn N_OFF        = { 252, 116, 72, 40 }, N_ON        = { 328, 116, 72, 40 };
static const Btn N_FROM_MINUS = { 214, 186, 42, 40 }, N_FROM_PLUS = { 358, 186, 42, 40 };
static const Btn N_TO_MINUS   = { 214, 256, 42, 40 }, N_TO_PLUS   = { 358, 256, 42, 40 };
static const Btn N_BACK       = { 180, 382, 120, 40 };

void NightScreen_Draw() {
  gfx->fillScreen(C_BLACK);
  UI_Text(240, 52, "NIGHT DIM", 3, C_GREEN);
  char buf[16];

  UI_TextL(84, 128, "Night dim", 2, C_GREEN);
  pill(N_OFF, "Off", !cfg.nightOn);
  pill(N_ON, "On", cfg.nightOn);

  const uint16_t col = cfg.nightOn ? C_GREEN : C_GREY;
  UI_TextL(84, 198, "From", 2, col);
  pill(N_FROM_MINUS, "-", false);
  snprintf(buf, sizeof(buf), "%02d:00", cfg.nightFrom);
  UI_Text(307, 198, buf, 2, col);
  pill(N_FROM_PLUS, "+", false);

  UI_TextL(84, 268, "Until", 2, col);
  pill(N_TO_MINUS, "-", false);
  snprintf(buf, sizeof(buf), "%02d:00", cfg.nightTo);
  UI_Text(307, 268, buf, 2, col);
  pill(N_TO_PLUS, "+", false);

  UI_Text(240, 328, "Dims the screen to 10%", 2, C_MID);
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
  return SA_NONE;
}

// ----------------------------------------------------------------- postcode --
// A QWERTY keypad with a row of digits above it. Ten keys across is the most the
// round screen takes: each key is 39 px wide (about 4.3 mm) and 52 px tall, and
// the rows sit across the middle, where the circle is widest. Everything is at
// least 15 px inside the rim.
//   typed postcode and status line above, keypad in the middle, three buttons below.
static const char* const P_ROWS[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
static const int P_Y0 = 136, P_KW = 39, P_KH = 52;
static int rowX0(int r) { return 240 - (int)strlen(P_ROWS[r]) * P_KW / 2; }   // each row centred
static const Btn P_BACK = { 105, 360, 84, 40 }, P_DEL = { 198, 360, 84, 40 }, P_OK = { 291, 360, 84, 40 };
static char     s_pc[8] = "";                 // up to 7 characters, no space
static char     s_pcStatus[24] = "";
static uint16_t s_pcStatusCol = C_MID;

void PlaceScreen_Open() {
  int n = 0;
  for (const char* p = cfg.postcode; *p && n < 7; p++) if (isalnum((unsigned char)*p)) s_pc[n++] = *p;
  s_pc[n] = '\0';
  PlaceScreen_SetStatus("Postcode", C_MID);
}
const char* PlaceScreen_Text() { return s_pc; }
void PlaceScreen_SetStatus(const char* s, uint16_t col) {
  strncpy(s_pcStatus, s, sizeof(s_pcStatus) - 1);
  s_pcStatus[sizeof(s_pcStatus) - 1] = '\0';
  s_pcStatusCol = col;
}

void PlaceScreen_Draw() {
  gfx->fillScreen(C_BLACK);
  UI_Text(240, 62, s_pc[0] ? s_pc : "-", 3, C_GREEN);
  UI_Text(240, 98, s_pcStatus, 2, s_pcStatusCol);
  char k[2] = "";
  for (int r = 0; r < 4; r++) {
    const int x0 = rowX0(r), y = P_Y0 + r * P_KH;
    for (int i = 0; P_ROWS[r][i]; i++) {
      const int x = x0 + i * P_KW;
      gfx->drawRoundRect(x + 2, y + 2, P_KW - 4, P_KH - 4, 7, C_MID);
      k[0] = P_ROWS[r][i];
      UI_Text(x + P_KW / 2, y + P_KH / 2 - 8, k, 2, C_GREEN);
    }
  }
  pill(P_BACK, "Back", false);
  pill(P_DEL, "Del", false);
  pill(P_OK, "OK", strlen(s_pc) >= 5);        // lit once it is long enough to look up
}

SettingsAction PlaceScreen_Tap(int x, int y) {
  const size_t n = strlen(s_pc);
  // The keypad first: keys in a row touch, so a tap lands on exactly one. A tap just
  // past the end of a shorter row (within half a key) counts as its end key.
  if (y >= P_Y0 && y < P_Y0 + 4 * P_KH) {
    const int r = (y - P_Y0) / P_KH, len = (int)strlen(P_ROWS[r]), x0 = rowX0(r);
    if (x < x0 - P_KW / 2 || x >= x0 + len * P_KW + P_KW / 2) return SA_NONE;
    int i = (x - x0) / P_KW;
    if (x < x0) i = 0;
    if (i >= len) i = len - 1;
    if (n < sizeof(s_pc) - 1) {
      s_pc[n] = P_ROWS[r][i];
      s_pc[n + 1] = '\0';
      PlaceScreen_SetStatus("Postcode", C_MID);
    }
    return SA_NONE;
  }
  if (hit(P_BACK, x, y)) return SA_CLOSE_PLACE;
  if (hit(P_DEL, x, y)) { if (n) s_pc[n - 1] = '\0'; PlaceScreen_SetStatus("Postcode", C_MID); return SA_NONE; }
  if (hit(P_OK, x, y) && n >= 5) return SA_PLACE_SUBMIT;
  return SA_NONE;
}
