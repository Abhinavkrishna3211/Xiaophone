// Spotify Connect receiver built on cspot. Adapted from spotifyesp's main.cpp / SpotifyPlayer
// (GPL-3). The phone finds the device over mDNS (zeroconf), sends its login to /spotify_info,
// and from then on the device streams and decodes the music itself.
#include "spotify_bridge.h"

#include <atomic>
#include <map>
#include <memory>
#include <string_view>
#include <variant>

#include <BellHTTPServer.h>
#include <BellTask.h>
#include <BellUtils.h>
#include <CSpotContext.h>
#include <CentralAudioBuffer.h>
#include <LoginBlob.h>
#include <Logger.h>
#include <MDNSService.h>
#include <SpircHandler.h>
#include <TrackPlayer.h>
#include <civetweb.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

namespace spotify {

static const char* TAG = "spotify";
static Hooks hooks;
static std::string devName;
static int pinB, pinL, pinD;
static std::atomic<bool> owning{false}, paused{true};

static constexpr uint32_t RELEASE_AFTER_PAUSE_MS = 5 * 60 * 1000;
// ponytail: fixed loudness cap so Spotify at 100% is about as loud as the radio; tune on hardware
static constexpr float MAX_GAIN = 0.6f;

static uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- saved login (NVS writes need an internal-RAM stack; the cspot tasks run on PSRAM) ----

static void onInternalStack(std::function<void()> fn) {
  struct Job { std::function<void()>* fn; SemaphoreHandle_t done; } job{&fn, xSemaphoreCreateBinary()};
  xTaskCreatePinnedToCore(
      [](void* p) {
        auto* j = (Job*)p;
        (*j->fn)();
        xSemaphoreGive(j->done);
        vTaskDelete(nullptr);
      },
      "sp_nvs", 4096, &job, 5, nullptr, 0);
  xSemaphoreTake(job.done, portMAX_DELAY);
  vSemaphoreDelete(job.done);
}

static bool loadLogin(std::string& user, std::vector<uint8_t>& auth) {
  bool ok = false;
  onInternalStack([&] {
    nvs_handle_t h;
    if (nvs_open("spotify", NVS_READONLY, &h) != ESP_OK) return;
    size_t ul = 0, al = 0;
    if (nvs_get_str(h, "user", nullptr, &ul) == ESP_OK && nvs_get_blob(h, "auth", nullptr, &al) == ESP_OK &&
        ul > 1 && al > 0 && al <= 1024) {
      user.resize(ul);
      auth.resize(al);
      ok = nvs_get_str(h, "user", user.data(), &ul) == ESP_OK && nvs_get_blob(h, "auth", auth.data(), &al) == ESP_OK;
      user.resize(ul - 1);
    }
    nvs_close(h);
  });
  return ok;
}

static void saveLogin(const std::string& user, const std::vector<uint8_t>& auth) {
  onInternalStack([&] {
    nvs_handle_t h;
    if (nvs_open("spotify", NVS_READWRITE, &h) != ESP_OK) return;
    if (user.empty()) nvs_erase_all(h);
    else {
      nvs_set_str(h, "user", user.c_str());
      nvs_set_blob(h, "auth", auth.data(), auth.size());
    }
    nvs_commit(h);
    nvs_close(h);
  });
}

// ---- speaker: our own I2S channel on the radio's pins, only while Spotify plays ----

class Speaker {
 public:
  void take() {
    if (tx) return;
    if (hooks.onActive) hooks.onActive(true);  // radio stops
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.dma_desc_num = 8;
    cc.dma_frame_num = 256;
    cc.auto_clear = true;  // silence on underrun
    if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK) { tx = nullptr; return; }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                     .bclk = (gpio_num_t)pinB,
                     .ws = (gpio_num_t)pinL,
                     .dout = (gpio_num_t)pinD,
                     .din = I2S_GPIO_UNUSED,
                     .invert_flags = {}},
    };
    i2s_channel_init_std_mode(tx, &sc);
    i2s_channel_enable(tx);
    owning = true;
    ESP_LOGI(TAG, "took the speaker");
  }

  void release() {
    if (!tx) return;
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    tx = nullptr;
    owning = false;
    if (hooks.onActive) hooks.onActive(false);  // radio re-routes the pins and resumes
    ESP_LOGI(TAG, "released the speaker");
  }

  void setVolume(int v) {  // Spotify 0..65535, squared for a natural curve
    float f = v / 65535.0f;
    gain = (int32_t)(f * f * MAX_GAIN * 65536);
  }

  void write(const uint8_t* data, size_t bytes) {
    int16_t buf[512];
    while (bytes >= 4 && tx) {
      size_t n = bytes < sizeof(buf) ? bytes & ~3u : sizeof(buf);
      memcpy(buf, data, n);
      if (hooks.mix) hooks.mix(buf, n / 4, 44100);
      for (size_t i = 0; i < n / 2; i++) buf[i] = (int16_t)((buf[i] * gain) >> 16);
      size_t w = 0;
      if (i2s_channel_write(tx, buf, n, &w, 1000) != ESP_OK) return;
      data += n;
      bytes -= n;
    }
  }

 private:
  i2s_chan_handle_t tx = nullptr;
  int32_t gain = (int32_t)(0.25f * MAX_GAIN * 65536);
};

