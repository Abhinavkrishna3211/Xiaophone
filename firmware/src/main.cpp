// Xiaophone: internet radio + Spotify Connect + USB speaker, album cover spinning like a record.
#include <Arduino.h>
#include "config.h"
#include "display.h"
#include "disc.h"
#include "ui_states.h"
#include "sounds.h"
#include "net.h"
#include "audio_player.h"
#include "cover.h"
#include <ESPmDNS.h>
#include "spotify_bridge.h"
#include "usb_speaker.h"
#include "nowplaying.h"

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
  nowPlayingBegin();
#endif
  logMem("ready");
}

void loop() {
  static uint32_t lastLog = 0, lastFrames = 0;
  uint32_t now = millis();
  netLoop();

#if !DEMO_MODE
  static bool spotifyOn = false;
  if (!spotifyOn && netUp()) {  // Spotify Connect: shows up as "Xiaophone" in the Spotify app
    spotifyOn = true;
    MDNS.begin("xiaophone");
    spotify::Hooks h;
    h.onTrack = [](const spotify::Track& t) { coverRequest(t.artist, t.title, t.imageUrl); };
    h.onActive = [](bool y) { audioYield(YIELD_SPOTIFY, y); };
    h.mix = soundsMix;
    spotify::begin("Xiaophone", PIN_BCLK, PIN_LRC, PIN_DIN, h);
    Serial.println("[SPOTIFY] receiver started");
  }
  static bool usbOn = false;
  if (!usbOn && now > 6000) {  // USB sound card: the first seconds of boot log stay on the USB console
    usbOn = true;
    Serial.println("[USB] switching USB to speaker mode, console ends here");
    Serial.flush();
    usbspk::Hooks u;
    u.canTake = [] { return !spotify::active(); };
    u.onActive = [](bool y) { audioYield(YIELD_USB, y); };
    u.mix = soundsMix;
    usbspk::begin(PIN_BCLK, PIN_LRC, PIN_DIN, USB_MAX_GAIN, u);
  }
#endif

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
