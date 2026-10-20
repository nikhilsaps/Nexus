// localserve.cpp - LocalServe HTTP file server (see localserve.h).
#include "localserve.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "safe_replace.h"
#include "sniffer.h"
#include "ble_hid.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

static const char* TAG = "localserve";
static volatile bool g_ls_served = false;  // set on first client request
#define SITES_ROOT   "/sdcard/sites"
#define CHUNK        4096
#define URL_MAX      1024                                   // longest URL path accepted
#define FSPATH_MAX   (sizeof(SITES_ROOT) + URL_MAX + NX_SR_SUFFIX_MAX)
#define SAVE_MAX     (4u * 1024u * 1024u)                   // largest body we will store
#define RECV_RETRIES 3                                      // consecutive recv timeouts tolerated
#define AE_MAX       192                                    // Accept-Encoding bytes we parse

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

// ---------------------------------------------------------------- gzip twin

// One Accept-Encoding entry: name, optional ";q=" weight. "gzip;q=0" is an
// explicit refusal, so a present token is not the same as an acceptable one.
static bool ae_accepts_gzip(const char* ae) {
  bool gzip_seen = false, gzip_ok = false, star_seen = false, star_ok = false;
  const char* p = ae;
  while (*p) {
    while (*p == ',' || *p == ' ' || *p == '\t') p++;
    if (!*p) break;
    const char* tok = p;
    while (*p && *p != ',') p++;
    const char* end = p;

    const char* semi = tok;
    while (semi < end && *semi != ';') semi++;
    const char* name_end = semi;
    while (name_end > tok && (name_end[-1] == ' ' || name_end[-1] == '\t')) name_end--;
    size_t nlen = (size_t)(name_end - tok);

    double q = 1.0;
    for (const char* s = semi; s < end; s++) {
      if (*s != ';') continue;
      const char* a = s + 1;
      while (a < end && (*a == ' ' || *a == '\t')) a++;
      if (a < end && (*a == 'q' || *a == 'Q')) {
        const char* eq = a + 1;
        while (eq < end && (*eq == ' ' || *eq == '\t')) eq++;
        if (eq < end && *eq == '=') q = strtod(eq + 1, NULL);
      }
    }

    if (nlen == 4 && !strncasecmp(tok, "gzip", 4)) { gzip_seen = true; gzip_ok = q > 0.0; }
    else if (nlen == 1 && tok[0] == '*')           { star_seen = true; star_ok = q > 0.0; }
  }
  if (gzip_seen) return gzip_ok;
  return star_seen && star_ok;
}

static bool client_accepts_gzip(httpd_req_t* req) {
  char ae[AE_MAX];
  // A header we cannot read whole (missing or longer than AE_MAX) is treated as
  // "no gzip": identity is always an acceptable encoding, and half a token
  // could hide the ";q=0" that refuses it.
  if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", ae, sizeof ae) != ESP_OK) return false;
  return ae_accepts_gzip(ae);
}

// Errors with no httpd_err_code_t of their own (409/413) go out as an explicit
// status line so the client still gets the right semantics.
static void send_status_json(httpd_req_t* req, const char* status, const char* msg) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Connection", "close");
  char body[128];
  snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}", msg);
  httpd_resp_sendstr(req, body);
}

// ---------------------------------------------------------------- GET

typedef enum {
  SEND_MISSING,   // no such file; caller may try another candidate
  SEND_OK,        // response complete
  SEND_ERR_SENT,  // failed before any body byte; an error response was sent
  SEND_ABORT      // failed mid-body; the connection must be dropped
} send_res_t;

