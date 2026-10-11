// Nexus BLE HID implementation (NimBLE transport via esp_hid).
#include "ble_hid.h"
#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"

extern "C" void ble_store_config_init(void);
extern "C" uint16_t g_nexus_conn;   // set in esp_hid_gap.c on connect

static const char* TAG = "bhid";
static esp_hidd_dev_t* s_dev = NULL;
static volatile bool s_connected = false;
static bool s_up   = false;   // NimBLE stack brought up once
static bool s_want = false;   // advertising is wanted (inside Control)

// ---- Composite HID report map: KB=ID1, Mouse=ID2, Gamepad=ID3 ----
static const uint8_t s_report_map[] = {
    // Keyboard (Report ID 1): 8-byte in (mod,resvd,6 keys), 1-byte LED out
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,

    // Mouse (Report ID 2): buttons(3)+pad(5), X, Y, Wheel
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,

    // Gamepad (Report ID 3): 16 buttons + X, Y
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x03,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0xC0,
};

static esp_hid_raw_report_map_t s_maps[] = {
    { .data = s_report_map, .len = sizeof(s_report_map) }
};

static esp_hid_device_config_t s_cfg = {
    .vendor_id         = 0x16C0,
    .product_id        = 0x05DF,
    .version           = 0x0100,
    .device_name       = "Nexus",
    .manufacturer_name = "Nexus",
    .serial_number     = "0001",
    .report_maps       = s_maps,
    .report_maps_len   = 1,
};

// esp_hid_gap.c (nimble) calls this when encryption completes = link ready.
extern "C" void ble_hid_task_start_up(void) { s_connected = true; }
extern "C" void ble_hid_task_shut_down(void) { }

static void hidd_cb(void* args, esp_event_base_t base, int32_t id, void* ed) {
    esp_hidd_event_t ev = (esp_hidd_event_t)id;
    switch (ev) {
    case ESP_HIDD_START_EVENT:
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        break; // wait for encryption -> ble_hid_task_start_up
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        if (s_want) esp_hid_ble_gap_adv_start();
        break;
    default:
        break;
    }
}

static void host_task(void* p) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void bhid_bringup(void) {
    ESP_ERROR_CHECK(esp_hid_gap_init(HID_DEV_MODE));
    ble_svc_gap_device_name_set(s_cfg.device_name);
    ESP_ERROR_CHECK(esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GENERIC, s_cfg.device_name));
    ESP_ERROR_CHECK(esp_hidd_dev_init(&s_cfg, ESP_HID_TRANSPORT_BLE, hidd_cb, &s_dev));
    esp_hidd_dev_battery_set(s_dev, 100);
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    nimble_port_freertos_init(host_task);
}

// Start BLE: first call brings the stack up; later calls just re-advertise.
void bhid_start(const char* name) {
    if (name) s_cfg.device_name = name;
    s_want = true;
    if (!s_up) { bhid_bringup(); s_up = true; }
    else       { esp_hid_ble_gap_adv_start(); }
}

// Stop BLE: drop any link and stop advertising (stack stays resident).
void bhid_stop(void) {
    s_want = false;
    if (!s_up) return;
    if (s_connected && g_nexus_conn != 0xFFFF) {
        ble_gap_terminate(g_nexus_conn, 0x13 /* remote user terminated */);
    }
    ble_gap_adv_stop();
}

bool bhid_active(void)    { return s_want; }
bool bhid_connected(void) { return s_connected; }

// Wipe all stored bonds (fixes stale pairings on hosts). Needs stack up.
bool bhid_forget_bonds(void) {
    if (!s_up) return false;
    return ble_store_clear() == 0;
}

// Copy the device's BLE identity MAC. False if the stack is not up yet.
bool bhid_mac(uint8_t out[6]) {
    if (!s_up) return false;
    return ble_hs_id_copy_addr(BLE_OWN_ADDR_PUBLIC, out, NULL) == 0;
}

void bhid_key_report(uint8_t modifier, const uint8_t keys6[6]) {
    if (!s_connected) return;
    uint8_t b[8] = {0};
    b[0] = modifier;
    for (int i = 0; i < 6; i++) b[2 + i] = keys6[i];
    esp_hidd_dev_input_set(s_dev, 0, 1, b, 8);
}

void bhid_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    if (!s_connected) return;
    uint8_t b[4] = { buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };
    esp_hidd_dev_input_set(s_dev, 0, 2, b, 4);
}

void bhid_send_gamepad(uint16_t buttons, int8_t x, int8_t y) {
    if (!s_connected) return;
    uint8_t b[4] = { (uint8_t)(buttons & 0xFF), (uint8_t)(buttons >> 8), (uint8_t)x, (uint8_t)y };
    esp_hidd_dev_input_set(s_dev, 0, 3, b, 4);
}
