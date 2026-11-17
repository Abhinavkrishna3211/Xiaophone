// Spotify Connect receiver. Plain types only, so Arduino code can include it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace spotify {

struct Track {
  std::string artist, title, imageUrl;
};

struct Hooks {
  std::function<void(const Track&)> onTrack;                 // new song started
  std::function<void(bool)> onActive;                        // true: about to take the speaker, false: released it
  std::function<void(int16_t*, size_t, uint32_t)> mix;       // stereo frames, sample rate: add effects in place
};

// Starts the receiver task. Wi-Fi must already be up and mDNS started.
void begin(const char* deviceName, int bclk, int lrclk, int dout, Hooks hooks);
bool active();   // owns the speaker
bool playing();  // owns the speaker and music is running (not paused)

}  // namespace spotify
