// Round Radar - background network task: aircraft feed, internet time, postcode.
//
// The task runs on core 0 so the scope on core 1 never waits for a download.
// It owns the live aircraft list; the drawing side takes a copy under a mutex.
#include "App.h"
#include "NetSink.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include "esp_sntp.h"
#include "esp_heap_caps.h"

// The certificate bundle built into the ESP32 core, so HTTPS servers are verified.
extern const uint8_t crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

static SemaphoreHandle_t s_lock;
static Aircraft  s_live[AC_MAX];
static Aircraft  s_tmp[AC_MAX];
static int       s_liveN = 0;
static volatile uint32_t s_version = 0;
static volatile uint32_t s_lastOkMs = 0;
static volatile bool s_everOk = false;
static volatile bool s_paused = true;
static volatile bool s_scopeActive = true;
static volatile bool s_kick = false;
static volatile bool s_resting = false;
static volatile bool s_timeSynced = false;
static long s_retryAfterS = 0;

// A record of how the fetches are going, for the log: when the data on screen goes
// old, the reason is still here to read afterwards.
static volatile uint32_t s_okN = 0, s_failN = 0, s_lastFailMs = 0, s_nextFetchMs = 0;
static volatile uint32_t s_connN = 0, s_bytes = 0;   // connections opened, body bytes downloaded
static char s_lastErr[40] = "none";
static bool s_feedRefused = false;     // the feed itself answered with an error

static bool fail(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_lastErr, sizeof(s_lastErr), fmt, ap);
  va_end(ap);
  s_failN++;
  s_lastFailMs = millis();
  Serial.printf("ADSB: %s\n", s_lastErr);
  return false;
}

void Net_Status(char* out, size_t cap) {
  const uint32_t now = millis();
  snprintf(out, cap, "fetches %u ok, %u failed, %u connections, %u KB, last failure \"%s\" %lu s ago, data %lu s old, next fetch in %ld s%s%s",
           (unsigned)s_okN, (unsigned)s_failN, (unsigned)s_connN, (unsigned)(s_bytes / 1024), s_lastErr,
           s_failN ? (unsigned long)((now - s_lastFailMs) / 1000) : 0UL,
           s_everOk ? (unsigned long)((now - s_lastOkMs) / 1000) : 0UL,
           (long)(s_nextFetchMs - now) / 1000, s_paused ? ", paused" : "", s_resting ? ", resting" : "");
}

static const size_t BODY_CAP = 512 * 1024;
static char* s_body = nullptr;

void     Net_SetPaused(bool p)      { s_paused = p; }
void     Net_SetScopeActive(bool on){ s_scopeActive = on; }
void     Net_Kick()                 { s_kick = true; }
void     Net_SetResting(bool on)    { if (s_resting && !on) s_kick = true; s_resting = on; }   // waking: fetch now
bool     Net_Resting()              { return s_resting; }
uint32_t Net_Version()              { return s_version; }
bool     Net_EverOk()               { return s_everOk; }
uint32_t Net_DataAgeMs()            { return s_everOk ? millis() - s_lastOkMs : 0xFFFFFFFFUL; }
bool     Net_TakeTimeSynced()       { if (!s_timeSynced) return false; s_timeSynced = false; return true; }

int Net_Snapshot(Aircraft* out) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  int n = s_liveN;
  memcpy(out, s_live, sizeof(Aircraft) * n);
  xSemaphoreGive(s_lock);
  return n;
}

static void onTimeSync(struct timeval*) { s_timeSynced = true; }

static void secureClient(NetworkClientSecure& c) {
  c.setCACertBundle(crt_bundle_start, crt_bundle_end - crt_bundle_start);
}

static bool readFloat(JsonObjectConst o, const char* key, float* out) {
  JsonVariantConst v = o[key];
  if (v.is<float>() || v.is<int>()) { *out = v.as<float>(); return true; }
  return false;
}

static void copyTrim(char* dst, size_t cap, const char* src) {
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
  for (int i = (int)strlen(dst) - 1; i >= 0 && dst[i] == ' '; i--) dst[i] = '\0';
}

