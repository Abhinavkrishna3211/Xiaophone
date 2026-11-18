#include "usb_speaker.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_device_uac.h"
#include "tusb.h"
#include "lwip/sockets.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <cmath>
#include <cstdarg>
#include <cstring>

namespace usbspk {

static const char* TAG = "USBSPK";
static const uint32_t RATE = CONFIG_UAC_SAMPLE_RATE;
static const int64_t HOLD_US = 5 * 60 * 1000000LL;  // silence this long gives the speaker back to the radio

static Hooks hooks;
static int pinB, pinL, pinD;
static float maxGain;
static i2s_chan_handle_t tx;
static SemaphoreHandle_t lock;
static volatile int64_t lastSound;
static volatile int32_t gain, volGain;
static volatile bool muted;

// ---- logs over Wi-Fi: once USB is a speaker there is no serial console.
// `python tools/netlog.py xiaophone.local` (or telnet) on port 23 shows the backlog, then live lines.
static const size_t LOG_SZ = 8192;
static char* logRing;  // in PSRAM: internal RAM is needed by USB and Wi-Fi
static size_t logHead;  // total bytes ever written
static volatile int logClient = -1;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t prevLog;

static int netLog(const char* fmt, va_list ap) {
  char line[200];
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(line, sizeof(line), fmt, ap2);
  va_end(ap2);
  if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
  portENTER_CRITICAL(&logMux);
  for (int i = 0; i < n; i++) logRing[logHead++ % LOG_SZ] = line[i];
  portEXIT_CRITICAL(&logMux);
  int c = logClient;
  if (c >= 0 && n > 0) send(c, line, n, MSG_DONTWAIT);
  return prevLog ? prevLog(fmt, ap) : n;
}

static void logServer(void*) {
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(23);
  bind(srv, (sockaddr*)&a, sizeof(a));
  listen(srv, 2);
  fcntl(srv, F_SETFL, O_NONBLOCK);
  char* copy = (char*)heap_caps_malloc(LOG_SZ, MALLOC_CAP_SPIRAM);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    int c = logClient;
    char b;
    if (c >= 0) {  // viewer gone (or its connection died): drop it
      int r = recv(c, &b, 1, MSG_DONTWAIT);
      if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { logClient = -1; close(c); }
    }
    int n = accept(srv, nullptr, nullptr);
    if (n < 0) continue;
    if (!copy) { close(n); continue; }
    c = logClient;  // a new viewer replaces a stale one
    logClient = -1;
    if (c >= 0) close(c);
    portENTER_CRITICAL(&logMux);
    size_t h = logHead, len = h < LOG_SZ ? h : LOG_SZ;
    for (size_t i = 0; i < len; i++) copy[i] = logRing[(h - len + i) % LOG_SZ];
    portEXIT_CRITICAL(&logMux);
    timeval tv = {2, 0};
    setsockopt(n, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    send(n, copy, len, 0);
    logClient = n;
  }
}

static void take() {
  if (hooks.onActive) hooks.onActive(true);  // radio stops
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 256;
  cc.auto_clear = true;  // silence on underrun
  i2s_chan_handle_t t;
  esp_err_t e = i2s_new_channel(&cc, &t, nullptr);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "no I2S channel: %s, internal heap %u", esp_err_to_name(e),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (hooks.onActive) hooks.onActive(false);  // give the radio back instead of leaving it muted
    return;
  }
  i2s_std_config_t sc = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                   .bclk = (gpio_num_t)pinB,
                   .ws = (gpio_num_t)pinL,
                   .dout = (gpio_num_t)pinD,
                   .din = I2S_GPIO_UNUSED,
                   .invert_flags = {}},
  };
  e = i2s_channel_init_std_mode(t, &sc);
  if (e == ESP_OK) e = i2s_channel_enable(t);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "I2S start failed: %s", esp_err_to_name(e));
    i2s_del_channel(t);
    if (hooks.onActive) hooks.onActive(false);
    return;
  }
  tx = t;  // only now: the feed task starts writing as soon as tx is set
  ESP_LOGI(TAG, "took the speaker (I2S %d Hz)", (int)RATE);
}

static void release() {  // feed task only, so nothing is writing to tx meanwhile
  i2s_chan_handle_t t = tx;
  tx = nullptr;
  i2s_channel_disable(t);
  i2s_del_channel(t);
  if (hooks.onActive) hooks.onActive(false);  // radio re-routes the pins and resumes
  ESP_LOGI(TAG, "released the speaker");
}

