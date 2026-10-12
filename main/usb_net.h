// usb_net.h - LocalServe Wired transport for Nexus.
// Brings the single USB-OTG port up as a USB-CDC-NCM network adapter: the host
// (Win11/macOS/Linux has a built-in NCM driver) sees a new wired adapter, the
// ESP is the gateway + DHCP server at 192.168.7.1, and LocalServe is reachable
// at http://192.168.7.1/. Mutually exclusive with USB-HID (one USB-OTG PHY):
// the caller stops USB-HID before usb_net_start(). BLE HID is unaffected.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool usb_net_start(void);   // install NCM + netif + DHCP server. false on fail.
void usb_net_stop(void);    // tear USB-net down, free the PHY (idempotent).
bool usb_net_running(void);
void usb_net_ip(char* out, int cap);  // the host-facing URL host (192.168.7.1).
const char* usb_net_last_err(void);   // which step failed + esp_err (diagnostic).

#ifdef __cplusplus
}
#endif
