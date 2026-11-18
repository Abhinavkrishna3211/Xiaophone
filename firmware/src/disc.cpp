#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <vector>
#include "disc.h"

static LGFX_Sprite spr[2], sleepSpr;
static int front = 0;
static bool isDefault[2] = {true, true};
static std::atomic<bool> pending{false};
static std::string pendingTitle;
static std::vector<uint32_t> gloss;  // (pixel index << 8) | alpha, sorted by index

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return (c >> 8) | (c << 8);
}

static inline uint16_t scale(uint16_t v, int k) {  // k/256
  uint16_t c = (v >> 8) | (v << 8);
  int r = ((c >> 11) & 31) * k >> 8, g = ((c >> 5) & 63) * k >> 8, b = (c & 31) * k >> 8;
  c = r << 11 | g << 5 | b;
  return (c >> 8) | (c << 8);
}

static inline uint16_t toWhite(uint16_t v, int a) {  // a/256 towards white
  uint16_t c = (v >> 8) | (v << 8);
  int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  r += (31 - r) * a >> 8; g += (63 - g) * a >> 8; b += (31 - b) * a >> 8;
  c = r << 11 | g << 5 | b;
  return (c >> 8) | (c << 8);
}

static uint16_t gold, spindle;  // raw buffer colours, set in discBegin

// Vinyl styling shared by cover and default discs: grooves, rim shade, spindle.
static void finish(uint16_t* p, bool fromCover) {
  for (int y = 0; y < 240; y++)
    for (int x = 0; x < 240; x++) {
      float dx = x - 119.5f, dy = y - 119.5f, r = sqrtf(dx * dx + dy * dy);
      uint16_t& c = p[y * 240 + x];
      if (r > 119.5f) { c = 0; continue; }
      if (r < 5.0f) { c = spindle; continue; }
      if (r < 6.5f) { c = gold; continue; }
      bool ring = r > 40 && r < 118 && fmodf(r, 6.0f) < 1.0f;
      if (fromCover) {
        if (ring) c = scale(c, 200);
        if (r > 117) c = scale(c, 150);
      }
    }
}

static void buildDefault(LGFX_Sprite& s) {
  s.fillScreen(0);
  s.fillCircle(120, 120, 119, lgfx::color565(22, 22, 26));
  for (int r = 62; r < 118; r += 3) s.drawCircle(120, 120, r, lgfx::color565(36, 36, 42));  // fine grooves
  s.drawCircle(120, 120, 90, lgfx::color565(12, 12, 14));                                    // track gap
  uint16_t g = lgfx::color565(212, 175, 55);
  for (int i = 0; i < 28; i++) {  // scalloped gold edge
    float a = i * 6.2832f / 28;
    s.fillCircle(120 + 56 * cosf(a), 120 + 56 * sinf(a), 5, g);
  }
  s.fillCircle(120, 120, 56, g);
  s.fillCircle(120, 120, 54, lgfx::color565(246, 236, 214));  // cream label
  for (int i = 0; i < 36; i++) {  // dotted inner ring
    float a = i * 6.2832f / 36;
    s.fillCircle(120 + 47 * cosf(a), 120 + 47 * sinf(a), 1, g);
  }
  s.setTextDatum(middle_center);
  s.setFont(&fonts::FreeSerifItalic9pt7b);
  s.setTextColor(lgfx::color565(140, 100, 45));
  s.drawString("Xiaophone", 120, 152);  // below the spindle (the heart sits above it)
  finish((uint16_t*)s.getBuffer(), false);
}

static void buildGloss() {
  // two soft reflections that stay still while the record turns: upper-left strong, lower-right faint
  for (int y = 0; y < 240; y++)
    for (int x = 0; x < 240; x++) {
      float dx = x - 119.5f, dy = y - 119.5f, r = sqrtf(dx * dx + dy * dy);
      if (r < 58 || r > 116) continue;
      float rw = fminf(1, (r - 58) / 14) * fminf(1, (116 - r) / 10);
      float a = atan2f(dy, dx) * 57.2958f;
      auto lobe = [&](float c, float sig, float peak) {
        float d = fabsf(fmodf(a - c + 540.0f, 360.0f) - 180.0f);
        return peak * expf(-d * d / (2 * sig * sig));
      };
      int al = (int)((lobe(-135, 16, 80) + lobe(45, 12, 40)) * rw);
      if (al >= 4) gloss.push_back((uint32_t)(y * 240 + x) << 8 | al);
    }
}

void discBegin() {
  gold = rgb(212, 175, 55);
  spindle = rgb(25, 22, 20);
  for (auto* s : {&spr[0], &spr[1], &sleepSpr}) {
    s->setPsram(true);
    s->setColorDepth(16);
    s->createSprite(240, 240);
  }
  buildDefault(spr[0]);
  buildDefault(sleepSpr);
  buildGloss();
  Serial.printf("[GFX] discs ready, gloss %u px\n", (unsigned)gloss.size());
}

LGFX_Sprite* discFront() { return &spr[front]; }
bool discFrontIsDefault() { return isDefault[front]; }
LGFX_Sprite* discSleep() { return &sleepSpr; }
bool discBackFree() { return !pending.load(); }

void discBuild(const uint16_t* cover) {
  int b = front ^ 1;
  if (cover) {
    memcpy(spr[b].getBuffer(), cover, 240 * 240 * 2);
    finish((uint16_t*)spr[b].getBuffer(), true);
  } else {
    buildDefault(spr[b]);
  }
  isDefault[b] = !cover;
}

void discCommit(const char* title) {
  pendingTitle = title;
  pending.store(true);
}

bool discPending(std::string& title) {
  if (!pending.load()) return false;
  title = pendingTitle;
  return true;
}

void discSwap() {
  front ^= 1;
  pending.store(false);
}

void discGloss(uint16_t* px, int y0, int h) {
  uint32_t lo = (uint32_t)y0 * 240, hi = (uint32_t)(y0 + h) * 240;
  auto it = std::lower_bound(gloss.begin(), gloss.end(), lo << 8);
  for (; it != gloss.end() && (*it >> 8) < hi; ++it) {
    uint16_t& c = px[(*it >> 8) - lo];
    c = toWhite(c, *it & 0xFF);
  }
}

void discFade(uint16_t* px, int n, int k) {
  if (k >= 255) return;
  for (int i = 0; i < n; i++) px[i] = scale(px[i], k);
}
