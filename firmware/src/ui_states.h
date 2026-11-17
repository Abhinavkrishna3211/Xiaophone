#pragma once
#include <stdint.h>
void uiBegin(bool demo);   // starts the render task on core 1
uint32_t uiFrames();       // frames rendered so far (for fps logging)
const char* uiStateName();
