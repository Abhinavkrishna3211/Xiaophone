#pragma once
#include <stdint.h>
void audioBegin();           // starts the audio task on core 0
bool audioStreaming();       // radio stream is playing
uint32_t audioSampleRate();
