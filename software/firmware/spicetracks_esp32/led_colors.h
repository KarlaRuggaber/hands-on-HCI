// Small colour maths for the light programme (replaces the parts of FastLED we used).
// FastLED itself is no longer included: the strip is driven by Adafruit NeoPixel only,
// exactly like the test sketch that works on the box.
#pragma once
#include <Arduino.h>

inline uint8_t qadd8(uint8_t a, uint8_t b) {
  int s = a + b;
  return s > 255 ? 255 : s;
}

// scale, but never turn a lit channel completely off
inline uint8_t scale8_video(uint8_t v, uint8_t s) {
  return ((v * s) >> 8) + ((v && s) ? 1 : 0);
}

struct CHSV {
  uint8_t hue = 0, sat = 0, val = 0;               // all 0..255, hue around the colour wheel
  CHSV() {}
  CHSV(uint8_t h, uint8_t s, uint8_t v) : hue(h), sat(s), val(v) {}
};

struct CRGB {
  uint8_t r = 0, g = 0, b = 0;
  CRGB() {}
  CRGB(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
  CRGB(const CHSV &c) {                             // HSV -> RGB, six sectors of 43
    uint8_t region = c.hue / 43;
    uint8_t rem = (c.hue - region * 43) * 6;
    uint8_t p = (c.val * (255 - c.sat)) >> 8;
    uint8_t q = (c.val * (255 - ((c.sat * rem) >> 8))) >> 8;
    uint8_t t = (c.val * (255 - ((c.sat * (255 - rem)) >> 8))) >> 8;
    switch (region) {
      case 0:  r = c.val; g = t; b = p; break;
      case 1:  r = q; g = c.val; b = p; break;
      case 2:  r = p; g = c.val; b = t; break;
      case 3:  r = p; g = q; b = c.val; break;
      case 4:  r = t; g = p; b = c.val; break;
      default: r = c.val; g = p; b = q; break;
    }
  }
  CRGB &nscale8_video(uint8_t s) {
    r = scale8_video(r, s); g = scale8_video(g, s); b = scale8_video(b, s);
    return *this;
  }
  CRGB &operator+=(const CRGB &o) {                 // adds and stops at 255
    r = qadd8(r, o.r); g = qadd8(g, o.g); b = qadd8(b, o.b);
    return *this;
  }
  CRGB operator+(const CRGB &o) const {
    CRGB c = *this;
    c += o;
    return c;
  }
};

inline CHSV rgb2hsv_approximate(const CRGB &c) {
  uint8_t mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
  CHSV h(0, 0, mx);
  if (mx == 0 || mx == mn) return h;               // grey: no hue
  int d = mx - mn;
  h.sat = 255 * d / mx;
  int hue;
  if (mx == c.r)      hue = 43 * (c.g - c.b) / d;
  else if (mx == c.g) hue = 86 + 43 * (c.b - c.r) / d;
  else                hue = 172 + 43 * (c.r - c.g) / d;
  h.hue = (uint8_t)(hue & 0xFF);
  return h;
}

// a (amount 0) .. b (amount 255)
inline CRGB blend(const CRGB &a, const CRGB &b, uint8_t amount) {
  return CRGB(a.r + (b.r - a.r) * amount / 255, a.g + (b.g - a.g) * amount / 255, a.b + (b.b - a.b) * amount / 255);
}

// sine wave between lo and hi, bpm times per minute
inline uint8_t beatsin8(int bpm, int lo = 0, int hi = 255, uint32_t timebase = 0, int phase = 0) {
  uint32_t t = millis() - timebase;
  uint16_t angle = (uint16_t)((uint64_t)t * bpm * 65536ULL / 60000ULL) + (uint16_t)((phase & 0xFF) << 8);
  float s = (sinf(angle * (6.2831853f / 65536.0f)) + 1.0f) * 0.5f;   // 0..1
  return lo + (uint8_t)(s * (hi - lo));
}

inline void fill_solid(CRGB *leds, int n, const CRGB &c) {
  for (int i = 0; i < n; i++) leds[i] = c;
}
