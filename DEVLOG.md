# Nexus - Development Log

A running record of what is built and how, so work can resume cleanly.
Last updated: 2026-10-20.

## Status: full toolkit built + flashed; offensive RF suite and LocalServe HTTPS proxy on device

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
| Wi-Fi: Evil-Twin (scan + clone SSID, DNS + HTTP, page from `/captive`) | done, verified |
| LocalServe: Wireless (SD-hosted static sites over Wi-Fi) | done, verified |
| LocalServe: Wired (SD-hosted static sites over USB-CDC-NCM) | done, verified |
| Wi-Fi Sniffer (promiscuous probe/client logger, served as live JSON over NCM) | done, built |
| Wi-Fi: Deauth (per-client + broadcast, associated-station discovery) | done, flashed (ca4322a) |
| Wi-Fi: Beacon spam (fake SSIDs, rotating ch 1/6/11) | done, flashed (ca4322a) |
| Wi-Fi: Karma (probe-response lure, harvest + respond) | done, flashed (ca4322a) |
| BLE: BLE Spam (Apple/Microsoft/Google pairing-prompt ads) | done, flashed (ca4322a) |
| Wi-Fi: Evil-Twin login capture (POSTed creds -> /sites/captive/creds.log) | done, flashed (ca4322a) |
| LocalServe: HTTPS proxy (`/api/fetch` GET+POST, CORS-free) | done, flashed (a4e8677) |
| LocalServe: display-off RAM reclaim (frees ~63KB canvas so TLS fits) | done, flashed (a4e8677) |
| LocalServe: heap stat endpoint (`/api/stat`) | done, flashed (a4e8677) |
| Flash: DIO -> QIO mode (~2x flash read bandwidth) | done, flashed (451aa6b) |

> Lab / own-gear use only for the RF offensive modes (Deauth / Beacon / Karma /
> BLE Spam / Evil-Twin). They transmit; use only on hardware you own or are
> authorized to test.

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
  and `PUT`/`POST /api/save/<path>` (temp file + rename). Also hosts the HTTPS
  proxy `/api/fetch` (GET+POST, CORS-free pass-through via esp_http_client + IDF
  cert bundle), `/api/stat` heap JSON, and the display-off RAM reclaim that frees
  the ~63KB canvas on the first request so a TLS handshake fits. Reuses the Wi-Fi
  scan/connect flow to get on the LAN; mutually exclusive like the other radios.
- `deauther.cpp` - 802.11 deauth/disassoc raw TX (`esp_wifi_80211_tx` with a
  frame-sanity override + `--allow-multiple-definition`) plus associated-station
  discovery. `beacon.cpp` - beacon-spam transmitter (fake SSIDs, rotating ch
  1/6/11). `karma.cpp` - probe-response lure (harvest directed probes, respond).
  `blespam.cpp` - BLE pairing-prompt ad spam (Apple/Microsoft/Google), borrows
  the NimBLE host so it is mutually exclusive with BLE HID. All lab / own-gear
  use only.
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
  BLE       -> Control (Keyboard/Mouse/Gamepad), Duck, BLE Spam, Forget Bonds, Info
  USB       -> Control (Keyboard/Mouse/Gamepad), Duck
  Storage   -> Browse, Card Info, Refresh
  WiFi      -> Scan, Hotspot, Evil-Twin, Deauth, Beacon, Karma, Free Heap
  LocalServe-> Wireless (WiFi), Wired (USB-NCM), Sniffer (NCM)
  Settings  -> Logging, Animation, Temp Bar (on/off),
               Temp Rate (2/5/10/30/60 s), Temp Debug, About
