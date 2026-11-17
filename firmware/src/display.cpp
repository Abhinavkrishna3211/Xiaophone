#include "display.h"
#include "config.h"

LGFX tft;

LGFX::LGFX() {
  auto b = bus.config();
  b.spi_host = SPI2_HOST;
  b.spi_mode = 0;
  b.freq_write = SPI_HZ;
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

void displayBegin() {
  tft.init();
  tft.fillScreen(TFT_BLACK);
  Serial.printf("[GFX] GC9A01 ready, SPI %d MHz\n", SPI_HZ / 1000000);
}