// Stream an SD file in chunks. Nothing larger than CHUNK is ever buffered.
static send_res_t send_file(httpd_req_t* req, const char* fspath, const char* urlpath, bool gz) {
  FILE* f = fopen(fspath, "rb");
  if (!f) {
    // Only "it isn't there" may fall through to the next candidate; a card
    // that is unmounted or failing must not be reported as 404.
    if (errno == ENOENT || errno == ENOTDIR) return SEND_MISSING;
    ESP_LOGW(TAG, "open %s failed: %s", fspath, strerror(errno));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot read file");
    return SEND_ERR_SENT;
  }

  char* buf = (char*)malloc(CHUNK);
  if (!buf) {
    fclose(f);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return SEND_ERR_SENT;
  }

  httpd_resp_set_type(req, mime_for(urlpath));
  if (gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");   // the body depends on it
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  bool read_failed = false, sock_failed = false;
  for (;;) {
    size_t n = fread(buf, 1, CHUNK, f);
    if (n == 0) {
      read_failed = ferror(f) != 0;   // short read without EOF: the card failed
      break;
    }
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { sock_failed = true; break; }
  }
  if (read_failed) ESP_LOGE(TAG, "read %s failed: %s", fspath, strerror(errno));
  free(buf);
  fclose(f);

  // A truncated body must never be terminated with the final empty chunk: that
  // would advertise the short file as complete. Dropping the connection is the
  // only way to tell an HTTP client the chunked body is broken.
  if (read_failed || sock_failed) return SEND_ABORT;

  httpd_resp_send_chunk(req, NULL, 0);
  return SEND_OK;
}

static esp_err_t get_handler(httpd_req_t* req) {
  g_ls_served = true;
  char* url = (char*)malloc(URL_MAX);
  char* fs  = (char*)malloc(FSPATH_MAX);
  char* gzp = (char*)malloc(FSPATH_MAX);
  if (!url || !fs || !gzp) {
    free(url); free(fs); free(gzp);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_OK;
  }
  esp_err_t ret = ESP_OK;

  if (strlcpy(url, req->uri, URL_MAX) >= (size_t)URL_MAX) {
    httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "uri too long");
    goto done;
  }
  { char* q = strchr(url, '?'); if (q) *q = 0; }                       // drop query string

  if (url[0] == 0 || (url[0] == '/' && url[1] == 0)) {
    strlcpy(url, "/index.html", URL_MAX);
  } else {
    size_t ul = strlen(url);
    if (ul && url[ul - 1] == '/' && strlcat(url, "index.html", URL_MAX) >= (size_t)URL_MAX) {
      httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "uri too long");
      goto done;
    }
  }

  // Component-wise check: rejects ".." and "." components, '\' and other FAT
  // separators, control characters, empty components, and the reserved
  // sidecars - so a client can neither escape /sites nor read a .nexus-bak.
  if (!nx_sr_rel_path_ok(url, URL_MAX - 1)) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found on SD");
    goto done;
  }

  if ((size_t)snprintf(fs,  FSPATH_MAX, SITES_ROOT "%s",    url) >= FSPATH_MAX ||
      (size_t)snprintf(gzp, FSPATH_MAX, SITES_ROOT "%s.gz", url) >= FSPATH_MAX) {
    httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "path too long");
    goto done;
  }

  {
    send_res_t r = SEND_MISSING;
    if (client_accepts_gzip(req)) r = send_file(req, gzp, url, true);
    if (r == SEND_MISSING)        r = send_file(req, fs,  url, false);

    if (r == SEND_ABORT) {
      ret = ESP_FAIL;                                   // close the socket
    } else if (r == SEND_MISSING) {
      if (!strcmp(url, "/index.html")) {                // fall back to launcher
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
        httpd_resp_send(req, FALLBACK, HTTPD_RESP_USE_STRLEN);
      } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found on SD");
      }
    }
  }

done:
  free(url); free(fs); free(gzp);
  return ret;
}

// ---------------------------------------------------------------- save

