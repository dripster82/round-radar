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
#define PING_MS 90

#define AP_SSID "Round Radar"
#define HTTP_USER_AGENT "RoundRadar/" FW_VERSION " (personal desk display)"
#define ADSB_URL_BASE "https://opendata.adsb.fi/api/v3/lat/"
#define POSTCODE_URL_BASE "https://api.postcodes.io/postcodes/"
#define TZ_UK "GMT0BST,M3.5.0/1,M10.5.0"

#define AC_MAX 100                    // aircraft kept and drawn (airborne only)
#define RANGE_COUNT 4
#define TRAIL_N 4

#define FETCH_NEAR_MS 5000UL          // range up to 25 km
#define FETCH_MID_MS 10000UL          // 50 km
#define FETCH_FAR_MS 15000UL          // 100 km
#define FETCH_BACKGROUND_MS 30000UL   // while the scope is not on screen
#define FETCH_MAX_BACKOFF_MS 60000UL
#define DATA_STALE_MS 60000UL         // aircraft drawn grey
#define DATA_HIDE_MS 600000UL         // aircraft not drawn at all
#define DETAIL_GRACE_POLLS 3

#define SWEEP_PERIOD_MS 4000UL
#define LONG_PRESS_MS 800UL
#define SWIPE_PX 60
#define TAP_RADIUS_PX 30
#define NIGHT_BRIGHTNESS 10
#define WIFI_JOIN_TIMEOUT_MS 20000UL
