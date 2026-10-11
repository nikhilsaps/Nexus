// Nexus - shell firmware (Cardputer / ESP32-S3). Stage 2: BLE HID live.
// Minimalist cyberdeck terminal. See STEERING.md / FEATURE.md / HARDWARE.md.
#include <M5GFX.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <string.h>
#include <stdio.h>
#include "ble_hid.h"
#include "usb_hid.h"

// ---------- Palette (cyberdeck terminal) ----------
static M5GFX display;
static M5Canvas canvas(&display);

static uint16_t C_BG, C_ACCENT, C_TEXT, C_DIM, C_AMBER, C_RED, C_SELBG;
static void init_palette() {
  C_BG     = display.color565(10, 14, 18);
  C_ACCENT = display.color565(0, 229, 199);
  C_TEXT   = display.color565(230, 235, 238);
  C_DIM    = display.color565(90, 100, 110);
  C_AMBER  = display.color565(255, 176, 0);
  C_RED    = display.color565(255, 70, 70);
  C_SELBG  = display.color565(20, 28, 36);
}

// ---------- Keyboard (verified scan + kmap) ----------
static const gpio_num_t SEL[3]  = {GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_11};
static const gpio_num_t ROWS[7] = {GPIO_NUM_13, GPIO_NUM_15, GPIO_NUM_3,
                                   GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7};
static const char KMAP[4][14] = {
  { '`','1','2','3','4','5','6','7','8','9','0','-','=', 0x08 },
  { 0x09,'q','w','e','r','t','y','u','i','o','p','[',']','\\' },
  { 0,0,'a','s','d','f','g','h','j','k','l',';','\'','\n' },
  { 0,0,0,'z','x','c','v','b','n','m',',','.','/',' ' }
};

static void kb_init() {
  for (int i = 0; i < 3; i++) {
    gpio_set_direction(SEL[i], GPIO_MODE_OUTPUT);
    gpio_set_level(SEL[i], 0);
  }
  for (int i = 0; i < 7; i++) {
    gpio_set_direction(ROWS[i], GPIO_MODE_INPUT);
    gpio_set_pull_mode(ROWS[i], GPIO_PULLUP_ONLY);
  }
}

// All currently-held chars; also reports whether the Aa (shift) key is down.
static int kb_scan_full(char* out, int maxn, bool* shift) {
  int n = 0;
  bool sh = false;
  for (int col = 0; col < 8; col++) {
    gpio_set_level(SEL[0], col & 1);
    gpio_set_level(SEL[1], (col >> 1) & 1);
    gpio_set_level(SEL[2], (col >> 2) & 1);
    esp_rom_delay_us(40);
    for (int i = 0; i < 7; i++) {
      if (gpio_get_level(ROWS[i]) == 0) {
        if (i == 0 && col == 1) sh = true;   // Aa key = [2][1]
        int x = 2 * i + (col > 3 ? 0 : 1);
        int y = 3 - (col > 3 ? col - 4 : col);
        if (x >= 0 && x < 14 && y >= 0 && y < 4) {
          char c = KMAP[y][x];
          if (c && n < maxn) out[n++] = c;
        }
      }
    }
  }
  if (shift) *shift = sh;
  return n;
}

// Edge-detected single key for menu navigation.
static char g_last = 0;
static char kb_poll() {
  char buf[8];
  int n = kb_scan_full(buf, 8, NULL);
  char c = (n > 0) ? buf[0] : 0;
  char out = 0;
  if (c && c != g_last) out = c;
  g_last = c;
  return out;
}

// ASCII char -> USB HID usage code (US layout).
static uint8_t ascii_usage(char c) {
  if (c >= 'a' && c <= 'z') return 4 + (c - 'a');
  if (c >= 'A' && c <= 'Z') return 4 + (c - 'A');
  if (c >= '1' && c <= '9') return 0x1E + (c - '1');
  switch (c) {
    case '0':  return 0x27;
    case ' ':  return 0x2C;
    case '\n': case '\r': return 0x28;
    case '\t': return 0x2B;
    case 0x08: return 0x2A;
    case '-':  return 0x2D;
    case '=':  return 0x2E;
    case '[':  return 0x2F;
    case ']':  return 0x30;
    case '\\': return 0x31;
    case ';':  return 0x33;
    case '\'': return 0x34;
    case '`':  return 0x35;
    case ',':  return 0x36;
    case '.':  return 0x37;
    case '/':  return 0x38;
  }
  return 0;
}