// Carry the trail over from the previous list: the aircraft is matched by its
// ICAO address, because the feed does not keep a stable order.
static void carryTrail(Aircraft& a) {
  a.trailN = 0;
  for (int i = 0; i < s_liveN; i++) {
    const Aircraft& p = s_live[i];
    if (strcmp(p.hex, a.hex) != 0) continue;
    const bool moved = fabsf(p.lat - a.lat) > 0.0003f || fabsf(p.lon - a.lon) > 0.0003f;
    if (!moved) {                       // same position: keep the trail as it was
      a.trailN = p.trailN;
      memcpy(a.trailLat, p.trailLat, sizeof(a.trailLat));
      memcpy(a.trailLon, p.trailLon, sizeof(a.trailLon));
      return;
    }
    a.trailLat[0] = p.lat; a.trailLon[0] = p.lon;
    int n = 1;
    for (int k = 0; k < p.trailN && n < TRAIL_N; k++, n++) {
      a.trailLat[n] = p.trailLat[k]; a.trailLon[n] = p.trailLon[k];
    }
    a.trailN = n;
    return;
  }
}

// One secure connection to the feed, kept open between fetches. Opening one costs
// about 6 KB of handshake, more than a short-range download itself, which matters on
// a phone hotspot.
static NetworkClientSecure s_feed;
static HTTPClient s_http;

static int feedGet(const char* url) {
  if (!s_feed.connected()) { secureClient(s_feed); s_connN++; }
  s_http.setConnectTimeout(8000);
  s_http.setTimeout(12000);
  s_http.setReuse(true);
  if (!s_http.begin(s_feed, url)) return 0;
  s_http.setUserAgent(HTTP_USER_AGENT);
  s_http.addHeader("Accept", "application/json");
  static const char* WANTED[] = { "Retry-After" };
  s_http.collectHeaders(WANTED, 1);
  return s_http.GET();
}
static void feedDrop() { s_http.end(); s_feed.stop(); }

static bool fetchAircraft() {
  s_retryAfterS = 0;
  s_feedRefused = false;
  if (!Net_HeapOk("ADSB")) return fail("low memory");

  char url[160];
  snprintf(url, sizeof(url), "%s%.5f/lon/%.5f/dist/%.1f", ADSB_URL_BASE,
           cfg.lat, cfg.lon, RANGES_KM[cfg.rangeIdx] / 1.852f);   // the feed takes nautical miles

  int code = feedGet(url);
  if (code < 0) {                                  // the feed closed the idle connection: once more, fresh
    feedDrop();
    code = feedGet(url);
  }
  if (code != 200) {
    if (s_http.hasHeader("Retry-After")) s_retryAfterS = s_http.header("Retry-After").toInt();
    s_feedRefused = code > 0;                      // below zero: never reached the feed
    feedDrop();
    return fail("HTTP %d, signal %d", code, (int)WiFi.RSSI());
  }
  long len = Net_ReadBody(s_http, (uint8_t*)s_body, BODY_CAP, "ADSB");
  if (len < 8) { feedDrop(); return fail("body %ld", len); }
  s_http.end();                                    // keeps the connection for the next fetch
  s_bytes += (uint32_t)len;

  JsonDocument filter;
  JsonObject f = filter["ac"][0].to<JsonObject>();
  static const char* KEYS[] = { "hex", "flight", "r", "t", "desc", "ownOp", "squawk", "alt_baro", "gs",
                                "track", "true_heading", "baro_rate", "lat", "lon" };
  for (const char* k : KEYS) f[k] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, s_body, (size_t)len, DeserializationOption::Filter(filter));
  if (err) return fail("JSON %s", err.c_str());
  JsonArrayConst ac = doc["ac"].as<JsonArrayConst>();
  if (ac.isNull()) { s_feedRefused = true; return fail("no aircraft array"); }

  int n = 0;
  for (JsonObjectConst p : ac) {
    if (n >= AC_MAX) break;
    Aircraft& a = s_tmp[n];
    if (!readFloat(p, "lat", &a.lat) || !readFloat(p, "lon", &a.lon)) continue;
    JsonVariantConst ab = p["alt_baro"];
    if (ab.is<const char*>()) continue;            // "ground"
    float v = 0;
    a.altFt    = readFloat(p, "alt_baro", &v) ? v : 0;
    a.gsKt     = readFloat(p, "gs", &v) ? v : 0;
    a.baroRate = readFloat(p, "baro_rate", &v) ? v : 0;
    a.hasTrack = readFloat(p, "track", &a.track) || readFloat(p, "true_heading", &a.track);
    if (!a.hasTrack) a.track = 0;
    copyTrim(a.callsign, sizeof(a.callsign), p["flight"] | "");
    copyTrim(a.hex, sizeof(a.hex), p["hex"] | "");
    copyTrim(a.type, sizeof(a.type), p["t"] | "");
    copyTrim(a.reg, sizeof(a.reg), p["r"] | "");
    copyTrim(a.desc, sizeof(a.desc), p["desc"] | "");
    copyTrim(a.op, sizeof(a.op), p["ownOp"] | "");
    copyTrim(a.squawk, sizeof(a.squawk), p["squawk"] | "");
    if (!a.hex[0]) continue;
    carryTrail(a);
    n++;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  memcpy(s_live, s_tmp, sizeof(Aircraft) * n);
  s_liveN = n;
  xSemaphoreGive(s_lock);
  s_lastOkMs = millis();
  s_everOk = true;
  s_okN++;
  s_version++;
  return true;
}