// PUT/POST /api/save/<path> : stream the body into <path>.nexus-tmp, then move
// the previous content to <path>.nexus-bak and the temp file into place
// (whole-file, last-write-wins). The ESP never parses the payload.
//
// Not power-loss atomic - see safe_replace.h. The guarantee is narrower: no
// failure path deletes the previous content. A failed save normally leaves the
// target exactly as it was, and in the one case where even the restoring
// rename fails, the original is reachable in the .nexus-bak sidecar instead of
// under its own name - recoverable, but the target is missing until then.
//
// Every failure exit returns ESP_FAIL: the body may be only partly read, and
// only closing the connection stops the unread remainder from being parsed as
// the next request on a keep-alive socket.
static esp_err_t save_handler(httpd_req_t* req) {
  g_ls_served = true;
  char* rel = (char*)malloc(URL_MAX);
  char* dst = (char*)malloc(FSPATH_MAX);
  char* tmp = (char*)malloc(FSPATH_MAX);
  char* bak = (char*)malloc(FSPATH_MAX);
  char* buf = (char*)malloc(CHUNK);
  if (!rel || !dst || !tmp || !bak || !buf) {
    free(rel); free(dst); free(tmp); free(bak); free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }

  const char* uri_rel = req->uri + strlen("/api/save");    // includes leading '/'
  if (strlcpy(rel, uri_rel, URL_MAX) >= (size_t)URL_MAX) {
    httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "uri too long");
    goto fail;
  }
  { char* q = strchr(rel, '?'); if (q) *q = 0; }           // query string is not filename

  // Same component-wise rules as GET: no traversal, no '\' or control bytes,
  // no "." / ".." components - and no writing to a reserved sidecar, which
  // would let a client overwrite the backup copy of its own file.
  if (!nx_sr_rel_path_ok(rel, URL_MAX - 1)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    goto fail;
  }
  if (req->content_len > SAVE_MAX) {
    send_status_json(req, "413 Payload Too Large", "body too large");
    goto fail;
  }
  if ((size_t)snprintf(dst, FSPATH_MAX, SITES_ROOT "%s", rel) >= FSPATH_MAX ||
      (size_t)snprintf(tmp, FSPATH_MAX, SITES_ROOT "%s" NX_SR_TMP_SUFFIX, rel) >= FSPATH_MAX ||
      (size_t)snprintf(bak, FSPATH_MAX, SITES_ROOT "%s" NX_SR_BAK_SUFFIX, rel) >= FSPATH_MAX) {
    httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "path too long");
    goto fail;
  }

  {
    // Exclusive create: a leftover or concurrent temp file is a reported
    // collision, not two writers interleaving into one file.
    FILE* f = nx_sr_open_exclusive(tmp);
    if (!f) {
      if (errno == EEXIST) {
        ESP_LOGW(TAG, "save %s: %s already exists", rel, tmp);
        send_status_json(req, "409 Conflict", "save already in progress");
      } else {
        ESP_LOGE(TAG, "save %s: open %s failed: %s", rel, tmp, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open fail");
      }
      goto fail;
    }

    size_t remaining = req->content_len;
    int timeouts = 0;
    bool err = false;
    while (remaining > 0) {
      size_t want = remaining < (size_t)CHUNK ? remaining : (size_t)CHUNK;
      int r = httpd_req_recv(req, buf, want);
      if (r == HTTPD_SOCK_ERR_TIMEOUT) {
        if (++timeouts > RECV_RETRIES) { err = true; break; }
        continue;
      }
      if (r <= 0) { err = true; break; }
      timeouts = 0;
      if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { err = true; break; }
      remaining -= (size_t)r;
    }
    // fclose can be where a deferred SD write error finally surfaces, so its
    // result decides whether the temp file is trustworthy.
    if (!nx_sr_fclose_checked(f)) err = true;

    if (err) {
      ESP_LOGE(TAG, "save %s: body write failed", rel);
      remove(tmp);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write fail");
      goto fail;
    }

    nx_sr_result_t rr = nx_sr_replace(tmp, dst, bak);
    if (rr == NX_SR_E_STALE_BAK) {
      // A sidecar from an interrupted save is never deleted to make room: it
      // may be the only copy left. Recovery only moves it back when the target
      // is missing; otherwise the client is told to resolve it.
      if (nx_sr_recover(dst, bak) == NX_SR_OK) rr = nx_sr_replace(tmp, dst, bak);
    }
    if (rr == NX_SR_E_STALE_BAK) {
      ESP_LOGE(TAG, "save %s refused: %s still on card", rel, bak);
      remove(tmp);
      send_status_json(req, "409 Conflict", "previous save left a backup sidecar");
      goto fail;
    }
    if (rr != NX_SR_OK) {
      ESP_LOGE(TAG, "save %s: %s", rel, nx_sr_strerr(rr));
      if (rr == NX_SR_E_RENAME_BAK_KEPT) {
        // The original is parked in the sidecar; keep it and discard the new
        // copy rather than leaving two candidates for the same name.
        ESP_LOGE(TAG, "original left at %s", bak);
      }
      remove(tmp);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, nx_sr_strerr(rr));
      goto fail;
    }
  }

  free(rel); free(dst); free(tmp); free(bak); free(buf);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;

fail:
  free(rel); free(dst); free(tmp); free(bak); free(buf);
  return ESP_FAIL;
}

