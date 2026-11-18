#include "esp_log.h"
// Album cover lookup: MusicBrainz search -> scored release-groups -> Cover Art Archive
// 250 px thumbnail -> JPEGDEC -> 240x240 RGB565 -> disc back buffer.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <vector>
#include <algorithm>
#include "cover.h"
#include "meta.h"
#include "disc.h"
#include "net.h"

#define UA "Xiaophone/1.0 ( https://github.com/Abhinavkrishna3211/Xiaophone )"
#define JPG_CAP (200 * 1024)
#define TMP_MAX 320  // decoded image is scaled down until it fits TMP_MAX x TMP_MAX (640 px -> half)
#define CACHE_N 20

struct Req { char artist[96]; char title[128]; char url[128]; };
static QueueHandle_t q;

static uint8_t* jpg;     // downloaded JPEG
static uint16_t* tmp;    // decoded image (<= 300x300)
static uint16_t* cover;  // final 240x240
static int tmpW, tmpH;
static JPEGDEC jpeg;
static uint32_t lastMb = 0;

struct CacheEntry { std::string key; uint16_t* px = nullptr; bool hit = false; };
static CacheEntry cache[CACHE_N];
static int cacheNext = 0;

static uint8_t* usbJpg;  // latest JPEG handed over by coverFromJpeg
static size_t usbLen;
static SemaphoreHandle_t usbLock;

void coverRequest(const std::string& artist, const std::string& title, const std::string& imageUrl) {
  Req r{};
  strlcpy(r.artist, artist.c_str(), sizeof(r.artist));
  strlcpy(r.title, title.c_str(), sizeof(r.title));
  strlcpy(r.url, imageUrl.c_str(), sizeof(r.url));
  xQueueOverwrite(q, &r);
}

void coverFromJpeg(const uint8_t* d, size_t n, const std::string& artist, const std::string& title) {
  if (!usbJpg || n > JPG_CAP) return;
  xSemaphoreTake(usbLock, portMAX_DELAY);
  memcpy(usbJpg, d, n);
  usbLen = n;
  xSemaphoreGive(usbLock);
  coverRequest(artist, title, "usb:");
}

static String urlEncode(const std::string& s) {
  String o;
  char b[4];
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { snprintf(b, sizeof(b), "%%%02X", c); o += b; }
  }
  return o;
}

static std::string noQuotes(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '"'), s.end());
  return s;
}

