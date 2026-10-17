// karma.h -- Karma / probe-response lure for Nexus. Listens for directed probe
// requests (phones hunting for their known networks), harvests the SSIDs, then
// floods beacons + probe responses for each so those "known" networks appear in
// range and clients lean toward them. Raw injection uses esp_wifi_80211_tx (the
// ieee80211_raw_frame_sanity_check override lives in deauther.cpp, program-wide).
// FOR TESTING YOUR OWN DEVICES ONLY.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool     karma_start(void);                 // park radio + harvest/respond
void     karma_stop(void);
bool     karma_running(void);
int      karma_ssid_count(void);            // unique SSIDs harvested
uint32_t karma_sent(void);                  // frames injected since start
uint8_t  karma_channel(void);               // channel last transmitted on
// Copy harvested SSID #i into out (NUL-terminated). Returns false if out of range.
bool     karma_ssid(int i, char* out, int len);

#ifdef __cplusplus
}
#endif
