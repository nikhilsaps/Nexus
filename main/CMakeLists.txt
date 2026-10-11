idf_component_register(
    SRCS "main.cpp" "ble_hid.cpp" "esp_hid_gap.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hid bt nvs_flash esp_event driver esp_adc esp_timer)
