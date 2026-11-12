// Round Radar - the extras on the second details page: the route and a photo.
//
// Neither is in the aircraft feed, so they are looked up only when that page is
// opened: the route from adsbdb.com by callsign, the photo by ICAO address from
// planespotters.net, or failing that airport-data.com (it has more light aircraft).
// One aircraft costs about 50 KB of data. The work runs on the network
// task; the drawing side takes a copy of the result under a mutex.
#include "App.h"
#include "NetSink.h"
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include "Display_ST7701.h"
#include "TCA9554.h"
#include "Watchdog.h"
#include <time.h>
#include "esp_heap_caps.h"

// stb_image reads the JPEGs. The decoder built into the chip cannot: Planespotters
// sends progressive JPEGs. Its working memory comes from the external RAM.
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_MALLOC(sz)        heap_caps_malloc(sz, MALLOC_CAP_SPIRAM)
#define STBI_REALLOC(p, sz)    heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM)
#define STBI_FREE(p)           heap_caps_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

extern const uint8_t crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

#define ROUTE_URL_BASE "https://api.adsbdb.com/v0/callsign/"
// Planespotters only answers a client that names a way to reach its author.
#define PS_USER_AGENT  "RoundRadar/" FW_VERSION " (+https://github.com/dripster82/round-radar)"
#define PS_URL_BASE    "https://api.planespotters.net/pub/photos/hex/"
#define PS_IMAGE_HOST  "https://t.plnspttrs.net/"
#define AD_HOST        "https://airport-data.com/"
#define AD_URL_BASE    AD_HOST "api/ac_thumb.json?n=1&m="

static const size_t BODY_CAP = 96 * 1024;
static const int    RAW_MAX  = 1024;           // largest picture accepted, each way

static SemaphoreHandle_t s_lock;
static AcInfo    s_info;                       // what the page shows
static char      s_reqHex[8], s_reqCall[10], s_reqType[10];
static volatile bool s_pending = false;
static uint8_t*  s_body = nullptr;

// What has been looked up is kept, so opening the same aircraft again costs no data:
// the last few in memory, ready to draw, and everything on the device's storage (see
// PHOTO_STORE in Config.h), which survives a restart.
static const int      CACHE_N = 24;                    // 120 KB of external RAM each
static const uint32_t CACHE_KEEP_MS = 24UL * 3600UL * 1000UL;
struct Slot { AcInfo info; uint16_t* px; uint32_t madeAt, usedAt; bool valid; };
static Slot s_cache[CACHE_N];
static int  s_target = 0;                      // the slot the lookup in hand writes its photo to
static bool s_trouble = false;                 // a lookup got no proper answer: do not remember "nothing found"

// Writing disturbs the screen (see saveLookups in Main.cpp), so a lookup is not
// written as it arrives: it waits here until the photo page is closed. The mutex
// guards the storage itself and what is waiting.
static SemaphoreHandle_t s_fsLock;
static uint8_t* s_saveJpg = nullptr;
static long     s_saveJpgLen = 0;              // 0 = no picture waiting
static AcInfo   s_saveRec;
static volatile bool s_saveWaiting = false;

static bool store();

void Info_Begin() {
  s_lock = xSemaphoreCreateMutex();
  s_fsLock = xSemaphoreCreateMutex();
  s_saveJpg = (uint8_t*)heap_caps_malloc(BODY_CAP, MALLOC_CAP_SPIRAM);
  s_body = (uint8_t*)heap_caps_malloc(BODY_CAP, MALLOC_CAP_SPIRAM);
  store();                                               // so the log says at start-up which storage is in use
  for (auto& c : s_cache) { c.px = (uint16_t*)heap_caps_malloc(PHOTO_W * PHOTO_H * 2, MALLOC_CAP_SPIRAM); c.valid = false; }
  memset(&s_info, 0, sizeof(s_info));
}

bool Info_Pending() { return s_pending; }

void Info_Get(AcInfo* out) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_info;
  xSemaphoreGive(s_lock);
}

