// Round Radar - shared types and the interface between the modules.
#pragma once
#include <Arduino.h>
#include "Config.h"
#include "Canvas16.h"

// RGB565 colours (the same palette as mockups.html).
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_GREEN 0x3FEF   // #3dff7a
#define C_MID   0x252A   // #22a552
#define C_DIM   0x1305   // #14602f
#define C_AMBER 0xFE49   // #ffc94d
#define C_RED   0xFAC9   // #ff5a4d
#define C_GREY  0x6B8D   // #6b736d

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
  bool    h24;
  bool    nightOn;
  uint8_t nightFrom;    // hour 0..23
  uint8_t nightTo;      // hour 0..23
  bool    ping;
};
extern Settings cfg;
void Settings_Load();
void Settings_Save();

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
  uint8_t trailN;
  float trailLat[TRAIL_N], trailLon[TRAIL_N];   // newest first
};

// ----------------------------------------------------------------- network --
void     Net_Begin();                 // starts the background task (core 0)
void     Net_SetPaused(bool paused);  // true during phone setup
void     Net_SetScopeActive(bool on); // fast polling only while the scope shows
void     Net_Kick();                  // fetch now (range changed)
uint32_t Net_Version();               // changes whenever a new list arrives
int      Net_Snapshot(Aircraft* out); // copies the list, returns the count
bool     Net_EverOk();
uint32_t Net_DataAgeMs();
bool     Net_TakeTimeSynced();        // true once after each internet time sync
bool     Net_LookupPostcode(const char* pc, float* lat, float* lon);

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
void Scope_Invalidate();              // range or position changed
void Scope_Draw(const Aircraft* list, int n, bool wifi);
bool Scope_HitTest(int x, int y, char* hexOut);
void Detail_Draw(const Aircraft& a, bool lost);

// ------------------------------------------------------------------- clock --
void Clock_Draw(int aircraft, bool wifi, bool dataFresh);

// ---------------------------------------------------------------- settings UI
enum SettingsAction { SA_NONE, SA_DONE, SA_WIFI, SA_OPEN_NIGHT, SA_CLOSE_NIGHT, SA_CHANGED,
                      SA_OPEN_PLACE, SA_CLOSE_PLACE, SA_PLACE_SUBMIT };
void SettingsScreen_Draw();
SettingsAction SettingsScreen_Tap(int x, int y);
void NightScreen_Draw();
SettingsAction NightScreen_Tap(int x, int y);
// Postcode keypad: changes the place without redoing the Wi-Fi setup.
void PlaceScreen_Open();                      // starts from the saved postcode
void PlaceScreen_Draw();
SettingsAction PlaceScreen_Tap(int x, int y);
const char* PlaceScreen_Text();               // what has been typed, no spaces
void PlaceScreen_SetStatus(const char* s, uint16_t col);

// ------------------------------------------------------------- phone setup --
struct PortalResult { char ssid[33]; char pass[65]; char postcode[10]; };
void Portal_Begin(bool canCancel);    // starts the setup network, draws the QR screen
bool Portal_Loop(PortalResult* out);  // true once the form has been submitted
void Portal_End();
void Setup_DrawSplash(const char* line);
void Setup_DrawConnecting(const char* ssid, int stage, const char* postcode);
void Setup_DrawFail(const char* title, const char* what, const char* l1, const char* l2, int secsLeft);

// -------------------------------------------------------------------- main --
void App_Setup();
void App_Loop();