// ---------- Battery (GPIO10 = ADC1_CH9, 2:1 divider) ----------
static adc_oneshot_unit_handle_t g_adc = NULL;
static void batt_init() {
  adc_oneshot_unit_init_cfg_t ucfg = {};
  ucfg.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&ucfg, &g_adc) != ESP_OK) { g_adc = NULL; return; }
  adc_oneshot_chan_cfg_t ccfg = {};
  ccfg.atten = ADC_ATTEN_DB_12;
  ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  adc_oneshot_config_channel(g_adc, ADC_CHANNEL_9, &ccfg);
}
static int  g_batt_pct = 0;
static bool g_batt_chg = false;
static void batt_sample() {
  if (!g_adc) return;
  long acc = 0;
  int ok = 0;
  for (int i = 0; i < 8; i++) {
    int raw = 0;
    if (adc_oneshot_read(g_adc, ADC_CHANNEL_9, &raw) == ESP_OK) { acc += raw; ok++; }
  }
  if (!ok) return;
  float vadc = (acc / (float)ok) * (3.3f / 4095.0f);
  float vbat = vadc * 2.0f;
  float pct = (vbat - 3.3f) / 0.9f * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  g_batt_pct = (int)(pct + 0.5f);
  g_batt_chg = (vbat > 4.15f);
}

// ---------- Radio status model (drives top-bar icons) ----------
// BLE: 0 off, 1 advertising, 2 connected.  WiFi: 0 off, else signal 1..100.
static int g_ble_state = 0;
static int g_usb_state = 0;   // 0 off, 1 waiting for host, 2 mounted
static int g_wifi_pct  = 0;   // 0 = not in use (icon hidden) until WiFi lands
#define NEXUS_PIN "123456"   // fixed pairing passkey (mirrors esp_hid_gap.c)

// ---------- Menu model ----------
enum ItemKind { KIND_SUBMENU, KIND_ACTION, KIND_TOGGLE, KIND_INFO };
struct Menu;
struct MenuItem {
  const char* label;
  ItemKind    kind;
  Menu*       sub;
  void      (*action)();
  bool*       flag;
  void      (*on_toggle)(bool);
};
struct Menu {
  const char* title;
  MenuItem*   items;
  int         count;
  int         sel;
  int         top;
};

// ---------- App views ----------
enum AppView { VIEW_MENU, VIEW_MSG, VIEW_KB, VIEW_MOUSE, VIEW_PAD };
static AppView g_view = VIEW_MENU;

// ---------- Transport (BLE vs USB) ----------
// HID screens are shared; g_xport decides which stack a report goes to.
enum Xport { XPORT_BLE, XPORT_USB };
static Xport g_xport = XPORT_BLE;
static const char* xport_name() { return g_xport == XPORT_USB ? "USB" : "BLE"; }
static bool hid_connected() { return g_xport == XPORT_USB ? uhid_connected() : bhid_connected(); }
static void hid_key(uint8_t mod, const uint8_t keys6[6]) {
  if (g_xport == XPORT_USB) uhid_key_report(mod, keys6); else bhid_key_report(mod, keys6);
}
static void hid_mouse(uint8_t b, int8_t dx, int8_t dy, int8_t w) {
  if (g_xport == XPORT_USB) uhid_send_mouse(b, dx, dy, w); else bhid_send_mouse(b, dx, dy, w);
}
static void hid_pad(uint16_t b, int8_t x, int8_t y) {
  if (g_xport == XPORT_USB) uhid_send_gamepad(b, x, y); else bhid_send_gamepad(b, x, y);
}

static bool g_log_on  = false;
static bool g_anim_on = true;

static Menu* g_stack[8];
static int   g_depth = 0;
static Menu* cur() { return g_stack[g_depth]; }

static const char* g_msg_title = 0;
static const char* g_msg_body  = 0;

// per-screen state (reset on entry)
static char    g_kbbuf[40];
static int     g_kblen = 0;
static uint8_t g_last_keys[6] = {0};
static uint8_t g_last_mod = 0;
static uint8_t g_mouse_prev = 0;   // nonzero if last mouse report was nonzero
static const int8_t MOUSE_SPD[3]      = {4, 8, 16};
static const char*  MOUSE_SPD_NAME[3] = {"SLOW", "MED", "FAST"};
static int     g_mouse_spd_i  = 1;     // default MED
static bool    g_mouse_s_prev = false; // edge-detect speed key
static uint8_t g_mouse_div    = 0;     // scroll throttle counter
static uint8_t g_mouse_btn_disp = 0;   // live button state (for pills)
static int8_t  g_mouse_whl_disp = 0;   // live scroll dir (for pill)
static uint16_t g_pad_btns = 0;
static int8_t  g_pad_x = 0, g_pad_y = 0;
static bool    g_prev_exit = false;
static char    g_prev_held[8] = {0};
static int     g_prev_n = 0;