// Called from the drawing side.
void Info_Request(const char* hex, const char* callsign, const char* type) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const uint32_t now = millis();
  Slot* hit = nullptr;
  for (auto& c : s_cache) {
    if (c.valid && now - c.madeAt > CACHE_KEEP_MS) c.valid = false;
    // The same aircraft on another flight has another route: that goes the long way round.
    if (c.valid && strcmp(c.info.hex, hex) == 0 && strcmp(c.info.callsign, callsign) == 0) hit = &c;
  }
  if (strcmp(s_info.hex, hex) == 0 && s_info.loading) {
    // already being looked up
  } else if (hit) {
    hit->usedAt = now;
    s_info = hit->info;
    Serial.printf("PHOTO: %s from memory\n", hex);
  } else {
    memset(&s_info, 0, sizeof(s_info));
    strlcpy(s_info.hex, hex, sizeof(s_info.hex));
    s_info.loading = true;
    strlcpy(s_reqHex, hex, sizeof(s_reqHex));
    strlcpy(s_reqCall, callsign, sizeof(s_reqCall));
    strlcpy(s_reqType, type, sizeof(s_reqType));
    s_pending = true;
  }
  xSemaphoreGive(s_lock);
}

// Publish what has been found so far, unless another aircraft has been asked for since.
static bool publish(const AcInfo& r) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool current = strcmp(s_info.hex, r.hex) == 0;
  if (current) s_info = r;
  xSemaphoreGive(s_lock);
  return current;
}

// The screen font is plain ASCII: turn accented Latin letters into their base letter
// and leave out anything else that will not draw.
static void copyAscii(char* dst, size_t cap, const char* src) {
  static const char FOLD[] = "AAAAAAACEEEEIIIIDNOOOOO OUUUUY saaaaaaaceeeeiiiidnooooo ouuuuy y";
  size_t n = 0;
  for (const uint8_t* p = (const uint8_t*)(src ? src : ""); *p && n + 1 < cap; p++) {
    if (*p < 0x80) { if (*p >= 0x20) dst[n++] = (char)*p; }
    else if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) { const char c = FOLD[p[1] - 0x80]; if (c != ' ') dst[n++] = c; p++; }
  }
  dst[n] = '\0';
}

static long get(HTTPClient& http, NetworkClientSecure& client, const char* url, const char* tag,
                const char* agent = HTTP_USER_AGENT) {
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  if (!http.begin(client, url)) return -1;
  http.setUserAgent(agent);
  const int code = http.GET();
  if (code != 200) {
    if (code != 404) s_trouble = true;             // 404 is a proper answer: not known
    Serial.printf("%s: HTTP %d\n", tag, code);
    http.end();
    return -1;
  }
  const long len = Net_ReadBody(http, s_body, BODY_CAP, tag);
  http.end();
  if (len < 0) s_trouble = true;
  return len;
}

// ----------------------------------------------------------------- storage --
// One aircraft is two files named after its ICAO address: the record (.inf) and the
// JPEG as it was downloaded (.jpg, about 30 KB; the drawn picture would be 120 KB).
// Only the network task touches the storage.
#define REC_MAGIC 0x52523034UL
struct Rec { uint32_t magic; uint32_t savedAt; uint8_t hasPhoto; AcInfo info; };
static bool decodeTo(uint16_t* dst, long len, int16_t* outW, int16_t* outH);


// A memory card is used if one is fitted, otherwise the chip's own storage. The card
// is much the better: it holds thousands of photos, and writing to it does not disturb
// the screen, where writing to the chip's own storage does.
static fs::FS* s_fs = nullptr;
static bool    s_onCard = false;
static const char* s_dir = "";                 // where the files go
static uint64_t s_free = 0;                    // room left, kept by hand: asking a card is slow

