// AirPlay speaker: a compact classic AirPlay (RAOP) receiver.
// RTSP on TCP 5000 sets up the stream, audio arrives as RTP over UDP (ALAC, usually AES encrypted),
// a jitter buffer in PSRAM reorders it and a play task feeds the speaker at 44.1 kHz.
// Protocol details follow the open shairport-sync receiver; the ALAC decoder is alac.c (MIT).
#include "airplay.h"
extern "C" {
#include "alac.h"
}
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mdns.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace airplay {

static const char* TAG = "AIRPLAY";
static const uint32_t RATE = 44100;
static const int RTSP_PORT = 5000, AUDIO_PORT = 6000, CONTROL_PORT = 6001, TIMING_PORT = 6002;
static const int MAX_FPP = 352;         // frames per packet the senders use
static const int SLOTS = 1024;          // jitter buffer, packets (power of two), ~8 s
static const int PREFILL = 120;         // packets queued before playing (~1 s): rides out Wi-Fi hiccups
static const int LOW = PREFILL / 2, HIGH = PREFILL * 3 / 2;
static const int REQ_MAX = 16384;

// Key pair of the original AirPort Express. It was published long ago and every open AirPlay
// receiver uses it: senders check that the speaker can answer their challenge with it.
static const char AIRPORT_KEY[] =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
    "wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U\n"
    "wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
    "/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/\n"
    "UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW\n"
    "BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa\n"
    "LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5\n"
    "NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm\n"
    "lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz\n"
    "aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu\n"
    "a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM\n"
    "oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z\n"
    "oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+\n"
    "k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL\n"
    "AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA\n"
    "cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
    "54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov\n"
    "17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc\n"
    "1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI\n"
    "LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ\n"
    "2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
    "-----END RSA PRIVATE KEY-----\n";

static Hooks hooks;
static int pinB, pinL, pinD;
static float maxGain;
static uint8_t mac[6];
static mbedtls_pk_context pk;
static int audioSock = -1, ctlSock = -1, timeSock = -1;
static i2s_chan_handle_t tx;  // owned by the play task
static volatile int32_t gain;

// stream set up by RTSP (the receive task only decodes while `session` is true)
static volatile bool session;
static volatile uint32_t gen;  // bumped on every setup/flush/teardown: both tasks restart their position
static int fpp = MAX_FPP;
static bool encrypted;
static mbedtls_aes_context aes;
static uint8_t aesIv[16];
static alac_file* alac;
static sockaddr_in senderCtl;  // sender's control port, for resend requests
static volatile bool flushSeqValid;
static volatile uint16_t flushSeq;

// jitter buffer: slot = seq % SLOTS, written by the receive task, read by the play task
struct Slot {
  volatile bool ok;
  uint16_t seq;
  int16_t pcm[MAX_FPP * 2];
};
static Slot* slots;
static volatile bool haveFirst, playerReady;
static volatile uint32_t firstGen;
static volatile uint16_t firstSeq, latestSeq, nextSeq;
static volatile uint32_t lost, late, underruns, nudges, resent, packets;
static volatile int64_t lastPacket;

static int rng(void*, unsigned char* b, size_t n) {
  esp_fill_random(b, n);
  return 0;
}

// ---------------------------------------------------------------- speaker
static void take() {
  if (hooks.onActive) hooks.onActive(true);  // radio stops
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 256;
  cc.auto_clear = true;
  i2s_chan_handle_t t;
  esp_err_t e = i2s_new_channel(&cc, &t, nullptr);
  if (e == ESP_OK) {
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
    e = i2s_channel_init_std_mode(t, &sc);
    if (e == ESP_OK) e = i2s_channel_enable(t);
    if (e != ESP_OK) i2s_del_channel(t);
  }
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "no I2S channel: %s", esp_err_to_name(e));
    if (hooks.onActive) hooks.onActive(false);
    return;
  }
  tx = t;
  ESP_LOGI(TAG, "took the speaker");
}

