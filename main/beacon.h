// beacon.h -- WiFi beacon-spam transmitter for Nexus. Floods 802.11 beacon
// frames for a built-in list of SSIDs, rotating channels 1/6/11 so the fake
// networks show up in nearby WiFi lists regardless of their channel. Each SSID
// gets a stable locally-administered BSSID so it appears as one steady network.
// Uses esp_wifi_80211_tx raw injection (the ieee80211_raw_frame_sanity_check
// override lives in deauther.cpp and applies program-wide).
// FOR TESTING YOUR OWN DEVICES ONLY.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool     beacon_start(void);     // park radio + start the burst timer
void     beacon_stop(void);
bool     beacon_running(void);
uint32_t beacon_sent(void);      // beacon frames sent since start
int      beacon_count(void);     // number of fake SSIDs
uint8_t  beacon_channel(void);   // channel last transmitted on

#ifdef __cplusplus
}
#endif
