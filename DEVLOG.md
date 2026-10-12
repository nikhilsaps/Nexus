# Nexus - Development Log

A running record of what is built and how, so work can resume cleanly.
Last updated: 2026-10-13.

## Status: all core features complete and verified on hardware

| Feature | State |
|---------|-------|
| BLE HID (keyboard / mouse / gamepad) | done, verified |
| USB HID (keyboard / mouse / gamepad) | done, verified |
| Ducky / BadUSB (DuckyScript from `/duck`) | done, verified |
| Storage browser (microSD, SPI2) | done, verified |
| Wi-Fi: Scan | done, verified |
| Wi-Fi: Connect + internet (HTTP-204) test | done, verified |
| Wi-Fi: live RSSI signal meter | done (on the connect status screen) |
| Die-temperature readout in status bar | done, verified 33C (toggle + 2/5/10/30/60s rate, NVS-saved) |
| Wi-Fi: NTP clock sync | done |
| Wi-Fi: Hotspot (SoftAP) | done |
| Wi-Fi: Captive portal (DNS + HTTP, page from `/captive`) | done |
| LocalServe: Wireless (SD-hosted static sites over Wi-Fi) | done, verified |
| LocalServe: Wired (SD-hosted static sites over USB-CDC-NCM) | done, verified |

## Design principles (do not break these)

1. **Lazy + mutually exclusive subsystems.** A radio (BLE / USB / Wi-Fi) or the
   SD card is started only while its menu subtree is on the navigation stack and
   fully torn down on leaving. Pattern: a `sync_*()` function compares
   `stack_has(&menu)` to a static `was` flag and starts/stops + mallocs/frees
   accordingly. This keeps the idle root menu near-zero dynamic RAM.
2. **RAM is the only real constraint** (512 KB SRAM, no PSRAM, 8 MB flash). The
   64 KB display canvas + one radio must coexist. Wi-Fi + lwIP and BLE/USB are
   never up at the same time. Measured free heap with Wi-Fi running: ~156 KB.
3. **ASCII-only source**, LF newlines, `-Werror=format-truncation` and
   `-Werror=misleading-indentation` are fatal. snprintf from a bounded array
   into a smaller buffer triggers format-truncation; launder through a
   `const char*` or enlarge the destination.

## Code map

- `main.cpp` - everything UI: palette, keyboard scan, battery, transport
  dispatch (BLE vs USB), menu model, `AppView` enum + per-screen `tick_*` /
  `draw_*`, and `app_main` with the ~62 Hz loop.
- `ble_hid.cpp` / `esp_hid_gap.c` - NimBLE composite HID + GAP/pairing.
- `usb_hid.cpp` - TinyUSB composite HID.
- `sd_store.cpp` - microSD on its own SPI2 bus; mount, list, read, write ops.
- `wifi_net.cpp` - Wi-Fi backend: start/stop, scan, connect, STA ip info,
  internet probe (HTTP 204 to detect real internet / captive portals), RSSI,
  SoftAP, captive portal (wildcard HTTP server + a UDP:53 DNS catch-all task
  answering every query with 192.168.4.1), and SNTP time sync.
- `localserve.cpp` - esp_http_server that serves static files from `/sites` on
  the SD card: content type by extension, chunked fopen streaming for big files,
  `.gz`-twin preference, a built-in launcher when `/sites` has no `index.html`,
  and `PUT`/`POST /api/save/<path>` (temp file + rename). Reuses the Wi-Fi
  scan/connect flow to get on the LAN; mutually exclusive like the other radios.
- `usb_net.cpp` - the Wired transport for LocalServe. Brings up a USB-CDC-NCM
  device via esp_tinyusb (`CONFIG_TINYUSB_NET_MODE_NCM`), wired to an
  `esp_netif` configured as a static gateway at 192.168.7.1/24 with a DHCP
  server. Two callbacks bridge L2 frames: TX via `tinyusb_net_send_sync`, RX via
  `esp_netif_receive`. Shares the single USB-OTG PHY with USB HID (so
  `uhid_stop()` runs first), and exposes `usb_net_last_err()` for on-screen
  diagnostics since serial is dead after display init.