static void reset_screen() {
  g_kblen = 0;
  g_kbbuf[0] = 0;
  memset(g_last_keys, 0, sizeof(g_last_keys));
  g_last_mod = 0;
  g_mouse_prev = 0;
  g_pad_btns = 0;
  g_pad_x = 0;
  g_pad_y = 0;
  g_prev_exit = false;
  g_prev_n = 0;
}

// ---------- Settings persistence ----------
static void apply_log() { esp_log_level_set("*", g_log_on ? ESP_LOG_INFO : ESP_LOG_NONE); }
static void nvs_load() {
  nvs_handle_t h;
  if (nvs_open("nexus", NVS_READONLY, &h) == ESP_OK) {
    uint8_t v = 0;
    if (nvs_get_u8(h, "log", &v) == ESP_OK) g_log_on = v;
    v = 1;
    if (nvs_get_u8(h, "anim", &v) == ESP_OK) g_anim_on = v;
    nvs_close(h);
  }
  apply_log();
}
static void nvs_save() {
  nvs_handle_t h;
  if (nvs_open("nexus", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "log", g_log_on ? 1 : 0);
    nvs_set_u8(h, "anim", g_anim_on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
}
static void on_log_toggle(bool v)  { apply_log(); nvs_save(); }
static void on_anim_toggle(bool v) { nvs_save(); }

// ---------- Actions ----------
static void show_msg(const char* t, const char* b) { g_msg_title = t; g_msg_body = b; g_view = VIEW_MSG; }
static void act_ble_kb()  { g_xport = XPORT_BLE; reset_screen(); g_view = VIEW_KB; }
static void act_ble_ms()  { g_xport = XPORT_BLE; reset_screen(); g_view = VIEW_MOUSE; }
static void act_ble_gp()  { g_xport = XPORT_BLE; reset_screen(); g_view = VIEW_PAD; }
static void act_usb_kb()  { g_xport = XPORT_USB; reset_screen(); g_view = VIEW_KB; }
static void act_usb_ms()  { g_xport = XPORT_USB; reset_screen(); g_view = VIEW_MOUSE; }
static void act_usb_gp()  { g_xport = XPORT_USB; reset_screen(); g_view = VIEW_PAD; }

static char g_info_buf[96];
static void act_forget() {
  if (bhid_forget_bonds())
    show_msg("FORGET BONDS", "Bonds cleared.\nRe-pair from the host.");
  else
    show_msg("FORGET BONDS", "Open Control first\nto enable BLE.");
}
static void act_info() {
  uint8_t m[6];
  if (bhid_mac(m)) {
    snprintf(g_info_buf, sizeof(g_info_buf),
      "name  Nexus\nmac   %02X:%02X:%02X:%02X:%02X:%02X\nstate %s",
      m[5], m[4], m[3], m[2], m[1], m[0],
      bhid_connected() ? "connected" : (bhid_active() ? "advertising" : "idle"));
  } else {
    snprintf(g_info_buf, sizeof(g_info_buf), "name  Nexus\nmac   (BLE off)\nstate off");
  }
  g_msg_title = "BLE INFO";
  g_msg_body  = g_info_buf;
  g_view      = VIEW_MSG;
}
static void act_about()   { show_msg("ABOUT", "NEXUS\nBLE + USB HID\nCardputer ESP32-S3\nnexus // cyberdeck"); }

// ---------- Menu tree ----------
static MenuItem control_items[] = {
  { "Keyboard", KIND_ACTION, 0, act_ble_kb, 0, 0 },
  { "Mouse",    KIND_ACTION, 0, act_ble_ms, 0, 0 },
  { "Gamepad",  KIND_ACTION, 0, act_ble_gp, 0, 0 },
};
static Menu control_menu = { "CONTROL", control_items, 3, 0, 0 };

static MenuItem ble_items[] = {
  { "Control",      KIND_SUBMENU, &control_menu, 0, 0, 0 },
  { "Forget Bonds", KIND_ACTION,  0, act_forget, 0, 0 },
  { "Info",         KIND_ACTION,  0, act_info,   0, 0 },
};
static Menu ble_menu = { "BLE", ble_items, 3, 0, 0 };

static MenuItem set_items[] = {
  { "Logging",   KIND_TOGGLE, 0, 0, &g_log_on,  on_log_toggle },
  { "Animation", KIND_TOGGLE, 0, 0, &g_anim_on, on_anim_toggle },
  { "About",     KIND_ACTION, 0, act_about, 0, 0 },
};
static Menu set_menu = { "SETTINGS", set_items, 3, 0, 0 };

// USB mirrors BLE: USB > Control > Keyboard / Mouse / Gamepad (native USB HID).
static MenuItem usb_control_items[] = {
  { "Keyboard", KIND_ACTION, 0, act_usb_kb, 0, 0 },
  { "Mouse",    KIND_ACTION, 0, act_usb_ms, 0, 0 },
  { "Gamepad",  KIND_ACTION, 0, act_usb_gp, 0, 0 },
};
static Menu usb_control_menu = { "USB CONTROL", usb_control_items, 3, 0, 0 };

static MenuItem usb_items[] = {
  { "Control", KIND_SUBMENU, &usb_control_menu, 0, 0, 0 },
};
static Menu usb_menu = { "USB", usb_items, 1, 0, 0 };

static MenuItem root_items[] = {
  { "BLE",      KIND_SUBMENU, &ble_menu, 0, 0, 0 },
  { "USB",      KIND_SUBMENU, &usb_menu, 0, 0, 0 },
  { "Settings", KIND_SUBMENU, &set_menu, 0, 0, 0 },
};
static Menu root_menu = { "NEXUS", root_items, 3, 0, 0 };

// ---------- Layout ----------
static const int W = 240, H = 135;
static const int BAR_H = 16;
static const int ROW_H = 22;
static const int LIST_Y = BAR_H + 4;
static const int VIS = (H - LIST_Y) / ROW_H;

// ---------- Top-bar icons ----------
// Bluetooth rune centered on cx, vertical span t..b. `on` = bright.
static void icon_bt(int cx, int t, int b, bool on) {
  uint16_t col = on ? C_ACCENT : C_DIM;
  int m = (t + b) / 2;
  int q1 = t + (b - t) / 4;
  int q3 = b - (b - t) / 4;
  int dx = 3;
  canvas.drawLine(cx, t, cx, b, col);
  canvas.drawLine(cx, t, cx + dx, q1, col);
  canvas.drawLine(cx + dx, q1, cx - dx, q3, col);
  canvas.drawLine(cx, b, cx + dx, q3, col);
  canvas.drawLine(cx + dx, q3, cx - dx, q1, col);
  (void)m;
}
// WiFi strength bars; right edge at rx, baseline y1. pct 0..100.
static void icon_wifi(int rx, int y1, int pct) {
  int bars = (pct <= 0) ? 0 : (pct < 34 ? 1 : (pct < 67 ? 2 : 3));
  for (int i = 0; i < 3; i++) {
    int bw = 2;
    int bh = 3 + i * 3;
    int bx = rx - (2 - i) * (bw + 1) - bw;
    uint16_t col = (i < bars) ? C_ACCENT : C_SELBG;
    canvas.fillRect(bx, y1 - bh, bw, bh, col);
  }
}

static void draw_statusbar(const char* title) {
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_TEXT, C_BG);
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.drawString(title, 6, 1);

  int rx = W - 6;
  // battery %
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", g_batt_pct);
  canvas.setTextDatum(textdatum_t::top_right);
  uint16_t bc = (g_batt_pct <= 15) ? C_RED : (g_batt_chg ? C_ACCENT : C_DIM);
  canvas.setTextColor(bc, C_BG);
  canvas.drawString(pct, rx, 1);
  int w = canvas.textWidth(pct);
  rx -= w;
  if (g_batt_chg) {
    int bx = rx - 7;
    canvas.fillTriangle(bx + 4, 2, bx, 8, bx + 4, 8, C_ACCENT);
    canvas.fillTriangle(bx + 2, 7, bx + 6, 7, bx + 2, 13, C_ACCENT);
    rx -= 9;
  }
  rx -= 8;
  // Bluetooth
  if (g_ble_state > 0) {
    icon_bt(rx - 3, 2, 13, g_ble_state == 2);
    rx -= 9;
    rx -= 8;
  }
  // WiFi (hidden until in use)
  if (g_wifi_pct > 0) {
    icon_wifi(rx, 13, g_wifi_pct);
    rx -= 12;
  }
  // USB HID state (dim = waiting for host, accent = mounted)
  if (g_usb_state > 0) {
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(g_usb_state == 2 ? C_ACCENT : C_DIM, C_BG);
    canvas.setTextDatum(textdatum_t::top_right);
    canvas.drawString("USB", rx, 1);
    rx -= canvas.textWidth("USB") + 6;
  }
  canvas.drawFastHLine(0, BAR_H, W, C_SELBG);
}

// ---------- Menu rendering ----------
static void draw_menu(Menu* m, float accent_y) {
  canvas.fillSprite(C_BG);
  draw_statusbar(m->title);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(textdatum_t::middle_left);

  bool overflow = m->count > VIS;
  int listw = overflow ? W - 6 : W;

  for (int r = 0; r < VIS; r++) {
    int idx = m->top + r;
    if (idx >= m->count) break;
    MenuItem* it = &m->items[idx];
    int y = LIST_Y + r * ROW_H;
    bool selected = (idx == m->sel);
    if (selected) canvas.fillRect(0, y, listw, ROW_H, C_SELBG);

    char buf[48];
    if (it->kind == KIND_TOGGLE) {
      snprintf(buf, sizeof(buf), "%s: %s", it->label, (*it->flag) ? "ON" : "OFF");
    } else {
      snprintf(buf, sizeof(buf), "%s", it->label);
    }
    canvas.setTextColor(selected ? C_TEXT : C_DIM, selected ? C_SELBG : C_BG);
    canvas.drawString(buf, 16, y + ROW_H / 2);

    if (selected && (it->kind == KIND_SUBMENU || it->kind == KIND_ACTION)) {
      canvas.setTextColor(C_ACCENT, C_SELBG);
      canvas.setTextDatum(textdatum_t::middle_right);
      canvas.drawString(">", listw - 8, y + ROW_H / 2);
      canvas.setTextDatum(textdatum_t::middle_left);
    }
  }

  float rel = accent_y - m->top;
  if (rel >= 0 && rel < VIS) {
    int y = LIST_Y + (int)(rel * ROW_H);
    canvas.fillRect(0, y + 2, 3, ROW_H - 4, C_ACCENT);
  }

  if (overflow) {
    int trackh = VIS * ROW_H;
    int thumbh = trackh * VIS / m->count;
    if (thumbh < 8) thumbh = 8;
    int range = m->count - VIS;
    int thumby = LIST_Y + (range ? (m->top * (trackh - thumbh) / range) : 0);
    canvas.fillRect(W - 3, LIST_Y, 2, trackh, C_SELBG);
    canvas.fillRect(W - 3, thumby, 2, thumbh, C_ACCENT);
  }
  if (g_ble_state == 1) {
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_AMBER, C_BG);
    canvas.setTextDatum(textdatum_t::bottom_right);
    canvas.drawString("PIN " NEXUS_PIN, W - 6, H - 2);
  }
  canvas.pushSprite(0, 0);
}

