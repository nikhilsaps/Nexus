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
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include "ble_hid.h"
#include "usb_hid.h"
#include "sd_store.h"

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
enum AppView { VIEW_MENU, VIEW_MSG, VIEW_KB, VIEW_MOUSE, VIEW_PAD, VIEW_FILES,
                VIEW_FILEACT, VIEW_TEXTIN, VIEW_CONFIRM, VIEW_DUCK, VIEW_DUCKRUN };
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

// ---------- Storage (microSD) screen state ----------
static char        g_path[256]       = "/";
static sd_entry_t* g_entries         = 0;      // heap, lazily allocated
static int         g_entry_count     = 0;
static int         g_file_sel        = 0;
static int         g_file_top        = 0;
static bool        g_entries_trunc   = false;
static bool        g_sd_ok           = false;
static char        g_sd_info_buf[200];

// ---------- Storage write-op state ----------
#define A_OPEN     1
#define A_RENAME   2
#define A_DELETE   3
#define A_NEWFILE  4
#define A_NEWDIR   5
#define TIN_RENAME  1
#define TIN_NEWFILE 2
#define TIN_NEWDIR  3
static const char* g_act_labels[6];
static int         g_act_codes[6];
static int         g_act_n   = 0;
static int         g_act_sel = 0;
static char        g_tin[SD_NAME_MAX];
static int         g_tin_len  = 0;
static int         g_tin_mode = 0;
static char        g_rename_old[SD_NAME_MAX];
static char        g_del_name[SD_NAME_MAX];
static bool        g_del_isdir = false;

// Build a card-relative path for `name` inside the current g_path.
static void entry_rel(const char* name, char* out, size_t n) {
  size_t pl = strlen(g_path);
  if (pl && g_path[pl - 1] == '/') snprintf(out, n, "%s%s", g_path, name);
  else                             snprintf(out, n, "%s/%s", g_path, name);
}

static void open_actions();
static void do_action(int code);
static void commit_text();

// Human-readable byte count: "931", "4.3K", "12M", "7.4G".
static void fmt_size(uint64_t b, char* out, size_t n) {
  if (b < 1024ULL)              { snprintf(out, n, "%u",   (unsigned)b); return; }
  if (b < 1024ULL*1024)         { snprintf(out, n, "%.1fK", b/1024.0);          return; }
  if (b < 1024ULL*1024*1024)    { snprintf(out, n, "%.1fM", b/(1024.0*1024));   return; }
  snprintf(out, n, "%.1fG", b/(1024.0*1024*1024));
}

// Re-read g_path into g_entries; resets selection to the top.
static void files_reload() {
  g_entry_count   = 0;
  g_entries_trunc = false;
  g_file_sel      = 0;
  g_file_top      = 0;
  if (g_sd_ok && g_entries) {
    g_entry_count = sd_list(g_path, g_entries, SD_MAX_ENTRIES, &g_entries_trunc);
  }
}

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

static void duck_open(Xport xp);   // defined below (mount SD + start radio + list /duck)
static void act_ble_duck() { duck_open(XPORT_BLE); }
static void act_usb_duck() { duck_open(XPORT_USB); }

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

// ---------- Storage actions ----------
static void act_browse() {
  if (!g_sd_ok || !g_entries) { show_msg("STORAGE", "No card.\nInsert microSD\nand pick Refresh."); return; }
  strcpy(g_path, "/");
  files_reload();
  g_view = VIEW_FILES;
}
static void act_card_info() {
  if (!g_sd_ok) { show_msg("CARD INFO", "No card mounted.\nPick Refresh after\ninserting a card."); return; }
  char name[32] = {0}; uint64_t cardb = 0, totb = 0, freb = 0;
  sd_info(name, sizeof(name), &cardb, &totb, &freb);
  char cs[16], ts[16], fs[16];
  fmt_size(cardb, cs, sizeof(cs));
  fmt_size(totb,  ts, sizeof(ts));
  fmt_size(freb,  fs, sizeof(fs));
  snprintf(g_sd_info_buf, sizeof(g_sd_info_buf),
           "card  %s\nsize  %s\nfs    FAT (%s)\nfree  %s", name, cs, ts, fs);
  show_msg("CARD INFO", g_sd_info_buf);
}
static void act_refresh() {
  sd_unmount();
  g_sd_ok = sd_mount();
  if (g_sd_ok && g_view == VIEW_FILES) files_reload();
  show_msg("STORAGE", g_sd_ok ? "Card remounted." : "No card found.");
}

