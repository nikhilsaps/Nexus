# Nexus

Firmware for the **M5Stack Cardputer** (ESP32-S3) that turns it into a pocket
cyberdeck: a **keyboard, mouse, and gamepad** that drives any host over either
**Bluetooth LE** or a **native USB** cable, with a clean, minimal UI. Built on
pure **ESP-IDF v5.4** (no Arduino) with **M5GFX** for the display, **NimBLE**
for Bluetooth, and **TinyUSB** for native USB HID.

## Features

### Top status bar
Always-on bar showing:
- **Battery** percentage, with a charging bolt and a low-battery warning color.
- **Bluetooth** icon: hidden when BLE is off, dim while advertising, cyan when
  a host is connected.
- **USB** label: hidden when USB is off, dim while waiting for a host, cyan
  once the host mounts the device.
- **Wi-Fi** icon with signal strength (shown only when Wi-Fi is in use).

### Menu & navigation
A smooth, scrollable menu driven entirely from the Cardputer keys:
- `;` up, `.` down
- `/` or `Enter` select
- `,` back, `` ` `` jump to root

### BLE > Control (the HID device)
BLE stays **off at boot**. It powers on only when you enter `BLE > Control`,
and powers back down when you leave, so the radio is never on unnecessarily.
While advertising, the pairing **PIN is shown on screen**.

- **Keyboard** - full transparent typing to the host. `Aa` = shift/uppercase,
  `Enter` sends a real Enter, `` ` `` exits. Keystrokes echo on screen.
- **Mouse** - `;./,//` move the cursor with adjustable speed
  (`s` cycles **SLOW / MED / FAST**). Clicks: `space` = left, `m` = right,
  `n` = middle. Scroll: `k` up, `j` down. Live `L R M` and scroll indicators
  show what's firing.
- **Gamepad** - 2 analog axes (`;./,//`) + 8 buttons
  (`a s d f q w space enter`) with a live on-screen button grid.

### BLE management
- **Forget Bonds** - clears stored pairings in one tap (fixes stale bonds on a
  host so you can re-pair cleanly).
- **Info** - device name, BLE MAC address, and live connection state.

Pairing uses a fixed passkey (shown on screen) with bonding, so your host
remembers Nexus and reconnects automatically.

### USB > Control (native USB HID)
USB mirrors BLE: the same keyboard, mouse, and gamepad screens, but reports go
out over a native **USB HID** connection instead of Bluetooth. Enter
`USB > Control` and the Cardputer enumerates on the host as a composite HID
device named **Nexus** (no pairing, no PIN).

USB is **lazy**, exactly like BLE: it is off at boot and installs only when you
enter `USB > Control`. Because the ESP32-S3 USB-C port is shared between the
serial/flashing interface and USB HID, the device uses serial on a normal boot
(so it stays flashable) and switches to USB HID only inside this menu. Restart
the Cardputer to return to the flashable serial mode.

## Hardware
- Device: M5Stack Cardputer (M5StampS3 / ESP32-S3FN8, 8 MB flash, no PSRAM)
- Display: ST7789 240x135
- Toolchain: ESP-IDF v5.4

## Build & flash
```
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

## Project layout
```
Nexus/
  README.md             this file
  CMakeLists.txt        ESP-IDF project
  sdkconfig.defaults    build config (source of truth)
  partitions.csv        partition table
  main/
    main.cpp            boot, UI, menu, status bar, HID screens
    ble_hid.cpp/.h      composite NimBLE HID (keyboard + mouse + gamepad)
    esp_hid_gap.c/.h    BLE GAP / advertising / pairing
    usb_hid.cpp/.h      composite TinyUSB HID (keyboard + mouse + gamepad)
```

## License
See [LICENSE](LICENSE).
