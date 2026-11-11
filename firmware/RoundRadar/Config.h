// Round Radar - build-time constants.
// The first block is what the drivers borrowed from MeteoPlaneRadar expect.
#pragma once

#define FW_VERSION "0.1.0"

// --- Needed by the borrowed drivers (Display, Board, Touch, NetSink, Watchdog)
#define I2C_SDA 15
#define I2C_SCL 7
#define BOARD_FORCE 0                 // 0 = detect the board at start-up
#define BOARD_FALLBACK BOARD_LCD_2_1
#define FLUSH_DEBUG 0
#define TOUCH_DEBUG 0
#define TOUCH_USE_INT 1
#define TOUCH_IDLE_POLL_MS 250
#define TOUCH_RELEASE_MS 60
#define NET_BODY_BUDGET_MS 15000UL
#define NET_MIN_HEAP 60000
#define WDT_TIMEOUT_S 30

// --- Round Radar ------------------------------------------------------------
// The buzzer shares expander pin 8, which MeteoPlaneRadar calls EXIO_LCD_PWR and
// simply holds low. On the 2.1" board Waveshare's own examples drive the buzzer
// from this pin. UNVERIFIED on hardware: Ping is off by default for that reason.
#define EXIO_BUZZER 8
#define PING_KM 5.0f
#define PING_MS 200

#define AP_SSID "Round Radar"
#define HTTP_USER_AGENT "RoundRadar/" FW_VERSION " (personal desk display)"
#define ADSB_URL_BASE "https://opendata.adsb.fi/api/v3/lat/"
#define POSTCODE_URL_BASE "https://api.postcodes.io/postcodes/"
#define TZ_UK "GMT0BST,M3.5.0/1,M10.5.0"

// Photos and routes already looked up are kept on the device's own storage, so they
// survive a restart and cost no mobile data the second time. Planespotters' public
// terms do not allow its pictures to be stored; this is switched on here with their
// permission, given to the author. Anyone else building this: ask them, or set it to 0.
#define PHOTO_STORE 1
// The memory card slot (Waveshare's wiring table): one data line, and the card's D3
// held high by expander pin 4. Clock and command share pins with the panel's set-up bus.
#define SD_CLK_PIN 2
#define SD_CMD_PIN 1
#define SD_D0_PIN 42
#define EXIO_SD_D3 4
#define CARD_DIR "/roundradar"
#define STORE_KEEP_S (30UL * 24UL * 3600UL)      // looked up again after a month
#define STORE_KEEP_NONE_S (3UL * 24UL * 3600UL)  // "no photo found": asked again after three days

#define AC_MAX 100                    // aircraft kept and drawn (airborne only)
#define RANGE_COUNT 4
#define TRAIL_N 4

// Fetch periods are set for a phone hotspot, where every download costs mobile data.
// The scope moves each aircraft along its track between fetches, so it stays smooth.
#define FETCH_NEAR_MS 15000UL         // range up to 25 km
#define FETCH_MID_MS 20000UL          // 50 km
#define FETCH_FAR_MS 30000UL          // 100 km
#define FETCH_BACKGROUND_MS 300000UL  // while the scope is not on screen (the clock's count)
#define NIGHT_BRIGHT_MS 30000UL        // in night hours, a touch brightens the screen for this long
#define NIGHT_AWAKE_MS 300000UL       // in night hours, fetching rests this long after the last touch
#define FETCH_MAX_BACKOFF_MS 60000UL
#define DATA_STALE_MS 90000UL         // aircraft drawn grey
#define DATA_HIDE_MS 600000UL         // aircraft not drawn at all
#define DETAIL_GRACE_POLLS 3

#define SWEEP_PERIOD_MS 4000UL
#define LONG_PRESS_MS 800UL
// The battery's voltage reaches this pin through a divider that passes one third of it
// (200K over 100K on Waveshare's schematic).
#define PIN_BATTERY 4
// The whole picture is drawn turned by this many degrees, so the device sits with its
// USB socket at the bottom (it is at about half past four with the picture unturned).
// "rotate <degrees>" over USB changes it on one device; 0 costs no frame rate.
#define SCREEN_TURN_DEG 315
#define PIN_BOOT 0                    // the BOOT button on the side; pressed = low
#define FACTORY_WARN_MS 1000UL        // held this long: the warning shows
#define FACTORY_HOLD_MS 8000UL        // held this long: everything saved is erased
#define SWIPE_PX 60
#define TAP_RADIUS_PX 30
#define WIFI_JOIN_TIMEOUT_MS 20000UL