// ---------- Menu tree ----------
static MenuItem control_items[] = {
  { "Keyboard", KIND_ACTION, 0, act_ble_kb, 0, 0 },
  { "Mouse",    KIND_ACTION, 0, act_ble_ms, 0, 0 },
  { "Gamepad",  KIND_ACTION, 0, act_ble_gp, 0, 0 },
};
static Menu control_menu = { "CONTROL", control_items, 3, 0, 0 };

static MenuItem ble_items[] = {
  { "Control",      KIND_SUBMENU, &control_menu, 0, 0, 0 },
  { "Duck",         KIND_ACTION,  0, act_ble_duck, 0, 0 },
  { "Forget Bonds", KIND_ACTION,  0, act_forget, 0, 0 },
  { "Info",         KIND_ACTION,  0, act_info,   0, 0 },
};
static Menu ble_menu = { "BLE", ble_items, 4, 0, 0 };

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
  { "Duck",    KIND_ACTION,  0, act_usb_duck, 0, 0 },
};
static Menu usb_menu = { "USB", usb_items, 2, 0, 0 };

// Storage: microSD browser (lazy-mount on entry, auto-unmount on leaving).
static MenuItem storage_items[] = {
  { "Browse",    KIND_ACTION, 0, act_browse,    0, 0 },
  { "Card Info", KIND_ACTION, 0, act_card_info, 0, 0 },
  { "Refresh",   KIND_ACTION, 0, act_refresh,   0, 0 },
};
static Menu storage_menu = { "STORAGE", storage_items, 3, 0, 0 };

static MenuItem root_items[] = {
  { "BLE",      KIND_SUBMENU, &ble_menu,     0, 0, 0 },
  { "USB",      KIND_SUBMENU, &usb_menu,     0, 0, 0 },
  { "Storage",  KIND_SUBMENU, &storage_menu, 0, 0, 0 },
  { "Settings", KIND_SUBMENU, &set_menu,     0, 0, 0 },
};
static Menu root_menu = { "NEXUS", root_items, 4, 0, 0 };

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

