// Spotify cover for the USB speaker: a USB sound card gets no song info, so this asks
// Spotify's Web API what the account is playing and hands the cover to the record.
// Needs SPOTIFY_CLIENT_ID + SPOTIFY_REFRESH_TOKEN in secrets.h (tools/spotify_token.py).
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "config.h"
#include "cover.h"
#include "net.h"
#include "nowplaying.h"
#include "usb_speaker.h"

#ifdef SPOTIFY_REFRESH_TOKEN
static String refreshTok, accessTok, lastId;
static uint32_t accessAt, accessFor;

static void saveToken() {  // Spotify may hand out a new refresh token; the old one then stops working
  Preferences p;
  p.begin("nowplay", false);
  p.putString("seed", SPOTIFY_REFRESH_TOKEN);
  p.putString("rt", refreshTok);
  p.end();
}

static bool refresh() {
  WiFiClientSecure cli;
  cli.setInsecure();  // hobby device: no CA bundle
  HTTPClient http;
  if (!http.begin(cli, "https://accounts.spotify.com/api/token")) return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST("grant_type=refresh_token&client_id=" SPOTIFY_CLIENT_ID "&refresh_token=" + refreshTok);
  String body = http.getString();
  http.end();
  JsonDocument d;
  if (code != 200 || deserializeJson(d, body)) {
    Serial.printf("[NOWPLAY] token refresh failed: HTTP %d\n", code);
    log_e("token refresh failed: HTTP %d %s", code, body.c_str());
    return false;
  }
  accessTok = d["access_token"].as<String>();
  accessFor = (d["expires_in"] | 3600) * 1000UL;
  accessAt = millis();
  if (d["refresh_token"].is<const char*>() && refreshTok != d["refresh_token"].as<String>()) {
    refreshTok = d["refresh_token"].as<String>();
    saveToken();
  }
  return true;
}

static void poll() {
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  if (!http.begin(cli, "https://api.spotify.com/v1/me/player/currently-playing?market=from_token")) return;
  http.addHeader("Authorization", "Bearer " + accessTok);
  int code = http.GET();
  if (code == 401) accessTok = "";
  if (code != 200) { http.end(); return; }  // 204: nothing playing
  String body = http.getString();
  http.end();

  JsonDocument filter;
  filter["is_playing"] = true;
  filter["item"]["id"] = true;
  filter["item"]["name"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["album"]["images"] = true;
  JsonDocument d;
  if (deserializeJson(d, body, DeserializationOption::Filter(filter))) return;
  String id = d["item"]["id"] | "";
  if (!d["is_playing"].as<bool>() || id.isEmpty() || id == lastId) return;
  lastId = id;

  const char* url = "";
  int best = 1 << 30;
  for (JsonObject im : d["item"]["album"]["images"].as<JsonArray>()) {  // the one nearest 300 px
    int dist = abs((int)(im["width"] | 0) - 300);
    if (dist < best) { best = dist; url = im["url"] | ""; }
  }
  const char* artist = d["item"]["artists"][0]["name"] | "";
  const char* title = d["item"]["name"] | "";
  Serial.printf("[NOWPLAY] %s - %s\n", artist, title);
  coverRequest(artist, title, url);
}

static void task(void*) {
  Preferences p;
  p.begin("nowplay", true);
  bool same = p.getString("seed", "") == SPOTIFY_REFRESH_TOKEN;  // a new token in secrets.h wins
  refreshTok = same ? p.getString("rt", SPOTIFY_REFRESH_TOKEN) : String(SPOTIFY_REFRESH_TOKEN);
  p.end();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(4000));
    if (!usbspk::active() || !netUp()) { lastId = ""; continue; }
    if (accessTok.isEmpty() || millis() - accessAt > accessFor - 60000) {
      if (!refresh()) { vTaskDelay(pdMS_TO_TICKS(30000)); continue; }
    }
    poll();
  }
}

void nowPlayingBegin() {
  // TLS needs a deep stack; keep it in PSRAM, internal RAM is tight
  xTaskCreatePinnedToCoreWithCaps(task, "nowplay", 12288, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
}
#else
void nowPlayingBegin() {}
#endif
