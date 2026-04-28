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

} // namespace save_state