static void release() {
  i2s_channel_disable(tx);
  i2s_del_channel(tx);
  tx = nullptr;
  if (hooks.onActive) hooks.onActive(false);  // radio comes back
  ESP_LOGI(TAG, "released the speaker (lost %u, late %u, resent %u, underruns %u)", (unsigned)lost,
           (unsigned)late, (unsigned)resent, (unsigned)underruns);
}

// ---------------------------------------------------------------- receive
static void requestResend(uint16_t first, uint16_t count) {
  if (count > 64 || !senderCtl.sin_port) return;  // a long gap is just played as silence
  uint8_t r[8] = {0x80, 0xD5, 0, 1, (uint8_t)(first >> 8), (uint8_t)first, (uint8_t)(count >> 8), (uint8_t)count};
  sendto(ctlSock, r, sizeof(r), 0, (sockaddr*)&senderCtl, sizeof(senderCtl));
}

static void onPacket(const uint8_t* p, int len, bool isResend) {
  static uint8_t* clear = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
  static uint32_t rxGen = ~0u;
  if (len <= 12 || len - 12 > 2048 || !session || !alac || !clear) return;
  uint16_t seq = (p[2] << 8) | p[3];
  uint32_t g = gen;
  if (rxGen != g) { rxGen = g; haveFirst = false; }
  if (!haveFirst) {
    if (isResend || (flushSeqValid && (int16_t)(seq - flushSeq) < 0)) return;  // from before a pause/seek
    firstSeq = latestSeq = seq;
    firstGen = g;
    haveFirst = true;
  } else {
    int16_t ahead = seq - latestSeq;
    if (ahead > 0) {
      if (ahead > 1 && !isResend) requestResend(latestSeq + 1, ahead - 1);
      latestSeq = seq;
    }
    if (playerReady && (int16_t)(seq - nextSeq) < 0) { late = late + 1; return; }  // already played past it
  }
  const uint8_t* d = p + 12;
  int n = len - 12;
  if (encrypted) {  // AES-128-CBC, IV reset for every packet, the tail under 16 bytes is sent in clear
    uint8_t iv[16];
    memcpy(iv, aesIv, 16);
    int a = n & ~15;
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, a, iv, d, clear);
    memcpy(clear + a, d + a, n - a);
  } else {
    memcpy(clear, d, n);
  }
  Slot& s = slots[seq & (SLOTS - 1)];
  s.ok = false;
  int out = fpp * 4;
  alac_decode_frame(alac, clear, s.pcm, &out);
  if (out <= 0) return;
  if (out < fpp * 4) memset((uint8_t*)s.pcm + out, 0, fpp * 4 - out);
  s.seq = seq;
  __sync_synchronize();
  s.ok = true;
  packets = packets + 1;
  lastPacket = esp_timer_get_time();
}

static void rxTask(void*) {
  uint8_t* b = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
  for (;;) {
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(audioSock, &fs);
    FD_SET(ctlSock, &fs);
    FD_SET(timeSock, &fs);
    timeval tv = {1, 0};
    int mx = audioSock > ctlSock ? audioSock : ctlSock;
    if (timeSock > mx) mx = timeSock;
    if (select(mx + 1, &fs, nullptr, nullptr, &tv) <= 0) continue;
    if (FD_ISSET(audioSock, &fs)) {
      int n = recv(audioSock, b, 2048, 0);
      if (n > 12 && (b[1] & 0x7f) == 0x60) onPacket(b, n, false);
    }
    if (FD_ISSET(ctlSock, &fs)) {
      int n = recv(ctlSock, b, 2048, 0);
      if (n > 16 && (b[1] & 0x7f) == 0x56) {  // a packet we asked for again, wrapped in 4 bytes
        resent = resent + 1;
        onPacket(b + 4, n - 4, true);
      }  // 0x54 sync packets: not needed without multi-room timing
    }
    if (FD_ISSET(timeSock, &fs)) recv(timeSock, b, 2048, 0);
  }
}

