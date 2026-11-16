// M0 bring-up: display colours + circle, 440 Hz test tone, PSRAM report.
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <ESP_I2S.h>

// GC9A01 pins
#define PIN_SCLK 7  // D8
#define PIN_MOSI 9  // D10
#define PIN_CS   2  // D1
#define PIN_DC   3  // D2
#define PIN_RST  1  // D0

// MAX98357 pins
#define PIN_BCLK 5  // D4
#define PIN_LRC  4  // D3
#define PIN_DIN  6  // D5

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 panel;
  lgfx::Bus_SPI bus;
public:
  LGFX() {
    auto b = bus.config();
    b.spi_host = SPI2_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;  // try 80 MHz later if stable
    b.pin_sclk = PIN_SCLK;
    b.pin_mosi = PIN_MOSI;
    b.pin_miso = -1;
    b.pin_dc = PIN_DC;
    b.dma_channel = SPI_DMA_CH_AUTO;
    bus.config(b);
    panel.setBus(&bus);

    auto p = panel.config();
    p.pin_cs = PIN_CS;
    p.pin_rst = PIN_RST;
    p.pin_busy = -1;
    p.panel_width = 240;
    p.panel_height = 240;
    p.invert = true;
    p.readable = false;
    panel.config(p);
    setPanel(&panel);
  }
};

LGFX tft;
I2SClass i2s;

void playTone(int freq, int ms) {
  const int rate = 44100;
  const int16_t amp = 6000;  // ~18% full scale, gentle on a tiny speaker
  int16_t buf[256 * 2];      // stereo frames
  int total = rate * ms / 1000;
  for (int n = 0; n < total; n += 256) {
    for (int i = 0; i < 256; i++) {
      int16_t s = amp * sinf(2 * PI * freq * (n + i) / rate);
      buf[2 * i] = buf[2 * i + 1] = s;
    }
    i2s.write((uint8_t *)buf, sizeof(buf));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // give USB CDC time to attach
  Serial.println("\n=== Xiaophone M0 ===");
  Serial.printf("PSRAM found: %s, size %u bytes, free %u\n",
                psramFound() ? "yes" : "NO", ESP.getPsramSize(), ESP.getFreePsram());
  Serial.printf("Flash size: %u bytes\n", ESP.getFlashChipSize());

  tft.init();
  for (uint32_t c : {TFT_RED, TFT_GREEN, TFT_BLUE}) {
    tft.fillScreen(c);
    delay(600);
  }
  tft.fillScreen(TFT_BLACK);
  tft.drawCircle(120, 120, 110, TFT_WHITE);
  tft.drawCircle(120, 120, 109, TFT_WHITE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(middle_center);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.drawString("Xiaophone", 120, 120);
  Serial.println("Display: done");

  i2s.setPins(PIN_BCLK, PIN_LRC, PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("I2S: begin FAILED");
    return;
  }
  Serial.println("I2S: playing 440 Hz for 1 s");
  playTone(440, 1000);
  i2s.end();
  Serial.println("I2S: done. M0 complete.");
}

void loop() {}