// Storage mirrors the radios: mount the SD (and grab the entry buffer) when the
// Storage subtree is on the stack, release both when it leaves.
static void sync_storage() {
  static bool was = false;
  bool now = stack_has(&storage_menu);
  if (now && !was) {
    g_sd_ok = sd_mount();
    if (g_sd_ok && !g_entries) {
      g_entries = (sd_entry_t*)malloc(sizeof(sd_entry_t) * SD_MAX_ENTRIES);
      if (!g_entries) g_sd_ok = false;
    }
  }
  if (!now && was) {
    if (g_entries) { free(g_entries); g_entries = 0; }
    sd_unmount();
    g_sd_ok = false;
  }
  was = now;
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

// ---------- Storage (microSD) browse screen ----------
static void draw_files() {
  canvas.fillSprite(C_BG);
  // Title = current path, trimmed from the left if too long for the bar.
  const char* title = g_path;
  if (strlen(title) > 22) title = title + strlen(title) - 22;
  draw_statusbar(title);

  canvas.setFont(&fonts::Font2);
  if (g_entry_count == 0) {
    canvas.setTextColor(C_DIM, C_BG);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.drawString("(empty)", 12, LIST_Y + 4);
  }

  char sz[16];
  for (int i = 0; i < VIS; i++) {
    int idx = g_file_top + i;
    if (idx >= g_entry_count) break;
    sd_entry_t* e = &g_entries[idx];
    int y = LIST_Y + i * ROW_H;
    bool sel = (idx == g_file_sel);
    if (sel) canvas.fillRoundRect(4, y, W - 8, ROW_H - 2, 3, C_SELBG);

    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setTextColor(e->is_dir ? C_ACCENT : C_TEXT, sel ? C_SELBG : C_BG);
    char nm[SD_NAME_MAX + 2];
    snprintf(nm, sizeof(nm), "%s%s", e->is_dir ? "/" : " ", e->name);
    canvas.drawString(nm, 12, y + 3);

    canvas.setTextDatum(textdatum_t::top_right);
    canvas.setTextColor(C_DIM, sel ? C_SELBG : C_BG);
    if (e->is_dir) canvas.drawString("dir", W - 12, y + 3);
    else { fmt_size(e->size, sz, sizeof(sz)); canvas.drawString(sz, W - 12, y + 3); }
  }

  // Footer: entry count (+ truncation flag) and hints.
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.setTextColor(C_DIM, C_BG);
  char ft[40];
  snprintf(ft, sizeof(ft), "%d%s  tab act  , up",
           g_entry_count, g_entries_trunc ? "+" : "");
  canvas.drawString(ft, 8, H - 2);
  canvas.pushSprite(0, 0);
}

// Append `name` to g_path (descend into a folder).
static void path_push(const char* name) {
  size_t n = strlen(g_path);
  if (n == 0 || g_path[n - 1] != '/') { strncat(g_path, "/", sizeof(g_path) - n - 1); n++; }
  strncat(g_path, name, sizeof(g_path) - n - 1);
}
// Drop the last path component (go up). Returns false if already at root.
static bool path_pop() {
  if (strcmp(g_path, "/") == 0) return false;
  size_t n = strlen(g_path);
  if (n && g_path[n - 1] == '/') g_path[--n] = 0;     // drop trailing slash
  while (n && g_path[n - 1] != '/') g_path[--n] = 0;  // drop component
  if (n == 0) strcpy(g_path, "/");
  else if (n > 1 && g_path[n - 1] == '/') g_path[n - 1] = 0;
  return true;
}

static void tick_files() {
  char k = kb_poll();
  if (!k) return;
  switch (k) {
    case ';':
      if (g_file_sel > 0) g_file_sel--;
      if (g_file_sel < g_file_top) g_file_top = g_file_sel;
      break;
    case '.':
      if (g_file_sel < g_entry_count - 1) g_file_sel++;
      if (g_file_sel >= g_file_top + VIS) g_file_top = g_file_sel - VIS + 1;
      break;
    case '\t':
      open_actions();
      break;
    case '/':
    case '\n':
    case '\r': {
      if (g_entry_count == 0) { open_actions(); break; }
      sd_entry_t* e = &g_entries[g_file_sel];
      if (e->is_dir) { path_push(e->name); files_reload(); }
      else open_actions();
      break;
    }
    case ',':
      if (path_pop()) files_reload();      // up one level
      else leave_screen();                 // at root -> back to Storage menu
      break;
    case '`':
      leave_screen();
      break;
    default: break;
  }
}

// ---------- Storage write ops: action sheet / text input / delete confirm ----------
static void open_actions() {
  g_act_n = 0; g_act_sel = 0;
  if (g_entry_count > 0) {
    g_act_labels[g_act_n] = "Open";   g_act_codes[g_act_n++] = A_OPEN;
    g_act_labels[g_act_n] = "Rename"; g_act_codes[g_act_n++] = A_RENAME;
    g_act_labels[g_act_n] = "Delete"; g_act_codes[g_act_n++] = A_DELETE;
  }
  g_act_labels[g_act_n] = "New File";   g_act_codes[g_act_n++] = A_NEWFILE;
  g_act_labels[g_act_n] = "New Folder"; g_act_codes[g_act_n++] = A_NEWDIR;
  g_view = VIEW_FILEACT;
}

static void begin_text(int mode, const char* preset) {
  g_tin_mode = mode;
  g_tin_len = 0; g_tin[0] = 0;
  if (preset) {
    strncpy(g_tin, preset, SD_NAME_MAX - 1);
    g_tin[SD_NAME_MAX - 1] = 0;
    g_tin_len = strlen(g_tin);
  }
  // Absorb keys still held from the action-sheet selection (e.g. the Enter
  // that chose this item) so tick_textin does not see them as fresh presses.
  char hld[8];
  g_prev_n = kb_scan_full(hld, 8, NULL);
  memcpy(g_prev_held, hld, g_prev_n);
  g_view = VIEW_TEXTIN;
}

static void do_action(int code) {
  sd_entry_t* e = (g_entry_count > 0) ? &g_entries[g_file_sel] : 0;
  switch (code) {
    case A_OPEN:
      if (!e) break;
      if (e->is_dir) { path_push(e->name); files_reload(); g_view = VIEW_FILES; }
      else {
        char sz[16]; fmt_size(e->size, sz, sizeof(sz));
        snprintf(g_sd_info_buf, sizeof(g_sd_info_buf), "%s\nsize  %s", e->name, sz);
        show_msg("FILE", g_sd_info_buf);
      }
      break;
    case A_RENAME:
      if (!e) break;
      strncpy(g_rename_old, e->name, SD_NAME_MAX - 1); g_rename_old[SD_NAME_MAX - 1] = 0;
      begin_text(TIN_RENAME, e->name);
      break;
    case A_DELETE:
      if (!e) break;
      strncpy(g_del_name, e->name, SD_NAME_MAX - 1); g_del_name[SD_NAME_MAX - 1] = 0;
      g_del_isdir = e->is_dir;
      g_view = VIEW_CONFIRM;
      break;
    case A_NEWFILE: begin_text(TIN_NEWFILE, 0); break;
    case A_NEWDIR:  begin_text(TIN_NEWDIR, 0);  break;
    default: break;
  }
}

static bool name_valid(const char* sname) {
  if (!sname[0]) return false;
  for (const char* q = sname; *q; q++)
    if (*q == '/' || *q == '\\' || (unsigned char)*q < 0x20) return false;
  return true;
}

static void commit_text() {
  if (!name_valid(g_tin)) { show_msg("NAME", "Invalid name.\nNo / or backslash,\nnot empty."); return; }
  char rel[400];
  bool ok = false;
  if (g_tin_mode == TIN_RENAME) {
    char oldrel[400];
    entry_rel(g_rename_old, oldrel, sizeof(oldrel));
    entry_rel(g_tin, rel, sizeof(rel));
    ok = sd_rename(oldrel, rel);
  } else if (g_tin_mode == TIN_NEWFILE) {
    entry_rel(g_tin, rel, sizeof(rel));
    ok = sd_make_file(rel);
  } else if (g_tin_mode == TIN_NEWDIR) {
    entry_rel(g_tin, rel, sizeof(rel));
    ok = sd_make_dir(rel);
  }
  if (!ok) { show_msg("STORAGE", "Operation failed.\nName may exist\nor card is full."); return; }
  files_reload();
  g_view = VIEW_FILES;
}

static void draw_fileact() {
  canvas.fillSprite(C_BG);
  const char* t = (g_entry_count > 0) ? g_entries[g_file_sel].name : "ACTIONS";
  draw_statusbar(t);
  canvas.setFont(&fonts::Font2);
  for (int i = 0; i < g_act_n; i++) {
    int y = LIST_Y + i * ROW_H;
    bool sel = (i == g_act_sel);
    if (sel) canvas.fillRoundRect(4, y, W - 8, ROW_H - 2, 3, C_SELBG);
    bool danger = (g_act_codes[i] == A_DELETE);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setTextColor(danger ? C_RED : C_TEXT, sel ? C_SELBG : C_BG);
    canvas.drawString(g_act_labels[i], 14, y + 3);
  }
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString("ret ok   , back", 8, H - 2);
  canvas.pushSprite(0, 0);
}

static void tick_fileact() {
  char k = kb_poll();
  if (!k) return;
  switch (k) {
    case ';': if (g_act_sel > 0) g_act_sel--; break;
    case '.': if (g_act_sel < g_act_n - 1) g_act_sel++; break;
    case '/':
    case '\n':
    case '\r': do_action(g_act_codes[g_act_sel]); break;
    case ',':
    case '`': g_view = VIEW_FILES; break;
    default: break;
  }
}

static void draw_textin() {
  canvas.fillSprite(C_BG);
  const char* ttl = (g_tin_mode == TIN_RENAME)  ? "Rename"
                  : (g_tin_mode == TIN_NEWFILE) ? "New File" : "New Folder";
  draw_statusbar(ttl);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString("name:", 10, LIST_Y + 2);
  int by = LIST_Y + 24;
  canvas.drawRoundRect(8, by, W - 16, 26, 3, C_DIM);
  canvas.setTextColor(C_ACCENT, C_BG);
  char shown[SD_NAME_MAX + 2];
  snprintf(shown, sizeof(shown), "%s_", g_tin);
  canvas.drawString(shown, 14, by + 5);
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString("ret save   ` cancel", 8, H - 2);
  canvas.pushSprite(0, 0);
}

static void tick_textin() {
  char h[8];
  bool shift = false;
  int n = kb_scan_full(h, 8, &shift);
  for (int i = 0; i < n; i++) {
    char c = h[i];
    bool was = false;
    for (int j = 0; j < g_prev_n; j++) if (g_prev_held[j] == c) { was = true; break; }
    if (was) continue;
    if (c == '`') { g_prev_n = 0; g_prev_held[0] = 0; g_view = VIEW_FILEACT; return; }
    if (c == '\n' || c == '\r') { memcpy(g_prev_held, h, n); g_prev_n = n; commit_text(); return; }
    if (c == 0x08) { if (g_tin_len > 0) g_tin[--g_tin_len] = 0; continue; }
    if (c == '\t') continue;
    if ((unsigned char)c < 0x20) continue;
    if (c == '/' || c == '\\') continue;
    char e = c;
    if (shift && e >= 'a' && e <= 'z') e = e - 'a' + 'A';
    if (g_tin_len < SD_NAME_MAX - 1) { g_tin[g_tin_len++] = e; g_tin[g_tin_len] = 0; }
  }
  memcpy(g_prev_held, h, n);
  g_prev_n = n;
}

static void draw_confirm() {
  canvas.fillSprite(C_BG);
  draw_statusbar("Delete?");
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.setTextColor(C_TEXT, C_BG);
  canvas.drawString(g_del_isdir ? "Delete folder:" : "Delete file:", 10, LIST_Y + 2);
  canvas.setTextColor(C_RED, C_BG);
  char nm[SD_NAME_MAX + 2];
  snprintf(nm, sizeof(nm), "%s", g_del_name);
  canvas.drawString(nm, 10, LIST_Y + 26);
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString("ret delete   , cancel", 8, H - 2);
  canvas.pushSprite(0, 0);
}

static void tick_confirm() {
  char k = kb_poll();
  if (!k) return;
  if (k == '/' || k == '\n' || k == '\r') {
    char rel[400];
    entry_rel(g_del_name, rel, sizeof(rel));
    bool ok = sd_delete(rel, g_del_isdir);
    if (!ok) { show_msg("STORAGE", g_del_isdir ? "Folder not empty\nor delete failed." : "Delete failed."); return; }
    files_reload();
    g_view = VIEW_FILES;
  } else if (k == ',' || k == '`') {
    g_view = VIEW_FILES;
  }
}

// ======================= DUCK (BadUSB) =======================
#define DUCK_MAX         64
#define DUCK_SCRIPT_MAX  8192
static sd_entry_t* g_duck = 0;
static int  g_duck_count = 0, g_duck_sel = 0, g_duck_top = 0;
static bool g_duck_ok = false;
static int  g_duckrun_idx = 0;
static int  g_duckrun_state = 0;   // 0 idle, 1 running, 2 done
static int  g_duck_chardelay = 5;  // ms between characters
static int  g_duck_defdelay  = 0;  // ms after each command

// ASCII -> HID usage (US layout) + whether Shift is needed.
static bool char_to_hid(char c, uint8_t* u, bool* sh) {
  *sh = false;
  if (c >= 'a' && c <= 'z') { *u = 4 + (c - 'a'); return true; }
  if (c >= 'A' && c <= 'Z') { *u = 4 + (c - 'A'); *sh = true; return true; }
  if (c >= '1' && c <= '9') { *u = 0x1E + (c - '1'); return true; }
  switch (c) {
    case '0': *u = 0x27; return true;
    case '!': *u = 0x1E; *sh = true; return true;
    case '@': *u = 0x1F; *sh = true; return true;
    case '#': *u = 0x20; *sh = true; return true;
    case '$': *u = 0x21; *sh = true; return true;
    case '%': *u = 0x22; *sh = true; return true;
    case '^': *u = 0x23; *sh = true; return true;
    case '&': *u = 0x24; *sh = true; return true;
    case '*': *u = 0x25; *sh = true; return true;
    case '(': *u = 0x26; *sh = true; return true;
    case ')': *u = 0x27; *sh = true; return true;
    case ' ': *u = 0x2C; return true;
    case '-': *u = 0x2D; return true;
    case '_': *u = 0x2D; *sh = true; return true;
    case '=': *u = 0x2E; return true;
    case '+': *u = 0x2E; *sh = true; return true;
    case '[': *u = 0x2F; return true;
    case '{': *u = 0x2F; *sh = true; return true;
    case ']': *u = 0x30; return true;
    case '}': *u = 0x30; *sh = true; return true;
    case '\\': *u = 0x31; return true;
    case '|': *u = 0x31; *sh = true; return true;
    case ';': *u = 0x33; return true;
    case ':': *u = 0x33; *sh = true; return true;
    case '\'': *u = 0x34; return true;
    case '"': *u = 0x34; *sh = true; return true;
    case '`': *u = 0x35; return true;
    case '~': *u = 0x35; *sh = true; return true;
    case ',': *u = 0x36; return true;
    case '<': *u = 0x36; *sh = true; return true;
    case '.': *u = 0x37; return true;
    case '>': *u = 0x37; *sh = true; return true;
    case '/': *u = 0x38; return true;
    case '?': *u = 0x38; *sh = true; return true;
    case '\t': *u = 0x2B; return true;
    case '\n': case '\r': *u = 0x28; return true;
    default: return false;
  }
}

static void duck_press(uint8_t mod, uint8_t usage) {
  uint8_t keys[6] = { usage, 0, 0, 0, 0, 0 };
  hid_key(mod, keys);
  vTaskDelay(pdMS_TO_TICKS(2));
  uint8_t empty[6] = {0};
  hid_key(0, empty);
}
static void duck_type_char(char c) {
  uint8_t u; bool sh;
  if (!char_to_hid(c, &u, &sh)) return;
  duck_press(sh ? 0x02 : 0, u);
  vTaskDelay(pdMS_TO_TICKS(g_duck_chardelay));
}
static void duck_type_string(const char* str) { for (; *str; str++) duck_type_char(*str); }

static bool is_mod_token(const char* t, uint8_t* mbit) {
  if (!strcasecmp(t,"GUI") || !strcasecmp(t,"WINDOWS") || !strcasecmp(t,"COMMAND") || !strcasecmp(t,"META")) { *mbit = 0x08; return true; }
  if (!strcasecmp(t,"CTRL") || !strcasecmp(t,"CONTROL")) { *mbit = 0x01; return true; }
  if (!strcasecmp(t,"ALT")  || !strcasecmp(t,"OPTION"))  { *mbit = 0x04; return true; }
  if (!strcasecmp(t,"SHIFT")) { *mbit = 0x02; return true; }
  return false;
}
static bool key_token_usage(const char* t, uint8_t* u, bool* sh) {
  *sh = false;
  if (strlen(t) == 1) return char_to_hid(t[0], u, sh);
  if ((t[0]=='F'||t[0]=='f') && isdigit((unsigned char)t[1])) {
    int n = atoi(t + 1); if (n >= 1 && n <= 12) { *u = 0x3A + (n - 1); return true; }
  }
  if (!strcasecmp(t,"ENTER")||!strcasecmp(t,"RETURN")) { *u=0x28; return true; }
  if (!strcasecmp(t,"ESC")||!strcasecmp(t,"ESCAPE"))   { *u=0x29; return true; }
  if (!strcasecmp(t,"BACKSPACE"))  { *u=0x2A; return true; }
  if (!strcasecmp(t,"TAB"))        { *u=0x2B; return true; }
  if (!strcasecmp(t,"SPACE"))      { *u=0x2C; return true; }
  if (!strcasecmp(t,"CAPSLOCK"))   { *u=0x39; return true; }
  if (!strcasecmp(t,"DELETE")||!strcasecmp(t,"DEL")) { *u=0x4C; return true; }
  if (!strcasecmp(t,"INSERT"))     { *u=0x49; return true; }
  if (!strcasecmp(t,"HOME"))       { *u=0x4A; return true; }
  if (!strcasecmp(t,"END"))        { *u=0x4D; return true; }
  if (!strcasecmp(t,"PAGEUP"))     { *u=0x4B; return true; }
  if (!strcasecmp(t,"PAGEDOWN"))   { *u=0x4E; return true; }
  if (!strcasecmp(t,"UP")||!strcasecmp(t,"UPARROW"))       { *u=0x52; return true; }
  if (!strcasecmp(t,"DOWN")||!strcasecmp(t,"DOWNARROW"))   { *u=0x51; return true; }
  if (!strcasecmp(t,"LEFT")||!strcasecmp(t,"LEFTARROW"))   { *u=0x50; return true; }
  if (!strcasecmp(t,"RIGHT")||!strcasecmp(t,"RIGHTARROW")) { *u=0x4F; return true; }
  if (!strcasecmp(t,"PRINTSCREEN")) { *u=0x46; return true; }
  if (!strcasecmp(t,"MENU")||!strcasecmp(t,"APP")) { *u=0x65; return true; }
  return false;
}

static bool duck_check_abort() {
  char h[8]; int n = kb_scan_full(h, 8, NULL);
  for (int i = 0; i < n; i++) if (h[i] == '`') return true;
  return false;
}

// Execute one script line (mutates `line`).
static void duck_exec_line(char* line) {
  char* p = line;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == 0) return;
  char cmd[16]; int ci = 0; char* q = p;
  while (*q && *q != ' ' && *q != '\t' && ci < 15) cmd[ci++] = *q++;
  cmd[ci] = 0;
  char* arg = q; while (*arg == ' ' || *arg == '\t') arg++;

  if (!strcasecmp(cmd, "REM")) return;
  if (!strcasecmp(cmd, "STRING"))   { duck_type_string(arg); return; }
  if (!strcasecmp(cmd, "STRINGLN")) { duck_type_string(arg); duck_press(0, 0x28); return; }
  if (!strcasecmp(cmd, "DELAY"))    { int ms = atoi(arg); if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms)); return; }
  if (!strcasecmp(cmd, "DEFAULT_DELAY") || !strcasecmp(cmd, "DEFAULTDELAY")) { g_duck_defdelay = atoi(arg); return; }

  // Otherwise a key / modifier combo: tokenize the whole line.
  uint8_t mod = 0, usage = 0; bool anyKey = false, shift = false;
  char* tk = strtok(line, " \t");
  while (tk) {
    uint8_t mbit;
    if (is_mod_token(tk, &mbit)) mod |= mbit;
    else { uint8_t u; bool s2; if (key_token_usage(tk, &u, &s2)) { usage = u; if (s2) shift = true; anyKey = true; } }
    tk = strtok(NULL, " \t");
  }
  if (shift) mod |= 0x02;
  if (mod || anyKey) duck_press(mod, usage);
}

