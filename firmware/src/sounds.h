#pragma once
bool soundsBegin();      // mount LittleFS, load crackle into PSRAM
bool soundsHaveChime();
void soundsCrackle();    // mix ~1 s of crackle over whatever is playing