static unsigned long basePeriod() {
  if (!s_scopeActive) return FETCH_BACKGROUND_MS;
  const float r = RANGES_KM[cfg.rangeIdx];
  if (r <= 25.0f) return FETCH_NEAR_MS;
  if (r <= 50.0f) return FETCH_MID_MS;
  return FETCH_FAR_MS;
}

static void netTask(void*) {
  bool sntpStarted = false;
  bool wasScope = true;
  int fails = 0;
  unsigned long nextFetch = 0;
  for (;;) {
    if (s_paused || s_resting || WiFi.status() != WL_CONNECTED || !cfg.hasPos) {
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }
    if (!sntpStarted) {
      sntp_set_time_sync_notification_cb(onTimeSync);
      configTzTime(TZ_UK, "pool.ntp.org", "time.google.com");
      sntpStarted = true;
    }
    if (Info_Pending()) {                            // the second details page wants a route and photo
      if (!Net_HeapOk("INFO")) feedDrop();           // make room for another secure connection
      Info_Service();
    }
    if (s_scopeActive && !wasScope) s_kick = true;   // back on the scope: refresh now
    wasScope = s_scopeActive;

    if (s_kick || (long)(millis() - nextFetch) >= 0) {
      s_kick = false;
      const bool ok = fetchAircraft();
      unsigned long period = basePeriod();
      if (ok || !s_feedRefused) {
        // A failure that never reached the feed (weak Wi-Fi, a timeout) is ours, not the
        // feed's: try again at the usual pace. Backing off here once left the scope
        // showing minutes-old aircraft after a few dropped downloads in a row.
        fails = 0;
      } else {
        if (fails < 4) fails++;
        period <<= fails;                            // back off: do not hammer a struggling feed
        if (period > FETCH_MAX_BACKOFF_MS) period = FETCH_MAX_BACKOFF_MS;
        if (s_retryAfterS > 0 && (unsigned long)s_retryAfterS * 1000UL > period)
          period = (unsigned long)s_retryAfterS * 1000UL;
      }
      nextFetch = millis() + period;
      s_nextFetchMs = nextFetch;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void Net_Begin() {
  s_lock = xSemaphoreCreateMutex();
  Info_Begin();
  s_body = (char*)heap_caps_malloc(BODY_CAP, MALLOC_CAP_SPIRAM);
  if (!s_body) Serial.println("NET: no memory for the download buffer");
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

// Runs on the main task, during setup only (the background task is paused).
bool Net_LookupPostcode(const char* pc, float* lat, float* lon) {
  char clean[10]; int n = 0;
  for (const char* p = pc; *p && n < 8; p++) if (isalnum((unsigned char)*p)) clean[n++] = toupper((unsigned char)*p);
  clean[n] = '\0';
  if (n < 5) return false;

  char url[96];
  snprintf(url, sizeof(url), "%s%s", POSTCODE_URL_BASE, clean);
  NetworkClientSecure client;
  secureClient(client);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  http.setUserAgent(HTTP_USER_AGENT);
  int code = http.GET();
  if (code != 200) { Serial.printf("POSTCODE: HTTP %d\n", code); http.end(); return false; }
  String body = http.getString();
  http.end();

  JsonDocument filter;
  filter["result"]["latitude"] = true;
  filter["result"]["longitude"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
  JsonVariantConst la = doc["result"]["latitude"], lo = doc["result"]["longitude"];
  if (!la.is<float>() || !lo.is<float>()) return false;
  *lat = la.as<float>(); *lon = lo.as<float>();
  return true;
}
