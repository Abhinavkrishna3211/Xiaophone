// Render task + Xiaophone's moods. The frame is drawn in four 240x60 strips into two
// internal-RAM buffers, so one strip renders while the previous one goes out over DMA.
#include <Arduino.h>
#include <string>
#include "ui_states.h"
#include "display.h"
#include "disc.h"
#include "net.h"
#include "audio_player.h"
#include "sounds.h"

enum State { SLEEP, WAKE, TUNING, PLAYING, LOST };
enum Tr { TR_NONE, TR_DOWN, TR_OUT, TR_IN };
static const char* NAMES[] = {"sleep", "wake", "tuning", "playing", "lost"};

static const int SH = 60;                  // strip height
static const float FULL = 198.0f;          // deg/s = 0.55 rev/s (33 1/3 rpm feel)
static const uint16_t KEY = 0xF81F;        // transparent key for glyph sprites (magenta)

static LGFX_Sprite strip[2];
static float angle = 0, speed = 0;
static int bright = 255;
static State st = SLEEP;
static uint32_t stT = 0, lostSince = 0;
static Tr tr = TR_NONE;
static uint32_t trT = 0;
static std::string nextTitle;
static volatile uint32_t frames = 0;
static bool demo = false;

uint32_t uiFrames() { return frames; }
const char* uiStateName() { return NAMES[st]; }

// ---------- curved song title ----------
static const int GMAX = 40;
static const float TR_R = 101;             // text radius
static LGFX_Sprite glyph[GMAX];
static float gAng[GMAX];
static int gN = 0;
static uint32_t titleT = 0;

static void titleSet(const std::string& t) {
  for (int i = 0; i < gN; i++) glyph[i].deleteSprite();
  gN = 0;
  std::string s;
  for (unsigned char c : t) if (c >= 32 && c < 127) s += (char)c;  // font is ASCII only
  if (s.empty()) return;
  const lgfx::IFont* f = &fonts::FreeSansBold9pt7b;
  tft.setFont(f);
  const float maxPx = 3.49f * TR_R;  // 200 degrees of arc
  auto width = [&](const std::string& x) { return (float)tft.textWidth(x.c_str()); };
  if (width(s) > maxPx) {
    while (s.size() > 3 && width(s + "..") > maxPx) s.pop_back();
    s += "..";
  }
  float total = width(s), x = 0, start = -1.5708f - total / 2 / TR_R;
  for (size_t i = 0; i < s.size() && gN < GMAX; i++) {
    char ch[2] = {s[i], 0};
    int w = tft.textWidth(ch);
    if (s[i] != ' ') {
      LGFX_Sprite& g = glyph[gN];
      g.setPsram(true);
      g.setColorDepth(16);
      g.createSprite(w + 4, 24);
      g.fillScreen(KEY);
      g.setFont(f);
      g.setTextDatum(middle_center);
      g.setTextColor((uint16_t)TFT_BLACK);
      int cx = (w + 4) / 2;
      for (int d = -1; d <= 1; d += 2) { g.drawString(ch, cx + d, 12); g.drawString(ch, cx, 12 + d); }
      g.setTextColor((uint16_t)TFT_WHITE);
      g.drawString(ch, cx, 12);
      g.setPivot(cx, 12);
      gAng[gN++] = start + (x + w / 2.0f) / TR_R;
    }
    x += w;
  }
}

static void titleDraw(LGFX_Sprite& s, int oy, uint32_t now) {
  if (!gN) return;
  int el = now - titleT;
  int n = el < 3000 ? el / 30 : gN - (el - 3000) / 30;  // type in, show ~3 s, dissolve out
  if (n > gN) n = gN;
  for (int i = 0; i < n; i++) {
    float x = 120 + TR_R * cosf(gAng[i]), y = 120 + TR_R * sinf(gAng[i]);
    if (y + 14 < oy || y - 14 > oy + SH) continue;
    glyph[i].pushRotateZoom(&s, x, y - oy, gAng[i] * 57.2958f + 90, 1, 1, KEY);
  }
}

