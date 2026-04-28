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


// ─── Triggered load: arm a load-on-game-start ────────────────────────────
//
// Call once at startup (e.g. from CLI flag handler) to arm a load.
// Then call try_load_armed() from the game's init/start routine — if
// armed, it loads the state into the provided cpumem buffer and returns
// the saved disc frame.  Caller must seek LDP to that frame.
//
// In test mode, additional parameters configure what to do after the load:
//   test_frame_offset: how many disc frames after the saved frame to
//                      apply the input
//   test_input:        which input to send (UP/DOWN/LEFT/RIGHT/BUTTON1/NONE)
//                      use '\0' to load without testing
//   test_timeout_ms:   how long to wait after the input before quitting
//
// Pass NULL to filename to disarm.

void arm_load(const char* filename,
              int32_t test_frame_offset,
              char test_input,
              uint32_t test_timeout_ms);

// True if a load has been armed.
bool is_load_armed();

// Try to perform the armed load.  cpumem must be at least cpumem_size bytes.
// Returns true if loaded; out_disc_frame is set to the saved frame.
// After this call, caller MUST issue ldp seek to *out_disc_frame.
bool try_load_armed(uint8_t* cpumem,
                    uint32_t cpumem_size,
                    uint32_t* out_disc_frame);

// Returns the test parameters from the armed load.
// Useful for the bot/explorer to know what input to apply at what frame.
int32_t  get_test_frame_offset();
char     get_test_input();
uint32_t get_test_timeout_ms();

} // namespace save_state