static void draw_msg() {
  canvas.fillSprite(C_BG);
  draw_statusbar(g_msg_title);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(textdatum_t::top_left);
  int y = LIST_Y + 2;
  const char* p = g_msg_body;
  char line[64];
  bool first = true;
  while (*p) {
    int i = 0;
    while (*p && *p != '\n' && i < 63) line[i++] = *p++;
    line[i] = 0;
    if (*p == '\n') p++;
    canvas.setTextColor(first ? C_ACCENT : C_TEXT, C_BG);
    canvas.drawString(line, 10, y);
    y += 20;
    first = false;
  }
  canvas.setTextColor(C_DIM, C_BG);
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.drawString(", back", 10, H - 4);
  canvas.pushSprite(0, 0);
}

// connection status chip (shared by HID screens)
static void draw_conn_chip() {
  bool c = hid_connected();
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(textdatum_t::top_left);
  if (c) {
    canvas.setTextColor(C_ACCENT, C_BG);
    canvas.drawString("connected", 10, LIST_Y);
    return;
  }
  if (g_xport == XPORT_USB) {
    canvas.setTextColor(C_AMBER, C_BG);
    canvas.drawString("waiting for USB host", 10, LIST_Y);
  } else {
    canvas.setTextColor(C_AMBER, C_BG);
    canvas.drawString("advertising", 10, LIST_Y);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.setTextDatum(textdatum_t::top_right);
    canvas.drawString("PIN " NEXUS_PIN, W - 10, LIST_Y);
  }
}

