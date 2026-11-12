// Round Radar - battery level, a guess at whether power is coming in, and the
// battery log on the memory card.
//
// The board has no pin that says "USB power is present", so that is worked out:
//   - a computer on the USB socket announces itself (certain);
//   - otherwise the battery's own voltage tells the story: it jumps up when a charger
//     is connected and drops when it is taken away, climbs while charging, and sits
//     at the top when full and still plugged in.
// With no battery fitted the empty socket reads as a full battery on charge.
#include "App.h"
#include <time.h>

static float s_volts = 0;             // smoothed
static bool  s_powered = false;
static float s_hist[16];              // one smoothed reading every 2 s: about 30 s of history
static int   s_histN = 0, s_histAt = 0;
static float s_slow = 0;              // the reading five minutes ago
static unsigned long s_slowAt = 0;

static float readVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BATTERY);
  return mv / 8 * 3 / 1000.0f;
}

float Power_Volts()   { return s_volts; }
bool  Power_Charging() { return s_powered; }

// Percent left, from the voltage of a lithium cell under this device's load. Rough:
// it reads high while charging, because charging lifts the voltage.
int Power_Percent() {
  static const float V[] = { 3.30f, 3.45f, 3.55f, 3.65f, 3.72f, 3.80f, 3.90f, 4.00f, 4.10f };
  static const int   P[] = {     0,     5,    12,    25,    40,    55,    72,    87,   100 };
  if (s_volts <= V[0]) return 0;
  for (int i = 1; i < 9; i++)
    if (s_volts < V[i]) return P[i - 1] + (int)((s_volts - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]));
  return 100;
}

// Called every pass of the main loop; does its work every two seconds, and adds a
// line to the card's log once a minute.
void Power_Tick(int backlight, int screen) {
  static unsigned long last = 0, lastLog = 0;
  static bool first = true;
  const unsigned long now = millis();
  if (!first && now - last < 2000) return;
  last = now;

  const float v = readVolts();
  s_volts = first ? v : s_volts + (v - s_volts) * 0.3f;

  if (first) { s_powered = s_volts >= 4.15f; s_slow = s_volts; s_slowAt = now; }
  if (s_histN == 16) {
    const float step = s_volts - s_hist[s_histAt];          // change over the last half minute
    if (step >= 0.040f) s_powered = true;                   // charger connected
    else if (step <= -0.040f) s_powered = false;            // charger taken away
  }
  s_hist[s_histAt] = s_volts;
  s_histAt = (s_histAt + 1) % 16;
  if (s_histN < 16) s_histN++;
  if (now - s_slowAt >= 300000UL) {                         // slow drift, in case the jump was missed
    if (s_volts - s_slow >= 0.008f) s_powered = true;
    else if (s_volts - s_slow <= -0.008f) s_powered = false;
    s_slow = s_volts; s_slowAt = now;
  }
  if (s_volts >= 4.15f) s_powered = true;                   // full and held there
  if (Serial.isPlugged()) s_powered = true;                 // a computer is on the socket

  if (first || now - lastLog >= 60000UL) {
    char when[20] = "-";
    if (Time_Valid()) { time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt); strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &lt); }
    char line[96];
    snprintf(line, sizeof(line), "%s,%lu,%.3f,%d,%d,%d,%d,%d,%s", when, now / 1000UL, s_volts, Power_Percent(),
             s_powered ? 1 : 0, (int)cfg.rotate, backlight, screen, first ? "start" : "");
    if (Info_LogLine(BATTERY_LOG, "time,uptime_s,volts,percent,power_in,turn_deg,backlight,screen,note", line)) lastLog = now;
    else if (first) lastLog = now - 50000UL;                // card busy or not ready: try again in ten seconds
  }
  first = false;
}
