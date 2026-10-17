// blespam.h -- BLE advertisement spam for Nexus. Rapidly broadcasts fake
// pairing/proximity advertisements with a fresh random address each burst,
// popping "connect new device" prompts on nearby phones/PCs:
//   Apple   -> iOS proximity-pair popups
//   MS      -> Windows Swift Pair toasts
//   Google  -> Android Fast Pair popups
// Borrows the NimBLE host from ble_hid (bhid_start/bhid_stop); exclusive with
// BLE HID. FOR TESTING YOUR OWN DEVICES ONLY.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BLESPAM_APPLE = 0, BLESPAM_MS = 1, BLESPAM_GOOGLE = 2, BLESPAM_ALL = 3 };

bool     blespam_start(int mode);   // mode = one of BLESPAM_*
void     blespam_stop(void);
bool     blespam_running(void);
uint32_t blespam_sent(void);        // advertisements emitted since start
int      blespam_mode(void);

#ifdef __cplusplus
}
#endif