// GET /api/sniff.json : live WiFi-sniffer table, straight from RAM (never SD).
static esp_err_t sniff_handler(httpd_req_t* req) {
  g_ls_served = true;
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

// --- URL-decode in place (%XX and +). Used by the /api/fetch proxy. ---
static int ls_hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c |= 0x20;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
static void ls_url_decode(char* str) {
  char* o = str;
  for (char* p = str; *p; ) {
    if (*p == '%') { int h = ls_hexv(p[1]), l = ls_hexv(p[2]);
      if (h >= 0 && l >= 0) { *o++ = (char)(h * 16 + l); p += 3; continue; } }
    if (*p == '+') { *o++ = ' '; p++; continue; }
    *o++ = *p++;
  }
  *o = 0;
}

// GET /api/fetch?url=<enc>&ref=<enc>&ua=<enc> : HTTPS proxy. Opens the target
// over TLS (cert bundle), streams the raw body straight back through one reused
// static buffer. Kills CORS because the browser only ever talks to this origin.
// The ESP never parses or fully buffers the body (fits 512KB-RAM, no PSRAM).
static esp_err_t fetch_handler(httpd_req_t* req) {
  g_ls_served = true;
  size_t qlen = httpd_req_get_url_query_len(req) + 1;
  if (qlen <= 1) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no query"); return ESP_OK; }
  if (qlen > 1500) qlen = 1500;
  char* q = (char*)malloc(qlen);
  if (!q) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
  httpd_req_get_url_query_str(req, q, qlen);

  char url[1024] = {0}, ref[512] = {0}, ua[256] = {0};
  httpd_query_key_value(q, "url", url, sizeof url);
  httpd_query_key_value(q, "ref", ref, sizeof ref);
  httpd_query_key_value(q, "ua",  ua,  sizeof ua);
  free(q);
  ls_url_decode(url); ls_url_decode(ref); ls_url_decode(ua);
  if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad url"); return ESP_OK;
  }

  esp_http_client_config_t cfg = {};
  cfg.url               = url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms        = 15000;
  cfg.buffer_size       = 1460;
  cfg.buffer_size_tx    = 1024;
  cfg.user_agent        = ua[0] ? ua : "Mozilla/5.0 (Nexus LocalServe)";
  bool is_post          = (req->method == HTTP_POST);
  cfg.method            = is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  esp_http_client_handle_t cl = esp_http_client_init(&cfg);
  if (!cl) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "client init"); return ESP_OK; }
  if (ref[0]) esp_http_client_set_header(cl, "Referer", ref);

  static char io[2048];   // reused, single-worker httpd: no per-request alloc

  if (is_post) {
    // Relay the incoming body + Content-Type to the upstream as a POST.
    char ctin[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctin, sizeof ctin) == ESP_OK && ctin[0])
      esp_http_client_set_header(cl, "Content-Type", ctin);
    int total = req->content_len;
    if (esp_http_client_open(cl, total) != ESP_OK) {
      esp_http_client_cleanup(cl);
      httpd_resp_set_status(req, "502 Bad Gateway");
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"upstream connect failed\"}");
      return ESP_OK;
    }
    int left = total;
    while (left > 0) {
      int want = left < (int)sizeof io ? left : (int)sizeof io;
      int r = httpd_req_recv(req, io, want);
      if (r <= 0) { esp_http_client_cleanup(cl); return ESP_FAIL; }
      if (esp_http_client_write(cl, io, r) < 0) { esp_http_client_cleanup(cl); return ESP_FAIL; }
      left -= r;
    }
  } else {
    if (esp_http_client_open(cl, 0) != ESP_OK) {
      esp_http_client_cleanup(cl);
      httpd_resp_set_status(req, "502 Bad Gateway");
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"upstream connect failed\"}");
      return ESP_OK;
    }
  }
  esp_http_client_fetch_headers(cl);

  char ctbuf[128] = "application/octet-stream";
  char* ct = NULL;
  if (esp_http_client_get_header(cl, "Content-Type", &ct) == ESP_OK && ct && ct[0]) {
    strncpy(ctbuf, ct, sizeof(ctbuf) - 1); ctbuf[sizeof(ctbuf) - 1] = 0;
  }
  httpd_resp_set_type(req, ctbuf);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  int n; bool sock_ok = true;
  while ((n = esp_http_client_read(cl, io, sizeof io)) > 0) {
    if (httpd_resp_send_chunk(req, io, n) != ESP_OK) { sock_ok = false; break; }
  }
  esp_http_client_close(cl);
  esp_http_client_cleanup(cl);
  if (sock_ok) httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// GET /api/stat : live heap report, so the UI can watch free RAM.