// ---- player: PCM from cspot -> speaker, plus play/pause/track events ----

class Player : public bell::Task {
 public:
  explicit Player(std::shared_ptr<cspot::SpircHandler> h) : bell::Task("sp_out", 8 * 1024, 0, 0), handler(h) {
    buf = std::make_shared<bell::CentralAudioBuffer>(128);
    auto hash = std::hash<std::string_view>();
    handler->getTrackPlayer()->setDataCallback([this, hash](uint8_t* d, size_t n, std::string_view id) {
      return buf->writePCM(d, n, hash(id));
    });
    handler->setEventHandler([this](std::unique_ptr<cspot::SpircHandler::Event> e) { onEvent(*e); });
    startTask();
  }

 private:
  using Ev = cspot::SpircHandler::EventType;
  std::shared_ptr<cspot::SpircHandler> handler;
  std::shared_ptr<bell::CentralAudioBuffer> buf;
  Speaker spk;
  std::atomic<bool> pauseReq{false}, playlistEnd{false}, releaseReq{false};
  uint32_t pausedAt = 0;

  void onEvent(cspot::SpircHandler::Event& e) {
    switch (e.eventType) {
      case Ev::PLAY_PAUSE:
        if (std::get<bool>(e.data)) pauseReq = true;
        else { pauseReq = false; paused = false; }
        break;
      case Ev::FLUSH:
      case Ev::SEEK:
        buf->clearBuffer();
        break;
      case Ev::DISC:
        buf->clearBuffer();
        paused = true;
        releaseReq = true;
        break;
      case Ev::PLAYBACK_START:
        paused = true;
        playlistEnd = false;
        buf->clearBuffer();
        break;
      case Ev::DEPLETED:
        playlistEnd = true;
        break;
      case Ev::VOLUME:
        spk.setVolume(std::get<int>(e.data));
        break;
      case Ev::TRACK_INFO: {
        auto& t = std::get<cspot::TrackInfo>(e.data);
        ESP_LOGI(TAG, "track: %s - %s", t.artist.c_str(), t.name.c_str());
        if (hooks.onTrack) hooks.onTrack({t.artist, t.name, t.imageUrl});
        break;
      }
      default:
        break;
    }
  }

  void runTask() override {
    size_t lastHash = 0;
    bool wasPaused = false;
    for (;;) {
      if (releaseReq.exchange(false)) spk.release();
      if (paused && !wasPaused) pausedAt = nowMs();
      wasPaused = paused;
      if (paused) {
        if (owning && nowMs() - pausedAt > RELEASE_AFTER_PAUSE_MS) spk.release();
        BELL_SLEEP_MS(10);
        continue;
      }
      auto* chunk = buf->readChunk();
      if (pauseReq.exchange(false)) paused = true;
      if (!chunk || chunk->pcmSize == 0) {
        if (playlistEnd.exchange(false)) handler->notifyAudioEnded();
        BELL_SLEEP_MS(10);
        continue;
      }
      if (lastHash != chunk->trackHash) {
        lastHash = chunk->trackHash;
        handler->notifyAudioReachedPlayback();
      }
      spk.take();
      spk.write(chunk->pcmData, chunk->pcmSize);
    }
  }
};

