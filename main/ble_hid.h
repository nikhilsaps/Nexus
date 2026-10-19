// Nexus BLE HID (composite keyboard + mouse + gamepad over NimBLE)
#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Start BLE: first call brings NimBLE+HID up, then advertises as `name`.
// Later calls just resume advertising (pass NULL to keep the name).
void bhid_start(const char* name);
// Stop BLE: drop any active link and stop advertising (stack stays up).
void bhid_stop(void);
// Fully release BLE: stop advertising, tear down the NimBLE host and the BT
// controller, returning their heap to the pool. Reversible: a later
// bhid_start() brings the stack back up. Call when switching to a RAM-hungry
// mode (e.g. LocalServe) after a BLE module ran.
void bhid_release(void);
// True while advertising is wanted (device is "on").
bool bhid_active(void);
// True once a central is connected and the link is encrypted.
bool bhid_connected(void);
// Clear all stored bonds (requires BLE to have been started). True on success.
bool bhid_forget_bonds(void);
// Copy the 6-byte BLE identity MAC. False if BLE is not up yet.
bool bhid_mac(uint8_t out[6]);

// Keyboard: modifier bitmap + up to 6 concurrent HID usage codes.
void bhid_key_report(uint8_t modifier, const uint8_t keys6[6]);
// Mouse: button bitmap (bit0 L, bit1 R, bit2 M) + relative dx/dy + wheel.
void bhid_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
// Gamepad: 16-button bitmap + two signed axes (-127..127).
void bhid_send_gamepad(uint16_t buttons, int8_t x, int8_t y);

#ifdef __cplusplus
}
#endif