// ---------------------------------------------------------------- play
static void playTask(void*) {
  int16_t* out = (int16_t*)heap_caps_malloc((MAX_FPP + 1) * 4, MALLOC_CAP_SPIRAM);
  uint32_t playGen = ~0u;
  bool started = false;
  for (;;) {
    if (session && haveFirst && !tx && (!hooks.canTake || hooks.canTake())) take();
    if (!session && tx) release();
    if (!tx) {
      playerReady = false;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    int frames = fpp;
    bool ready = haveFirst && firstGen == gen;
    if (ready && playGen != firstGen) {  // new stream or after a flush: start where the sender starts
      playGen = firstGen;
      nextSeq = firstSeq;
      started = false;
      playerReady = true;
    }
    int depth = ready ? (int16_t)(latestSeq - nextSeq) + 1 : 0;
    if (!started && ready && depth >= PREFILL) started = true;
    if (started && depth <= 0) { started = false; underruns = underruns + 1; }
    if (!started) {
      memset(out, 0, frames * 4);  // filling up or paused: silence
    } else {
      Slot& s = slots[nextSeq & (SLOTS - 1)];
      if (s.ok && s.seq == nextSeq) {
        memcpy(out, s.pcm, frames * 4);
        s.ok = false;
      } else {
        memset(out, 0, frames * 4);
        lost = lost + 1;
      }
      nextSeq = nextSeq + 1;
      // our 44.1 kHz and the sender's never match exactly: drop or repeat one frame to hold the level
      if (depth > HIGH) { frames--; nudges = nudges + 1; }
      else if (depth < LOW) {
        out[frames * 2] = out[frames * 2 - 2];
        out[frames * 2 + 1] = out[frames * 2 - 1];
        frames++;
        nudges = nudges + 1;
      }
      if (hooks.mix) hooks.mix(out, frames, RATE);
      int32_t g = gain;
      for (int i = 0; i < frames * 2; i++) out[i] = (int16_t)((out[i] * g) >> 16);
    }
    size_t done = 0;
    if (i2s_channel_write(tx, out, frames * 4, &done, 200) != ESP_OK) vTaskDelay(1);  // paced by the speaker
    static int64_t lastLog;
    int64_t now = esp_timer_get_time();
    if (now - lastLog > 10000000) {
      lastLog = now;
      ESP_LOGI(TAG, "queued %d, packets %u, lost %u, late %u, resent %u, underruns %u, nudges %u", depth,
               (unsigned)packets, (unsigned)lost, (unsigned)late, (unsigned)resent, (unsigned)underruns,
               (unsigned)nudges);
    }
  }
}

// ---------------------------------------------------------------- RTSP
static const char* header(const char* req, const char* name) {  // value of a header, or null
  size_t nl = strlen(name);
  for (const char* l = strstr(req, "\r\n"); l && l[2] && !(l[2] == '\r' && l[3] == '\n'); l = strstr(l + 2, "\r\n")) {
    const char* h = l + 2;
    if (!strncasecmp(h, name, nl) && h[nl] == ':') {
      h += nl + 1;
      while (*h == ' ') h++;
      return h;
    }
  }
  return nullptr;
}

static void copyValue(char* dst, size_t sz, const char* v) {  // up to the end of the line
  size_t i = 0;
  while (v && v[i] && v[i] != '\r' && v[i] != '\n' && i + 1 < sz) { dst[i] = v[i]; i++; }
  dst[i] = 0;
}

static bool b64decode(const char* v, uint8_t* out, size_t sz, size_t* len) {  // senders drop the '=' padding
  char tmp[512];
  size_t n = 0;
  while (v[n] && v[n] != '\r' && v[n] != '\n' && n < sizeof(tmp) - 4) { tmp[n] = v[n]; n++; }
  while (n % 4) tmp[n++] = '=';
  return mbedtls_base64_decode(out, sz, len, (const uint8_t*)tmp, n) == 0;
}

static bool appleResponse(int cli, const char* challenge, char* out, size_t sz) {
  uint8_t buf[48] = {};
  size_t n = 0;
  if (!b64decode(challenge, buf, 16, &n) || n > 16) return false;
  sockaddr_in a = {};
  socklen_t al = sizeof(a);
  getsockname(cli, (sockaddr*)&a, &al);
  memcpy(buf + n, &a.sin_addr.s_addr, 4);
  n += 4;
  memcpy(buf + n, mac, 6);
  n += 6;
  if (n < 32) n = 32;
  uint8_t sig[256];
  mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
  mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
  if (mbedtls_rsa_pkcs1_sign(rsa, rng, nullptr, MBEDTLS_MD_NONE, n, buf, sig)) return false;
  size_t ol = 0;
  if (mbedtls_base64_encode((uint8_t*)out, sz, &ol, sig, sizeof(sig))) return false;
  while (ol && out[ol - 1] == '=') out[--ol] = 0;
  return true;
}

static void endStream() {
  session = false;
  gen = gen + 1;
}

// Parses the stream description. Returns false for formats we can't play.
static bool announce(const char* sdp) {
  int f[12] = {};
  bool gotFmtp = false, gotKey = false, gotIv = false;
  uint8_t key[16];
  for (const char* l = sdp; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : nullptr) {
    if (!strncmp(l, "a=rtpmap:", 9) && !strstr(l, "AppleLossless")) {
      char t[48];
      copyValue(t, sizeof(t), l);
      ESP_LOGW(TAG, "unsupported format: %s", t);
      return false;
    }
    if (!strncmp(l, "a=fmtp:", 7))
      gotFmtp = sscanf(l + 7, "%d %d %d %d %d %d %d %d %d %d %d %d", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5],
                       &f[6], &f[7], &f[8], &f[9], &f[10], &f[11]) == 12;
    if (!strncmp(l, "a=aesiv:", 8)) {
      size_t n = 0;
      gotIv = b64decode(l + 8, aesIv, sizeof(aesIv), &n) && n == 16;
    }
    if (!strncmp(l, "a=rsaaeskey:", 12)) {
      uint8_t enc[256];
      size_t n = 0, kl = 0;
      mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
      mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
      gotKey = b64decode(l + 12, enc, sizeof(enc), &n) && n == 256 &&
               !mbedtls_rsa_pkcs1_decrypt(rsa, rng, nullptr, &kl, enc, key, sizeof(key)) && kl == 16;
    }
  }
  if (!gotFmtp || f[1] < 1 || f[1] > MAX_FPP || f[3] != 16 || f[7] != 2 || f[11] != (int)RATE) {
    ESP_LOGW(TAG, "unsupported stream: %d frames, %d bit, %d ch, %d Hz", f[1], f[3], f[7], f[11]);
    return false;
  }
  endStream();
  vTaskDelay(pdMS_TO_TICKS(30));  // ponytail: lets the receive task finish a packet; a lock if this ever races
  if (alac) alac_free(alac);
  alac = alac_create(16, 2);
  if (!alac) return false;
  alac->setinfo_max_samples_per_frame = f[1];
  alac->setinfo_7a = f[2];
  alac->setinfo_sample_size = f[3];
  alac->setinfo_rice_historymult = f[4];
  alac->setinfo_rice_initialhistory = f[5];
  alac->setinfo_rice_kmodifier = f[6];
  alac->setinfo_7f = f[7];
  alac->setinfo_80 = f[8];
  alac->setinfo_82 = f[9];
  alac->setinfo_86 = f[10];
  alac->setinfo_8a_rate = f[11];
  alac_allocate_buffers(alac);
  fpp = f[1];
  encrypted = gotKey && gotIv;
  if (encrypted) mbedtls_aes_setkey_dec(&aes, key, 128);
  ESP_LOGI(TAG, "stream: ALAC %d frames/packet, %s", fpp, encrypted ? "encrypted" : "clear");
  return true;
}

