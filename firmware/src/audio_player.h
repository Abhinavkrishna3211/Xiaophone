#pragma once
#include <stdint.h>
void audioBegin();           // starts the audio task on core 0
bool audioStreaming();       // radio, Spotify or USB is playing
bool audioPaused();          // Spotify or USB holds the speaker but is paused
enum { YIELD_SPOTIFY = 1, YIELD_USB = 2, YIELD_AIRPLAY = 4 };
void audioYield(uint8_t who, bool y);  // true: lend the speaker, false: give it back
uint32_t audioSampleRate();