// ---- receiver: zeroconf login, then the Spotify session loop ----

static void receiverTask(void*) {
  auto blob = std::make_shared<cspot::LoginBlob>(devName);
  std::atomic<bool> gotBlob{false};

  auto server = std::make_shared<bell::BellHTTPServer>(8080);
  server->registerGet("/spotify_info", [server, blob](mg_connection*) {
    return server->makeJsonResponse(blob->buildZeroconfInfo());
  });
  server->registerPost("/spotify_info", [server, blob, &gotBlob](mg_connection* conn) {
    nlohmann::json obj;
    obj["status"] = 101;
    obj["spotifyError"] = 0;
    obj["statusString"] = "ERROR-OK";
    auto* ri = mg_get_request_info(conn);
    if (ri->content_length > 0 && !gotBlob) {  // ponytail: first login wins until reboot
      std::string body(ri->content_length, '\0');
      size_t got = 0;
      while (got < body.size()) {
        int n = mg_read(conn, body.data() + got, body.size() - got);
        if (n <= 0) break;
        got += n;
      }
      body.resize(got);
      mg_header hd[16];
      int num = mg_split_form_urlencoded(body.data(), hd, 16);
      std::map<std::string, std::string> q;
      for (int i = 0; i < num; i++) q[hd[i].name] = hd[i].value;
      try {
        blob->loadZeroconfQuery(q);
        gotBlob = true;
      } catch (const std::exception& ex) {
        ESP_LOGE(TAG, "login failed: %s", ex.what());
        obj["status"] = 303;
        obj["statusString"] = "ERROR-INVALID-ARGUMENTS";
      }
    }
    return server->makeJsonResponse(obj.dump());
  });
  auto mdns = bell::MDNSService::registerService(devName, "_spotify-connect", "_tcp", "", 8080,
                                                 {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});

  std::string savedUser;
  std::vector<uint8_t> savedAuth;
  bool usingSaved = loadLogin(savedUser, savedAuth);
  if (usingSaved) {
    blob->username = savedUser;
    blob->authData = savedAuth;
    blob->authType = AuthenticationType_AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS;
    gotBlob = true;
    ESP_LOGI(TAG, "using saved login");
  } else {
    ESP_LOGI(TAG, "waiting for the Spotify app: pick \"%s\" in the device list", devName.c_str());
  }
  while (!gotBlob) BELL_SLEEP_MS(500);

  auto ctx = cspot::Context::createFromBlob(blob);
  ctx->config.audioFormat = AudioFormat_OGG_VORBIS_160;  // easier on Wi-Fi and CPU than 320
  bool connected = false;
  for (int i = 0; i < 5 && !connected; i++) {
    try {
      ctx->session->connectWithRandomAp();
      connected = true;
    } catch (const std::exception& ex) {
      ESP_LOGE(TAG, "AP connect failed: %s", ex.what());
      BELL_SLEEP_MS(2000);
    }
  }
  if (!connected) esp_restart();
  try {
    ctx->config.authData = ctx->session->authenticate(blob);
  } catch (const std::exception& ex) {
    ESP_LOGE(TAG, "authenticate failed: %s", ex.what());
    esp_restart();
  }
  if (ctx->config.authData.empty()) {
    ESP_LOGE(TAG, "login rejected%s", usingSaved ? ", forgetting it" : "");
    if (usingSaved) saveLogin("", {});
    esp_restart();
  }
  if (!usingSaved || ctx->config.authData != savedAuth) saveLogin(blob->username, ctx->config.authData);

  auto handler = std::make_shared<cspot::SpircHandler>(ctx);
  ctx->session->startTask();
  auto player = std::make_shared<Player>(handler);
  ESP_LOGI(TAG, "ready as \"%s\"", devName.c_str());
  for (;;) ctx->session->handlePacket();
}

void begin(const char* deviceName, int bclk, int lrclk, int dout, Hooks h) {
  devName = deviceName;
  pinB = bclk;
  pinL = lrclk;
  pinD = dout;
  hooks = std::move(h);
  bell::setDefaultLogger();
  xTaskCreatePinnedToCoreWithCaps(receiverTask, "spotify", 32 * 1024, nullptr, 5, nullptr, 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool active() { return owning; }
bool playing() { return owning && !paused; }

}  // namespace spotify
