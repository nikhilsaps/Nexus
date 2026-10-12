// localserve.cpp - LocalServe HTTP file server (see localserve.h).
#include "localserve.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "sniffer.h"

static const char* TAG = "localserve";
#define SITES_ROOT  "/sdcard/sites"
#define CHUNK       4096
#define PATH_MAX_   1152
#define PATHGZ_MAX_ 1184

static httpd_handle_t g_srv = 0;

static const char* mime_for(const char* path) {
  const char* d = strrchr(path, '.');
  if (!d) return "application/octet-stream";
  d++;
  if (!strcasecmp(d, "html") || !strcasecmp(d, "htm"))  return "text/html; charset=utf-8";
  if (!strcasecmp(d, "js")   || !strcasecmp(d, "mjs"))  return "application/javascript; charset=utf-8";
  if (!strcasecmp(d, "css"))   return "text/css; charset=utf-8";
  if (!strcasecmp(d, "json"))  return "application/json; charset=utf-8";
  if (!strcasecmp(d, "svg"))   return "image/svg+xml";
  if (!strcasecmp(d, "png"))   return "image/png";
  if (!strcasecmp(d, "jpg")  || !strcasecmp(d, "jpeg")) return "image/jpeg";
  if (!strcasecmp(d, "gif"))   return "image/gif";
  if (!strcasecmp(d, "webp"))  return "image/webp";
  if (!strcasecmp(d, "ico"))   return "image/x-icon";
  if (!strcasecmp(d, "wasm"))  return "application/wasm";
  if (!strcasecmp(d, "woff2")) return "font/woff2";
  if (!strcasecmp(d, "woff"))  return "font/woff";
  if (!strcasecmp(d, "txt"))   return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

// Built-in launcher, served when /sdcard/sites/index.html is absent so that
// "the server is up" is verifiable before the card has any sites.
static const char FALLBACK[] =
"<!doctype html><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Nexus LocalServe</title>"
"<style>body{margin:0;font:16px/1.6 system-ui,sans-serif;background:#0f1115;"
"color:#e6e6e6;display:grid;place-items:center;min-height:100vh}"
".c{max-width:560px;padding:2rem;border:1px solid #2a2d35;border-radius:14px;"
"background:#161922;box-shadow:0 10px 40px rgba(0,0,0,.4)}"
"h1{margin:0 0 .4rem}.ok{color:#4ade80}"
"code{background:#0f1115;padding:.1rem .4rem;border-radius:6px;color:#8bd5ff}</style>"
"<div class=c><h1>LocalServe is up <span class=ok>&#10003;</span></h1>"
"<p>The Cardputer is serving over the network. No sites are on the card yet.</p>"
"<p>On the microSD create <code>/sites/</code>, drop a site folder inside with an "
"<code>index.html</code>, then reload. The ESP just streams the files; your "
"browser is the app.</p></div>";

// Stream an SD file in chunks. Returns false only if the file does not exist.
static bool send_file(httpd_req_t* req, const char* fspath, const char* urlpath, bool gz) {
  FILE* f = fopen(fspath, "rb");
  if (!f) return false;
  httpd_resp_set_type(req, mime_for(urlpath));
  if (gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  char* buf = (char*)malloc(CHUNK);
  if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return true; }
  size_t n;
  while ((n = fread(buf, 1, CHUNK, f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { free(buf); fclose(f); return true; }
  }
  free(buf);
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0);   // terminate chunked response
  return true;
}

static esp_err_t get_handler(httpd_req_t* req) {
  char* url = (char*)malloc(PATH_MAX_);
  char* fs  = (char*)malloc(PATH_MAX_);
  char* gz  = (char*)malloc(PATHGZ_MAX_);
  if (!url || !fs || !gz) { free(url); free(fs); free(gz);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }

  strlcpy(url, req->uri, PATH_MAX_);
  char* q = strchr(url, '?'); if (q) *q = 0;              // drop query string
  if (strstr(url, "..")) {                                 // no path traversal
    free(url); free(fs); free(gz);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "nope"); return ESP_OK;
  }
  if (url[0] == 0 || (url[0] == '/' && url[1] == 0)) strlcpy(url, "/index.html", PATH_MAX_);
  else { size_t ul = strlen(url); if (ul && url[ul-1] == '/') strlcat(url, "index.html", PATH_MAX_); }  // dir index

  snprintf(fs, PATH_MAX_,  SITES_ROOT "%s",    url);
  snprintf(gz, PATHGZ_MAX_, SITES_ROOT "%s.gz", url);

  bool served = send_file(req, gz, url, true) || send_file(req, fs, url, false);
  if (!served) {
    if (!strcmp(url, "/index.html")) {                     // fall back to launcher
      httpd_resp_set_type(req, "text/html; charset=utf-8");
      httpd_resp_send(req, FALLBACK, HTTPD_RESP_USE_STRLEN);
    } else {
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found on SD");
    }
  }
  free(url); free(fs); free(gz);
  return ESP_OK;
}

// PUT/POST /api/save/<path> : stream the body to <path>.tmp, then rename over
// the target (whole-file, last-write-wins). The ESP never parses the payload.
static esp_err_t save_handler(httpd_req_t* req) {
  const char* rel = req->uri + strlen("/api/save");       // includes leading '/'
  if (!rel[0] || strstr(rel, "..")) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path"); return ESP_OK;
  }
  char* dst = (char*)malloc(PATH_MAX_);
  char* tmp = (char*)malloc(PATHGZ_MAX_);
  char* buf = (char*)malloc(CHUNK);
  if (!dst || !tmp || !buf) { free(dst); free(tmp); free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
  snprintf(dst, PATH_MAX_,   SITES_ROOT "%s",     rel);
  snprintf(tmp, PATHGZ_MAX_, SITES_ROOT "%s.tmp", rel);

  FILE* f = fopen(tmp, "wb");
  if (!f) { free(dst); free(tmp); free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open fail"); return ESP_OK; }

  int remaining = req->content_len;
  bool err = false;
  while (remaining > 0) {
    int want = remaining < CHUNK ? remaining : CHUNK;
    int r = httpd_req_recv(req, buf, want);
    if (r <= 0) { err = true; break; }
    if ((int)fwrite(buf, 1, r, f) != r) { err = true; break; }
    remaining -= r;
  }
  fclose(f);
  free(buf);
  if (err) { remove(tmp); free(dst); free(tmp);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write fail"); return ESP_OK; }

  remove(dst);                 // FATFS rename won't overwrite an existing file
  if (rename(tmp, dst) != 0) { remove(tmp); free(dst); free(tmp);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename fail"); return ESP_OK; }
  free(dst); free(tmp);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

// GET /api/sniff.json : live WiFi-sniffer table, straight from RAM (never SD).
static esp_err_t sniff_handler(httpd_req_t* req) {
  const size_t cap = 10240;  // 48 devices * ~170 bytes/row + header
  char* buf = (char*)malloc(cap);
  if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
  int n = sniffer_json(buf, cap);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, buf, n);
  free(buf);
  return ESP_OK;
}

bool ls_http_start(void) {
  if (g_srv) return true;
  httpd_config_t hc        = HTTPD_DEFAULT_CONFIG();
  hc.uri_match_fn          = httpd_uri_match_wildcard;
  hc.stack_size            = 10240;   // FATFS/SPI + heap path bufs need headroom
  hc.lru_purge_enable      = true;
  hc.max_uri_handlers      = 5;
  hc.recv_wait_timeout     = 10;
  hc.send_wait_timeout     = 10;
  if (httpd_start(&g_srv, &hc) != ESP_OK) { g_srv = 0; return false; }

  httpd_uri_t save_put  = {}; save_put.uri = "/api/save/*"; save_put.method = HTTP_PUT;  save_put.handler = save_handler;
  httpd_uri_t save_post = {}; save_post.uri = "/api/save/*"; save_post.method = HTTP_POST; save_post.handler = save_handler;
  httpd_uri_t sniff     = {}; sniff.uri  = "/api/sniff.json"; sniff.method = HTTP_GET; sniff.handler = sniff_handler;
  httpd_uri_t getall    = {}; getall.uri = "/*";          getall.method = HTTP_GET;  getall.handler = get_handler;
  httpd_register_uri_handler(g_srv, &save_put);
  httpd_register_uri_handler(g_srv, &save_post);
  httpd_register_uri_handler(g_srv, &sniff);
  httpd_register_uri_handler(g_srv, &getall);
  ESP_LOGI(TAG, "LocalServe httpd started (root %s)", SITES_ROOT);
  return true;
}

void ls_http_stop(void) {
  if (g_srv) { httpd_stop(g_srv); g_srv = 0; ESP_LOGI(TAG, "LocalServe httpd stopped"); }
}

bool ls_http_running(void) { return g_srv != 0; }
