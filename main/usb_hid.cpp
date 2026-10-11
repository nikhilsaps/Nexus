// Nexus USB HID implementation (native USB via TinyUSB / esp_tinyusb).
#include "usb_hid.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

// ---- HID report descriptor: KB=ID1, Mouse=ID2, Gamepad=ID3 ----
static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(2)),
    TUD_HID_REPORT_DESC_GAMEPAD(HID_REPORT_ID(3)),
};

// ---- String descriptors (index 0 = LANGID 0x0409 English) ----
static const char s_langid[2] = { 0x09, 0x04 };
static const char* s_str_desc[] = {
    s_langid,       // 0: language id
    "Nexus",        // 1: Manufacturer
    "Nexus",        // 2: Product
    "0001",         // 3: Serial
    "Nexus HID",    // 4: HID interface
};

// ---- Configuration descriptor (one HID interface, IN endpoint 0x81) ----
#define NEXUS_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, NEXUS_CFG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(s_hid_report_desc), 0x81,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
};

// ---- Device descriptor ----
static const tusb_desc_device_t s_dev_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x16C0,
    .idProduct          = 0x05DF,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static bool s_want = false;
static bool s_installed = false;

// ---- TinyUSB device HID callbacks (weak overrides, C linkage) ----
extern "C" uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return s_hid_report_desc;
}
extern "C" uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
        hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}
extern "C" void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
        hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

// Install TinyUSB on first call; later calls are no-ops while installed.
void uhid_start(const char* name) {
    (void)name;
    s_want = true;
    if (s_installed) return;
    tinyusb_config_t cfg = {};
    cfg.device_descriptor        = &s_dev_desc;
    cfg.string_descriptor        = s_str_desc;
    cfg.string_descriptor_count  = sizeof(s_str_desc) / sizeof(s_str_desc[0]);
    cfg.external_phy             = false;
    cfg.configuration_descriptor = s_cfg_desc;
    if (tinyusb_driver_install(&cfg) == ESP_OK) s_installed = true;
}

// Fully tear TinyUSB down (frees the USB PHY so a restart returns to serial).
void uhid_stop(void) {
    s_want = false;
    if (!s_installed) return;
    tinyusb_driver_uninstall();
    s_installed = false;
}

bool uhid_active(void)    { return s_want; }
bool uhid_connected(void) { return s_installed && tud_mounted(); }

void uhid_key_report(uint8_t modifier, const uint8_t keys6[6]) {
    if (!s_installed || !tud_mounted()) return;
    tud_hid_keyboard_report(1, modifier, (uint8_t*)keys6);
}

void uhid_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    if (!s_installed || !tud_mounted()) return;
    tud_hid_mouse_report(2, buttons, dx, dy, wheel, 0);
}

void uhid_send_gamepad(uint16_t buttons, int8_t x, int8_t y) {
    if (!s_installed || !tud_mounted()) return;
    tud_hid_gamepad_report(3, x, y, 0, 0, 0, 0, 0, (uint32_t)buttons);
}