static void duck_run(const char* name) {
  char path[SD_NAME_MAX + 8];
  snprintf(path, sizeof(path), "/duck/%s", name);
  char* buf = (char*)malloc(DUCK_SCRIPT_MAX);
  if (!buf) { show_msg("DUCK", "Out of memory."); return; }
  int n = sd_read_file(path, buf, DUCK_SCRIPT_MAX);
  if (n < 0) { free(buf); show_msg("DUCK", "Read failed."); return; }
  g_duck_defdelay = 0;
  char* line = buf;
  while (line && *line) {
    char* nl = strchr(line, '\n');
    if (nl) *nl = 0;
    size_t ll = strlen(line);
    if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
    duck_exec_line(line);
    if (g_duck_defdelay > 0) vTaskDelay(pdMS_TO_TICKS(g_duck_defdelay));
    if (duck_check_abort()) break;
    line = nl ? nl + 1 : 0;
  }
  free(buf);
}

static bool ends_txt(const char* n) {
  size_t l = strlen(n);
  return l > 4 && !strcasecmp(n + l - 4, ".txt");
}
static void duck_reload() {
  g_duck_count = 0; g_duck_sel = 0; g_duck_top = 0;
  if (!g_duck_ok || !g_duck) return;
  bool t; int n = sd_list("/duck", g_duck, DUCK_MAX, &t);
  int w = 0;
  for (int i = 0; i < n; i++)
    if (!g_duck[i].is_dir && ends_txt(g_duck[i].name)) { if (w != i) g_duck[w] = g_duck[i]; w++; }
  g_duck_count = w;
}

