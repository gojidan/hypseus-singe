#pragma once

#include <stdint.h>

/*
 * explorer.h
 *
 * Auto-explorer for Dragon's Lair / Space Ace ROM mapping.
 * Injects a single designated move throughout the entire game,
 * auto-restarts after game over, and logs all ROM responses via rom_logger.
 *
 * Activate with command-line flag: -explorerR / -explorerL / -explorerU /
 *   -explorerD / -explorerB / -explorerN (none = baseline run)
 *
 * After 6 runs (one per move) × 3 difficulties the acceptance window matrix
 * for every scene is complete.
 */

namespace explorer {

// Parse move char and arm the explorer.
// move_char: 'U' 'L' 'D' 'R' 'B' 'N'
// Returns false if move_char is unknown.
bool init(char move_char);

bool is_active();

// Drive the state machine — call once from lair::do_nmi()
void tick();

// Notify lives register change — call from cpu_mem_write at 0xE03E
void on_lives(uint8_t n);

// Notify scene jump — call from ldp::pre_search()
void on_search(uint32_t from, uint32_t to);

} // namespace explorer