static bool store() {
  static bool tried = false;
  if (!PHOTO_STORE) return false;
  if (tried) return s_fs != nullptr;
  tried = true;
  LCD_ReleaseSetupBus();
  TCA9554_SetPin(EXIO_SD_D3, true);
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
  if (SD_MMC.begin("/sdcard", true) && SD_MMC.cardType() != CARD_NONE) {
    if (!SD_MMC.exists(CARD_DIR)) SD_MMC.mkdir(CARD_DIR);
    s_fs = &SD_MMC;
    s_onCard = true;
    s_dir = CARD_DIR;
    s_free = SD_MMC.totalBytes() - SD_MMC.usedBytes();
    Serial.printf("STORE: memory card, %u MB, %u MB free\n", (unsigned)(SD_MMC.totalBytes() >> 20), (unsigned)(s_free >> 20));
  } else if (LittleFS.begin(true, "/store", 8, "spiffs")) {     // formats itself the first time
    s_fs = &LittleFS;
    s_free = LittleFS.totalBytes() - LittleFS.usedBytes();
    Serial.printf("STORE: no memory card, using built-in storage, %u KB free of %u KB\n", (unsigned)(s_free / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
  } else {
    Serial.println("STORE: storage did not start, photos will not be kept");
  }
  return s_fs != nullptr;
}

// The file that names a stored aircraft of this type. False if the type is not known.
static bool typePath(char* out, size_t cap, const char* type) {
  char clean[10]; int n = 0;
  for (const char* p = type; *p && n < 8; p++) if (isalnum((unsigned char)*p)) clean[n++] = *p;
  clean[n] = '\0';
  if (!n) return false;
  snprintf(out, cap, "%s/type-%s.txt", s_dir, clean);
  return true;
}

static void filePath(char* out, size_t cap, const char* hex, const char* ext) {
  snprintf(out, cap, "%s/%s.%s", s_dir, hex, ext);
}

// Delete the files written longest ago until there is room for this many bytes.
static void makeRoom(size_t need) {
  for (int guard = 0; guard < 400 && s_free < need + 96 * 1024; guard++) {
    char oldest[48] = "";
    time_t when = 0;
    size_t size = 0;
    File dir = s_fs->open(s_dir[0] ? s_dir : "/");
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (!f.isDirectory() && (!oldest[0] || f.getLastWrite() < when)) { when = f.getLastWrite(); size = f.size(); strlcpy(oldest, f.path(), sizeof(oldest)); }
      f.close();
    }
    dir.close();
    if (!oldest[0]) return;
    s_fs->remove(oldest);
    s_free += size;
  }
}

static void writePhoto(const char* hex, const uint8_t* data, long len) {
  makeRoom((size_t)len);
  char path[48];
  filePath(path, sizeof(path), hex, "jpg");
  File f = s_fs->open(path, "w");
  if (!f) return;
  const size_t n = f.write(data, (size_t)len);
  f.close();
  if (n != (size_t)len) s_fs->remove(path);
  else s_free = s_free > n ? s_free - n : 0;
}

static void writeRecord(const AcInfo& r) {
  makeRoom(sizeof(Rec));
  Rec rec;
  memset(&rec, 0, sizeof(rec));
  rec.magic = REC_MAGIC;
  rec.savedAt = (uint32_t)time(nullptr);
  rec.hasPhoto = r.photo != nullptr && !r.similar;
  rec.info = r;
  rec.info.photo = nullptr;
  if (r.similar) {                                       // a stand-in is chosen afresh each time
    rec.info.similar = false;
    rec.info.credit[0] = rec.info.site[0] = rec.info.link[0] = '\0';
  }
  char path[48];
  filePath(path, sizeof(path), r.hex, "inf");
  File f = s_fs->open(path, "w");
  if (!f) return;
  f.write((const uint8_t*)&rec, sizeof(rec));
  f.close();
  // Note this aircraft as the one to stand in for others of its type.
  if (rec.hasPhoto && typePath(path, sizeof(path), r.type)) {
    File t = s_fs->open(path, "w");
    if (t) { t.print(r.hex); t.close(); }
  }
}

// While an aircraft's own photo downloads, or when it has none, show a stored photo
// of another aircraft of the same type, marked as such on the page.
static void attachSimilar(AcInfo& r) {
  if (r.photo) return;
  xSemaphoreTake(s_fsLock, portMAX_DELAY);
  char path[48], hex[8] = "";
  Rec rec;
  bool ok = store() && typePath(path, sizeof(path), r.type);
  if (ok) {
    File t = s_fs->open(path, "r");
    ok = t && t.readBytes(hex, sizeof(hex) - 1) >= 6;
    if (t) t.close();
    for (char* p = hex; *p; p++) if (!isalnum((unsigned char)*p)) { *p = '\0'; break; }
    ok = ok && hex[0] && strcmp(hex, r.hex) != 0;
  }
  if (ok) {
    filePath(path, sizeof(path), hex, "inf");
    File f = s_fs->open(path, "r");
    ok = f && f.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec) && rec.magic == REC_MAGIC && rec.hasPhoto;
    if (f) f.close();
  }
  if (ok) {
    filePath(path, sizeof(path), hex, "jpg");
    File j = s_fs->open(path, "r");
    const size_t n = j ? j.size() : 0;
    ok = n >= 100 && n < BODY_CAP && j.read(s_body, n) == n && decodeTo(s_cache[s_target].px, (long)n, &r.pw, &r.ph);
    if (j) j.close();
  }
  xSemaphoreGive(s_fsLock);
  if (!ok) return;
  r.photo = s_cache[s_target].px;
  r.similar = true;
  strlcpy(r.credit, rec.info.credit, sizeof(r.credit));
  strlcpy(r.site, rec.info.site, sizeof(r.site));
  strlcpy(r.link, rec.info.link, sizeof(r.link));
  Serial.printf("PHOTO: %s shows %s meanwhile, the same type (%s)\n", r.hex, hex, r.type);
}

