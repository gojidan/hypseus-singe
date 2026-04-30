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
// Call from CLI flag handler.  When a subsequent ldp::pre_search() seeks
// to `target_frame`, the save_state framework dumps the current state
// to `path`.
//
// Multiple targets can be armed (one Hypseus run captures many scenes).
// Each call appends to the armed-targets list.  Each target fires at
// most once (consumed on first hit).
//
// quit_after_save: when true, Hypseus quits after the LAST armed target
// has been consumed (i.e. when the list becomes empty).  Pass true on
// the FINAL arm call of a batch — it is sticky once set.
//
// Pass NULL or "" to path to disarm ALL targets.

void arm_save_on_search(uint32_t target_frame,
                        const char* path,
                        bool quit_after_save);

// True if at least one save target is still armed (waiting to fire).
bool has_armed_saves();

// Number of armed save targets still waiting to fire.
int  armed_saves_pending();


// ─── Triggered save: arm a save-after-N-accepts ─────────────────────────
//
// 2026-04-29: extension for multi-slot scenes (Singe = 12 moves in one
// canonical, Tentacles, Three Caves, Socker Boppers, Elevator).
//
// When ROM searches to `scene_canonical` the framework starts counting
// accept events from 0 in that scene.  When the Nth accept fires inside
// that scene, the state is saved to `path` BEFORE returning from the
// accept hook (so the saved disc frame is the frame at which the input
// landed).  N=1 means "after the 1st accept", N=2 "after the 2nd", etc.
//
// IMPORTANT (2026-04-29 nota dell'utente): "accept" means the ROM
// recognized the input, NOT that the move is correct.  An accept can
// be followed by either:
//   - a search to the next canonical scene (correct move)
//   - a search to a death cinematic frame (accepted but wrong)
// arm_save_after_accept fires on EVERY accept, so the loaded state may
// represent an "accepted-but-wrong" branch.  In Phase 3 (analyze) we
// distinguish the two by reading what the ROM searches to AFTER the
// accept.  For multi-slot scanning purposes, only "accept + still in
// scene" save states are useful — the analyzer must filter those.
//
// quit_after_save semantics match arm_save_on_search (sticky once set).

// 2026-04-29 sera: delay_nmi parameter — number of NMI ticks (40Hz) to
// wait AFTER the accept emission before saving.  delay_nmi=0 saves
// immediately at the accept beep (original behaviour).  delay_nmi=20
// (~0.5s) lets the ROM enter the post-accept state (e.g. stagger) and
// gives VLDP time to receive its commands, producing a "cleaner" save.
void arm_save_after_accept(uint32_t scene_canonical,
                           int accept_count,
                           const char* path,
                           bool quit_after_save,
                           int delay_nmi = 0);

// Called by the per-game accept-detection hook (lair::write_ioport for
// DL/SA) when ROM writes the "accept" beep trigger.  Updates the
// per-scene accept counter and arms any armed save whose
// (scene, count) now matches.
//
// 2026-04-29 sera: with delayed-save, the actual save() call is deferred
// until tick_nmi() has counted down delay_nmi ticks.  This lets the ROM
// finish the accept routine and enter the next state (e.g. stagger) so
// VLDP receives its commands and the saved state is "clean".
// Returns true if a save was triggered or armed for delayed-save.
bool notify_accept(uint8_t* cpumem,
                   uint32_t cpumem_size,
                   uint32_t current_frame);

// Called from the per-game NMI handler (lair::do_nmi for DL/SA) ~40Hz.
// If a delayed save is pending, decrements its counter and performs the
// save when it reaches 0.
void tick_nmi(uint8_t* cpumem,
              uint32_t cpumem_size,
              uint32_t current_frame);

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

// 2026-04-30: Approach D — input chaining.
// arm_load_chain stores N (offset, input) pairs.  The first N-1 are
// "setup" steps that drive the ROM to slot N's listening state by
// chaining the correct slot 1..N-1 inputs.  The N-th step is the
// "test" step.  After it, capture timeout_ms and quit.
//
// max chain length: 16 (TEST_MAX_STEPS).
void arm_load_chain(const char* filename,
                    const int32_t* offsets,
                    const char* inputs,
                    int n_steps,
                    uint32_t timeout_ms);

// Number of chain steps (1 if armed via arm_load, N if via arm_load_chain).
int     get_test_chain_count();
// Per-step accessors (index 0..count-1).
int32_t get_test_chain_offset(int index);
char    get_test_chain_input(int index);

} // namespace save_state
