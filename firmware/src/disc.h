#pragma once
#include <LovyanGFX.hpp>
#include <string>

// Two 240x240 disc sprites in PSRAM: the render task spins the front one while the
// cover task builds the next one into the back. Handoff is a single atomic flag.
void discBegin();
LGFX_Sprite* discFront();
bool discFrontIsDefault();
LGFX_Sprite* discSleep();  // default label for the sleeping face; leaves the loaded cover alone

// cover task side
bool discBackFree();
void discBuild(const uint16_t* cover);  // 240x240 big-endian RGB565, or nullptr for the default label
void discCommit(const char* title);

// render task side
bool discPending(std::string& title);
void discSwap();

// per-strip post effects on big-endian RGB565 pixels (strip covers rows y0..y0+h-1)
void discGloss(uint16_t* px, int y0, int h);  // static highlight, drawn after rotation
void discFade(uint16_t* px, int n, int k);    // k: 0..255 brightness

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b);  // big-endian RGB565 (sprite byte order)
