idf_component_register(
    SRCS "main.cpp" "ble_hid.cpp" "esp_hid_gap.c" "usb_hid.cpp" "sd_store.cpp" "wifi_net.cpp" "localserve.cpp" "usb_net.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_hid bt nvs_flash esp_event driver esp_adc esp_timer esp_tinyusb fatfs sdmmc esp_driver_sdspi esp_wifi esp_netif lwip esp_http_server esp_driver_tsens)
