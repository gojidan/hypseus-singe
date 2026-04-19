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

bool is_active();

// Drive the state machine — call once from lair::do_nmi().
// Returns an Action for lair to apply via this->input_enable/disable.
Action tick();

// Notify lives register change — call from cpu_mem_write at 0xE03E.
void on_lives(uint8_t n);

// Notify scene jump — call from ldp::pre_search().
void on_search(uint32_t from, uint32_t to);

} // namespace explorer
