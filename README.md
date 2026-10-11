# Nexus

A best-in-class firmware platform for the **M5Stack Cardputer** (ESP32-S3),
built on pure ESP-IDF. The goal: make every piece of Cardputer hardware -
screen, keyboard, mic, speaker, SD, IR, RGB LED, Grove, Wi-Fi/BLE - available
through one clean, efficient, powerful toolset.

## Status
Bootstrapping. Hardware fully mapped and verified; see **[HARDWARE.md](HARDWARE.md)**
for the complete, source-verified pin map and per-peripheral reference (the
starting point for all driver work).

## Target
- Device: M5Stack Cardputer (SKU K132), M5StampS3 / ESP32-S3FN8, 8 MB flash, no PSRAM
- Toolchain: ESP-IDF v5.4.4, built on `electrolux@192.168.1.106`, flashed over COM3

## Layout (planned)
```
Nexus/
  HARDWARE.md      complete hardware reference (done)
  README.md        this file
  main/            app entry + feature modules
  components/      per-peripheral drivers (display, keyboard, audio, ir, sd, ...)
  web/             on-device web UI (served from LittleFS)
```