// ---- elastic buffer: USB packets come on the host's clock, the speaker runs on ours.
// They never match exactly (and packets arrive in bursts), so keep ~50 ms queued and drop or
// repeat one frame now and then to hold that level. Without it the speaker runs dry every second or so.
static const uint32_t RING = 8192;                    // stereo frames (170 ms), power of two
static const uint32_t TARGET = RATE / 20, LOW = TARGET / 2, HIGH = TARGET * 3 / 2;
static const uint32_t CHUNK = RATE / 200;             // 5 ms per speaker write
static uint32_t* ring;                                // L/R packed, in PSRAM
static std::atomic<uint32_t> wr{0}, rd{0};            // frames ever written / read
static volatile uint32_t overruns, underruns, nudges;

static esp_err_t onAudio(uint8_t* data, size_t bytes, void*) {
  const uint32_t* f = (const uint32_t*)data;
  size_t n = bytes / 4;
  static uint32_t calls;
  if (calls++ % 5000 == 0)
    ESP_LOGI(TAG, "audio in: %u calls, %u bytes, queued %u, underruns %u, overruns %u, nudges %u, gain %d%s",
             (unsigned)calls, (unsigned)bytes, (unsigned)(wr - rd), (unsigned)underruns, (unsigned)overruns,
             (unsigned)nudges, (int)gain, muted ? " MUTED" : "");
  bool sound = false;
  const int16_t* s = (const int16_t*)data;
  for (size_t i = 0; i < n * 2 && !sound; i++) sound = s[i] > 8 || s[i] < -8;
  if (sound) lastSound = esp_timer_get_time();
  if (!tx && sound) {
    static int64_t lastTry;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!tx && now - lastTry > 2000000 && (!hooks.canTake || hooks.canTake())) { lastTry = now; take(); }
    xSemaphoreGive(lock);
  }
  if (!tx) return ESP_OK;
  uint32_t w = wr.load(std::memory_order_relaxed);
  if (w - rd.load(std::memory_order_acquire) + n > RING) { overruns = overruns + 1; return ESP_OK; }
  for (size_t i = 0; i < n; i++) ring[(w + i) & (RING - 1)] = f[i];
  wr.store(w + n, std::memory_order_release);
  return ESP_OK;
}

