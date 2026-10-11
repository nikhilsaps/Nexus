// wifi_net.h -- lazy WiFi subsystem for Nexus (scan / connect+test / hotspot).
// WiFi is mutually exclusive with BLE/USB: start on entering the WiFi menu,
// full teardown on leaving so the stack reclaims all SRAM (no PSRAM on this board).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WIFI_SSID_MAX 33   /* 32 chars + null */

typedef struct {
  char    ssid[WIFI_SSID_MAX];
  uint8_t bssid[6];
  int8_t  rssi;
  uint8_t channel;
  uint8_t authmode;        /* wifi_auth_mode_t */
} wifi_ap_t;

#ifdef __cplusplus
extern "C" {
#endif

bool   wifi_net_start(void);     /* init stack + STA/AP netifs */
void   wifi_net_stop(void);      /* full teardown, free RAM */
bool   wifi_net_running(void);

/* Scan nearby APs (STA mode, blocking). Returns count written (<= max). */
int    wifi_net_scan(wifi_ap_t* out, int max);

/* Connect as STA. Blocks up to timeout_ms. 1 = got IP, 0 = fail/timeout. */
int    wifi_net_connect(const char* ssid, const char* pass, int timeout_ms);
void   wifi_net_disconnect(void);
bool   wifi_net_sta_ipinfo(char* ip, char* gw, int len);  /* dotted strings */

/* Internet reachability via HTTP 204 probe (also flags captive portals). */
int    wifi_net_check_internet(void);

/* Live RSSI of the currently connected AP (0 if not connected). */
int    wifi_net_rssi(void);

/* SoftAP hotspot. pass "" or < 8 chars => open network. */
bool   wifi_net_ap_start(const char* ssid, const char* pass);
void   wifi_net_ap_stop(void);
int    wifi_net_ap_clients(void);
bool   wifi_net_ap_ipinfo(char* ip, int len);

/* Sync clock from NTP (needs internet). Writes "YYYY-MM-DD HH:MM:SS IST". */
bool   wifi_net_sntp_sync(char* out, int len);

/* Captive portal: open SoftAP named ssid + DNS catch-all + HTTP serving html. */
bool   wifi_net_cp_start(const char* ssid, const char* html, int htmllen);
void   wifi_net_cp_stop(void);
int    wifi_net_cp_hits(void);

size_t wifi_net_free_heap(void);
const char* wifi_net_auth_str(uint8_t authmode);

#ifdef __cplusplus
}
#endif