static void flushLocked() {
  if (!s_saveWaiting) return;
  if (store()) {
    if (s_saveJpgLen > 0) writePhoto(s_saveRec.hex, s_saveJpg, s_saveJpgLen);
    writeRecord(s_saveRec);
  }
  s_saveJpgLen = 0;
  s_saveWaiting = false;
}

bool Info_SaveWaiting() { return s_saveWaiting; }

void Info_FlushSaves() {
  xSemaphoreTake(s_fsLock, portMAX_DELAY);
  flushLocked();
  xSemaphoreGive(s_fsLock);
}

// Hold the record, and the JPEG still in s_body if there is one, until the page closes.
static void saveLater(const AcInfo& r, long jpgLen) {
  if (!PHOTO_STORE || !s_saveJpg) return;
  xSemaphoreTake(s_fsLock, portMAX_DELAY);
  flushLocked();                                         // one still waiting from before: rare
  if (store() && s_onCard) {                             // a card can be written at once
    const unsigned long t0 = millis();
    if (jpgLen > 0) writePhoto(r.hex, s_body, jpgLen);
    writeRecord(r);
    Serial.printf("STORE: %s written to the card in %lu ms\n", r.hex, millis() - t0);
    xSemaphoreGive(s_fsLock);
    return;
  }
  if (jpgLen > 0) memcpy(s_saveJpg, s_body, (size_t)jpgLen);
  s_saveJpgLen = jpgLen;
  s_saveRec = r;
  s_saveWaiting = true;
  xSemaphoreGive(s_fsLock);
}

static int loadStoredLocked(AcInfo& r, const char* callsign);
static int loadStored(AcInfo& r, const char* callsign) {
  xSemaphoreTake(s_fsLock, portMAX_DELAY);
  const int res = loadStoredLocked(r, callsign);
  xSemaphoreGive(s_fsLock);
  return res;
}

// Fill r from storage. 0 = not there (or too old), 1 = all of it, 2 = the photo but
// not the route, because the aircraft is now on another flight.
static int loadStoredLocked(AcInfo& r, const char* callsign) {
  if (!store()) return 0;
  char path[48];
  filePath(path, sizeof(path), r.hex, "inf");
  File f = s_fs->open(path, "r");
  if (!f) return 0;
  Rec rec;
  const bool ok = f.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec) && rec.magic == REC_MAGIC && strcmp(rec.info.hex, r.hex) == 0;
  f.close();
  if (!ok) return 0;
  const uint32_t now = (uint32_t)time(nullptr);
  const uint32_t keep = rec.hasPhoto ? STORE_KEEP_S : STORE_KEEP_NONE_S;
  if (now > 1700000000UL && rec.savedAt > 1700000000UL && now - rec.savedAt > keep) return 0;   // only once the clock is set

  rec.info.photo = nullptr;
  rec.info.loading = false;
  if (rec.hasPhoto) {
    filePath(path, sizeof(path), r.hex, "jpg");
    File j = s_fs->open(path, "r");
    if (!j) return 0;
    const size_t n = j.size();
    const bool got = n >= 100 && n < BODY_CAP && j.read(s_body, n) == n;
    j.close();
    if (!got || !decodeTo(s_cache[s_target].px, (long)n, &rec.info.pw, &rec.info.ph)) return 0;
    rec.info.photo = s_cache[s_target].px;
  }
  r = rec.info;
  Serial.printf("PHOTO: %s from storage%s\n", r.hex, rec.hasPhoto ? "" : " (none known)");
  if (strcmp(r.callsign, callsign) == 0) return 1;
  r.from[0] = r.to[0] = r.fromCity[0] = r.toCity[0] = r.airline[0] = '\0';
  strlcpy(r.callsign, callsign, sizeof(r.callsign));
  return 2;
}

