// ir_tx.cpp -- universal IR "TV power" transmitter (see ir_tx.h).
//
// The code table (ir_codes.h) stores each TV power signal as raw on/off timing
// in microseconds, always starting with a mark (carrier on). This covers every
// protocol uniformly (NEC, Samsung, Sony SIRC, RC5, ...) because the protocol
// math was already expanded at build time -- the firmware just replays timing.
// The RMT channel runs at 1 MHz (1 tick = 1 us) so durations are literal us;
// the carrier (frequency + 33% duty) is re-applied per code since brands differ
// (36 kHz Philips, 38 kHz most, 40 kHz Sony). Carrier affects only the marks.
#include "ir_tx.h"
#include "ir_codes.h"
#include <string.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "ir_tx";

#define IR_GPIO        44
#define IR_RESOLUTION  1000000      // 1 MHz -> 1 tick == 1 us
#define SWEEP_MS       220          // gap between codes in blast mode
#define MANUAL_REPEAT  3            // times to send a code in manual mode
#define MAX_SYM        64           // max RMT symbols per code (longest is 50)

static bool                 g_blast   = false;
static bool                 g_manual  = false;
static volatile uint32_t    g_sent    = 0;
static volatile int         g_idx     = 0;
static esp_timer_handle_t   g_tmr     = 0;
static rmt_channel_handle_t g_chan    = 0;
static rmt_encoder_handle_t g_enc     = 0;
static int                  g_cur_freq = 0;   // carrier currently applied

// Build RMT symbols for one raw code. t[] alternates mark,space,mark,... (us),
// starting on a mark; n may be odd (final lone mark). Returns symbol count.
static size_t build_syms(const uint16_t* t, int n, rmt_symbol_word_t* out) {
  size_t k = 0;
  for (int i = 0; i < n; i += 2) {
    out[k].level0 = 1; out[k].duration0 = t[i];
    if (i + 1 < n) { out[k].level1 = 0; out[k].duration1 = t[i + 1]; }
    else           { out[k].level1 = 0; out[k].duration1 = 0; }
    k++;
  }
  return k;
}

static bool apply_carrier(int freq) {
  if (freq == g_cur_freq) return true;
  rmt_carrier_config_t carrier = {};
  carrier.frequency_hz = freq;
  carrier.duty_cycle   = 0.33f;
  if (rmt_apply_carrier(g_chan, &carrier) != ESP_OK) return false;
  g_cur_freq = freq;
  return true;
}

// Send one code once (channel must be acquired + idle).
static void send_once(int idx) {
  if (!g_chan || !g_enc) return;
  if (idx < 0 || idx >= IR_CODE_COUNT) return;
  const IrCode* c = &IR_CODES[idx];
  if (!apply_carrier(c->freq)) return;
  rmt_symbol_word_t sym[MAX_SYM];
  size_t n = build_syms(c->t, c->n, sym);
  rmt_transmit_config_t tx = {};
  tx.loop_count = 0;
  if (rmt_transmit(g_chan, g_enc, sym, n * sizeof(rmt_symbol_word_t), &tx) == ESP_OK) {
    rmt_tx_wait_all_done(g_chan, 200);
    g_sent = g_sent + 1;
  }
}

// ---- channel lifecycle (shared by blast + manual) ----
static bool acquire(void) {
  if (g_chan) return true;
  g_cur_freq = 0;
  rmt_tx_channel_config_t ccfg = {};
  ccfg.gpio_num          = (gpio_num_t)IR_GPIO;
  ccfg.clk_src           = RMT_CLK_SRC_DEFAULT;
  ccfg.resolution_hz     = IR_RESOLUTION;
  ccfg.mem_block_symbols = 64;
  ccfg.trans_queue_depth = 4;
  if (rmt_new_tx_channel(&ccfg, &g_chan) != ESP_OK) { g_chan = 0; return false; }

  rmt_copy_encoder_config_t ecfg = {};
  if (rmt_new_copy_encoder(&ecfg, &g_enc) != ESP_OK) {
    rmt_del_channel(g_chan); g_chan = 0; return false;
  }
  if (rmt_enable(g_chan) != ESP_OK) {
    rmt_del_encoder(g_enc); g_enc = 0;
    rmt_del_channel(g_chan); g_chan = 0; return false;
  }
  return true;
}

static void release(void) {
  if (g_chan) rmt_disable(g_chan);
  if (g_enc)  { rmt_del_encoder(g_enc); g_enc = 0; }
  if (g_chan) { rmt_del_channel(g_chan); g_chan = 0; }
  g_cur_freq = 0;
}

// ---- blast sweep ----
static void tick_cb(void* arg) {
  (void)arg;
  int i = g_idx;
  send_once(i);
  g_idx = (i + 1) % IR_CODE_COUNT;
}

bool ir_blast_start(void) {
  if (g_blast) return true;
  if (g_manual) return false;
  if (!acquire()) return false;
  g_sent = 0; g_idx = 0;
  esp_timer_create_args_t ta = {};
  ta.callback = tick_cb;
  ta.name     = "ir_blast";
  if (esp_timer_create(&ta, &g_tmr) != ESP_OK) { release(); return false; }
  esp_timer_start_periodic(g_tmr, (uint64_t)SWEEP_MS * 1000);
  g_blast = true;
  ESP_LOGI(TAG, "IR blast start (%d codes)", IR_CODE_COUNT);
  return true;
}

void ir_blast_stop(void) {
  if (!g_blast) return;
  if (g_tmr) { esp_timer_stop(g_tmr); esp_timer_delete(g_tmr); g_tmr = 0; }
  release();
  g_blast = false;
  ESP_LOGI(TAG, "IR blast stop (sent %u)", (unsigned)g_sent);
}

bool     ir_blast_running(void) { return g_blast; }
uint32_t ir_blast_sent(void)    { return g_sent; }
int      ir_blast_index(void)   { int i = g_idx - 1; if (i < 0) i = IR_CODE_COUNT - 1; return i; }

// ---- manual single-code ----
bool ir_manual_begin(void) {
  if (g_manual) return true;
  if (g_blast) return false;
  if (!acquire()) return false;
  g_sent = 0;
  g_manual = true;
  return true;
}

void ir_manual_end(void) {
  if (!g_manual) return;
  release();
  g_manual = false;
}

bool ir_manual_send(int idx) {
  if (!g_manual) return false;
  if (idx < 0 || idx >= IR_CODE_COUNT) return false;
  for (int r = 0; r < MANUAL_REPEAT; r++) send_once(idx);
  return true;
}

bool ir_manual_active(void) { return g_manual; }

int ir_count(void) { return IR_CODE_COUNT; }
int ir_code_freq(int idx) {
  if (idx < 0 || idx >= IR_CODE_COUNT) return 0;
  return IR_CODES[idx].freq;
}
