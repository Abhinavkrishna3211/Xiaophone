// AirPlay speaker: shows up as "Xiaophone" in the iPhone's AirPlay menu (and in Spotify's
// device list under AirPlay). Classic AirPlay (RAOP): ALAC audio over RTP, 44.1 kHz stereo.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace airplay {

struct Hooks {
  std::function<bool()> canTake;                       // false: someone else owns the speaker
  std::function<void(bool)> onActive;                  // true: about to take the speaker, false: released it
  std::function<void(int16_t*, size_t, uint32_t)> mix; // stereo frames, sample rate: add effects in place
};

// Call once Wi-Fi and mDNS are up.
void begin(const char* name, int bclk, int lrclk, int dout, float maxGain, Hooks hooks);
bool active();   // a sender is connected and we own the speaker
bool playing();  // owns the speaker and audio arrived in the last second

}  // namespace airplay