// ---------- sparkles ----------
struct Sparkle { float a, r; uint32_t t0, life; };
static Sparkle sp[16];

static void sparklesSpawn(uint32_t now) {
  for (auto& p : sp) p = {random(360) * 0.01745f, 106.0f + random(12), now + random(700), 450u + random(450)};
}

static void sparklesDraw(LGFX_Sprite& s, int oy, uint32_t now) {
  uint16_t c1 = lgfx::color565(255, 215, 90), c2 = lgfx::color565(255, 250, 220);
  for (auto& p : sp) {
    if (now < p.t0 || now > p.t0 + p.life) continue;
    float k = (float)(now - p.t0) / p.life;
    int sz = 1 + (int)(5 * sinf(3.1416f * k));
    int x = 120 + p.r * cosf(p.a), y = 120 + p.r * sinf(p.a) - oy;
    if (y + sz < 0 || y - sz >= SH) continue;
    s.drawFastHLine(x - sz, y, 2 * sz + 1, c1);
    s.drawFastVLine(x, y - sz, 2 * sz + 1, c1);
    int d = sz / 2;
    s.drawLine(x - d, y - d, x + d, y + d, c1);
    s.drawLine(x - d, y + d, x + d, y - d, c1);
    s.fillCircle(x, y, 1, c2);
  }
}

// ---------- label decorations ----------
static void heartDraw(LGFX_Sprite& s, int oy, float scale) {
  float th = angle * 0.0174533f, cs = cosf(th), sn = sinf(th);
  float hx = 120 + 27 * sn, hy = 120 - 27 * cs - oy;  // sits above the spindle, turns with the label
  if (hy + 20 < 0 || hy - 20 > SH) return;
  auto P = [&](float x, float y, float& ox, float& oy2) {
    x *= scale; y *= scale;
    ox = hx + x * cs - y * sn;
    oy2 = hy + x * sn + y * cs;
  };
  uint16_t c = lgfx::color565(212, 160, 50);
  float ax, ay, bx, by, cx, cy;
  P(-5, -3, ax, ay); s.fillCircle(ax, ay, 5.5f * scale, c);
  P(5, -3, ax, ay);  s.fillCircle(ax, ay, 5.5f * scale, c);
  P(-10.3f, -0.5f, ax, ay); P(10.3f, -0.5f, bx, by); P(0, 11, cx, cy);
  s.fillTriangle(ax, ay, bx, by, cx, cy, c);
}

static void zzzDraw(LGFX_Sprite& s, int oy, uint32_t now) {
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextDatum(middle_center);
  s.setTextColor(lgfx::color565(235, 195, 90));
  for (int k = 0; k < 3; k++) {
    float p = fmodf(now / 2400.0f + k / 3.0f, 1.0f);
    if (p > 0.9f) continue;
    s.setTextSize(0.8f + 0.9f * p);
    s.drawString("z", 134 + 36 * p, 104 - 56 * p - oy);
  }
  s.setTextSize(1);
}

static void noteDraw(LGFX_Sprite& s, int oy, uint32_t now) {
  int y = 84 + (int)(3 * sinf(now / 300.0f)) - oy;
  uint16_t c = lgfx::color565(140, 100, 45);
  s.fillEllipse(115, y + 16, 5, 4, c);
  s.fillRect(119, y, 2, 17, c);
  s.drawLine(120, y, 127, y + 6, c);
  s.drawLine(120, y + 1, 127, y + 7, c);
}

static void dotsDraw(LGFX_Sprite& s, int oy, uint32_t now) {
  int on = (now / 400) % 4;
  for (int k = 0; k < on; k++) {
    s.fillCircle(108 + 12 * k, 96 - oy, 4, (uint16_t)TFT_BLACK);
    s.fillCircle(108 + 12 * k, 96 - oy, 3, (uint16_t)TFT_WHITE);
  }
}

