// Round Radar - phone setup: the device's own Wi-Fi network, a one-page form,
// and the screens shown on the round display while setting up.
#include "App.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
extern "C" {
#include "qrcode.h"
}

static WebServer* s_srv = nullptr;
static DNSServer  s_dns;
static String     s_options;          // <option> list of nearby networks
static bool       s_submitted = false;
static unsigned long s_submittedAt = 0;
static PortalResult s_result;

static String esc(const String& s) {
  String o;
  for (char c : s) {
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}

static const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Round Radar setup</title><style>body{font:17px -apple-system,sans-serif;margin:0;padding:20px;max-width:420px;"
  "margin:auto;color:#111}h1{font-size:22px}label{display:block;margin:16px 0 4px;font-weight:600}"
  "input{width:100%;box-sizing:border-box;font-size:17px;padding:10px;border:1px solid #9aa;border-radius:8px}"
  "button{width:100%;margin-top:22px;padding:13px;font-size:17px;font-weight:600;color:#fff;background:#14753a;border:0;"
  "border-radius:8px}small{color:#566}</style>";

static void sendForm() {
  String p = FPSTR(PAGE_HEAD);
  p += "<h1>Round Radar setup</h1><form method=post action=/save>"
       "<label for=s>Wi-Fi network</label><input id=s name=ssid list=nets required maxlength=32 autocapitalize=none autocorrect=off value=\"";
  p += esc(cfg.ssid);
  p += "\"><datalist id=nets>";
  p += s_options;
  p += "</datalist><label for=p>Password</label><input id=p name=pass type=password maxlength=64>"
       "<label for=c>Postcode</label><input id=c name=pc required maxlength=8 autocapitalize=characters value=\"";
  p += esc(cfg.postcode);
  p += "\"><small>Used once to place the radar. UK postcodes only.</small>"
       "<button>Save and connect</button></form>";
  s_srv->send(200, "text/html", p);
}

static void handleSave() {
  String ssid = s_srv->arg("ssid"), pass = s_srv->arg("pass"), pc = s_srv->arg("pc");
  ssid.trim(); pc.trim();
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64 || pc.length() == 0 || pc.length() > 9) {
    s_srv->sendHeader("Location", "/");
    s_srv->send(303, "text/plain", "");
    return;
  }
  memset(&s_result, 0, sizeof(s_result));
  strncpy(s_result.ssid, ssid.c_str(), sizeof(s_result.ssid) - 1);
  strncpy(s_result.pass, pass.c_str(), sizeof(s_result.pass) - 1);
  strncpy(s_result.postcode, pc.c_str(), sizeof(s_result.postcode) - 1);

  String p = FPSTR(PAGE_HEAD);
  p += "<h1>Saved</h1><p>The display is now joining <b>" + esc(ssid) + "</b>.</p>"
       "<p>Watch the round screen. If it cannot join, it will show this setup network again.</p>"
       "<small>You can close this page.</small>";
  s_srv->send(200, "text/html", p);
  s_submitted = true;
  s_submittedAt = millis();
}

static void redirectHome() {
  s_srv->sendHeader("Location", "http://192.168.4.1/");
  s_srv->send(302, "text/plain", "");
}

static void drawQrScreen(bool canCancel) {
  gfx->fillScreen(C_BLACK);
  UI_Text(240, 46, "SET UP", 3, C_GREEN);
  UI_Text(240, 84, "Scan with your phone", 2, C_MID);

  // Joining details as a Wi-Fi QR code: phones offer to join when they see it.
  QRCode qr;
  static uint8_t data[128];   // version 3 needs 106 bytes
  qrcode_initText(&qr, data, 3, ECC_LOW, "WIFI:T:nopass;S:" AP_SSID ";;");
  const int m = 6, size = qr.size * m, x0 = 240 - size / 2, y0 = 122;
  gfx->fillRoundRect(x0 - 12, y0 - 12, size + 24, size + 24, 6, C_WHITE);
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y)) gfx->fillRect(x0 + x * m, y0 + y * m, m, m, C_BLACK);

  UI_Text(240, 322, "or join Wi-Fi", 2, C_MID);
  UI_Text(240, 346, AP_SSID, 3, C_GREEN);
  UI_Text(240, 382, "then open 192.168.4.1", 2, C_MID);
  if (canCancel) UI_Text(240, 416, "Hold to go back", 2, C_AMBER);
  gfx->flush();
}

