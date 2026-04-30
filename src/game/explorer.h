#pragma once

#include <stdint.h>

/*
 * explorer.h
 *
 * Auto-explorer for Dragon's Lair / Space Ace ROM mapping.
 *
 * Two modes:
 *   Simple:  inject one move throughout the game (-explorerR / -explorerL / etc.)
 *   Guided:  follow the correct sequence for each scene using frame-accurate
 *            slot timings derived from real ROM logs (-explorerG[delta])
 *
 * Simple mode:
 *   -explorerR          pulse RIGHT throughout the game
 *   -explorerR5         pulse RIGHT, wait 5 s after game start before pulsing
 *   -explorerN          no move (baseline run)
 *
 * Guided mode:
 *   -explorerG          replay correct inputs at logged frame offsets, delta=0
 *   -explorerG10        same but +10 disc frames later per slot (probe window end)
 *   -explorerG-10       same but -10 disc frames earlier per slot (probe window start)
 *
 * After 6 simple runs (one per move) the wrong-input map for every scene is
 * complete.  Guided runs at varying delta then pin the exact acceptance window
 * boundaries at each difficulty level.
 */

namespace explorer {

// Action returned by tick() — lair::do_nmi() applies these via
// this->input_enable() / this->input_disable() to bypass the coin queue
// and write directly to the ROM hardware registers.
//
// press_mask / release_mask are bitmasks: bit i = (1u << SWITCH_i).
// Multiple bits may be set to press/release several switches in one tick.
struct Action {
    uint32_t press_mask;    // SWITCH_* bitmask to press this tick,   0 = none
    uint32_t release_mask;  // SWITCH_* bitmask to release this tick, 0 = none
};

// Simple mode: pulse one move throughout the game.
// move_char:  'U' 'L' 'D' 'R' 'B' 'N'
// delay_sec:  seconds to wait after entering PLAYING before pulsing (default 0)
// Returns false if move_char is unknown.
bool init(char move_char, uint32_t delay_sec = 0);

// Guided mode: follow per-scene correct inputs at logged frame offsets.
// delta_frames: signed frame offset applied to every slot timing.
//   0  → use the exactly-logged timing
//  +N  → press N disc frames later  (probe window end)
//  -N  → press N disc frames earlier (probe window start)
bool init_guided(int32_t delta_frames = 0);

// Hard difficulty guided mode: uses SCENE_TABLE_HARD (different offsets, more
// slots in some scenes, alias frames for start_dead recognition).
bool init_guided_hard(int32_t delta_frames = 0);

// Scan mode: like guided, but for one specific slot in one scene the offset
// and/or input is varied automatically across visits (via the death queue).
// frame:       scene start disc frame (e.g. 22936)
// slot:        1-based slot index to scan
// input_char:  'L' 'R' 'U' 'D' 'B' or '0' to keep the SCENE_TABLE input
// start_delta: delta relative to SCENE_TABLE offset for the first visit
// step:        delta increment per visit (positive = scan upward, negative = downward)
bool init_scan(uint32_t frame, int slot, char input_char,
               int32_t start_delta, int32_t step);

// Global-shift mask: add a scene:slot entry to the "forced to original offset"
// list.  During -marabelli<N> runs, masked slots ignore the shift and use their
// SCENE_TABLE offset.  Used to unblock scan of later slots after earlier ones
// are already characterised.  frame=scene_start (e.g. 2353), slot_1based=1..N
bool add_shift_mask(uint32_t frame, int slot_1based);

// 2026-04-28: Test mode (frame-by-frame deterministic scan).
// Used together with -loadstate: after the state is loaded, the bot starts
// directly in PLAYING (no BOOT/ATTRACT/COIN), waits for current_disc to reach
// target_frame, applies a single input pulse, captures outcomes for
// timeout_ms, then sets the quitflag.
//
// scene_canonical_frame: frame from which the loaded state was saved
// frame_offset:          disc frames after canonical to apply the input
//                        (target frame = canonical + offset)
// input_char:            'U' 'D' 'L' 'R' 'B' (BUTTON1) — or '\0' = no press
//                        (just observe what the ROM does at that frame)
// timeout_ms:            how long to record events after the input before
//                        quitting (typical: 3000-5000 ms)
//
// Returns false if input_char is invalid.
bool init_test_mode(uint32_t scene_canonical_frame,
                    int32_t  frame_offset,
                    char     input_char,
                    uint32_t timeout_ms);

// 2026-04-30: Approach D — input chaining for slot 2+ scans.
// Instead of saving state at slot N entry (which has VLDP state issues),
// we load slot 1 entry save and chain N-1 "correct" inputs of preceding
// slots before applying the test input for slot N.  ROM transitions
// naturally through slot 1 -> slot 2 -> ... in real time, no save/load
// disruption.  VLDP plays cleanly because the load is at slot 1 entry
// (the same point that worked at 99.94% match for slot 1 scan).
//
// scene_canonical_frame: the saved-state's canonical frame (e.g. 1887)
// steps_offsets / steps_inputs / n_steps: list of (offset, input) pairs.
//   Each step waits for disc to reach scene_canonical+offset, applies
//   input, releases, advances to next step.  The LAST step is the
//   "test step" — after it, we count timeout_ms and quit.
// timeout_ms: how long to record events after the LAST input.
//
// For Vestibule slot 2 scan, n_steps=2:
//   step[0] = (offset=58, input='R')   # slot 1 correct
//   step[1] = (offset=110, input='X')  # slot 2 test
struct TestStep {
    int32_t offset;
    char    input;
};
bool init_test_mode_chain(uint32_t scene_canonical_frame,
                          const TestStep* steps,
                          int n_steps,
                          uint32_t timeout_ms);

bool is_active();

// Drive the state machine — call once from lair::do_nmi().
// current_disc_frame: pass g_ldp->get_current_frame() — used by guided mode
// to fire at the exact same disc frame as the reference human run, independent
// of NMI rate fluctuations.
Action tick(uint32_t current_disc_frame);

// Notify lives register change — call from cpu_mem_write at 0xE03E.
void on_lives(uint8_t n);

// Notify scene jump — call from ldp::pre_search().
void on_search(uint32_t from, uint32_t to);

} // namespace explorer