static esp_err_t stat_handler(httpd_req_t* req) {
  g_ls_served = true;
  char body[160];
  snprintf(body, sizeof body,
    "{\"free\":%u,\"min_free\":%u,\"largest_block\":%u}",
    (unsigned)esp_get_free_heap_size(),
    (unsigned)esp_get_minimum_free_heap_size(),
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr(req, body);
  return ESP_OK;
}

// GET /api/ls?path=/sub : list a directory under the web root as JSON:
// {"path":"/sub","entries":[{"name":"x","size":123,"dir":false},...]}. Hidden
// dotfiles and the reserved .nexus-* sidecars are omitted. Same path rules as
// GET/save (no traversal); "/" lists the web root itself.
static int ls_json_name(char* buf, int o, int cap, const char* name) {
  for (const char* p = name; *p && o < cap - 8; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\') { buf[o++] = '\\'; buf[o++] = (char)c; }
    else if (c < 0x20)         { o += snprintf(buf + o, cap - o, "\\u%04x", c); }
    else                        buf[o++] = (char)c;
  }
  return o;
}
static esp_err_t ls_handler(httpd_req_t* req) {
  g_ls_served = true;
  char rel[256] = "/";
  size_t qlen = httpd_req_get_url_query_len(req) + 1;
  if (qlen > 1 && qlen < 240) {
    char* q = (char*)malloc(qlen);
    if (q) {
      httpd_req_get_url_query_str(req, q, qlen);
      char val[200] = {0};
      if (httpd_query_key_value(q, "path", val, sizeof val) == ESP_OK) {
        ls_url_decode(val);
        if (val[0]) strlcpy(rel, val, sizeof rel);
      }
      free(q);
    }
  }
  if (strcmp(rel, "/") != 0 && !nx_sr_rel_path_ok(rel, sizeof(rel) - 1)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path"); return ESP_OK;
  }
  char dirpath[FSPATH_MAX];
  if ((size_t)snprintf(dirpath, sizeof dirpath, SITES_ROOT "%s", rel) >= sizeof dirpath) {
    httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "path too long"); return ESP_OK;
  }
  size_t dl = strlen(dirpath);
  if (dl > sizeof(SITES_ROOT) && dirpath[dl - 1] == '/') dirpath[dl - 1] = 0;

  DIR* d = opendir(dirpath);
  if (!d) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such dir"); return ESP_OK; }

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  { char h[300]; int o = snprintf(h, sizeof h, "{\"path\":\"");
    o = ls_json_name(h, o, (int)sizeof h, rel);
    o += snprintf(h + o, sizeof h - o, "\",\"entries\":[");
    httpd_resp_send_chunk(req, h, o); }

  struct dirent* e;
  bool first = true;
  char full[FSPATH_MAX];
  char line[400];
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    if (nx_sr_is_sidecar(e->d_name)) continue;
    long sz = 0; int isdir = 0;
    if ((size_t)snprintf(full, sizeof full, "%s/%s", dirpath, e->d_name) < sizeof full) {
      struct stat st;
      if (stat(full, &st) == 0) { isdir = S_ISDIR(st.st_mode) ? 1 : 0; sz = (long)st.st_size; }
    }
    int o = snprintf(line, sizeof line, "%s{\"name\":\"", first ? "" : ",");
    o = ls_json_name(line, o, (int)sizeof line, e->d_name);
    o += snprintf(line + o, sizeof line - o, "\",\"size\":%ld,\"dir\":%s}",
                  sz, isdir ? "true" : "false");
    httpd_resp_send_chunk(req, line, o);
    first = false;
  }
  closedir(d);
  httpd_resp_send_chunk(req, "]}", 2);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// DELETE /api/save/<path> : remove one file under the web root. Same path rules