static void draw_kb_screen() {
  canvas.fillSprite(C_BG);
  char _t[24]; snprintf(_t, sizeof(_t), "%s KEYBOARD", xport_name());
  draw_statusbar(_t);
  draw_conn_chip();
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString("type to send  (Aa = shift)", 10, LIST_Y + 22);
  // echo buffer
  canvas.setTextColor(C_TEXT, C_BG);
  canvas.drawString(g_kbbuf, 10, LIST_Y + 44);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.drawString("` exit", 10, H - 4);
  canvas.pushSprite(0, 0);
}

static void mouse_pill(int x, int y, const char* s, bool on) {
  const int w = 30, hgt = 18;
  canvas.fillRoundRect(x, y, w, hgt, 4, on ? C_ACCENT : C_SELBG);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.setTextColor(on ? C_BG : C_DIM, on ? C_ACCENT : C_SELBG);
  canvas.drawString(s, x + w / 2, y + hgt / 2);
}
static void draw_mouse_screen() {
  canvas.fillSprite(C_BG);
  char _t[24]; snprintf(_t, sizeof(_t), "%s MOUSE", xport_name());
  draw_statusbar(_t);
  draw_conn_chip();
  canvas.setFont(&fonts::Font2);

  // movement + scroll hint
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString("move ;./,/   scroll k/j", 10, LIST_Y + 20);

  // speed readout
  char sb[16];
  snprintf(sb, sizeof(sb), "SPD %s", MOUSE_SPD_NAME[g_mouse_spd_i]);
  canvas.setTextDatum(textdatum_t::top_right);
  canvas.setTextColor(C_ACCENT, C_BG);
  canvas.drawString(sb, W - 10, LIST_Y + 20);

  // live button + scroll pills
  int py = LIST_Y + 44;
  mouse_pill(10,  py, "L",  g_mouse_btn_disp & 1);
  mouse_pill(46,  py, "R",  g_mouse_btn_disp & 2);
  mouse_pill(82,  py, "M",  g_mouse_btn_disp & 4);
  bool su = g_mouse_whl_disp > 0, sd = g_mouse_whl_disp < 0;
  mouse_pill(132, py, su ? "UP" : (sd ? "DN" : "WH"), su || sd);

  // key legend
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString("L=spc R=m M=n  s=speed", 10, py + 24);

  canvas.setTextColor(C_DIM, C_BG);
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.drawString("` exit", 10, H - 4);
  canvas.pushSprite(0, 0);
}

