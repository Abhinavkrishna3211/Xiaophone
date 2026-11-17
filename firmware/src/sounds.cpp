#include <Arduino.h>
#include <LittleFS.h>
#include "sounds.h"
#include "audio_player.h"

static int16_t* crack = nullptr;
static uint32_t crackLen = 0, crackRate = 22050;
static volatile bool crackOn = false;
static uint32_t crackPos = 0;  // 16.16 fixed point
static bool chimeOk = false;

// Minimal WAV reader: finds "fmt " and "data" chunks of a 16-bit mono file.
static bool loadWav(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint8_t h[12];
  f.read(h, 12);
  while (f.available() >= 8) {
    char id[4]; uint32_t sz;
    f.read((uint8_t*)id, 4); f.read((uint8_t*)&sz, 4);
    if (!memcmp(id, "fmt ", 4)) {
      uint8_t fmt[16]; f.read(fmt, 16); f.seek(sz - 16, SeekCur);
      crackRate = fmt[4] | fmt[5] << 8 | fmt[6] << 16;
    } else if (!memcmp(id, "data", 4)) {
      crack = (int16_t*)ps_malloc(sz);
      if (!crack) return false;
      crackLen = f.read((uint8_t*)crack, sz) / 2;
      return true;
    } else f.seek(sz, SeekCur);
  }
  return false;
}

bool soundsBegin() {
  if (!LittleFS.begin(false)) {
    Serial.println("[AUDIO] LittleFS mount failed (run: pio run -t uploadfs)");
    return false;
  }
  chimeOk = LittleFS.exists("/chime.wav");
  bool ok = loadWav("/crackle.wav");
  Serial.printf("[AUDIO] sounds: chime %s, crackle %s (%u samples @ %u Hz)\n",
                chimeOk ? "ok" : "MISSING", ok ? "ok" : "MISSING", crackLen, crackRate);
  return ok;
}

bool soundsHaveChime() { return chimeOk; }

void soundsCrackle() {
  if (!crack) return;
  crackPos = 0;
  crackOn = true;
}

// Called by ESP32-audioI2S for every output block (stereo int16 frames). Must set *cont,
// otherwise the library drops the block.
void audio_process_i2s(int16_t* buf, int32_t n, bool* cont) {
  *cont = true;
  if (!crackOn) return;
  uint32_t sr = audioSampleRate();
  if (!sr) return;
  uint32_t step = ((uint64_t)crackRate << 16) / sr;
  for (int32_t i = 0; i < n; i++) {
    uint32_t idx = crackPos >> 16;
    if (idx >= crackLen) { crackOn = false; return; }
    int32_t c = crack[idx] / 2;  // half level: quiet under the music
    for (int ch = 0; ch < 2; ch++) {
      int32_t v = buf[2 * i + ch] + c;
      buf[2 * i + ch] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
    }
    crackPos += step;
  }
}