// Empty a folder on the card, folders inside it included.
static void emptyFolder(const char* path, int depth) {
  for (int guard = 0; guard < 20000; guard++) {
    File dir = SD_MMC.open(path);
    if (!dir) return;
    File f = dir.openNextFile();
    if (!f) { dir.close(); return; }
    char child[160];
    strlcpy(child, f.path(), sizeof(child));
    const bool isDir = f.isDirectory();
    const size_t size = f.size();
    f.close();
    dir.close();
    if (depth == 0) Serial.printf("CARD: removing %s%s (%u bytes)\n", child, isDir ? "/" : "", (unsigned)size);
    bool ok;
    if (isDir) { if (depth < 8) emptyFolder(child, depth + 1); ok = SD_MMC.rmdir(child); }
    else ok = SD_MMC.remove(child);
    if (!ok) { Serial.printf("CARD: could not remove %s\n", child); return; }
  }
}

// Sent over USB as "list card": what is in the card's top folder, and one level down.
// Adds a line to a file in the card's folder, with a heading line if the file is new.
// Only ever on a memory card: writing the chip's own storage disturbs the screen. Does
// not wait if the card is busy with a photo; the caller tries again later.
bool Info_LogLine(const char* name, const char* header, const char* line) {
  if (!s_fsLock || xSemaphoreTake(s_fsLock, 0) != pdTRUE) return false;
  bool ok = false;
  if (store() && s_onCard) {
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", CARD_DIR, name);
    const bool fresh = !SD_MMC.exists(path);
    File f = SD_MMC.open(path, FILE_APPEND);
    if (f) {
      if (fresh) f.println(header);
      ok = f.println(line) > 0;
      f.close();
    }
  }
  xSemaphoreGive(s_fsLock);
  return ok;
}

void Info_PrintFile(const char* name) {
  xSemaphoreTake(s_fsLock, portMAX_DELAY);
  char path[48];
  snprintf(path, sizeof(path), "%s/%s", CARD_DIR, name);
  File f = (store() && s_onCard) ? SD_MMC.open(path, "r") : File();
  if (!f) Serial.printf("LOG: no %s on the card\n", name);
  else {
    Serial.printf("LOG: %s, %u bytes\n", name, (unsigned)f.size());
    uint8_t buf[256];
    for (int n; (n = f.read(buf, sizeof(buf))) > 0; ) { Serial.write(buf, n); Watchdog_Feed(); }
    f.close();
    Serial.println("LOG: end");
  }
  xSemaphoreGive(s_fsLock);
}

void Info_DeleteFile(const char* name) {
  xSemaphoreTake(s_fsLock, portMAX_DELAY);
  char path[48];
  snprintf(path, sizeof(path), "%s/%s", CARD_DIR, name);
  const bool gone = store() && s_onCard && SD_MMC.remove(path);
  Serial.printf("LOG: %s %s\n", name, gone ? "deleted" : "not found");
  xSemaphoreGive(s_fsLock);
}

void Info_ListCard() {
  xSemaphoreTake(s_fsLock, portMAX_DELAY);
  if (!store() || !s_onCard) { Serial.println("CARD: no card in use"); xSemaphoreGive(s_fsLock); return; }
  File dir = SD_MMC.open("/");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) { Serial.printf("CARD: %s (%u bytes)\n", f.path(), (unsigned)f.size()); f.close(); continue; }
    int n = 0; uint64_t bytes = 0; char first[3][64] = { "", "", "" };
    for (File g = f.openNextFile(); g; g = f.openNextFile()) { if (n < 3) strlcpy(first[n], g.name(), 64); n++; bytes += g.size(); g.close(); }
    Serial.printf("CARD: %s/ holds %d items, %u KB directly: %s %s %s\n", f.path(), n, (unsigned)(bytes / 1024), first[0], first[1], first[2]);
    f.close();
  }
  dir.close();
  Serial.println("CARD: end of list");
  xSemaphoreGive(s_fsLock);
}