static void feedTask(void*) {
  static int16_t out[CHUNK * 2];
  bool started = false;
  for (;;) {
    if (tx && esp_timer_get_time() - lastSound > HOLD_US) {  // long silence: radio gets the speaker back
      xSemaphoreTake(lock, portMAX_DELAY);
      release();
      xSemaphoreGive(lock);
    }
    if (!tx) {
      started = false;
      rd.store(wr.load(std::memory_order_acquire), std::memory_order_release);  // start fresh next time
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    uint32_t r = rd.load(std::memory_order_relaxed), have = wr.load(std::memory_order_acquire) - r;
    if (!started && have >= TARGET) started = true;
    if (started && have < CHUNK + 1) { started = false; underruns = underruns + 1; }
    if (started) {
      // take one frame more or less than we play to steer back toward TARGET
      uint32_t use = have > HIGH ? CHUNK + 1 : have < LOW ? CHUNK - 1 : CHUNK;
      if (use != CHUNK) nudges = nudges + 1;
      for (uint32_t i = 0; i < CHUNK; i++) {
        uint32_t v = ring[(r + i * use / CHUNK) & (RING - 1)];
        out[i * 2] = (int16_t)(v & 0xffff);
        out[i * 2 + 1] = (int16_t)(v >> 16);
      }
      rd.store(r + use, std::memory_order_release);
      if (hooks.mix) hooks.mix(out, CHUNK, RATE);
      int32_t g = muted ? 0 : gain;
      for (uint32_t i = 0; i < CHUNK * 2; i++) out[i] = (int16_t)((out[i] * g) >> 16);
    } else {
      memset(out, 0, sizeof(out));  // filling up: play silence meanwhile
    }
    size_t done = 0;
    if (i2s_channel_write(tx, out, sizeof(out), &done, 100) != ESP_OK)  // blocks: paced by the speaker clock
      vTaskDelay(1);
  }
}

// ---- album covers over the USB serial port (the sound card gets no song info).
// Packet: "XPC1", u16 meta length, u32 JPEG length (little endian), meta = artist TAB title (UTF-8),
// then a baseline JPEG. Replies "OK" or "ERR ..." lines. tools/usb_cover.py is the sender.
static const size_t COVER_MAX = 200 * 1024;
static uint8_t* coverBuf;  // in PSRAM
static volatile int64_t lastCover = -1;

static size_t cdcRead(uint8_t* dst, size_t n, int timeoutMs) {
  size_t got = 0;
  int64_t end = esp_timer_get_time() + timeoutMs * 1000LL;
  while (got < n) {
    uint32_t a = tud_cdc_available();
    if (a) got += tud_cdc_read(dst + got, std::min<uint32_t>(a, n - got));
    else if (esp_timer_get_time() > end) break;
    else vTaskDelay(pdMS_TO_TICKS(2));
  }
  return got;
}

static void reply(const char* s) {
  if (!tud_cdc_connected()) return;
  tud_cdc_write_str(s);
  tud_cdc_write_flush();
}

static void coverTask(void*) {
  static char meta[256];
  for (;;) {
    uint8_t b;
    int m = 0;
    while (m < 4) {  // resync on the magic, so a half-sent packet never wedges the link
      if (cdcRead(&b, 1, 1000) != 1) continue;
      m = b == "XPC1"[m] ? m + 1 : (b == 'X' ? 1 : 0);
    }
    uint8_t h[6];
    if (cdcRead(h, 6, 500) != 6) { reply("ERR header\n"); continue; }
    size_t ml = h[0] | h[1] << 8;
    size_t jl = h[2] | h[3] << 8 | h[4] << 16 | (size_t)h[5] << 24;
    if (ml >= sizeof(meta) || jl == 0 || jl > COVER_MAX) { reply("ERR size\n"); continue; }
    if (cdcRead((uint8_t*)meta, ml, 1000) != ml || cdcRead(coverBuf, jl, 5000) != jl) { reply("ERR short\n"); continue; }
    meta[ml] = 0;
    char* title = strchr(meta, '\t');
    if (title) *title++ = 0;
    ESP_LOGI(TAG, "cover from USB: %u bytes, %s - %s", (unsigned)jl, meta, title ? title : "");
    lastCover = esp_timer_get_time();
    if (hooks.onCover) hooks.onCover(coverBuf, jl, meta, title ? title : "");
    reply("OK\n");
  }
}

static void onVolume(uint32_t v, void*) {  // host volume 0..100 = -50..0 dB
  float db = (v > 100 ? 100 : (int)v) / 2.0f - 50.0f;
  gain = v == 0 ? 0 : (int32_t)(powf(10.0f, db / 20.0f) * maxGain * 65536);
  ESP_LOGI(TAG, "volume %u (%.0f dB), gain %d", (unsigned)v, db, (int)gain);
}

static void onMute(uint32_t m, void*) {
  muted = m;
  ESP_LOGI(TAG, "mute %u", (unsigned)m);
}

void begin(int bclk, int lrclk, int dout, float mg, Hooks h) {
  pinB = bclk, pinL = lrclk, pinD = dout, maxGain = mg, hooks = h;
  gain = (int32_t)(maxGain * 65536);  // host starts at 0 dB until it sends a volume
  lock = xSemaphoreCreateMutex();
  esp_log_level_set(TAG, ESP_LOG_INFO);
  logRing = (char*)heap_caps_calloc(1, LOG_SZ, MALLOC_CAP_SPIRAM);
  if (logRing) prevLog = esp_log_set_vprintf(netLog);
  if (logRing) xTaskCreatePinnedToCore(logServer, "netlog", 3072, nullptr, 1, nullptr, 0);
  uac_device_config_t cfg = {};
  cfg.output_cb = onAudio;
  cfg.set_volume_cb = onVolume;
  cfg.set_mute_cb = onMute;
  ring = (uint32_t*)heap_caps_malloc(RING * 4, MALLOC_CAP_SPIRAM);
  // stack in PSRAM: internal RAM is needed by USB and Wi-Fi (this task never touches flash)
  if (!ring || xTaskCreatePinnedToCoreWithCaps(feedTask, "usbfeed", 4096, nullptr, 6, nullptr, 1,
                                               MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "no memory for the audio buffer");
    return;
  }
  if (uac_device_init(&cfg) != ESP_OK) { ESP_LOGE(TAG, "USB audio init failed"); return; }
  coverBuf = (uint8_t*)heap_caps_malloc(COVER_MAX, MALLOC_CAP_SPIRAM);
  if (!coverBuf || xTaskCreatePinnedToCoreWithCaps(coverTask, "usbcover", 4096, nullptr, 1, nullptr, 0,
                                                   MALLOC_CAP_SPIRAM) != pdPASS)
    ESP_LOGE(TAG, "no memory for USB covers");
  ESP_LOGI(TAG, "USB speaker ready");
}

bool active() { return tx != nullptr; }
bool playing() { return tx && esp_timer_get_time() - lastSound < 1000000; }
bool hostCovers() { return lastCover >= 0 && esp_timer_get_time() - lastCover < 10 * 60 * 1000000LL; }

}  // namespace usbspk
