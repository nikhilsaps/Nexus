// deauther.h -- active 802.11 deauth/disassoc transmitter + client discovery
// for Nexus. Floods deauthentication + disassociation management frames spoofed
// from a chosen AP's BSSID while parking the radio on that AP's channel. A
// broadcast target (FF:FF:FF:FF:FF:FF) knocks every client off the AP; a list
// of specific client MACs targets just those stations. The station scanner
// passively discovers which clients are associated with a chosen AP so you can
// pick a few. FOR TESTING YOUR OWN NETWORK AND DEVICES ONLY.
//
// Assumes wifi_net_start() has initialised the WiFi stack (this module owns
// set_mode / promiscuous / set_channel). Uses esp_wifi_80211_tx raw injection
// plus the well-known ieee80211_raw_frame_sanity_check() override that lets
// management frames through the IDF filter.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Station discovery (clients connected to a chosen AP) ----
// Park the radio on `channel` and promiscuously collect client MACs seen in
// frames to/from `bssid` (non-hopping). Call stationscan_stop() before starting
// a deauth burst -- it releases the promiscuous RX hook the burst does not need.
bool     stationscan_start(const uint8_t bssid[6], uint8_t channel);
void     stationscan_stop(void);
bool     stationscan_running(void);
int      stationscan_count(void);     // stations discovered so far
uint32_t stationscan_frames(void);    // frames seen since scan start
// Copy entry i (mac/rssi/packets). Returns false if i is out of range.
bool     stationscan_get(int i, uint8_t mac[6], int8_t* rssi, uint32_t* packets);

// Short human label for a MAC: "(random)" for a privacy/randomized (locally-
// administered) address, else a vendor name from a small built-in OUI table,
// else the 3-byte OUI in hex. `out` is NUL-terminated.
void     mac_label(const uint8_t mac[6], char* out, int len);

// ---- Deauth transmitter ----
// Flood deauth/disassoc at a set of client MACs. targets points at count rows
// of 6 bytes; count==0 (or targets==NULL) broadcasts to all clients.
// Returns false if the radio could not be parked on the channel.
bool     deauther_start_targets(const uint8_t bssid[6], uint8_t channel,
                                const uint8_t (*targets)[6], int count);
// Convenience: single target (NULL = broadcast). Wraps deauther_start_targets.
bool     deauther_start(const uint8_t bssid[6], uint8_t channel, const uint8_t* target);
void     deauther_stop(void);
bool     deauther_running(void);
uint32_t deauther_sent(void);         // deauth/disassoc frames sent since start
uint8_t  deauther_channel(void);      // channel being transmitted on
const uint8_t* deauther_bssid(void);  // 6-byte target BSSID of the active burst
int      deauther_target_count(void); // 0 = broadcast, else number of stations

#ifdef __cplusplus
}
#endif