// Sent over USB as "wipe card": removes everything on the memory card, formatting it
// first if it cannot be read, and starts using it.
void Info_WipeCard() {
  xSemaphoreTake(s_fsLock, portMAX_DELAY);
  store();
  if (!s_onCard) {
    Serial.println("CARD: not readable, trying to format it");
    SD_MMC.end();
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
    if (SD_MMC.begin("/sdcard", true, true) && SD_MMC.cardType() != CARD_NONE) {
      s_fs = &SD_MMC; s_onCard = true; s_dir = CARD_DIR;
    } else {
      Serial.println("CARD: no card found");
      xSemaphoreGive(s_fsLock);
      return;
    }
  }
  emptyFolder("/", 0);
  SD_MMC.mkdir(CARD_DIR);
  s_free = SD_MMC.totalBytes() - SD_MMC.usedBytes();
  for (auto& c : s_cache) c.valid = false;
  s_saveWaiting = false;
  Serial.printf("CARD: wiped, %u MB, %u MB free\n", (unsigned)(SD_MMC.totalBytes() >> 20), (unsigned)(s_free >> 20));
  xSemaphoreGive(s_fsLock);
}

// Forget every photo and route kept: on the card (our folder only), on the chip's own
// storage, and in memory. Used by the factory reset, and over USB as "clear photos".
void Info_EraseStore() {
  xSemaphoreTake(s_fsLock, portMAX_DELAY);
  store();
  if (s_onCard) {
    emptyFolder(CARD_DIR, 1);
    s_free = SD_MMC.totalBytes() - SD_MMC.usedBytes();
    if (LittleFS.begin(false, "/store", 8, "spiffs")) { LittleFS.format(); LittleFS.end(); }   // photos from before the card
  } else if (s_fs) {
    LittleFS.format();
    s_free = LittleFS.totalBytes() - LittleFS.usedBytes();
  }
  s_saveWaiting = false;
  s_saveJpgLen = 0;
  xSemaphoreGive(s_fsLock);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (auto& c : s_cache) c.valid = false;
  memset(&s_info, 0, sizeof(s_info));
  xSemaphoreGive(s_lock);
  Serial.printf("STORE: cleared, %s, %u MB free\n", s_onCard ? "memory card" : "built-in storage", (unsigned)(s_free >> 20));
}

// ------------------------------------------------------------------- route --
static void lookUpRoute(AcInfo& r, const char* callsign) {
  if (!callsign[0]) return;
  char url[80];
  snprintf(url, sizeof(url), ROUTE_URL_BASE "%s", callsign);
  NetworkClientSecure client;
  client.setCACertBundle(crt_bundle_start, crt_bundle_end - crt_bundle_start);
  HTTPClient http;
  const long len = get(http, client, url, "ROUTE");     // 404 = a callsign it does not know
  if (len < 8) return;

  JsonDocument filter;
  JsonObject f = filter["response"]["flightroute"].to<JsonObject>();
  f["airline"]["name"] = true;
  for (const char* end : { "origin", "destination" }) { f[end]["iata_code"] = true; f[end]["municipality"] = true; }
  JsonDocument doc;
  if (deserializeJson(doc, (const char*)s_body, (size_t)len, DeserializationOption::Filter(filter))) return;
  JsonObjectConst fr = doc["response"]["flightroute"];
  if (fr.isNull()) return;
  copyAscii(r.from, sizeof(r.from), fr["origin"]["iata_code"] | "");
  copyAscii(r.to, sizeof(r.to), fr["destination"]["iata_code"] | "");
  copyAscii(r.fromCity, sizeof(r.fromCity), fr["origin"]["municipality"] | "");
  copyAscii(r.toCity, sizeof(r.toCity), fr["destination"]["municipality"] | "");
  copyAscii(r.airline, sizeof(r.airline), fr["airline"]["name"] | "");
  if (!r.from[0] || !r.to[0]) r.from[0] = r.to[0] = '\0';
}

