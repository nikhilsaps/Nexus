// sniffer.cpp -- passive WiFi sniffer / probe + client logger (see sniffer.h).
#include "sniffer.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "sniffer";

#define SNIFF_MAX   48      // unique devices tracked (LRU-evicted when full)
#define SSID_LEN    33      // 32 + NUL
#define HOP_MS      250     // channel dwell time
#define CH_MIN      1
#define CH_MAX      13

enum { T_PROBE = 0, T_AP = 1, T_CLIENT = 2 };

typedef struct {
  uint8_t  mac[6];
  char     ssid[SSID_LEN];  // probed/beaconed name ("" if none)
  int8_t   rssi;
  uint8_t  channel;
  uint8_t  type;
  uint32_t count;
  uint32_t last_ms;
} dev_t;

static dev_t             g_dev[SNIFF_MAX];
static int               g_ndev   = 0;
static volatile uint32_t g_frames = 0;
static volatile uint8_t  g_chan   = CH_MIN;
static bool              g_running = false;
static esp_timer_handle_t g_hop   = 0;
static portMUX_TYPE      g_mux    = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Copy an SSID IE (id 0) if present at/after `ie_off` into out (ASCII-sanitised).
static void grab_ssid(const uint8_t* p, int len, int ie_off, char* out) {
  out[0] = 0;
  int i = ie_off;
  while (i + 2 <= len) {
    uint8_t id = p[i], l = p[i + 1];
    if (i + 2 + l > len) break;
    if (id == 0) {                       // SSID element
      int n = l < SSID_LEN - 1 ? l : SSID_LEN - 1;
      int o = 0;
      for (int k = 0; k < n; k++) {
        uint8_t c = p[i + 2 + k];
        out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
      }
      out[o] = 0;
      return;
    }
    i += 2 + l;
  }
}

// Insert/update a device record. Runs inside the promiscuous callback.
static void touch(const uint8_t* mac, const char* ssid, int8_t rssi,
                  uint8_t ch, uint8_t type) {
  portENTER_CRITICAL(&g_mux);
  int found = -1, oldest = 0;
  for (int i = 0; i < g_ndev; i++) {
    if (!memcmp(g_dev[i].mac, mac, 6)) { found = i; break; }
    if (g_dev[i].last_ms < g_dev[oldest].last_ms) oldest = i;
  }
  int idx;
  if (found >= 0)            idx = found;
  else if (g_ndev < SNIFF_MAX) idx = g_ndev++;
  else                       idx = oldest;     // evict LRU

  dev_t* d = &g_dev[idx];
  if (found < 0) { memcpy(d->mac, mac, 6); d->ssid[0] = 0; d->count = 0; }
  d->rssi    = rssi;
  d->channel = ch;
  d->type    = type;
  d->count++;
  d->last_ms = now_ms();
  if (ssid && ssid[0]) strncpy(d->ssid, ssid, SSID_LEN - 1), d->ssid[SSID_LEN - 1] = 0;
  portEXIT_CRITICAL(&g_mux);
}

static void promisc_cb(void* buf, wifi_promiscuous_pkt_type_t ptype) {
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 16) return;                        // too short for addr2
  g_frames++;

  uint8_t fc0 = p[0];
  uint8_t ftype = (fc0 >> 2) & 0x3;
  uint8_t fsub  = (fc0 >> 4) & 0xf;
  const uint8_t* a2 = p + 10;                  // transmitter address
  int8_t rssi = pkt->rx_ctrl.rssi;
  uint8_t ch  = pkt->rx_ctrl.channel;
  char ssid[SSID_LEN];

  if (ftype == 0) {                            // management
    if (fsub == 4) {                           // probe request
      grab_ssid(p, len, 24, ssid);
      touch(a2, ssid, rssi, ch, T_PROBE);
    } else if (fsub == 8 || fsub == 5) {       // beacon / probe response
      grab_ssid(p, len, 36, ssid);             // skip 12-byte fixed params
      touch(a2, ssid, rssi, ch, T_AP);
    }
  } else if (ftype == 2) {                      // data -> a connected client
    touch(a2, "", rssi, ch, T_CLIENT);
  }
}