// as save; refuses directories and anything that is not already a plain file.
static esp_err_t delete_handler(httpd_req_t* req) {
  g_ls_served = true;
  char rel[URL_MAX];
  const char* uri_rel = req->uri + strlen("/api/save");   // includes leading /
  if (strlcpy(rel, uri_rel, sizeof rel) >= sizeof rel) {
    httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "uri too long"); return ESP_OK;
  }
  { char* q = strchr(rel, '?'); if (q) *q = 0; }
  if (!nx_sr_rel_path_ok(rel, sizeof(rel) - 1)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path"); return ESP_OK;
  }
  char dst[FSPATH_MAX];
  if ((size_t)snprintf(dst, sizeof dst, SITES_ROOT "%s", rel) >= sizeof dst) {
    httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "path too long"); return ESP_OK;
  }
  struct stat st;
  if (stat(dst, &st) != 0) { send_status_json(req, "404 Not Found", "no such file"); return ESP_OK; }
  if (S_ISDIR(st.st_mode)) { send_status_json(req, "409 Conflict", "is a directory"); return ESP_OK; }
  if (remove(dst) != 0) {
    ESP_LOGE(TAG, "delete %s failed: %s", rel, strerror(errno));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed"); return ESP_OK;
  }
  ESP_LOGI(TAG, "deleted %s", rel);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

bool ls_http_start(void) {
  if (g_srv) return true;
  bhid_release();   // reclaim NimBLE host+controller heap if a BLE module ran
  g_ls_served = false;   // fresh session: no client has connected yet
  httpd_config_t hc        = HTTPD_DEFAULT_CONFIG();
  hc.uri_match_fn          = httpd_uri_match_wildcard;
  hc.stack_size            = 10240;   // FATFS/SPI + heap path bufs need headroom
  hc.lru_purge_enable      = true;
  hc.max_uri_handlers      = 10;
  hc.recv_wait_timeout     = 10;
  hc.send_wait_timeout     = 10;
  if (httpd_start(&g_srv, &hc) != ESP_OK) { g_srv = 0; return false; }

  httpd_uri_t save_put  = {}; save_put.uri  = "/api/save/*";    save_put.method  = HTTP_PUT;  save_put.handler  = save_handler;
  httpd_uri_t save_post = {}; save_post.uri = "/api/save/*";    save_post.method = HTTP_POST; save_post.handler = save_handler;
  httpd_uri_t sniff     = {}; sniff.uri     = "/api/sniff.json"; sniff.method    = HTTP_GET;  sniff.handler     = sniff_handler;
  httpd_uri_t fetch     = {}; fetch.uri     = "/api/fetch";     fetch.method     = HTTP_GET;  fetch.handler     = fetch_handler;
  httpd_uri_t fetch_p   = {}; fetch_p.uri   = "/api/fetch";     fetch_p.method   = HTTP_POST; fetch_p.handler   = fetch_handler;
  httpd_uri_t stat      = {}; stat.uri      = "/api/stat";      stat.method      = HTTP_GET;  stat.handler      = stat_handler;
  httpd_uri_t getall    = {}; getall.uri    = "/*";             getall.method    = HTTP_GET;  getall.handler    = get_handler;
  httpd_uri_t ls_get    = {}; ls_get.uri    = "/api/ls";        ls_get.method    = HTTP_GET;    ls_get.handler    = ls_handler;
  httpd_uri_t del       = {}; del.uri       = "/api/save/*";    del.method       = HTTP_DELETE; del.handler       = delete_handler;

  // The wildcard GET handler shadows everything, so a partially registered
  // server would silently serve 404s for /api/save instead of saving. Unwind
  // instead: a server that cannot route all four URIs must not come up.
  const httpd_uri_t* uris[] = { &save_put, &save_post, &ls_get, &del, &sniff, &fetch, &fetch_p, &stat, &getall };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    esp_err_t e = httpd_register_uri_handler(g_srv, uris[i]);
    if (e != ESP_OK) {
      ESP_LOGE(TAG, "register %s failed: %s", uris[i]->uri, esp_err_to_name(e));
      httpd_stop(g_srv);
      g_srv = 0;
      return false;
    }
  }
  ESP_LOGI(TAG, "LocalServe httpd started (root %s)", SITES_ROOT);
  return true;
}

void ls_http_stop(void) {
  if (g_srv) { httpd_stop(g_srv); g_srv = 0; ESP_LOGI(TAG, "LocalServe httpd stopped"); }
}

bool ls_http_running(void) { return g_srv != 0; }

// True once any client has made a request this session (used by the UI to
// free the display framebuffer and hand its RAM to TLS/the proxy).
bool ls_http_served(void) { return g_ls_served; }
