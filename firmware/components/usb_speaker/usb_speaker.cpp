#include "usb_speaker.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_device_uac.h"

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

static void take() {
  if (hooks.onActive) hooks.onActive(true);  // radio stops
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 256;
  cc.auto_clear = true;  // silence on underrun
  if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK) { tx = nullptr; return; }
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
  i2s_channel_init_std_mode(tx, &sc);
  i2s_channel_enable(tx);
  ESP_LOGI(TAG, "took the speaker");
}

static void release() {
  i2s_channel_disable(tx);
  i2s_del_channel(tx);
  tx = nullptr;
  if (hooks.onActive) hooks.onActive(false);  // radio re-routes the pins and resumes
  ESP_LOGI(TAG, "released the speaker");
}

static esp_err_t onAudio(uint8_t* data, size_t bytes, void*) {
  int16_t* s = (int16_t*)data;
  size_t n = bytes / 2;
  bool sound = false;
  for (size_t i = 0; i < n && !sound; i++) sound = s[i] > 8 || s[i] < -8;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (sound) lastSound = esp_timer_get_time();
  if (!tx && sound && (!hooks.canTake || hooks.canTake())) take();
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

static void onVolume(uint32_t v, void*) {  // host volume 0..100, squared for a natural curve
  float f = (v > 100 ? 100 : v) / 100.0f;
  gain = (int32_t)(f * f * maxGain * 65536);
  ESP_LOGI(TAG, "volume %u", (unsigned)v);
}

static void onMute(uint32_t m, void*) { muted = m; }

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
  gain = (int32_t)(0.25f * maxGain * 65536);
  lock = xSemaphoreCreateMutex();
  uac_device_config_t cfg = {};
  cfg.output_cb = onAudio;
  cfg.set_volume_cb = onVolume;
  cfg.set_mute_cb = onMute;
  if (uac_device_init(&cfg) != ESP_OK) { ESP_LOGE(TAG, "USB audio init failed"); return; }
  xTaskCreatePinnedToCore(watchTask, "usbspk", 3072, nullptr, 2, nullptr, 0);
}

bool active() { return tx != nullptr; }
bool playing() { return tx && esp_timer_get_time() - lastSound < 1000000; }

}  // namespace usbspk