// GET with redirects and 5 s timeouts. Caller reads the stream, then calls http.end().
static int httpOpen(HTTPClient& http, WiFiClientSecure& cli, const String& url) {
  cli.setInsecure();     // hobby device: no CA bundle
  http.useHTTP10(true);  // no chunked encoding, so the stream is plain JSON/JPEG
  http.setTimeout(5000);
  http.setConnectTimeout(5000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(UA);
  if (!http.begin(cli, url)) return -1;
  return http.GET();
}

static bool getJson(const String& url, JsonDocument& doc, JsonDocument& filter) {
  WiFiClientSecure cli;
  HTTPClient http;
  int code = httpOpen(http, cli, url);
  if (code != 200) {
    ESP_LOGI("COVER", "HTTP %d for %s", code, url.c_str());
    http.end();
    return false;
  }
  DeserializationError e = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (e) ESP_LOGI("COVER", "JSON error: %s", e.c_str());
  return !e;
}

static int download(const String& url) {
  WiFiClientSecure cli;
  HTTPClient http;
  int code = httpOpen(http, cli, url);
  int len = http.getSize();
  if (code != 200 || len > JPG_CAP) {
    ESP_LOGI("COVER", "image HTTP %d, size %d", code, len);
    http.end();
    return 0;
  }
  WiFiClient* s = http.getStreamPtr();
  int n = 0;
  uint32_t t0 = millis();
  while ((len < 0 || n < len) && n < JPG_CAP && millis() - t0 < 5000) {
    int a = s->available();
    if (a > 0) n += s->readBytes(jpg + n, std::min(a, JPG_CAP - n));
    else if (!s->connected()) break;
    else vTaskDelay(5);
  }
  http.end();
  return (len < 0 || n == len) ? n : 0;
}

static int onDraw(JPEGDRAW* d) {
  for (int y = 0; y < d->iHeight; y++) {
    int ty = d->y + y;
    if (ty >= tmpH) break;
    for (int x = 0; x < d->iWidth; x++) {
      int tx = d->x + x;
      if (tx < tmpW) tmp[ty * tmpW + tx] = d->pPixels[y * d->iWidth + x];
    }
  }
  return 1;
}

// Decode jpg -> tmp, then centre-crop (or scale) into the 240x240 cover.
static bool decode(int len) {
  if (!jpeg.openRAM(jpg, len, onDraw)) return false;
  int w = jpeg.getWidth(), h = jpeg.getHeight(), sc = 1;
  while ((w / sc > TMP_MAX || h / sc > TMP_MAX) && sc < 8) sc *= 2;
  int opt = sc == 2 ? JPEG_SCALE_HALF : sc == 4 ? JPEG_SCALE_QUARTER : sc == 8 ? JPEG_SCALE_EIGHTH : 0;
  tmpW = w / sc;
  tmpH = h / sc;
  if (tmpW > TMP_MAX || tmpH > TMP_MAX || tmpW < 16 || tmpH < 16) { jpeg.close(); return false; }
  memset(tmp, 0, TMP_MAX * TMP_MAX * 2);
  jpeg.setPixelType(RGB565_BIG_ENDIAN);  // same byte order as LovyanGFX sprites
  bool ok = jpeg.decode(0, 0, opt);
  jpeg.close();
  if (!ok) return false;
  int m = std::min(tmpW, tmpH);
  float s = m / 240.0f;  // fit the short side to the disc
  for (int y = 0; y < 240; y++)
    for (int x = 0; x < 240; x++) {
      int sx = tmpW / 2 + (int)((x - 120) * s), sy = tmpH / 2 + (int)((y - 120) * s);
      cover[y * 240 + x] = (sx >= 0 && sy >= 0 && sx < tmpW && sy < tmpH) ? tmp[sy * tmpW + sx] : 0;
    }
  return true;
}

struct Cand { std::string rg, date, rel; int score; };

static bool lookup(const std::string& artist, const std::string& title) {
  // 1. MusicBrainz recording search (max 1 request/s)
  String url = "https://musicbrainz.org/ws/2/recording/?query=" +
               urlEncode("recording:\"" + noQuotes(title) + "\" AND artist:\"" + noQuotes(artist) + "\"") +
               "&fmt=json&limit=15";
  while (millis() - lastMb < 1100) vTaskDelay(50);
  lastMb = millis();

  JsonDocument filter;
  filter["recordings"][0]["title"] = true;
  JsonObject fr = filter["recordings"][0]["releases"][0].to<JsonObject>();
  fr["title"] = true;
  fr["status"] = true;
  fr["date"] = true;
  fr["release-group"]["id"] = true;
  fr["release-group"]["primary-type"] = true;
  fr["release-group"]["secondary-types"] = true;
  JsonDocument doc;
  if (!getJson(url, doc, filter)) return false;

  // 2. score releases of recordings whose title equals / starts with ours
  std::string want = metaLower(title);
  std::vector<Cand> cands;
  for (JsonObject rec : doc["recordings"].as<JsonArray>()) {
    std::string rt = metaLower(rec["title"] | "");
    if (rt.compare(0, want.size(), want) != 0) continue;
    for (JsonObject rel : rec["releases"].as<JsonArray>()) {
      JsonObject g = rel["release-group"];
      std::string id = g["id"] | "";
      if (id.empty()) continue;
      int score = 0;
      std::string pt = g["primary-type"] | "";
      if (pt == "Album" || pt == "Single" || pt == "EP") score += 2;
      bool bad = false;
      for (JsonVariant st : g["secondary-types"].as<JsonArray>()) {
        std::string s = st | "";
        if (s == "Compilation" || s == "Live" || s == "Remix" || s == "DJ-mix" || s == "Mixtape/Street") bad = true;
      }
      if (!bad) score += 4;  // Soundtrack is NOT penalised
      if (strcmp(rel["status"] | "", "Official") == 0) score += 1;
      std::string date = rel["date"] | "";
      if (date.empty()) date = "9999";
      Cand c{id, date, rel["title"] | "", score};
      auto it = std::find_if(cands.begin(), cands.end(), [&](const Cand& x) { return x.rg == id; });
      if (it == cands.end()) cands.push_back(c);
      else if (score > it->score || (score == it->score && date < it->date)) *it = c;
    }
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    return a.score != b.score ? a.score > b.score : a.date < b.date;
  });
  ESP_LOGI("COVER", "%u candidate release-groups", (unsigned)cands.size());

  // 3. Cover Art Archive: first front image of the top 8, stop at the first that decodes
  JsonDocument cf;
  cf["images"][0]["front"] = true;
  cf["images"][0]["thumbnails"]["250"] = true;
  cf["images"][0]["thumbnails"]["small"] = true;
  for (size_t i = 0; i < cands.size() && i < 8; i++) {
    if (!netUp()) return false;
    JsonDocument cd;
    if (!getJson("https://coverartarchive.org/release-group/" + String(cands[i].rg.c_str()), cd, cf)) continue;
    String img;
    for (JsonObject im : cd["images"].as<JsonArray>()) {
      if (!(im["front"] | false)) continue;
      img = im["thumbnails"]["250"] | (im["thumbnails"]["small"] | "");
      break;
    }
    if (img.isEmpty()) continue;
    img.replace("http://", "https://");
    int n = download(img);
    if (n && decode(n)) {
      Serial.printf("[COVER] using \"%s\" (%s, score %d, rg %s)\n", cands[i].rel.c_str(), cands[i].date.c_str(),
                    cands[i].score, cands[i].rg.c_str());
      return true;
    }
    ESP_LOGI("COVER", "image download/decode failed, trying next");
  }
  return false;
}