// ------------------------------------------------------------------- photo --
// Decode the JPEG in s_body and scale it, smoothly, to fit the photo area.
static bool decodeTo(uint16_t* dst, long len, int16_t* outW, int16_t* outH) {
  int w = 0, h = 0, n = 0;
  if (!stbi_info_from_memory(s_body, (int)len, &w, &h, &n) || w < 8 || h < 8 || w > RAW_MAX || h > RAW_MAX) {
    Serial.printf("PHOTO: not a picture this can read (%d x %d)\n", w, h);
    return false;
  }
  uint8_t* rgb = stbi_load_from_memory(s_body, (int)len, &w, &h, &n, 3);
  if (!rgb) { Serial.println("PHOTO: decode failed"); return false; }

  const float k = fminf((float)PHOTO_W / w, (float)PHOTO_H / h);
  const int dw = (int)(w * k), dh = (int)(h * k);
  for (int y = 0; y < dh; y++) {
    const float fy = fmaxf(0.0f, fminf((y + 0.5f) / k - 0.5f, h - 1.0f));
    const int y0 = (int)fy, y1 = y0 + 1 < h ? y0 + 1 : y0, wy = (int)((fy - y0) * 256);
    for (int x = 0; x < dw; x++) {
      const float fx = fmaxf(0.0f, fminf((x + 0.5f) / k - 0.5f, w - 1.0f));
      const int x0 = (int)fx, x1 = x0 + 1 < w ? x0 + 1 : x0, wx = (int)((fx - x0) * 256);
      const uint8_t *a = rgb + (y0 * w + x0) * 3, *b = rgb + (y0 * w + x1) * 3;
      const uint8_t *c = rgb + (y1 * w + x0) * 3, *d = rgb + (y1 * w + x1) * 3;
      int ch[3];
      for (int i = 0; i < 3; i++) {
        const int top = a[i] * (256 - wx) + b[i] * wx, bot = c[i] * (256 - wx) + d[i] * wx;
        ch[i] = (top * (256 - wy) + bot * wy) >> 16;
      }
      dst[y * dw + x] = (uint16_t)(((ch[0] & 0xF8) << 8) | ((ch[1] & 0xFC) << 3) | (ch[2] >> 3));
    }
  }
  stbi_image_free(rgb);
  *outW = (int16_t)dw;
  *outH = (int16_t)dh;
  return true;
}

// Fetch the picture at url into the buffer not on show, and make it the photo.
static long s_newJpgLen = 0;
static char s_link[sizeof(((AcInfo*)0)->link)];   // the page of the photo about to be fetched
static bool takePhoto(AcInfo& r, HTTPClient& http, NetworkClientSecure& client, const char* url,
                      const char* agent, const char* who, const char* site) {
  const long len = get(http, client, url, "PHOTO", agent);
  if (len < 100) return false;
  int16_t w = 0, h = 0;
  if (!decodeTo(s_cache[s_target].px, len, &w, &h)) return false;
  s_newJpgLen = len;                                     // the JPEG as downloaded, still in s_body
  r.photo = s_cache[s_target].px;
  r.pw = w;
  r.ph = h;
  r.similar = false;
  strlcpy(r.credit, who[0] ? who : "unknown", sizeof(r.credit));
  strlcpy(r.site, site, sizeof(r.site));
  strlcpy(r.link, s_link, sizeof(r.link));
  Serial.printf("PHOTO: %s from %s, %ld bytes, shown %dx%d\n", r.hex, site, len, w, h);
  return true;
}

static bool photoFromPlanespotters(AcInfo& r) {
  char url[96], imageUrl[160], who[24];
  {
    snprintf(url, sizeof(url), PS_URL_BASE "%s", r.hex);
    NetworkClientSecure client;
    client.setCACertBundle(crt_bundle_start, crt_bundle_end - crt_bundle_start);
    HTTPClient http;
    const long len = get(http, client, url, "PHOTO", PS_USER_AGENT);
    if (len < 8) return false;
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)s_body, (size_t)len)) return false;
    const char* image = doc["photos"][0]["thumbnail_large"]["src"] | "";
    // Only ever fetch a picture from the photo site's own picture server.
    if (strncmp(image, PS_IMAGE_HOST, strlen(PS_IMAGE_HOST)) != 0) return false;
    strlcpy(imageUrl, image, sizeof(imageUrl));
    copyAscii(who, sizeof(who), doc["photos"][0]["photographer"] | "");
    const char* link = doc["photos"][0]["link"] | "";    // used unchanged, as the site asks
    strlcpy(s_link, strncmp(link, "https://www.planespotters.net/", 30) == 0 ? link : "", sizeof(s_link));
  }                                                      // that connection closes before the next opens
  NetworkClientSecure client;
  client.setCACertBundle(crt_bundle_start, crt_bundle_end - crt_bundle_start);
  HTTPClient http;
  return takePhoto(r, http, client, imageUrl, PS_USER_AGENT, who, "planespotters.net");
}

