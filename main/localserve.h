// localserve.h - LocalServe HTTP file server for Nexus.
// Serves a static multi-site tree from the microSD (/sdcard/sites), streaming
// files in small chunks (the ESP never buffers a whole file in RAM, and never
// parses site content). Transport-agnostic: the caller brings up WiFi (STA) or
// USB-RNDIS first, then calls ls_http_start(). Features: gzip-twin preference,
// Content-Type by extension, a built-in fallback launcher, and a tiny
// whole-file save API (PUT/POST /api/save/<path> -> temp file + rename).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ls_http_start(void);   // start esp_http_server (idempotent). false on fail.
void ls_http_stop(void);    // stop + free the server (idempotent).
bool ls_http_running(void);
bool ls_http_served(void);  // true after the first client request this session

#ifdef __cplusplus
}
#endif
