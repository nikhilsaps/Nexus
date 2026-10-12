// sniffer.h -- passive WiFi sniffer / probe + client logger for Nexus.
// Puts the radio in promiscuous mode and hops channels, collecting a live
// table of nearby devices (probe requests, beacons/APs, data clients) in a
// small RAM ring. Nothing is persisted. Assumes wifi_net_start() has already
// initialised the WiFi stack (the sniffer owns set_mode/start/promiscuous).
// Serve the JSON over any active netif (USB-NCM is ideal: the radio stays free
// to hop channels while USB carries the HTTP). The viewer HTML lives on SD.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool     sniffer_start(void);   // promiscuous + channel hop. false on fail.
void     sniffer_stop(void);    // promiscuous off, stop hop, clear table.
bool     sniffer_running(void);

uint8_t  sniffer_channel(void); // channel currently being listened on
uint32_t sniffer_frames(void);  // total frames seen since start
uint32_t sniffer_devices(void); // unique devices currently in the table

// Serialize the live table to JSON into buf. Returns bytes written (excl NUL).
int      sniffer_json(char* buf, size_t len);

#ifdef __cplusplus
}
#endif