static bool photoFromAirportData(AcInfo& r) {
  char url[96], imageUrl[160], who[24];
  snprintf(url, sizeof(url), AD_URL_BASE "%s", r.hex);
  NetworkClientSecure client;                            // one connection for the lookup and the picture
  client.setCACertBundle(crt_bundle_start, crt_bundle_end - crt_bundle_start);
  HTTPClient http;
  const long len = get(http, client, url, "PHOTO");      // 404 = no photo of this aircraft
  if (len < 8) return false;
  JsonDocument doc;
  if (deserializeJson(doc, (const char*)s_body, (size_t)len)) return false;
  const char* image = doc["data"][0]["image"] | "";
  if (strncmp(image, AD_HOST, strlen(AD_HOST)) != 0) return false;
  strlcpy(imageUrl, image, sizeof(imageUrl));
  copyAscii(who, sizeof(who), doc["data"][0]["photographer"] | "");
  const char* link = doc["data"][0]["link"] | "";
  strlcpy(s_link, strncmp(link, AD_HOST, strlen(AD_HOST)) == 0 ? link : "", sizeof(s_link));
  return takePhoto(r, http, client, imageUrl, HTTP_USER_AGENT, who, "airport-data.com");
}

static void lookUpPhoto(AcInfo& r) {
  if (photoFromPlanespotters(r)) return;
  if (!photoFromAirportData(r)) Serial.printf("PHOTO: none for %s\n", r.hex);
}

// Runs on the network task.
void Info_Service() {
  if (!s_body || !s_cache[CACHE_N - 1].px) { s_pending = false; return; }
  AcInfo r;
  char callsign[sizeof(s_reqCall)];
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_pending = false;
  memset(&r, 0, sizeof(r));
  strlcpy(r.hex, s_reqHex, sizeof(r.hex));
  strlcpy(callsign, s_reqCall, sizeof(callsign));
  // Write into an empty slot, or else the one opened longest ago.
  s_target = 0;
  for (int i = 0; i < CACHE_N; i++) {
    if (!s_cache[i].valid) { s_target = i; break; }
    if ((int32_t)(s_cache[i].usedAt - s_cache[s_target].usedAt) < 0) s_target = i;
  }
  s_cache[s_target].valid = false;
  xSemaphoreGive(s_lock);
  s_trouble = false;

  strlcpy(r.callsign, callsign, sizeof(r.callsign));
  char type[sizeof(r.type)];
  strlcpy(type, s_reqType, sizeof(type));
  const int stored = loadStored(r, callsign);
  strlcpy(r.type, type, sizeof(r.type));
  attachSimilar(r);                                      // only if it has no photo of its own to hand
  if (stored != 1) {
    r.loading = true;
    if (r.photo && !publish(r)) return;                  // a stored photo shows while the rest comes
    lookUpRoute(r, callsign);
    Serial.printf("ROUTE: %s %s -> %s\n", callsign, r.from[0] ? r.from : "?", r.to[0] ? r.to : "?");
    if (!publish(r)) return;                             // the route shows while the photo comes
    s_newJpgLen = 0;
    if (stored == 0) lookUpPhoto(r);
    r.loading = false;
    if ((r.photo && !r.similar) || !s_trouble) saveLater(r, s_newJpgLen);
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if ((r.photo && !r.similar) || !s_trouble) {           // a failed lookup is tried again next time
    Slot& c = s_cache[s_target];
    c.info = r;
    c.madeAt = c.usedAt = millis();
    c.valid = true;
  }
  xSemaphoreGive(s_lock);
  publish(r);
}