static void draw_pad_screen() {
  canvas.fillSprite(C_BG);
  char _t[24]; snprintf(_t, sizeof(_t), "%s GAMEPAD", xport_name());
  draw_statusbar(_t);
  draw_conn_chip();
  // button pads
  const char* labels[8] = { "A", "B", "X", "Y", "L", "R", "St", "Se" };
  for (int i = 0; i < 8; i++) {
    int col = i % 4;
    int row = i / 4;
    int bx = 12 + col * 40;
    int by = LIST_Y + 24 + row * 30;
    bool on = (g_pad_btns >> i) & 1;
    canvas.fillRoundRect(bx, by, 32, 24, 4, on ? C_ACCENT : C_SELBG);
    canvas.setTextColor(on ? C_BG : C_DIM, on ? C_ACCENT : C_SELBG);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setFont(&fonts::Font2);
    canvas.drawString(labels[i], bx + 16, by + 12);
  }
  // dpad direction readout
  canvas.setTextDatum(textdatum_t::top_right);
  canvas.setTextColor(C_DIM, C_BG);
  char d[16];
  snprintf(d, sizeof(d), "x%+d y%+d", g_pad_x / 127, g_pad_y / 127);
  canvas.drawString(d, W - 8, LIST_Y);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.drawString("` exit  ;./,/ dpad  asdf/qw btns", 10, H - 4);
  canvas.pushSprite(0, 0);
}

// ---------- Boot splash ----------
static void boot_splash() {
  canvas.setFont(&fonts::Font4);
  canvas.setTextDatum(textdatum_t::middle_center);
  const char* word = "NEXUS";
  canvas.fillSprite(C_BG);
  int tw = canvas.textWidth(word);
  for (int step = 0; step <= W; step += 16) {
    canvas.fillSprite(C_BG);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.drawString(word, W / 2, H / 2);
    if (step < W) canvas.fillRect(step, 0, W - step, H, C_BG);
    if (step < W) canvas.drawFastVLine(step, 0, H, C_ACCENT);
    canvas.pushSprite(0, 0);
    vTaskDelay(pdMS_TO_TICKS(16));
  }
  canvas.fillSprite(C_BG);
  canvas.setTextColor(C_TEXT, C_BG);
  canvas.drawString(word, W / 2, H / 2);
  canvas.fillRect(W / 2 - tw / 2, H / 2 + 16, tw, 2, C_ACCENT);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.drawString("cyberdeck // stage 2", W / 2, H / 2 + 30);
  canvas.pushSprite(0, 0);
  vTaskDelay(pdMS_TO_TICKS(600));
}

// ---------- Menu navigation ----------
static void move_sel(Menu* m, int delta) {
  int old = m->sel;
  int ns = m->sel + delta;
  if (ns < 0) ns = m->count - 1;
  if (ns >= m->count) ns = 0;
  m->sel = ns;
  if (m->sel < m->top) m->top = m->sel;
  if (m->sel >= m->top + VIS) m->top = m->sel - VIS + 1;
  if (g_anim_on && old != m->sel) {
    float from = old, to = m->sel;
    const int frames = 5;
    for (int f = 1; f <= frames; f++) {
      float t = f / (float)frames;
      draw_menu(m, from + (to - from) * t);
      vTaskDelay(pdMS_TO_TICKS(16));
    }
  }
}

