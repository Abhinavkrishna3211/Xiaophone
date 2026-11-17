// USB speaker: a phone or computer sees the Xiaophone as a USB sound card
// (48 kHz, 16-bit stereo) and plays straight into the MAX98357.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace usbspk {

struct Hooks {
  std::function<bool()> canTake;                       // false: someone else owns the speaker
  std::function<void(bool)> onActive;                  // true: about to take the speaker, false: released it
  std::function<void(int16_t*, size_t, uint32_t)> mix; // stereo frames, sample rate: add effects in place
};

// Switches the USB port from the serial console to USB audio (logs over USB stop here).
void begin(int bclk, int lrclk, int dout, float maxGain, Hooks hooks);
bool active();   // owns the speaker
bool playing();  // owns the speaker and sound arrived in the last second

}  // namespace usbspk
