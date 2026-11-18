#include <Arduino.h>
#include <LittleFS.h>
#include "esp_log.h"
#include "Audio.h"
#include "audio_player.h"
#include "config.h"
#include "net.h"
#include "meta.h"
#include "cover.h"
#include "sounds.h"
#include "spotify_bridge.h"
#include "usb_speaker.h"
#include "airplay.h"

static Audio* audio;  // deleted while another source owns the speaker: frees its ~28 KB of DMA RAM
static volatile uint32_t rate = 44100;
static volatile bool streaming = false;
static volatile uint8_t yieldMask = 0;  // bit per speaker borrower (Spotify, USB, AirPlay)
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

bool audioStreaming() { return streaming || spotify::playing() || usbspk::playing() || airplay::playing(); }
bool audioPaused() { return (spotify::active() && !spotify::playing()) || (usbspk::active() && !usbspk::playing()) ||
                          (airplay::active() && !airplay::playing()); }
void audioYield(uint8_t who, bool y) {
  if (y) yieldMask |= who; else yieldMask &= ~who;
  for (int i = 0; y && audio && i < 100; i++) vTaskDelay(10);  // let the radio free the I2S pins before they're taken
}
uint32_t audioSampleRate() { return rate; }

static void onInfo(Audio::msg_t m) {
  switch (m.e) {
    case Audio::evt_streamtitle: {
      Serial.printf("[META] %s\n", m.msg);
      std::string artist, title;
      if (yieldMask || usbspk::hostCovers()) break;  // another source owns the record: a late radio title mustn't replace its cover
      if (metaParse(m.msg, artist, title)) coverRequest(artist, title);
      else Serial.println("[META] (ignored: station ID / no artist)");
      break;
    }
    case Audio::evt_name:    ESP_LOGW("AUDIO", "station: %s", m.msg); break;
    case Audio::evt_bitrate: ESP_LOGW("AUDIO", "bitrate: %s", m.msg); break;
    case Audio::evt_eof:     ESP_LOGW("AUDIO", "end of stream: %s", m.msg); break;
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
        delete audio;  // stops the song, releases its I2S channel and task
        audio = nullptr;
        streaming = false;
        mode = IDLE;
        chimed = true;
        ESP_LOGW("AUDIO", "radio paused");
      } else {  // speaker free again: route the I2S pins back to our channel and reconnect
        audio = new Audio();
        audio->setPinout(PIN_BCLK, PIN_LRC, PIN_DIN);
        audio->setVolume(VOLUME);
        lastTry = 0;
        ESP_LOGW("AUDIO", "radio back");
      }
    }
    if (yielded) { vTaskDelay(20); continue; }
    audio->loop();
    rate = audio->getSampleRate();
    uint32_t now = millis();

    if (!netUp()) {
      if (mode != IDLE) {
        audio->stopSong();
        mode = IDLE;
        ESP_LOGW("AUDIO", "stopped: Wi-Fi down");
      }
      streaming = false;
      vTaskDelay(20);
      continue;
    }

    switch (mode) {
      case IDLE:
        if (!chimed) {  // wake-up chime, once per boot
          chimed = true;
          if (soundsHaveChime() && audio->connecttoFS(LittleFS, "/chime.wav")) { mode = CHIME; t = now; }
        } else if (!lastTry || now - lastTry > 3000) {
          lastTry = now;
          ESP_LOGW("AUDIO", "connecting station %d: %s", station + 1, stations[station]);
          if (audio->connecttohost(stations[station])) { mode = PLAYING; t = now; emptySince = 0; }
          else if (++fails >= 2) { station = (station + 1) % NST; fails = 0; }
        }
        break;

      case CHIME:
        if (!audio->isRunning() || now - t > 4000) { audio->stopSong(); mode = IDLE; lastTry = 0; }
        break;

      case PLAYING: {
        bool running = audio->isRunning();
        // stall guard: running but no data for 8 s
        if (running && audio->inBufferFilled() == 0) { if (!emptySince) emptySince = now; }
        else emptySince = 0;
        if (!running || (emptySince && now - emptySince > 8000)) {
          ESP_LOGW("AUDIO", "stream lost, reconnecting");
          audio->stopSong();
          streaming = false;
          mode = IDLE;
          lastTry = now;
          if (now - t < 30000 && ++fails >= 2) { station = (station + 1) % NST; fails = 0; }  // failing fast: next station
          break;
        }
        streaming = true;
        if (now - t > 30000) fails = 0;
        static uint32_t lastLog;
        if (now - lastLog > 10000) {
          lastLog = now;
          ESP_LOGI("AUDIO", "radio buffer %u bytes, %u Hz", (unsigned)audio->inBufferFilled(), (unsigned)rate);
        }
        break;
      }
    }
    vTaskDelay(1);
  }
}

void audioBegin() {
  Audio::audio_info_callback = onInfo;
  audio = new Audio();
  audio->setPinout(PIN_BCLK, PIN_LRC, PIN_DIN);
  audio->setVolume(VOLUME);
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 5, nullptr, 0);
  Serial.printf("[AUDIO] task started, %d stations, volume %d\n", NST, VOLUME);
}
