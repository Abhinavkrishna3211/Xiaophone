#pragma once
#include <stddef.h>
#include <stdint.h>
bool soundsBegin();      // mount LittleFS, load crackle into PSRAM
bool soundsHaveChime();
void soundsCrackle();    // mix ~1 s of crackle over whatever is playing
void soundsMix(int16_t* buf, size_t frames, uint32_t rate);  // adds the crackle to stereo PCM
