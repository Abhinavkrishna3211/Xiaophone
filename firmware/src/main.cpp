// Xiaophone: internet radio + album cover spinning like a record.
#include <Arduino.h>
#include "config.h"
#include "display.h"
#include "disc.h"
#include "ui_states.h"
#include "sounds.h"
#include "net.h"
#include "audio_player.h"
#include "cover.h"

#if DEMO_MODE
static const char* DEMO[][2] = {
  {"Fleetwood Mac", "Dreams"},
  {"Daft Punk", "Get Lucky"},
  {"Michael Jackson", "Billie Jean"},
  {"Queen", "Bohemian Rhapsody"},
  {"Arijit Singh", "Tum Hi Ho"},
};
#endif

static void logMem(const char* when) {
  Serial.printf("[MEM] %s: heap free %u (min %u), PSRAM free %u / %u\n", when, ESP.getFreeHeap(),
                ESP.getMinFreeHeap(), ESP.getFreePsram(), ESP.getPsramSize());
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== Xiaophone ===");
  logMem("boot");
  displayBegin();
  discBegin();
  uiBegin(DEMO_MODE);
  soundsBegin();
  netBegin();
  coverBegin();
#if DEMO_MODE
  Serial.println("[AUDIO] demo mode: radio off");
#else
  audioBegin();
#endif
  logMem("ready");
}

void loop() {
  static uint32_t lastLog = 0, lastFrames = 0;
  uint32_t now = millis();
  netLoop();

  if (now - lastLog >= 5000) {
    uint32_t f = uiFrames();
    Serial.printf("[GFX] %.1f fps, state %s\n", (f - lastFrames) * 1000.0f / (now - lastLog), uiStateName());
    lastFrames = f;
    lastLog = now;
    logMem("status");
  }

#if DEMO_MODE
  static uint32_t lastDemo = 0;
  static int demoIdx = 0;
  if (netUp() && (!lastDemo || now - lastDemo >= 20000)) {
    lastDemo = now;
    Serial.printf("[META] demo: %s - %s\n", DEMO[demoIdx][0], DEMO[demoIdx][1]);
    coverRequest(DEMO[demoIdx][0], DEMO[demoIdx][1]);
    demoIdx = (demoIdx + 1) % 5;
  }
#endif
  delay(20);
}
