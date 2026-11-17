#include <WiFi.h>
#include "net.h"
#include "config.h"

static uint32_t downSince = 0, lastKick = 0;

void netBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // power save causes audio dropouts
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
    if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      Serial.printf("[WIFI] connected, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      Serial.printf("[WIFI] disconnected (reason %d)\n", info.wifi_sta_disconnected.reason);
  });
  Serial.printf("[WIFI] connecting to \"%s\"\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

bool netUp() { return WiFi.status() == WL_CONNECTED; }

void netLoop() {
  uint32_t now = millis();
  if (netUp()) { downSince = 0; return; }
  if (!downSince) downSince = now;
  if (now - downSince > 10000 && now - lastKick > 10000) {
    lastKick = now;
    Serial.println("[WIFI] still down, retrying");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}
