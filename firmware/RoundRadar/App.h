// Round Radar - shared types and the interface between the modules.
#pragma once
#include <Arduino.h>
#include "Config.h"
#include "Canvas16.h"

// RGB565 colours (the same palette as mockups.html).
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED   0xFAC9   // #ff5a4d
// These five follow the theme: scope green by day, and red in the night hours when the
// night colour is set to red (red light is kinder to eyes in the dark).
extern uint16_t C_GREEN, C_MID, C_DIM, C_AMBER, C_GREY;
void Theme_Set(bool red);
bool Theme_Red();

extern Canvas16* gfx;
extern const float RANGES_KM[RANGE_COUNT];

// ---------------------------------------------------------------- settings --
struct Settings {
  char    ssid[33];
  char    pass[65];
  char    postcode[10];
  float   lat, lon;
  bool    hasPos;
  uint8_t screen;       // 0 = scope, 1 = clock
  uint8_t rangeIdx;
  uint8_t bright;       // 10..100
  int16_t rotate;       // the whole picture turned by this many degrees (see SCREEN_TURN_DEG)
  bool    h24;
  bool    nightOn;
  uint8_t nightFrom;    // hour 0..23
  uint8_t nightTo;      // hour 0..23
  uint8_t nightLevel;   // brightness in the night hours, percent
  bool    nightRed;     // red instead of green in the night hours
  bool    ping;
};
extern Settings cfg;
void Settings_Load();
void Settings_Save();
void Settings_Erase();                 // factory reset: forget everything saved

// ---------------------------------------------------------------- aircraft --
struct Aircraft {
  float lat, lon;
  float track;          // degrees
  float altFt;
  float gsKt;
  float baroRate;       // ft/min
  bool  hasTrack;
  char  callsign[10];
  char  hex[8];
  char  type[10];
  char  reg[12];
  char  desc[32];       // "BOEING 737 MAX 8"
  char  op[26];         // owner or operator, where the feed knows it
  char  squawk[6];
  uint8_t trailN;
  float trailLat[TRAIL_N], trailLon[TRAIL_N];   // newest first
};

// ----------------------------------------------------------------- network --
void     Net_Begin();                 // starts the background task (core 0)
void     Net_SetPaused(bool paused);  // true during phone setup
void     Net_SetScopeActive(bool on); // fast polling only while the scope shows
void     Net_SetResting(bool on);     // night rest: no downloads until touched
bool     Net_Resting();
void     Net_Kick();                  // fetch now (range changed)
uint32_t Net_Version();               // changes whenever a new list arrives
int      Net_Snapshot(Aircraft* out); // copies the list, returns the count
bool     Net_EverOk();
uint32_t Net_DataAgeMs();
bool     Net_TakeTimeSynced();        // true once after each internet time sync
void     Net_Status(char* out, size_t cap);   // how the fetches are going, for the log
bool     Net_LookupPostcode(const char* pc, float* lat, float* lon);

// ------------------------------------------------- route and photo lookups --
#define PHOTO_W 300
#define PHOTO_H 200
struct AcInfo {
  char hex[8];
  char callsign[10];                  // the flight the route belongs to
  char type[10];                      // type code, e.g. B38M
  bool similar;                       // the photo is of another aircraft of this type, standing in
  bool loading;                       // still looking things up
  char from[5], to[5];                // airport codes; empty when the route is not known
  char fromCity[20], toCity[20];
  char airline[26];
  const uint16_t* photo;              // null when there is none (yet)
  int16_t pw, ph;                     // its size, at most PHOTO_W x PHOTO_H
  char credit[24];                    // the photographer
  char site[20];                      // the photo site it came from
  char link[200];                     // the photo's own page there, shown as a QR code
};
void Info_Begin();
void Info_Request(const char* hex, const char* callsign, const char* type);   // looked up in the background
void Info_Get(AcInfo* out);
bool Info_Pending();
void Info_Service();                  // network task only
bool Info_SaveWaiting();              // something looked up is waiting to be written to storage
void Info_FlushSaves();               // write it now; the screen is disturbed while it runs
void Info_ListCard();                 // prints what is on the memory card
void Info_WipeCard();                 // removes everything on the memory card
void Info_EraseStore();               // factory reset: forget the stored photos and routes
bool Info_LogLine(const char* name, const char* header, const char* line);   // adds a line to a file on the card; false if not written
void Info_PrintFile(const char* name);   // prints a file from the card over USB
void Info_DeleteFile(const char* name);

