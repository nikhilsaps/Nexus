# Nexus

Firmware for the **M5Stack Cardputer** (ESP32-S3) that turns it into a pocket
BLE cyberdeck: a wireless **keyboard, mouse, and gamepad** with a clean,
minimal UI. Built on pure **ESP-IDF v5.4** (no Arduino) with **M5GFX** for the
display and **NimBLE** for Bluetooth.

## Features

### Top status bar
Always-on bar showing:
- **Battery** percentage, with a charging bolt and a low-battery warning color.
- **Bluetooth** icon: hidden when BLE is off, dim while advertising, cyan when
  a host is connected.
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
```

## License
See [LICENSE](LICENSE).
