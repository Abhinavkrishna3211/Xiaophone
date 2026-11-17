#pragma once
#include "secrets.h"

#define DEMO_MODE 0          // 1 = no radio; cycle 5 known songs' covers every 20 s (for filming)
#define VOLUME    12         // 0..21
#define SPI_HZ    40000000   // display SPI clock; try 80000000 if the picture stays clean

// Pins (XIAO ESP32S3 Plus)
#define PIN_SCLK 7   // D8
#define PIN_MOSI 9   // D10
#define PIN_CS   2   // D1
#define PIN_DC   3   // D2
#define PIN_RST  1   // D0
#define PIN_BCLK 5   // D4
#define PIN_LRC  4   // D3
#define PIN_DIN  6   // D5