void  Power_Tick(int backlight, int screen);   // every pass of the main loop
float Power_Volts();
int   Power_Percent();
bool  Power_Charging();               // power is coming in (worked out, see Power.cpp)

// -------------------------------------------------------------------- time --
void Time_Begin();                    // time zone + read the clock chip
void Time_SaveToChip();               // after an internet sync
bool Time_Valid();

// ---------------------------------------------------------------- UI helpers
void UI_Text(int cx, int y, const char* s, int size, uint16_t col);       // centred
void UI_TextL(int x, int y, const char* s, int size, uint16_t col);
void UI_TextR(int xRight, int y, const char* s, int size, uint16_t col);
void UI_Pill(int x, int y, int w, int h, const char* s, bool on);
const char* UI_Compass(float deg);
void Geo_DistBearing(float lat1, float lon1, float lat2, float lon2, float* km, float* brg);

// ------------------------------------------------------------------- scope --
void Scope_Begin();
void Scope_ThemeChanged();            // rebuild the palette
void Scope_Invalidate();              // range or position changed
void Scope_Draw(const Aircraft* list, int n, bool wifi);
bool Scope_HitTest(int x, int y, char* hexOut);
#define DETAIL_PAGES 3
void Detail_Draw(const Aircraft& a, bool lost, int page);   // 0 = figures, 1 = photo and route, 2 = QR code to the photo's page

// ------------------------------------------------------------------- clock --
void Clock_Draw(int aircraft, bool wifi, bool dataFresh);

// ---------------------------------------------------------------- settings UI
enum SettingsAction { SA_NONE, SA_DONE, SA_WIFI, SA_OPEN_NIGHT, SA_CLOSE_NIGHT, SA_CHANGED,
                      SA_OPEN_PLACE, SA_KEYS_BACK, SA_KEYS_OK,
                      SA_WIFI_PICK, SA_WIFI_BACK, SA_WIFI_RESCAN, SA_WIFI_PHONE };
void SettingsScreen_Draw();
SettingsAction SettingsScreen_Tap(int x, int y);
void NightScreen_Draw();
SettingsAction NightScreen_Tap(int x, int y);
// QWERTY keypad, shared by the postcode and the Wi-Fi password.
void Keypad_OpenPostcode();                   // starts from the saved postcode
void Keypad_OpenPassword(const char* ssid);   // adds capitals and symbols
void Keypad_Draw();
SettingsAction Keypad_Tap(int x, int y);
const char* Keypad_Text();                    // what has been typed
void Keypad_SetStatus(const char* s, uint16_t col);
// List of nearby Wi-Fi networks, for setting up on the device itself.
void WifiScreen_Scan(bool canBack);           // blocks for a few seconds
void WifiScreen_Draw();
SettingsAction WifiScreen_Tap(int x, int y);
const char* WifiScreen_Ssid();                // the network tapped
bool WifiScreen_Open();                       // true if it needs no password

// ------------------------------------------------------------- phone setup --
struct PortalResult { char ssid[33]; char pass[65]; char postcode[10]; };
void Portal_Begin(bool canCancel);    // starts the setup network, draws the QR screen
bool Portal_Loop(PortalResult* out);  // true once the form has been submitted
void Portal_End();
void Portal_Redraw();                 // draw the QR screen again
void Setup_DrawSplash(const char* line);
void Setup_DrawConnecting(const char* ssid, int stage, const char* postcode);
void Setup_DrawFail(const char* title, const char* what, const char* l1, const char* l2, int secsLeft);

// -------------------------------------------------------------------- main --
void App_Setup();
void App_Loop();