```

## SD card layout (user-provided)

- `/duck/*.txt`     - DuckyScript payloads.
- `/captive/*.html` - captive-portal pages (folder auto-created on first use).
- `/sites/`         - static site(s) LocalServe hosts over the LAN (put an
                      `index.html` here; subfolders and assets are served too).
- `/sites/sniff/`   - the Wi-Fi Sniffer dashboard (`index.html`); captured data
                      itself stays in RAM and is never written to the card.

## AppView states

`VIEW_MENU, VIEW_MSG, VIEW_KB, VIEW_MOUSE, VIEW_PAD, VIEW_FILES, VIEW_FILEACT,
VIEW_TEXTIN, VIEW_CONFIRM, VIEW_DUCK, VIEW_DUCKRUN, VIEW_WIFISCAN, VIEW_WIFISTAT,
VIEW_HOTSPOT, VIEW_CAPPICK, VIEW_CAPTIVE, VIEW_SNIFF`

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
- A dedicated full-screen signal-meter view (currently folded into the connect
  status screen).
- Optional second ESP32-C3 as an ESP-AT Wi-Fi co-processor over UART, to allow
  simultaneous Wi-Fi + BLE/USB (would lift the mutual-exclusivity limit).

(Shipped since first listed here: the offensive Wi-Fi modes - Deauth, Beacon and
Karma - and BLE Spam are now built and flashed; the Evil-Twin captive portal
gained login/form capture to `/sites/captive/creds.log`. See the dated sections
below.)

## Wi-Fi Sniffer notes (2026-10-13)

- `sniffer.cpp`: promiscuous mode + a periodic `esp_timer` hopping channels
  1-13 (250 ms dwell). The promiscuous callback runs in the Wi-Fi task, so it
  only parses the 802.11 header (type/subtype, addr2, SSID IE) and drops a
  record into a 48-entry LRU table under a `portMUX` spinlock - no SD, no heavy
  work in the callback. `sniffer_json()` snapshots the table under the lock,
  then serialises outside it.
- Assumes `wifi_net_start()` has inited the stack (LocalServe subtree does
  this); the sniffer owns `set_mode(STA)/start/promiscuous/set_channel`.
  Teardown (`sniffer_stop`) runs before `wifi_net_stop` in `sync_localserve`.
- Served over USB-NCM so the radio stays free to hop while USB carries HTTP
  (separate hardware - the one combination of promiscuous + httpd that does
  NOT fight over the radio/channel). `/api/sniff.json` lives in `localserve.cpp`;
  the viewer page is a normal SD asset (editable without reflashing).

## Evil-Twin notes (2026-10-13)

- Renamed from "Captive". `act_eviltwin()` runs a Wi-Fi scan; selecting an AP
  copies its SSID into the open captive AP name, then opens the `/captive` page
  picker. Internal enum names (`VIEW_CAPTIVE`/`VIEW_CAPPICK`) and the `/captive`
  SD path are unchanged; only the user-facing strings and the clone step are new.
- Verified on hardware: clones a chosen SSID and serves the page. Auto-join is
  limited by design (open-vs-WPA2 mismatch, no deauth) - expected, not a bug.

## 2026-10-15 - UI and LocalServe reliability optimization

- Dirty redraws across tool screens and non-blocking menu animation; existing
  FreeRTOS input delay preserved. On-demand Settings > Memory diagnostics.
- Opt-in persisted Power Save (80-240 MHz DFS), default OFF; no light sleep.
- Wired LocalServe no longer starts WiFi. Wireless hosting skips the unrelated
  internet probe. Partial WiFi startup/scan failures clean up resources.
- Bounded folder navigation, mouse release on exit, text-entry invalidation,
  storage allocation cleanup, temperature error display and quieter samples.
- LocalServe checks write/flush/close errors; backup/restore replacement keeps
  originals on failure. Existing backup sidecars are never blindly deleted.
  Failed restores leave the original in .nexus-bak; this is NOT power-loss atomic.
- Saves limited to 4 MiB; exclusive temp creation; FAT-safe path guards; gzip
  negotiation, I/O error reporting and handler-registration cleanup.
- A retained backup blocks later saves (409) when both copies exist. Resolve
  deliberately on SD. URLs are still not percent-decoded. No authentication
  added: use LocalServe on trusted networks only.
- ESP-IDF 5.4.4 build passed, app 1,440,080 bytes (54% app partition free).
- COM3 app-only flash passed with esptool hash verification. NVS/layout unchanged.
- Host checks: 12 Python source guards; safe_replace suite 110 checks; independent
  recovery tests, path boundary tests and eight gzip cases all passed. Native
  C++ tests ran with AddressSanitizer/UndefinedBehaviorSanitizer on Linux g++.
- Actual previous application read back before flash and compared against saved
  binary; rollback retained in Aki workspace nexus-review/rollback and remote
  build/pre-optimization-device-app.bin.
- Interactive UI, HID, SD HTTP saves and optional DFS behavior not verified on
  hardware in this pass. No measured battery/latency claims.

## 2026-10-17 - Offensive RF suite + Evil-Twin login capture (ca4322a)

- Added four active-radio tools (lab / own-gear only). All obey the lazy teardown
  rule: the tool transmits only while its screen is on the nav stack, stops on `,`
  or `` ` ``.
- `deauther.cpp` (Wi-Fi > Deauth): pick an AP from a scan, passively discover its
  associated stations (vendor-labelled, RSSI + packet counts), then `d` to deauth
  a selected client or broadcast `FF:FF:...` to knock all clients off, radio parked
  on the AP channel. Raw deauth/disassoc frames via `esp_wifi_80211_tx`; a
  program-wide `ieee80211_raw_frame_sanity_check()` override plus the linker
  `--allow-multiple-definition` flag in `main/CMakeLists.txt` get them past the IDF
  frame filter. Live frames-sent counter.
- `beacon.cpp` (Wi-Fi > Beacon): floods beacon frames for a built-in fake-SSID list,
  rotating ch 1/6/11; each SSID keeps a stable locally-administered BSSID. Live
  beacon count + channel.
- `karma.cpp` (Wi-Fi > Karma): listens for directed probe requests, harvests the
  SSIDs phones are hunting for, and floods beacons + probe responses for each. Live
  harvested-SSID and frames-sent counts.
- `blespam.cpp` (BLE > BLE Spam): rapid fake pairing/proximity ads with a fresh
  random address per burst - Apple proximity-pair, Windows Swift Pair, Android Fast
  Pair (or all three). Borrows the NimBLE host, so mutually exclusive with BLE
  Control. Live advertisements-sent counter.
- Evil-Twin gained login/form capture: any login form POSTed by a joined client is
  written to `/sites/captive/creds.log` with a running saved-count on screen. `g`
  on the page picker drops three built-in phishing templates onto the card.
- Built + flashed to COM3. Lab / own-gear / authorized-test use only.

## 2026-10-19 - LocalServe HTTPS proxy + RAM reclaim + API docs (a4e8677)

- `/api/fetch` HTTPS proxy added to `localserve.cpp` (GET + POST wired to one
  handler): a dumb CORS-free pass-through via `esp_http_client` + the IDF cert
  bundle, streamed through a single reused 2KB buffer, never buffering the whole
  body. `url`/`ref`/`ua` in the query string; POST relays body + Content-Type to
  the upstream. 15s timeout; upstream connect failure -> 502. Open LAN relay, no
  host allowlist (fine for a personal device).
- Display-off RAM reclaim: the first HTTP request frees the ~63KB display canvas
  and sleeps the panel, lifting the largest contiguous block from ~31KB to ~63KB
  so a TLS handshake fits in the 512KB/no-PSRAM budget. Screen goes dark by design;
  restart to return to the menu.
- `/api/stat` returns `{free, min_free, largest_block}` for live heap watching.
- API docs published in the companion Nexus-Extras repo: LOCALSERVE-API-GUIDE.md
  (LAN serving + proxy + save + stat) and CAPTIVE-PORTAL-API-GUIDE.md (isolated
  Evil-Twin store). These are the contract any served web app (Atlas, Akashic)
  builds against.

## 2026-10-20 - Flash read speed (451aa6b)

- Switched flash from DIO to QIO mode for ~2x flash read bandwidth on the
  ESP32-S3; the bootloader upgrades itself at boot.