// Radios follow their Control subtree: on when inside it, off otherwise.
static bool stack_has(Menu* t) {
  for (int i = 0; i <= g_depth; i++) if (g_stack[i] == t) return true;
  return false;
}
static void sync_radios() {
  static bool was_ble = false, was_usb = false;
  bool nb = stack_has(&control_menu);
  bool nu = stack_has(&usb_control_menu);
  if (nb && !was_ble) bhid_start("Nexus");
  if (!nb && was_ble) bhid_stop();
  if (nu && !was_usb) uhid_start("Nexus");
  if (!nu && was_usb) uhid_stop();
  was_ble = nb;
  was_usb = nu;
}

static void enter_item(Menu* m) {
  MenuItem* it = &m->items[m->sel];
  switch (it->kind) {
    case KIND_SUBMENU:
      if (g_depth < 7 && it->sub) g_stack[++g_depth] = it->sub;
      break;
    case KIND_ACTION:
      if (it->action) it->action();
      break;
    case KIND_TOGGLE:
      if (it->flag) {
        *it->flag = !*it->flag;
        if (it->on_toggle) it->on_toggle(*it->flag);
      }
      break;
    default: break;
  }
}

// returns true if the user pressed ` (exit) this tick (edge-detected)
static bool held_has(const char* h, int n, char c) {
  for (int i = 0; i < n; i++) if (h[i] == c) return true;
  return false;
}
static bool exit_pressed(const char* h, int n) {
  bool now = held_has(h, n, '`');
  bool edge = now && !g_prev_exit;
  g_prev_exit = now;
  return edge;
}
static void leave_screen() {
  g_view = VIEW_MENU;
  g_last = '`';            // suppress stale backtick in menu poll
}

// ---------- HID screen ticks ----------
static void kb_echo(char c) {
  if (c == 8) {                       // backspace trims the echo
    if (g_kblen > 0) g_kbbuf[--g_kblen] = 0;
    return;
  }
  if (c == '\n' || c == '\r') c = ' ';
  if (c < 0x20) return;
  if (g_kblen >= (int)sizeof(g_kbbuf) - 1) {   // scroll left
    memmove(g_kbbuf, g_kbbuf + 1, g_kblen);
    g_kblen--;
  }
  g_kbbuf[g_kblen++] = c;
  g_kbbuf[g_kblen] = 0;
}
static void tick_kb() {
  char h[8];
  bool shift = false;
  int n = kb_scan_full(h, 8, &shift);
  if (exit_pressed(h, n)) {
    uint8_t empty[6] = {0};
    hid_key(0, empty);
    g_prev_n = 0;
    g_prev_held[0] = 0;
    leave_screen();
    return;
  }
  // echo characters newly pressed since last tick
  for (int i = 0; i < n; i++) {
    char c = h[i];
    if (c == '`') continue;
    bool was = false;
    for (int j = 0; j < g_prev_n; j++) if (g_prev_held[j] == c) { was = true; break; }
    if (!was) {
      char e = c;
      if (shift && e >= 'a' && e <= 'z') e = e - 'a' + 'A';
      kb_echo(e);
    }
  }
  memcpy(g_prev_held, h, n);
  g_prev_n = n;

  uint8_t keys[6] = {0};
  int k = 0;
  for (int i = 0; i < n && k < 6; i++) {
    if (h[i] == '`') continue;
    uint8_t u = ascii_usage(h[i]);
    if (u) keys[k++] = u;
  }
  uint8_t mod = shift ? 0x02 : 0;
  bool changed = (mod != g_last_mod);
  for (int i = 0; i < 6; i++) if (keys[i] != g_last_keys[i]) changed = true;
  if (changed) {
    hid_key(mod, keys);
    memcpy(g_last_keys, keys, sizeof(keys));
    g_last_mod = mod;
  }
}

static void tick_mouse() {
  char h[8];
  int n = kb_scan_full(h, 8, NULL);
  if (exit_pressed(h, n)) { leave_screen(); return; }

  // speed cycle on 's' edge (SLOW -> MED -> FAST)
  bool s_now = held_has(h, n, 's');
  if (s_now && !g_mouse_s_prev) g_mouse_spd_i = (g_mouse_spd_i + 1) % 3;
  g_mouse_s_prev = s_now;
  int8_t sp = MOUSE_SPD[g_mouse_spd_i];

  int8_t dx = 0, dy = 0, whl = 0;
  uint8_t btn = 0;
  if (held_has(h, n, ';')) dy -= sp;
  if (held_has(h, n, '.')) dy += sp;
  if (held_has(h, n, ',')) dx -= sp;
  if (held_has(h, n, '/')) dx += sp;
  if (held_has(h, n, ' ')) btn |= 1;   // left
  if (held_has(h, n, 'm')) btn |= 2;   // right
  if (held_has(h, n, 'n')) btn |= 4;   // middle

  // scroll (throttled ~10/s): k = up, j = down
  bool up = held_has(h, n, 'k');
  bool dn = held_has(h, n, 'j');
  if (up || dn) {
    if (g_mouse_div == 0) whl = up ? 1 : -1;
    g_mouse_div = (g_mouse_div + 1) % 6;
  } else {
    g_mouse_div = 0;
  }

  g_mouse_btn_disp = btn;
  g_mouse_whl_disp = up ? 1 : (dn ? -1 : 0);

  uint8_t nonzero = (dx || dy || btn || whl) ? 1 : 0;
  if (nonzero || g_mouse_prev) hid_mouse(btn, dx, dy, whl);
  g_mouse_prev = nonzero;
}