static void hop_cb(void* arg) {
  (void)arg;
  uint8_t c = g_chan + 1;
  if (c > CH_MAX) c = CH_MIN;
  g_chan = c;
  esp_wifi_set_channel(c, WIFI_SECOND_CHAN_NONE);
}

bool sniffer_start(void) {
  if (g_running) return true;
  portENTER_CRITICAL(&g_mux);
  g_ndev = 0; g_frames = 0;
  portEXIT_CRITICAL(&g_mux);
  g_chan = CH_MIN;

  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
  if (esp_wifi_start() != ESP_OK) return false;

  wifi_promiscuous_filter_t filt = {};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(promisc_cb);
  if (esp_wifi_set_promiscuous(true) != ESP_OK) { esp_wifi_stop(); return false; }
  esp_wifi_set_channel(CH_MIN, WIFI_SECOND_CHAN_NONE);

  esp_timer_create_args_t ta = {};
  ta.callback = hop_cb;
  ta.name     = "sniffhop";
  if (esp_timer_create(&ta, &g_hop) == ESP_OK)
    esp_timer_start_periodic(g_hop, HOP_MS * 1000);

  g_running = true;
  ESP_LOGI(TAG, "sniffer started");
  return true;
}

void sniffer_stop(void) {
  if (!g_running) return;
  if (g_hop) { esp_timer_stop(g_hop); esp_timer_delete(g_hop); g_hop = 0; }
  esp_wifi_set_promiscuous(false);
  esp_wifi_stop();
  portENTER_CRITICAL(&g_mux);
  g_ndev = 0;
  portEXIT_CRITICAL(&g_mux);
  g_running = false;
  ESP_LOGI(TAG, "sniffer stopped");
}

bool     sniffer_running(void) { return g_running; }
uint8_t  sniffer_channel(void) { return g_chan; }
uint32_t sniffer_frames(void)  { return g_frames; }
uint32_t sniffer_devices(void) { return (uint32_t)g_ndev; }

static const char* type_str(uint8_t t) {
  return t == T_AP ? "ap" : t == T_CLIENT ? "client" : "probe";
}

// Append JSON-escaped text, bounded. Returns new write offset.
static int esc(char* b, int o, int cap, const char* s) {
  for (; *s && o < cap - 2; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { b[o++] = '\\'; b[o++] = c; }
    else                        b[o++] = c;
  }
  return o;
}

int sniffer_json(char* buf, size_t len) {
  int cap = (int)len;
  int o = 0;
  uint32_t now = now_ms();

  // Snapshot the table under the lock, then serialise outside it.
  static dev_t snap[SNIFF_MAX];
  int n;
  portENTER_CRITICAL(&g_mux);
  n = g_ndev;
  memcpy(snap, g_dev, sizeof(dev_t) * (n < SNIFF_MAX ? n : SNIFF_MAX));
  portEXIT_CRITICAL(&g_mux);

  o += snprintf(buf + o, cap - o, "{\"ch\":%u,\"frames\":%u,\"devices\":%d,\"list\":[",
                (unsigned)g_chan, (unsigned)g_frames, n);
  for (int i = 0; i < n && o < cap - 220; i++) {
    dev_t* d = &snap[i];
    if (i) { if (o < cap - 1) buf[o++] = ','; }
    o += snprintf(buf + o, cap - o,
      "{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ssid\":\"",
      d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    o = esc(buf, o, cap, d->ssid);
    o += snprintf(buf + o, cap - o,
      "\",\"rssi\":%d,\"ch\":%u,\"type\":\"%s\",\"count\":%u,\"age\":%u}",
      d->rssi, (unsigned)d->channel, type_str(d->type),
      (unsigned)d->count, (unsigned)((now - d->last_ms) / 1000));
  }
  if (o < cap - 2) { buf[o++] = ']'; buf[o++] = '}'; }
  buf[o] = 0;
  return o;
}