static void setVolume(float v) {  // sender sends -30..0, -144 is mute
  if (v <= -100) gain = 0;
  else gain = (int32_t)(powf(10.0f, v * 1.6f / 20.0f) * maxGain * 65536);  // -30 maps to about -48 dB
  ESP_LOGI(TAG, "volume %.1f", v);
}

// Reads and answers one request. False: connection closed.
static bool handle(int cli, char* req) {
  int len = 0, hdrEnd = -1;
  while (hdrEnd < 0) {
    if (len >= REQ_MAX) return false;
    int n = recv(cli, req + len, REQ_MAX - len, 0);
    if (n <= 0) return false;
    len += n;
    req[len] = 0;
    char* e = strstr(req, "\r\n\r\n");
    if (e) hdrEnd = e - req + 4;
  }
  const char* cl = header(req, "Content-Length");
  int bodyLen = cl ? atoi(cl) : 0, need = hdrEnd + bodyLen;
  if (bodyLen < 0 || need > REQ_MAX) return false;  // we never ask for artwork, so bodies stay small
  while (len < need) {
    int n = recv(cli, req + len, need - len, 0);
    if (n <= 0) return false;
    len += n;
  }
  req[len] = 0;
  const char* body = req + hdrEnd;

  char method[24], cseq[16], extra[600] = "";
  sscanf(req, "%23s", method);
  copyValue(cseq, sizeof(cseq), header(req, "CSeq"));
  const char* status = "200 OK";
  size_t xl = 0;

  const char* ch = header(req, "Apple-Challenge");
  if (ch) {
    char resp[400];
    if (appleResponse(cli, ch, resp, sizeof(resp))) xl += snprintf(extra + xl, sizeof(extra) - xl, "Apple-Response: %s\r\n", resp);
  }
  if (!strcmp(method, "OPTIONS")) {
    xl += snprintf(extra + xl, sizeof(extra) - xl,
                   "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n");
  } else if (!strcmp(method, "ANNOUNCE")) {
    if (!announce(body)) status = "415 Unsupported Media Type";
  } else if (!strcmp(method, "SETUP")) {
    const char* t = header(req, "Transport");
    char tr[200];
    copyValue(tr, sizeof(tr), t);
    const char* cp = strstr(tr, "control_port=");
    sockaddr_in peer = {};
    socklen_t pl = sizeof(peer);
    getpeername(cli, (sockaddr*)&peer, &pl);
    peer.sin_port = cp ? htons(atoi(cp + 13)) : 0;
    senderCtl = peer;
    flushSeqValid = false;
    gen = gen + 1;
    session = alac != nullptr;
    xl += snprintf(extra + xl, sizeof(extra) - xl,
                   "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%d;control_port=%d;timing_port=%d\r\n"
                   "Session: 1\r\n",
                   AUDIO_PORT, CONTROL_PORT, TIMING_PORT);
    ESP_LOGI(TAG, "stream set up");
  } else if (!strcmp(method, "RECORD")) {
    xl += snprintf(extra + xl, sizeof(extra) - xl, "Audio-Latency: 11025\r\n");
  } else if (!strcmp(method, "FLUSH")) {
    const char* ri = header(req, "RTP-Info");
    const char* s = ri ? strstr(ri, "seq=") : nullptr;
    if (s) { flushSeq = (uint16_t)atoi(s + 4); flushSeqValid = true; }
    gen = gen + 1;
  } else if (!strcmp(method, "TEARDOWN")) {
    endStream();
    ESP_LOGI(TAG, "sender stopped");
  } else if (!strcmp(method, "SET_PARAMETER")) {
    const char* v = strstr(body, "volume:");
    if (v) setVolume(strtof(v + 7, nullptr));
  }
  char resp[900];
  int rl = snprintf(resp, sizeof(resp),
                    "RTSP/1.0 %s\r\nCSeq: %s\r\nServer: AirTunes/105.1\r\nAudio-Jack-Status: connected; type=analog\r\n%s\r\n",
                    status, cseq, extra);
  send(cli, resp, rl, 0);
  return true;
}

