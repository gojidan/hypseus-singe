// save_state.h — minimal save/load state for Z80-based games
// 2026-04-28: framework for frame-by-frame deterministic scan
//
// Saves: m80_context (Z80 registers + interrupt state) + cpumem dump + disc frame
// Loads: restores everything; caller must seek LDP to disc_frame after load.
#pragma once

#include <stdint.h>

namespace save_state {

// Save current state to file.
// cpumem: pointer to game's cpumem buffer (64 KB for DL).
// cpumem_size: usually 0x10000 (cpu::MEM_SIZE).
// disc_frame: current LDP frame (caller passes ldp::get_current_frame() or similar).
// Returns true on success.
bool save(const char* filename,
          const uint8_t* cpumem,
          uint32_t cpumem_size,
          uint32_t disc_frame);

// Load state from file.
// cpumem: pointer where to write cpumem (must be at least cpumem_size bytes).
// cpumem_size: expected size; if mismatch with file's saved size, fails.
// out_disc_frame: optional output for the saved disc frame.
// Returns true on success.
bool load(const char* filename,
          uint8_t* cpumem,
          uint32_t cpumem_size,
          uint32_t* out_disc_frame);


// ─── Triggered save: arm a save-on-search ────────────────────────────────
//
// Call once at startup (e.g. from CLI flag handler).  When a subsequent
// ldp::pre_search() seeks to `target_frame`, the save_state framework
// dumps the current state to `path` and (optionally) sets the quitflag
// so Hypseus terminates after the save.
//
// Pass NULL or "" to path to disarm.

void arm_save_on_search(uint32_t target_frame,
                        const char* path,
                        bool quit_after_save);

// Called by ldp::pre_search() with the search target frame.  If a save
// has been armed and the frame matches, performs the save now.
// `cpumem`, `cpumem_size`: passed from the active game (g_game).
// Returns true if a save was performed.
bool check_search_save(uint32_t search_to_frame,
                       uint8_t* cpumem,
                       uint32_t cpumem_size);

} // namespace save_state