static void coverTask(void*) {
  Req r;
  for (;;) {
    xQueueReceive(q, &r, portMAX_DELAY);
    uint32_t t0 = millis();
    std::string key = metaLower(r.artist) + "|" + metaLower(r.title);
    ESP_LOGI("COVER", "lookup: %s - %s", r.artist, r.title);

    CacheEntry* ce = nullptr;
    for (auto& c : cache)
      if (c.key == key) ce = &c;
    bool found;
    if (!strcmp(r.url, "usb:")) {  // the computer sent the picture: decode it, skip the cache
      xSemaphoreTake(usbLock, portMAX_DELAY);
      size_t n = usbLen;
      memcpy(jpg, usbJpg, n);
      xSemaphoreGive(usbLock);
      found = decode(n);
    } else if (ce) {
      found = ce->hit;
      if (found) memcpy(cover, ce->px, 240 * 240 * 2);
      ESP_LOGI("COVER", "cache hit%s", found ? "" : " (no cover)");
    } else {
      if (r.url[0]) {  // Spotify gives the cover URL directly
        int n = netUp() ? download(r.url) : 0;
        found = n && decode(n);
      } else {
        found = netUp() && lookup(r.artist, r.title);
      }
      CacheEntry& c = cache[cacheNext];
      cacheNext = (cacheNext + 1) % CACHE_N;
      c.key = key;
      c.hit = found;
      if (found) {
        if (!c.px) c.px = (uint16_t*)ps_malloc(240 * 240 * 2);
        if (c.px) memcpy(c.px, cover, 240 * 240 * 2);
        else c.key.clear();
      }
    }
    if (!found) ESP_LOGI("COVER", "no cover, using default label");

    while (!discBackFree()) vTaskDelay(50);   // previous cover still being shown in
    if (uxQueueMessagesWaiting(q)) continue;  // a newer song arrived meanwhile: skip this one
    discBuild(found ? cover : nullptr);
    discCommit(r.title);
    ESP_LOGI("COVER", "ready in %lu ms", millis() - t0);
  }
}

void coverBegin() {
  q = xQueueCreate(1, sizeof(Req));
  jpg = (uint8_t*)ps_malloc(JPG_CAP);
  tmp = (uint16_t*)ps_malloc(TMP_MAX * TMP_MAX * 2);
  cover = (uint16_t*)ps_malloc(240 * 240 * 2);
  usbJpg = (uint8_t*)ps_malloc(JPG_CAP);
  usbLock = xSemaphoreCreateMutex();
  // core 0, below the audio task: TLS + JPEG work is preempted whenever audio needs the CPU
  xTaskCreatePinnedToCore(coverTask, "cover", 16384, nullptr, 1, nullptr, 0);
}
