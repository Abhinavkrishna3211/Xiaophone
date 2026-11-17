#pragma once
#include <stdint.h>
void audioBegin();           // starts the audio task on core 0
bool audioStreaming();       // radio or Spotify is playing
bool audioPaused();          // Spotify holds the speaker but is paused
void audioYield(bool y);     // true: hand the speaker to Spotify, false: take it back
uint32_t audioSampleRate();
