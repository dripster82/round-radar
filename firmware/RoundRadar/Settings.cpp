// Round Radar - settings kept in the board's built-in settings memory (NVS).
#include "App.h"
#include <Preferences.h>

Settings cfg;
static Preferences prefs;

void Settings_Load() {
  memset(&cfg, 0, sizeof(cfg));
  prefs.begin("rr", true);
  prefs.getString("ssid", cfg.ssid, sizeof(cfg.ssid));
  prefs.getString("pass", cfg.pass, sizeof(cfg.pass));
  prefs.getString("pc", cfg.postcode, sizeof(cfg.postcode));
  cfg.lat       = prefs.getFloat("lat", 0);
  cfg.lon       = prefs.getFloat("lon", 0);
  cfg.hasPos    = prefs.getBool("haspos", false);
  cfg.screen    = prefs.getUChar("screen", 0);
  cfg.rangeIdx  = prefs.getUChar("range", 1);
  cfg.bright    = prefs.getUChar("bright", 70);
  cfg.h24       = prefs.getBool("h24", true);
  cfg.nightOn   = prefs.getBool("night", true);
  cfg.nightFrom = prefs.getUChar("nfrom", 22);
  cfg.nightTo   = prefs.getUChar("nto", 7);
  cfg.nightLevel = prefs.getUChar("nlevel", 10);
  cfg.nightRed  = prefs.getBool("nred", false);
  cfg.ping      = prefs.getBool("ping", false);
  prefs.end();

  // Guard against values written by an older or damaged build.
  if (cfg.screen > 1) cfg.screen = 0;
  if (cfg.rangeIdx >= RANGE_COUNT) cfg.rangeIdx = 1;
  if (cfg.bright < 10 || cfg.bright > 100) cfg.bright = 70;
  if (cfg.nightFrom > 23) cfg.nightFrom = 22;
  if (cfg.nightTo > 23) cfg.nightTo = 7;
  if (cfg.nightLevel < 1 || cfg.nightLevel > 50) cfg.nightLevel = 10;
}

void Settings_Erase() {
  prefs.begin("rr", false);
  prefs.clear();
  prefs.end();
}

void Settings_Save() {
  prefs.begin("rr", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("pc", cfg.postcode);
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putBool("haspos", cfg.hasPos);
  prefs.putUChar("screen", cfg.screen);
  prefs.putUChar("range", cfg.rangeIdx);
  prefs.putUChar("bright", cfg.bright);
  prefs.putBool("h24", cfg.h24);
  prefs.putBool("night", cfg.nightOn);
  prefs.putUChar("nfrom", cfg.nightFrom);
  prefs.putUChar("nto", cfg.nightTo);
  prefs.putUChar("nlevel", cfg.nightLevel);
  prefs.putBool("nred", cfg.nightRed);
  prefs.putBool("ping", cfg.ping);
  prefs.end();
}