## Menu tree

```
NEXUS (root)
  BLE       -> Control (Keyboard/Mouse/Gamepad), Duck, Forget Bonds, Info
  USB       -> Control (Keyboard/Mouse/Gamepad), Duck
  Storage   -> Browse, Card Info, Refresh
  WiFi      -> Scan, Hotspot, Captive, Free Heap
  LocalServe-> Wireless (WiFi), Wired (USB-NCM)
  Settings  -> Logging, Animation, Temp Bar (on/off),
               Temp Rate (2/5/10/30/60 s), Temp Debug, About
```

## SD card layout (user-provided)

- `/duck/*.txt`     - DuckyScript payloads.
- `/captive/*.html` - captive-portal pages (folder auto-created on first use).
- `/sites/`         - static site(s) LocalServe hosts over the LAN (put an
                      `index.html` here; subfolders and assets are served too).

## AppView states

`VIEW_MENU, VIEW_MSG, VIEW_KB, VIEW_MOUSE, VIEW_PAD, VIEW_FILES, VIEW_FILEACT,
VIEW_TEXTIN, VIEW_CONFIRM, VIEW_DUCK, VIEW_DUCKRUN, VIEW_WIFISCAN, VIEW_WIFISTAT,
VIEW_HOTSPOT, VIEW_CAPPICK, VIEW_CAPTIVE`

The on-screen text-entry (`VIEW_TEXTIN`) is shared: `g_tin_mode` selects the
purpose (file rename/new, Wi-Fi password, hotspot SSID/pass, captive name) and
`commit_text()` routes to the right handler.

## Build

Standard ESP-IDF v5.4:

```sh
idf.py set-target esp32s3   # only after changing sdkconfig.defaults
idf.py build
idf.py -p <PORT> flash
```

Components required (main/CMakeLists.txt REQUIRES): esp_hid, bt, nvs_flash,
esp_event, driver, esp_adc, esp_timer, esp_tinyusb, fatfs, sdmmc,
esp_driver_sdspi, esp_wifi, esp_netif, lwip, esp_http_server.

## Known limits / notes

- Ducky assumes a US keyboard layout on the target host.
- Captive portal page is read into RAM (cap 24 KB) then the SD is unmounted.
- Internet test uses an HTTP 204 probe, so a captive portal / 200 response reads
  as "no internet", which is correct.
- NTP time is set to IST (UTC+5:30) and is lost on power cycle (no RTC battery).
- LocalServe Wired uses NCM (not RNDIS): esp_tinyusb ships an NCM descriptor and
  Kconfig out of the box, RNDIS would need hand-written descriptors. Gotcha: the
  component only falls back to its built-in default descriptor when no HID/MIDI/
  RNDIS/DFU/BTH class is compiled in. Nexus compiles in HID, so that fallback is
  off and `tinyusb_driver_install` returns ESP_ERR_INVALID_ARG on a NULL config -
  `usb_net.cpp` must (and does) supply its own NCM device/config/string
  descriptors.
- Any USB mode (HID or Wired/NCM) seizes the shared USB-OTG PHY, so the serial
  console disappears while it is active. To reflash after testing Wired,
  power-cycle the board first so the port re-enumerates as serial-JTAG.
- Screen left-margin: content is shifted right by `LMARGIN` (main.cpp, default
  12 px) and x=0..LMARGIN-1 is blacked out at boot. This masks a damaged column
  near the left edge on one physical unit; set `LMARGIN = 0` to restore
  full-width output on an undamaged screen.

## Ideas / not yet built

- LAN host scanner (ping-sweep the subnet once connected).
- Captive portal with form capture / multiple pages / asset serving.
- A dedicated full-screen signal-meter view (currently folded into the connect
  status screen).
- Optional second ESP32-C3 as an ESP-AT Wi-Fi co-processor over UART, to allow
  simultaneous Wi-Fi + BLE/USB (would lift the mutual-exclusivity limit).
- "Lab only" offensive Wi-Fi modes (deauth, beacon spam, packet sniffer, Evil
  Twin) - intentionally deferred; use only on networks you own/are authorized.