static void tick_pad() {
  char h[8];
  int n = kb_scan_full(h, 8, NULL);
  if (exit_pressed(h, n)) {
    hid_pad(0, 0, 0);
    leave_screen();
    return;
  }
  uint16_t btns = 0;
  int8_t x = 0, y = 0;
  if (held_has(h, n, ';')) y = -127;
  if (held_has(h, n, '.')) y = 127;
  if (held_has(h, n, ',')) x = -127;
  if (held_has(h, n, '/')) x = 127;
  if (held_has(h, n, 'a')) btns |= 1 << 0;
  if (held_has(h, n, 's')) btns |= 1 << 1;
  if (held_has(h, n, 'd')) btns |= 1 << 2;
  if (held_has(h, n, 'f')) btns |= 1 << 3;
  if (held_has(h, n, 'q')) btns |= 1 << 4;
  if (held_has(h, n, 'w')) btns |= 1 << 5;
  if (held_has(h, n, ' ')) btns |= 1 << 6;
  if (held_has(h, n, '\n')) btns |= 1 << 7;
  if (btns != g_pad_btns || x != g_pad_x || y != g_pad_y) {
    hid_pad(btns, x, y);
    g_pad_btns = btns;
    g_pad_x = x;
    g_pad_y = y;
  }
}

// ---------- Main ----------
extern "C" void app_main(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  display.begin();
  display.setRotation(1);
  display.setBrightness(160);
  init_palette();
  canvas.setColorDepth(16);
  canvas.createSprite(W, H);   // grab the big contiguous block before BLE

  kb_init();
  batt_init();
  nvs_load();

  boot_splash();

  // BLE is OFF at boot; it starts only when you enter BLE > Control.

  g_stack[0] = &root_menu;
  g_depth = 0;

  batt_sample();
  uint32_t last_batt = xTaskGetTickCount();
  draw_menu(cur(), (float)cur()->sel);

  while (true) {
    // periodic battery
    uint32_t now = xTaskGetTickCount();
    bool batt_tick = false;
    if (now - last_batt >= pdMS_TO_TICKS(2000)) {
      batt_sample();
      last_batt = now;
      batt_tick = true;
    }
    int new_ble = bhid_active() ? (bhid_connected() ? 2 : 1) : 0;
    bool ble_changed = (new_ble != g_ble_state);
    g_ble_state = new_ble;
    int new_usb = uhid_active() ? (uhid_connected() ? 2 : 1) : 0;
    bool usb_changed = (new_usb != g_usb_state);
    g_usb_state = new_usb;

    switch (g_view) {
      case VIEW_MENU: {
        char k = kb_poll();
        bool dirty = batt_tick || ble_changed || usb_changed;
        if (k) {
          Menu* m = cur();
          switch (k) {
            case ';': move_sel(m, -1); dirty = true; break;
            case '.': move_sel(m, +1); dirty = true; break;
            case '/':
            case '\n':
            case '\r': enter_item(m); dirty = true; break;
            case ',': if (g_depth > 0) g_depth--; dirty = true; break;
            case '`': g_depth = 0; dirty = true; break;
            default: break;
          }
          sync_radios();
        }
        if (dirty && g_view == VIEW_MENU) draw_menu(cur(), (float)cur()->sel);
        break;
      }
      case VIEW_MSG: {
        char k = kb_poll();
        if (k == ',' || k == '`') { g_view = VIEW_MENU; g_last = k; }
        else draw_msg();
        break;
      }
      case VIEW_KB:    tick_kb();    if (g_view == VIEW_KB)    draw_kb_screen();    break;
      case VIEW_MOUSE: tick_mouse(); if (g_view == VIEW_MOUSE) draw_mouse_screen(); break;
      case VIEW_PAD:   tick_pad();   if (g_view == VIEW_PAD)   draw_pad_screen();   break;
    }
    vTaskDelay(pdMS_TO_TICKS(16));
  }
}