void Portal_Begin(bool canCancel) {
  Setup_DrawSplash("Looking for Wi-Fi");
  // A network already joined stays joined beside the setup network, so aircraft keep
  // arriving until the form is saved. A join still in progress would block the search.
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  delay(100);

  // List nearby networks once, for the form's suggestions.
  s_options = "";
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && i < 20; i++) {
    String s = WiFi.SSID(i);
    if (s.length() && s_options.indexOf("\"" + esc(s) + "\"") < 0) s_options += "<option value=\"" + esc(s) + "\">";
  }
  WiFi.scanDelete();

  WiFi.softAP(AP_SSID);
  delay(200);
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(53, "*", WiFi.softAPIP());      // every name leads here, so the form pops up

  s_submitted = false;
  s_srv = new WebServer(80);
  s_srv->on("/", HTTP_GET, sendForm);
  s_srv->on("/save", HTTP_POST, handleSave);
  s_srv->onNotFound(redirectHome);
  s_srv->begin();
  Serial.printf("SETUP: network \"%s\" up at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  drawQrScreen(canCancel);
}

void Portal_Redraw() { if (s_srv) drawQrScreen(true); }

bool Portal_Loop(PortalResult* out) {
  if (!s_srv) return false;
  s_dns.processNextRequest();
  s_srv->handleClient();
  // Give the "Saved" page a moment to reach the phone before the network goes away.
  if (s_submitted && millis() - s_submittedAt > 1500) {
    *out = s_result;
    s_submitted = false;
    return true;
  }
  return false;
}

void Portal_End() {
  if (s_srv) { s_srv->stop(); delete s_srv; s_srv = nullptr; }
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

// ------------------------------------------------------- screens during setup
void Setup_DrawSplash(const char* line) {
  gfx->fillScreen(C_BLACK);
  gfx->drawCircle(240, 190, 70, C_DIM);
  gfx->drawCircle(240, 190, 35, C_DIM);
  gfx->drawLine(240, 190, 295, 146, C_GREEN);
  gfx->drawLine(241, 190, 296, 146, C_GREEN);
  UI_Text(240, 296, "ROUND RADAR", 4, C_GREEN);
  UI_Text(240, 346, line, 2, C_MID);
  UI_Text(240, 410, "v" FW_VERSION, 2, C_DIM);
  gfx->flush();
}

void Setup_DrawConnecting(const char* ssid, int stage, const char* postcode) {
  gfx->fillScreen(C_BLACK);
  char buf[48];
  UI_Text(240, 120, "CONNECTING", 3, C_GREEN);
  snprintf(buf, sizeof(buf), "Wi-Fi %.20s", ssid);
  UI_Text(240, 196, buf, 2, C_GREEN);
  UI_Text(240, 224, stage == 0 ? "joining" : "joined", 2, stage == 0 ? C_AMBER : C_MID);
  if (stage >= 1) {
    snprintf(buf, sizeof(buf), "Place %s", postcode);
    UI_Text(240, 286, buf, 2, C_GREEN);
    UI_Text(240, 314, "looking up", 2, C_AMBER);
  }
  gfx->flush();
}

void Setup_DrawFail(const char* title, const char* what, const char* l1, const char* l2, int secsLeft) {
  gfx->fillScreen(C_BLACK);
  char buf[32];
  UI_Text(240, 110, title, 3, C_RED);
  snprintf(buf, sizeof(buf), "%.24s", what);
  UI_Text(240, 186, buf, 3, C_GREEN);
  UI_Text(240, 240, l1, 2, C_AMBER);
  UI_Text(240, 266, l2, 2, C_AMBER);
  snprintf(buf, sizeof(buf), "Back to setup in %d s", secsLeft);
  UI_Text(240, 340, buf, 2, C_MID);
  gfx->flush();
}
