idf_component_register(
    SRCS "main.cpp" "ble_hid.cpp" "esp_hid_gap.c" "usb_hid.cpp" "sd_store.cpp" "wifi_net.cpp" "localserve.cpp" "usb_net.cpp" "sniffer.cpp" "deauther.cpp" "beacon.cpp" "blespam.cpp" "karma.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_pm esp_hid bt nvs_flash esp_event driver esp_adc esp_timer esp_tinyusb fatfs sdmmc esp_driver_sdspi esp_wifi esp_netif lwip esp_http_server esp_driver_tsens)

# deauther.cpp overrides ieee80211_raw_frame_sanity_check(), which is a strong
# symbol in libnet80211.a on this esp_wifi build. Let our definition win so raw
# management-frame injection (esp_wifi_80211_tx) is not filtered.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--allow-multiple-definition")
