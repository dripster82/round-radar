// =============================================================================
//  MeteoPlaneRadar
//  Canvas16 - off-screen 16-bit (RGB565) canvas whose framebuffer lives in
//  PSRAM. The whole UI is drawn into it with normal Arduino_GFX calls and then
//  pushed to the panel in ONE shot via flush() -> a single
//  esp_lcd_panel_draw_bitmap of the screen.
//
//  Why not Arduino_Canvas? Its flush() calls draw16bitRGBBitmap on the output,
//  which on our esp_lcd RGB panel falls back to pixel-by-pixel drawing - 230k
//  tiny writes into the panel framebuffer while the RGB DMA is reading it, i.e.
//  exactly the PSRAM double-write/read contention that makes pixels flicker.
//  Canvas16 keeps the buffer in PSRAM and flushes it as one bulk transfer, so
//  the DMA sees a single read burst. Verified fix ported from the SatRadar
//  project. Only rotation 0 is implemented (we never rotate).
//
//  Project: MeteoPlaneRadar - live aircraft radar on a round touchscreen
//  Author:  Petr / chiptron.cz   (vyvoj / development: chiptron.cz)
//  Board:   Waveshare ESP32-S3-Touch-LCD-2.1 (round 480x480 display, ST7701)
// =============================================================================
// Modified for Round Radar: the smooth-font hook (AAFont) was removed.
#pragma once
#include <Arduino_GFX_Library.h>
#include "esp_heap_caps.h"
#include "Display_ST7701.h"
#include <math.h>
#include <string.h>

class Canvas16 : public Arduino_GFX {
 public:
  Canvas16(int16_t w, int16_t h) : Arduino_GFX(w, h) {}

  ~Canvas16() {
    if (_owned && _fb) heap_caps_free(_fb);
  }

  // Draw into one of the panel's own framebuffers (zero-copy double buffering).
  // idx says which one it is, so flush() knows which to switch to next.
  void useExternalBuffer(uint16_t* fb, int idx) {
    if (_owned && _fb) heap_caps_free(_fb);
    _fb = fb;
    _back = fb;
    _fbIndex = idx;
    _owned = false;
  }

