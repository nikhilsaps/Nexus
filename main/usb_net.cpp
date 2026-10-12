// usb_net.cpp - USB-CDC-NCM network adapter + gateway for LocalServe Wired.
// esp_tinyusb builds the NCM USB device from its default descriptors (we pass a
// zeroed tinyusb_config_t). We own an esp_netif at 192.168.7.1/24 with a DHCP
// server, and shuttle L2 frames both ways: netif TX -> tinyusb_net_send_sync,
// USB RX -> esp_netif_receive. HTTP (esp_http_server) binds 0.0.0.0 so it is
// served on this interface automatically once the link is up.
#include "usb_net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_net.h"
#include "tusb.h"

// --- NCM-only USB descriptors. HID is compiled in (CONFIG_TINYUSB_HID_COUNT=1),
// so esp_tinyusb refuses its built-in default descriptor and demands our own. ---
enum { ITF_NUM_NET = 0, ITF_NUM_NET_DATA, ITF_NUM_TOTAL };
enum { EPNUM_NET_NOTIF = 1, EPNUM_NET_DATA = 2 };
enum { STRID_LANGID = 0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL,
       STRID_NET_INTERFACE, STRID_MAC };

#define NCM_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN)

static const tusb_desc_device_t s_net_dev = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x4001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_net_cfg[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, NCM_CFG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_NET, STRID_NET_INTERFACE, STRID_MAC,
                           (0x80 | EPNUM_NET_NOTIF), 64, EPNUM_NET_DATA,
                           (0x80 | EPNUM_NET_DATA), 64, CFG_TUD_NET_MTU),
};

static const char s_net_langid[2] = { 0x09, 0x04 };
static char s_mac_str[13] = "020000000000";  // filled at runtime (12 hex chars)
static const char* s_net_str[] = {
    s_net_langid,     // 0: LANGID
    "Nexus",          // 1: Manufacturer
    "Nexus USB-NCM",  // 2: Product
    "0001",           // 3: Serial
    "USB net",        // 4: NET interface
    s_mac_str,        // 5: MAC
};

static const char* TAG = "usbnet";
static esp_netif_t* s_netif = NULL;
static bool s_up = false;
static char s_err[96] = "";

const char* usb_net_last_err(void) { return s_err; }

// lwip wants to transmit a frame out the USB interface.
static esp_err_t netif_transmit(void* h, void* buffer, size_t len) {
    (void)h;
    tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
    return ESP_OK;
}

// lwip is done with a buffer we handed to esp_netif_receive().
static void netif_free_rx(void* h, void* buffer) {
    (void)h;
    free(buffer);
}

// A frame arrived from the USB host; copy it into lwip.
static esp_err_t usb_recv_cb(void* buffer, uint16_t len, void* ctx) {
    (void)ctx;
    if (!s_netif) return ESP_OK;
    void* buf = malloc(len);
    if (!buf) return ESP_ERR_NO_MEM;
    memcpy(buf, buffer, len);
    if (esp_netif_receive(s_netif, buf, len, buf) != ESP_OK) {
        free(buf);
    }
    return ESP_OK;
}

bool usb_net_start(void) {
    if (s_up) return true;
    s_err[0] = 0;
    esp_err_t ei = esp_netif_init();  // idempotent; WiFi path already did this

    static esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    ip.ip.addr      = ESP_IP4TOADDR(192, 168, 7, 1);
    ip.gw.addr      = ESP_IP4TOADDR(192, 168, 7, 1);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);

    esp_netif_inherent_config_t base;
    memset(&base, 0, sizeof(base));
    base.flags      = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
    base.ip_info    = &ip;
    base.if_key     = "usbnet";
    base.if_desc    = "usb";
    base.route_prio = 10;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);

    esp_netif_driver_ifconfig_t drv;
    memset(&drv, 0, sizeof(drv));
    drv.handle                = (void*)1;   // non-NULL sentinel handle
    drv.transmit              = netif_transmit;
    drv.driver_free_rx_buffer = netif_free_rx;

    esp_netif_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.base   = &base;
    cfg.driver = &drv;
    cfg.stack  = ESP_NETIF_NETSTACK_DEFAULT_ETH;

    s_netif = esp_netif_new(&cfg);
    if (!s_netif) {
        snprintf(s_err, sizeof(s_err), "netif_new NULL\n(init=%d %s)", ei, esp_err_to_name(ei));
        return false;
    }

    esp_err_t em = esp_netif_set_mac(s_netif, mac);
    esp_netif_action_start(s_netif, NULL, 0, NULL);  // returns void
    esp_err_t ed = esp_netif_dhcps_start(s_netif);  // harmless if already started

    // Supply an explicit NCM descriptor set (default is refused: HID compiled in).
    snprintf(s_mac_str, sizeof(s_mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    tinyusb_config_t tcfg;
    memset(&tcfg, 0, sizeof(tcfg));
    tcfg.external_phy             = false;
    tcfg.device_descriptor        = &s_net_dev;
    tcfg.string_descriptor        = s_net_str;
    tcfg.string_descriptor_count  = sizeof(s_net_str) / sizeof(s_net_str[0]);
    tcfg.configuration_descriptor = s_net_cfg;
    esp_err_t ti = tinyusb_driver_install(&tcfg);
    if (ti != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "tusb_install\n%s\n(mac=%d dh=%d)",
                 esp_err_to_name(ti), em, ed);
        esp_netif_destroy(s_netif); s_netif = NULL;
        return false;
    }

    tinyusb_net_config_t ncfg;
    memset(&ncfg, 0, sizeof(ncfg));
    memcpy(ncfg.mac_addr, mac, 6);
    ncfg.on_recv_callback = usb_recv_cb;
    esp_err_t tn = tinyusb_net_init(TINYUSB_USBDEV_0, &ncfg);
    if (tn != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "net_init\n%s", esp_err_to_name(tn));
        tinyusb_driver_uninstall();
        esp_netif_destroy(s_netif); s_netif = NULL;
        return false;
    }

    s_up = true;
    ESP_LOGI(TAG, "USB-NCM up at 192.168.7.1");
    return true;
}

void usb_net_stop(void) {
    if (!s_up) return;
    tinyusb_driver_uninstall();
    if (s_netif) {
        esp_netif_action_stop(s_netif, NULL, 0, NULL);
        esp_netif_destroy(s_netif);
        s_netif = NULL;
    }
    s_up = false;
}

bool usb_net_running(void) { return s_up; }

void usb_net_ip(char* out, int cap) {
    snprintf(out, cap, "192.168.7.1");
}