// ---------- state machine ----------
static void update(uint32_t now, float dt) {
  bool net = netUp(), str = demo ? net : audioStreaming();
  State prev = st;
  switch (st) {
    case SLEEP:   if (net) st = WAKE; break;
    case WAKE:    if (now - stT > 1500) st = TUNING; break;
    case TUNING:  if (str) st = PLAYING; break;
    case PLAYING:
      if (net && str) lostSince = 0;
      else if (!lostSince) lostSince = now;
      else if (now - lostSince > 2000) st = LOST;
      break;
    case LOST:    if (net && str) st = PLAYING; break;
  }
  if (st != prev) { stT = now; Serial.printf("[GFX] state: %s\n", NAMES[st]); }

  float target = st == PLAYING ? FULL : st == TUNING ? 40 : 0;
  float k = st == LOST ? 0.5f : 2.5f;  // lost: slow wind-down

  if (tr == TR_NONE && st >= TUNING && discPending(nextTitle)) { tr = TR_DOWN; trT = now; }
  switch (tr) {
    case TR_NONE: break;
    case TR_DOWN:  // quick spin-down
      target = 0; k = 6;
      if (speed < 15 || now - trT > 900) { tr = TR_OUT; trT = now; if (!demo) soundsCrackle(); }
      break;
    case TR_OUT:   // fade to dark, swap
      target = 0;
      bright = 255 - 255 * (int)(now - trT) / 350;
      if (now - trT >= 350) {
        discSwap();
        titleSet(nextTitle);
        titleT = now;
        sparklesSpawn(now);
        tr = TR_IN; trT = now; bright = 0;
      }
      break;
    case TR_IN:    // fade in while spinning back up
      bright = 255 * (int)(now - trT) / 500;
      if (now - trT >= 500) { bright = 255; tr = TR_NONE; }
      break;
  }
  if (bright < 0) bright = 0;
  if (bright > 255) bright = 255;
  speed += (target - speed) * fminf(1.0f, k * dt);
  angle = fmodf(angle + speed * dt, 360.0f);
}

static void overlays(LGFX_Sprite& s, int oy, uint32_t now) {
  bool def = discFrontIsDefault();
  if (def && bright == 255 && st != LOST) {
    float pulse = st == SLEEP ? 1 + 0.05f * sinf(now / 480.0f)
                : st == WAKE  ? 1 + 0.45f * sinf(3.1416f * fminf(1, (now - stT) / 800.0f))
                              : 1 + 0.10f * sinf(now / 220.0f);
    heartDraw(s, oy, pulse);
  }
  if (st == SLEEP) zzzDraw(s, oy, now);
  if (st == TUNING && def) noteDraw(s, oy, now);
  if (st == LOST) dotsDraw(s, oy, now);
  sparklesDraw(s, oy, now);
  titleDraw(s, oy, now);
}

static void renderTask(void*) {
  TickType_t last = xTaskGetTickCount();
  uint32_t prev = millis();
  for (;;) {
    uint32_t now = millis();
    float dt = (now - prev) / 1000.0f;
    prev = now;
    update(now, dt);

    LGFX_Sprite* d = discFront();
    tft.startWrite();
    for (int i = 0; i < 240 / SH; i++) {
      LGFX_Sprite& s = strip[i & 1];
      int oy = i * SH;
      d->pushRotateZoom(&s, 120, 120 - oy, angle, 1, 1);
      uint16_t* px = (uint16_t*)s.getBuffer();
      discFade(px, 240 * SH, bright);
      discGloss(px, oy, SH);
      overlays(s, oy, now);
      tft.pushImageDMA(0, oy, 240, SH, (lgfx::swap565_t*)px);
    }
    tft.endWrite();
    frames++;
    if (xTaskDelayUntil(&last, pdMS_TO_TICKS(40)) == pdFALSE) vTaskDelay(1);  // ~25 fps cap, always yield
  }
}

void uiBegin(bool demoMode) {
  demo = demoMode;
  for (auto& s : strip) {
    s.setPsram(false);  // internal RAM, DMA capable
    s.setColorDepth(16);
    s.createSprite(240, SH);
  }
  stT = millis();
  xTaskCreatePinnedToCore(renderTask, "render", 8192, nullptr, 2, nullptr, 1);
  Serial.println("[GFX] render task started");
}