  // Round Radar: show everything turned by this many degrees, so the device can sit
  // with its USB socket wherever suits. The screen is round, so any angle works. The
  // picture is then drawn into a buffer of its own and turned into the panel's on
  // flush(), which costs some frame rate; at 0 nothing changes and nothing is copied.
  void setTurn(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg == _turn || _owned || !_back) return;
    if (deg && !_off) {
      _off = (uint16_t*)heap_caps_malloc((size_t)WIDTH * HEIGHT * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!_off) return;
      memset(_off, 0, (size_t)WIDTH * HEIGHT * 2);
    }
    _turn = deg;
    const float th = deg * 0.01745329252f;
    _cos = cosf(th);
    _sin = sinf(th);
    _fb = deg ? _off : _back;
  }
  int turn() const { return _turn; }

  // Where a point on the glass is in the picture as drawn (for touches).
  void turnPoint(int* x, int* y) const {
    if (!_turn) return;
    const float dx = *x - 239.5f, dy = *y - 239.5f;
    *x = (int)lroundf(239.5f + dx * _cos + dy * _sin);
    *y = (int)lroundf(239.5f - dx * _sin + dy * _cos);
  }

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    (void)speed;
    if (!_fb) {
      size_t bytes = (size_t)WIDTH * HEIGHT * 2;
      _fb = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      _owned = true;
    }
    return _fb != nullptr;
  }

  uint16_t* getFramebuffer() { return _fb; }

  // Single controlled flush. Signature must match Arduino_GFX::flush(bool)
  // exactly so a call through an Arduino_GFX* is dispatched here, not to the base.
  //
  // In zero-copy mode this hands the finished framebuffer to the panel and then
  // moves drawing to the other one, so the next frame is never drawn into the
  // buffer currently on screen. Doing the switch HERE means every gfx->flush()
  // in the project (radar, weather, WiFi portal, OTA) gets it automatically.
  void flush(bool force_flush = false) override {
    (void)force_flush;
    if (!_fb) return;
    if (_owned) { LCD_Flush(_fb); return; }
    if (_turn) turnInto(_back);
    LCD_Flush(_back);
    int next = _fbIndex ^ 1;
    uint16_t* nb = LCD_FrameBuffer(next);
    if (nb) { _back = nb; _fbIndex = next; }
    _fb = _turn ? _off : _back;
  }

  // --- Core writers (rotation 0 only). All are bounds-checked so a stray
  //     coordinate (e.g. a city label projected off-screen) can never write
  //     past the framebuffer. ---
  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    if (!_fb) return;
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    _fb[(int32_t)y * WIDTH + x] = color;
  }

  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    if (!_fb || h == 0) return;
    if (h < 0) { y += h + 1; h = -h; }
    if (x < 0 || x > (WIDTH - 1)) return;
    if (y > (HEIGHT - 1)) return;
    int16_t y2 = y + h - 1;
    if (y2 < 0) return;
    if (y < 0) { h += y; y = 0; }
    if (y2 > (HEIGHT - 1)) h = (HEIGHT - 1) - y + 1;
    uint16_t* p = _fb + (int32_t)y * WIDTH + x;
    while (h--) { *p = color; p += WIDTH; }
  }

  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    if (!_fb || w == 0) return;
    if (w < 0) { x += w + 1; w = -w; }
    if (y < 0 || y > (HEIGHT - 1)) return;
    if (x > (WIDTH - 1)) return;
    int16_t x2 = x + w - 1;
    if (x2 < 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x2 > (WIDTH - 1)) w = (WIDTH - 1) - x + 1;
    uint16_t* p = _fb + (int32_t)y * WIDTH + x;
    while (w--) *p++ = color;
  }

  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h,
                               uint16_t color) override {
    if (!_fb || w <= 0 || h <= 0) return;
    // Clip to the framebuffer.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > WIDTH)  w = WIDTH  - x;
    if (y + h > HEIGHT) h = HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    uint16_t* row = _fb + (int32_t)y * WIDTH + x;
    for (int16_t j = 0; j < h; j++) {
      for (int16_t i = 0; i < w; i++) row[i] = color;
      row += WIDTH;
    }
  }

 private:
  // Copy the drawn picture into dst, turned about the centre. Only the round part of
  // the screen is done, a row at a time, stepping through the source in fixed point;
  // each dot takes the nearest source dot.
  void turnInto(uint16_t* dst) {
    static int16_t half[480];
    static bool made = false;
    if (!made) {
      for (int y = 0; y < 480; y++) { const float dy = y - 239.5f; half[y] = (int16_t)sqrtf(fmaxf(0.0f, 239.0f * 239.0f - dy * dy)); }
      made = true;
    }
    const int32_t c = (int32_t)lroundf(_cos * 65536.0f), s = (int32_t)lroundf(_sin * 65536.0f);
    for (int y = 0; y < 480; y++) {
      const int x0 = 240 - half[y], x1 = 239 + half[y];
      if (x1 < x0) continue;
      const float dx = x0 - 239.5f, dy = y - 239.5f;
      int32_t fx = (int32_t)((239.5f + dx * _cos + dy * _sin) * 65536.0f) + 32768;
      int32_t fy = (int32_t)((239.5f - dx * _sin + dy * _cos) * 65536.0f) + 32768;
      uint16_t* d = dst + y * 480 + x0;
      for (int x = x0; x <= x1; x++, fx += c, fy -= s) {
        const uint32_t sx = (uint32_t)(fx >> 16), sy = (uint32_t)(fy >> 16);
        *d++ = (sx < 480 && sy < 480) ? _off[sy * 480 + sx] : 0;
      }
    }
  }

  uint16_t* _off = nullptr;    // the picture as drawn, when turned
  uint16_t* _back = nullptr;   // the panel buffer not on screen
  int       _turn = 0;
  float     _cos = 1.0f, _sin = 0.0f;
  uint16_t* _fb = nullptr;
  int       _fbIndex = 0;     // which panel framebuffer _fb points at
  bool      _owned = false;   // false = the buffer belongs to the panel driver
};
