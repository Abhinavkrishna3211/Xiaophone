#include <Arduino.h>
#include <LittleFS.h>
#include "Audio.h"
#include "audio_player.h"
#include "config.h"
#include "net.h"
#include "meta.h"
#include "cover.h"
#include "sounds.h"
#include "spotify_bridge.h"
#include "usb_speaker.h"

static Audio audio;
static volatile bool streaming = false;
static volatile uint8_t yieldMask = 0;  // bit per speaker borrower (Spotify, USB)
static const char* stations[] = {
  STATION_URL_1,
#ifdef STATION_URL_2
  STATION_URL_2,
#endif
#ifdef STATION_URL_3
  STATION_URL_3,
#endif
};
static const int NST = sizeof(stations) / sizeof(stations[0]);

bool audioStreaming() { return streaming || spotify::playing() || usbspk::playing(); }
bool audioPaused() { return (spotify::active() && !spotify::playing()) || (usbspk::active() && !usbspk::playing()); }
void audioYield(uint8_t who, bool y) { if (y) yieldMask |= who; else yieldMask &= ~who; }
uint32_t audioSampleRate() { return audio.getSampleRate(); }

static void onInfo(Audio::msg_t m) {
  switch (m.e) {
    case Audio::evt_streamtitle: {
      Serial.printf("[META] %s\n", m.msg);
      std::string artist, title;
      if (metaParse(m.msg, artist, title)) coverRequest(artist, title);
      else Serial.println("[META] (ignored: station ID / no artist)");
      break;
    }
    case Audio::evt_name:    Serial.printf("[AUDIO] station: %s\n", m.msg); break;
    case Audio::evt_bitrate: Serial.printf("[AUDIO] bitrate: %s\n", m.msg); break;
    case Audio::evt_eof:     Serial.printf("[AUDIO] end of stream: %s\n", m.msg); break;
    default: break;
  }
}

enum Mode { IDLE, CHIME, PLAYING };

static void audioTask(void*) {
  Mode mode = IDLE;
  bool chimed = false;
  int station = 0, fails = 0;
  uint32_t t = 0, lastTry = 0, emptySince = 0;
  bool yielded = false;

  for (;;) {
    if ((yieldMask != 0) != yielded) {
      yielded = yieldMask != 0;
      if (yielded) {  // Spotify or USB takes the speaker
        audio.stopSong();
        streaming = false;
        mode = IDLE;
        chimed = true;
        Serial.println("[AUDIO] radio paused");
      } else {  // speaker free again: route the I2S pins back to our channel and reconnect
        audio.setPinout(PIN_BCLK, PIN_LRC, PIN_DIN);
        lastTry = 0;
        Serial.println("[AUDIO] radio back");
      }
    }
    if (yielded) { vTaskDelay(20); continue; }
    audio.loop();
    uint32_t now = millis();

    if (!netUp()) {
      if (mode != IDLE) {
        audio.stopSong();
        mode = IDLE;
        Serial.println("[AUDIO] stopped: Wi-Fi down");
      }
      streaming = false;
      vTaskDelay(20);
      continue;
    }

    switch (mode) {
      case IDLE:
        if (!chimed) {  // wake-up chime, once per boot
          chimed = true;
          if (soundsHaveChime() && audio.connecttoFS(LittleFS, "/chime.wav")) { mode = CHIME; t = now; }
        } else if (!lastTry || now - lastTry > 3000) {
          lastTry = now;
          Serial.printf("[AUDIO] connecting station %d: %s\n", station + 1, stations[station]);
          if (audio.connecttohost(stations[station])) { mode = PLAYING; t = now; emptySince = 0; }
          else if (++fails >= 2) { station = (station + 1) % NST; fails = 0; }
        }
        break;

      case CHIME:
        if (!audio.isRunning() || now - t > 4000) { audio.stopSong(); mode = IDLE; lastTry = 0; }
        break;

      case PLAYING: {
        bool running = audio.isRunning();
        // stall guard: running but no data for 8 s
        if (running && audio.inBufferFilled() == 0) { if (!emptySince) emptySince = now; }
        else emptySince = 0;
        if (!running || (emptySince && now - emptySince > 8000)) {
          Serial.println("[AUDIO] stream lost, reconnecting");
          audio.stopSong();
          streaming = false;
          mode = IDLE;
          lastTry = now;
          if (now - t < 30000 && ++fails >= 2) { station = (station + 1) % NST; fails = 0; }  // failing fast: next station
          break;
        }
        streaming = true;
        if (now - t > 30000) fails = 0;
        break;
      }
    }
    vTaskDelay(1);
  }
}

void audioBegin() {
  Audio::audio_info_callback = onInfo;
  audio.setPinout(PIN_BCLK, PIN_LRC, PIN_DIN);
  audio.setVolume(VOLUME);
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 5, nullptr, 0);
  Serial.printf("[AUDIO] task started, %d stations, volume %d\n", NST, VOLUME);
}