static void duck_open(Xport xp) {
  g_xport = xp;
  if (xp == XPORT_USB) uhid_start("Nexus"); else bhid_start("Nexus");
  g_duck_ok = sd_mount();
  if (g_duck_ok) {
    sd_make_dir("/duck");    // harmless if it exists
    if (!g_duck) { g_duck = (sd_entry_t*)malloc(sizeof(sd_entry_t) * DUCK_MAX); if (!g_duck) g_duck_ok = false; }
  }
  duck_reload();
  g_view = VIEW_DUCK;
}
static void duck_close() {
  if (g_xport == XPORT_USB) uhid_stop(); else bhid_stop();
  if (g_duck) { free(g_duck); g_duck = 0; }
  sd_unmount();
  g_duck_ok = false; g_duck_count = 0;
  leave_screen();
}

static void draw_duck() {
  canvas.fillSprite(C_BG);
  char ttl[16]; snprintf(ttl, sizeof(ttl), "DUCK %s", xport_name());
  draw_statusbar(ttl);
  canvas.setFont(&fonts::Font2);
  if (g_duck_count == 0) {
    canvas.setTextColor(C_DIM, C_BG);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.drawString("no scripts in /duck", 12, LIST_Y + 4);
    canvas.drawString("add .txt payloads", 12, LIST_Y + 26);
  }
  for (int i = 0; i < VIS; i++) {
    int idx = g_duck_top + i;
    if (idx >= g_duck_count) break;
    int y = LIST_Y + i * ROW_H;
    bool sel = (idx == g_duck_sel);
    if (sel) canvas.fillRoundRect(4, y, W - 8, ROW_H - 2, 3, C_SELBG);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setTextColor(C_TEXT, sel ? C_SELBG : C_BG);
    canvas.drawString(g_duck[idx].name, 12, y + 3);
  }
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString(", back", 8, H - 2);
  canvas.pushSprite(0, 0);
}
static void tick_duck() {
  char k = kb_poll();
  if (!k) return;
  switch (k) {
    case ';': if (g_duck_sel > 0) g_duck_sel--; if (g_duck_sel < g_duck_top) g_duck_top = g_duck_sel; break;
    case '.': if (g_duck_sel < g_duck_count - 1) g_duck_sel++; if (g_duck_sel >= g_duck_top + VIS) g_duck_top = g_duck_sel - VIS + 1; break;
    case '/': case '\n': case '\r':
      if (g_duck_count > 0) { g_duckrun_idx = g_duck_sel; g_duckrun_state = 0; g_view = VIEW_DUCKRUN; }
      break;
    case ',': case '`': duck_close(); break;
    default: break;
  }
}

