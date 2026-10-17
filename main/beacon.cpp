// beacon.cpp -- WiFi beacon-spam transmitter (see beacon.h).
#include "beacon.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "beacon";

#define TICK_MS 100          // burst interval (every SSID sent each tick)

// Built-in fake network names. Add/trim freely (keep each <= 32 chars).
static const char* SSIDS[] = {
  "Free WiFi",
  "FBI Surveillance Van",
  "Pretty Fly for a WiFi",
  "Hide Yo Kids Hide Yo WiFi",
  "Tell My WiFi Love Her",
  "Drop It Like Its Hotspot",
  "The LAN Before Time",
  "Nacho WiFi",
  "Bill Wi the Science Fi",
  "It Hurts When IP",
  "Wu-Tang LAN",
  "Winternet Is Coming",
  "Abraham Linksys",
  "Loading...",
  "Virus.exe",
  "Area 51 Test Site",
};
#define NSSID ((int)(sizeof(SSIDS) / sizeof(SSIDS[0])))

static bool               g_running = false;
static volatile uint32_t  g_sent    = 0;
static esp_timer_handle_t g_tmr      = 0;
static uint8_t            g_chan     = 1;
static const uint8_t      CHANS[3]   = {1, 6, 11};
static int                g_chi      = 0;
static uint8_t            g_buf[128];

static void send_beacon(const char* ssid, int idx, uint8_t chan) {
  int slen = (int)strlen(ssid);
  if (slen > 32) slen = 32;
  int o = 0;
  g_buf[o++] = 0x80; g_buf[o++] = 0x00;            // frame control: beacon
  g_buf[o++] = 0x00; g_buf[o++] = 0x00;            // duration
  for (int i = 0; i < 6; i++) g_buf[o++] = 0xff;   // DA = broadcast
  uint8_t bss[6] = {0x02, 0x11, 0x22, 0x33, 0x44, (uint8_t)idx};
  for (int i = 0; i < 6; i++) g_buf[o++] = bss[i]; // SA = BSSID
  for (int i = 0; i < 6; i++) g_buf[o++] = bss[i]; // BSSID
  g_buf[o++] = 0x00; g_buf[o++] = 0x00;            // sequence (driver fills)
  for (int i = 0; i < 8; i++) g_buf[o++] = 0x00;   // timestamp
  g_buf[o++] = 0x64; g_buf[o++] = 0x00;            // beacon interval
  g_buf[o++] = 0x01; g_buf[o++] = 0x04;            // capability info (ESS)
  g_buf[o++] = 0x00; g_buf[o++] = (uint8_t)slen;   // SSID element
  memcpy(g_buf + o, ssid, slen); o += slen;
  g_buf[o++] = 0x01; g_buf[o++] = 0x08;            // supported rates
  g_buf[o++] = 0x82; g_buf[o++] = 0x84; g_buf[o++] = 0x8b; g_buf[o++] = 0x96;
  g_buf[o++] = 0x24; g_buf[o++] = 0x30; g_buf[o++] = 0x48; g_buf[o++] = 0x6c;
  g_buf[o++] = 0x03; g_buf[o++] = 0x01; g_buf[o++] = chan;   // DS param (channel)
  if (esp_wifi_80211_tx(WIFI_IF_STA, g_buf, o, true) == ESP_OK)
    g_sent = g_sent + 1;
}

static void tick_cb(void* arg) {
  (void)arg;
  uint8_t ch = CHANS[g_chi];
  g_chi = (g_chi + 1) % 3;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  g_chan = ch;
  for (int i = 0; i < NSSID; i++) send_beacon(SSIDS[i], i, ch);
}

bool beacon_start(void) {
  if (g_running) return true;
  g_sent = 0; g_chi = 0;
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
  if (esp_wifi_start() != ESP_OK) return false;
  esp_wifi_set_promiscuous(true);                  // park: no association
  esp_wifi_set_channel(CHANS[0], WIFI_SECOND_CHAN_NONE);
  esp_timer_create_args_t ta = {};
  ta.callback = tick_cb;
  ta.name     = "beacon";
  if (esp_timer_create(&ta, &g_tmr) != ESP_OK) {
    esp_wifi_set_promiscuous(false);
    return false;
  }
  esp_timer_start_periodic(g_tmr, (uint64_t)TICK_MS * 1000);
  g_running = true;
  ESP_LOGI(TAG, "beacon spam start (%d ssids)", NSSID);
  return true;
}

void beacon_stop(void) {
  if (!g_running) return;
  if (g_tmr) { esp_timer_stop(g_tmr); esp_timer_delete(g_tmr); g_tmr = 0; }
  esp_wifi_set_promiscuous(false);
  g_running = false;
  ESP_LOGI(TAG, "beacon spam stop (sent %u)", (unsigned)g_sent);
}

bool     beacon_running(void) { return g_running; }
uint32_t beacon_sent(void)    { return g_sent; }
int      beacon_count(void)   { return NSSID; }
uint8_t  beacon_channel(void) { return g_chan; }