static void rtspTask(void*) {
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(RTSP_PORT);
  bind(srv, (sockaddr*)&a, sizeof(a));
  listen(srv, 2);
  char* req = (char*)heap_caps_malloc(REQ_MAX + 1, MALLOC_CAP_SPIRAM);
  int cli = -1;
  for (;;) {
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(srv, &fs);
    if (cli >= 0) FD_SET(cli, &fs);
    if (select((cli > srv ? cli : srv) + 1, &fs, nullptr, nullptr, nullptr) <= 0) continue;
    if (FD_ISSET(srv, &fs)) {
      int n = accept(srv, nullptr, nullptr);
      if (n >= 0) {
        if (cli >= 0) { endStream(); close(cli); }  // a new sender takes over
        cli = n;
        setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ESP_LOGI(TAG, "sender connected");
        continue;
      }
    }
    if (cli >= 0 && FD_ISSET(cli, &fs) && !handle(cli, req)) {
      endStream();
      close(cli);
      cli = -1;
      ESP_LOGI(TAG, "sender disconnected");
    }
  }
}

static int udpSocket(int port) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  bind(s, (sockaddr*)&a, sizeof(a));
  return s;
}

void begin(const char* name, int bclk, int lrclk, int dout, float mg, Hooks h) {
  pinB = bclk, pinL = lrclk, pinD = dout, maxGain = mg, hooks = h;
  setVolume(-15);
  esp_log_level_set(TAG, ESP_LOG_INFO);
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  mbedtls_pk_init(&pk);
  mbedtls_aes_init(&aes);
  if (mbedtls_pk_parse_key(&pk, (const uint8_t*)AIRPORT_KEY, sizeof(AIRPORT_KEY), nullptr, 0, rng, nullptr)) {
    ESP_LOGE(TAG, "key parse failed");
    return;
  }
  slots = (Slot*)heap_caps_calloc(SLOTS, sizeof(Slot), MALLOC_CAP_SPIRAM);
  if (!slots) { ESP_LOGE(TAG, "no memory for the buffer"); return; }
  audioSock = udpSocket(AUDIO_PORT);
  ctlSock = udpSocket(CONTROL_PORT);
  timeSock = udpSocket(TIMING_PORT);
  // stacks in PSRAM: internal RAM is needed by USB and Wi-Fi (these tasks never touch flash)
  xTaskCreatePinnedToCoreWithCaps(rtspTask, "airplay", 12288, nullptr, 3, nullptr, 0, MALLOC_CAP_SPIRAM);
  xTaskCreatePinnedToCoreWithCaps(rxTask, "airplayrx", 6144, nullptr, 7, nullptr, 0, MALLOC_CAP_SPIRAM);
  xTaskCreatePinnedToCoreWithCaps(playTask, "airplayout", 4096, nullptr, 6, nullptr, 1, MALLOC_CAP_SPIRAM);

  char inst[80];
  snprintf(inst, sizeof(inst), "%02X%02X%02X%02X%02X%02X@%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name);
  mdns_txt_item_t txt[] = {
      {"txtvers", "1"}, {"ch", "2"},       {"cn", "1"},        {"et", "0,1"},  {"ek", "1"},
      {"sv", "false"},  {"da", "true"},    {"sr", "44100"},    {"ss", "16"},   {"pw", "false"},
      {"vn", "3"},      {"tp", "UDP"},     {"sf", "0x4"},      {"am", "Xiaophone"}, {"vs", "105.1"},
  };
  esp_err_t e = mdns_service_add(inst, "_raop", "_tcp", RTSP_PORT, txt, sizeof(txt) / sizeof(txt[0]));
  if (e != ESP_OK) ESP_LOGE(TAG, "mDNS advert failed: %s", esp_err_to_name(e));
  else ESP_LOGI(TAG, "ready as \"%s\"", name);
}

bool active() { return tx != nullptr; }
bool playing() { return tx && esp_timer_get_time() - lastPacket < 1000000; }

}  // namespace airplay
