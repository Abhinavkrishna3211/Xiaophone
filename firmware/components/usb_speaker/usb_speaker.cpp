#include "usb_speaker.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_device_uac.h"
#include "lwip/sockets.h"
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
static char logRing[8192];
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
  for (int i = 0; i < n; i++) logRing[logHead++ % sizeof(logRing)] = line[i];
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
  listen(srv, 1);
  for (;;) {
    int c = accept(srv, nullptr, nullptr);
    if (c < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    static char copy[sizeof(logRing)];
    portENTER_CRITICAL(&logMux);
    size_t h = logHead, len = h < sizeof(logRing) ? h : sizeof(logRing);
    for (size_t i = 0; i < len; i++) copy[i] = logRing[(h - len + i) % sizeof(logRing)];
    portEXIT_CRITICAL(&logMux);
    send(c, copy, len, 0);
    logClient = c;
    char b;
    while (recv(c, &b, 1, 0) > 0) {}  // until the viewer disconnects
    logClient = -1;
    close(c);
  }
}

static void take() {
  if (hooks.onActive) hooks.onActive(true);  // radio stops
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 256;
  cc.auto_clear = true;  // silence on underrun
  esp_err_t e = i2s_new_channel(&cc, &tx, nullptr);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "no I2S channel: %s, internal heap %u", esp_err_to_name(e),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    tx = nullptr;
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
  e = i2s_channel_init_std_mode(tx, &sc);
  if (e == ESP_OK) e = i2s_channel_enable(tx);
  if (e != ESP_OK) ESP_LOGE(TAG, "I2S start failed: %s", esp_err_to_name(e));
  ESP_LOGI(TAG, "took the speaker (I2S %d Hz)", (int)RATE);
}

static void release() {
  i2s_channel_disable(tx);
  i2s_del_channel(tx);
  tx = nullptr;
  if (hooks.onActive) hooks.onActive(false);  // radio re-routes the pins and resumes
  ESP_LOGI(TAG, "released the speaker");
}

static esp_err_t onAudio(uint8_t* data, size_t bytes, void*) {
  static int16_t s[1024];  // own copy: the USB interrupt refills its buffer every millisecond
  size_t n = (bytes > sizeof(s) ? sizeof(s) : bytes) / 2;
  memcpy(s, data, n * 2);
  static uint32_t calls;
  if (calls++ % 5000 == 0) ESP_LOGI(TAG, "audio in: %u calls, %u bytes, first sample %d, gain %d%s", (unsigned)calls, (unsigned)bytes, s[0], (int)gain, muted ? " MUTED" : "");
  bool sound = false;
  for (size_t i = 0; i < n && !sound; i++) sound = s[i] > 8 || s[i] < -8;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (sound) lastSound = esp_timer_get_time();
  static int64_t lastTry;
  int64_t now = esp_timer_get_time();
  if (!tx && sound && now - lastTry > 2000000 && (!hooks.canTake || hooks.canTake())) { lastTry = now; take(); }
  if (tx) {
    if (hooks.mix) hooks.mix(s, n / 2, RATE);
    int32_t g = muted ? 0 : gain;
    for (size_t i = 0; i < n; i++) s[i] = (int16_t)((s[i] * g) >> 16);
    size_t w = 0;
    i2s_channel_write(tx, s, n * 2, &w, 100);
  }
  xSemaphoreGive(lock);
  return ESP_OK;
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

static void watchTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    xSemaphoreTake(lock, portMAX_DELAY);
    if (tx && esp_timer_get_time() - lastSound > HOLD_US) release();
    xSemaphoreGive(lock);
  }
}

void begin(int bclk, int lrclk, int dout, float mg, Hooks h) {
  pinB = bclk, pinL = lrclk, pinD = dout, maxGain = mg, hooks = h;
  gain = (int32_t)(maxGain * 65536);  // host starts at 0 dB until it sends a volume
  lock = xSemaphoreCreateMutex();
  esp_log_level_set(TAG, ESP_LOG_INFO);
  prevLog = esp_log_set_vprintf(netLog);
  xTaskCreatePinnedToCore(logServer, "netlog", 3072, nullptr, 1, nullptr, 0);
  uac_device_config_t cfg = {};
  cfg.output_cb = onAudio;
  cfg.set_volume_cb = onVolume;
  cfg.set_mute_cb = onMute;
  if (uac_device_init(&cfg) != ESP_OK) { ESP_LOGE(TAG, "USB audio init failed"); return; }
  xTaskCreatePinnedToCore(watchTask, "usbspk", 3072, nullptr, 2, nullptr, 0);
  ESP_LOGI(TAG, "USB speaker ready");
}

bool active() { return tx != nullptr; }
bool playing() { return tx && esp_timer_get_time() - lastSound < 1000000; }

}  // namespace usbspk
