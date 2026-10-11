// Nexus USB HID (composite keyboard + mouse + gamepad over TinyUSB / native USB).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Start USB: install TinyUSB and enumerate as a composite HID device.
// `name` is accepted only for API parity with BLE (the USB device name is
// fixed in the string descriptors). Safe to call repeatedly.
void uhid_start(const char* name);
// Stop USB: fully uninstall TinyUSB (tears down the stack and the USB PHY).
void uhid_stop(void);
// True while USB HID is wanted (driver installed / "on").
bool uhid_active(void);
// True once a USB host has enumerated and mounted the device.
bool uhid_connected(void);

// Keyboard: modifier bitmap + up to 6 concurrent HID usage codes.
void uhid_key_report(uint8_t modifier, const uint8_t keys6[6]);
// Mouse: button bitmap (bit0 L, bit1 R, bit2 M) + relative dx/dy + wheel.
void uhid_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
// Gamepad: 16-button bitmap + two signed axes (-127..127).
void uhid_send_gamepad(uint16_t buttons, int8_t x, int8_t y);

#ifdef __cplusplus
}
#endif
