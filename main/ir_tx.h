// ir_tx.h -- IR transmitter for Nexus. Drives the front IR emitter on GPIO44
// via the RMT peripheral. Ships a universal "TV power" code table (202 codes
// expanded from the Flipper universal tv.ir library, multi-protocol, each with
// its own carrier frequency) stored as raw on/off microsecond timing.
//
// Three uses:
//   * Blast  -- walk the whole table on a timer so pointing it at any TV turns
//               it off in ~45s (TV-B-Gone style).
//   * Manual -- send one chosen code (for the "Find My TV" step-through: there
//               is NO IR receiver on this board, so the TV reacting is the only
//               feedback; the user saves the code that worked).
// Uses no radio, so it coexists with everything; acquires RMT on start/begin
// and frees it on stop/end. FOR YOUR OWN DEVICES / WITH PERMISSION.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- auto-blast sweep (loops the whole table) ----
bool     ir_blast_start(void);     // acquire RMT + start the sweep timer
void     ir_blast_stop(void);      // stop timer + release RMT
bool     ir_blast_running(void);
uint32_t ir_blast_sent(void);      // codes sent since start
int      ir_blast_index(void);     // index of the code last transmitted

// ---- manual single-code mode (Find My TV / My TV replay) ----
bool     ir_manual_begin(void);    // acquire RMT (no timer)
void     ir_manual_end(void);      // release RMT
bool     ir_manual_send(int idx);  // send code idx a few times; false if busy/bad idx
bool     ir_manual_active(void);

int      ir_count(void);           // number of codes in the table
int      ir_code_freq(int idx);    // carrier Hz of a code (for the UI)

#ifdef __cplusplus
}
#endif