static void draw_duckrun() {
  canvas.fillSprite(C_BG);
  draw_statusbar(g_duck[g_duckrun_idx].name);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(textdatum_t::top_left);
  bool conn = hid_connected();
  if (g_duckrun_state == 1)      { canvas.setTextColor(C_AMBER, C_BG);  canvas.drawString("RUNNING...", 12, LIST_Y + 4); }
  else if (g_duckrun_state == 2) { canvas.setTextColor(C_ACCENT, C_BG); canvas.drawString("done", 12, LIST_Y + 4); }
  else if (conn)                 { canvas.setTextColor(C_ACCENT, C_BG); canvas.drawString("host connected", 12, LIST_Y + 4); }
  else if (g_xport == XPORT_USB) { canvas.setTextColor(C_AMBER, C_BG);  canvas.drawString("waiting for USB host", 12, LIST_Y + 4); }
  else                           { canvas.setTextColor(C_AMBER, C_BG);  canvas.drawString("advertising  PIN " NEXUS_PIN, 12, LIST_Y + 4); }
  canvas.setTextColor(C_TEXT, C_BG);
  canvas.drawString(conn ? "Enter: run payload" : "connect host first", 12, LIST_Y + 28);
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString("` abort during run   , back", 8, H - 2);
  canvas.pushSprite(0, 0);
}
static void tick_duckrun() {
  char k = kb_poll();
  if (!k) return;
  if (k == '/' || k == '\n' || k == '\r') {
    if (!hid_connected()) return;
    g_duckrun_state = 1;
    draw_duckrun();                 // show RUNNING before the blocking run
    duck_run(g_duck[g_duckrun_idx].name);
    g_duckrun_state = 2;
  } else if (k == ',' || k == '`') {
    g_view = VIEW_DUCK;
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
          sync_storage();
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
      case VIEW_FILES:   tick_files();   if (g_view == VIEW_FILES)   draw_files();   break;
      case VIEW_FILEACT: tick_fileact(); if (g_view == VIEW_FILEACT) draw_fileact(); break;
      case VIEW_TEXTIN:  tick_textin();  if (g_view == VIEW_TEXTIN)  draw_textin();  break;
      case VIEW_CONFIRM: tick_confirm(); if (g_view == VIEW_CONFIRM) draw_confirm(); break;
      case VIEW_DUCK:    tick_duck();    if (g_view == VIEW_DUCK)    draw_duck();    break;
      case VIEW_DUCKRUN: tick_duckrun(); if (g_view == VIEW_DUCKRUN) draw_duckrun(); break;
    }
    vTaskDelay(pdMS_TO_TICKS(16));
  }
}
