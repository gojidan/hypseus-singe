// save_state.cpp — implementation of save/load state.
// File format:
//   [8 bytes] magic "HYPSV01\0"
//   [4 bytes] cpumem_size (little-endian uint32)
//   [4 bytes] disc_frame (little-endian uint32)
//   [N bytes] m80_context struct (sizeof of struct, host endian)
//   [cpumem_size bytes] cpumem dump
//
// NOTE: not portable across CPUs/compilers (struct layout depends on host).
//       This is OK for our purposes (one machine producing & consuming).

#include "save_state.h"
#include "cpu/m80.h"            // for M80_REG_COUNT enum (used by m80_internal.h)
#include "cpu/m80_internal.h"   // for struct m80_context and extern g_context
#include "hypseus.h"            // for set_quitflag()

#include <stdio.h>
#include <string.h>
#include <vector>

extern struct m80_context g_context;

namespace save_state {

static const char MAGIC[8] = { 'H','Y','P','S','V','0','1','\0' };

// Armed save-on-search state.
// 2026-04-29: list of targets so a single Hypseus run can save many scenes
// while the bot plays through the game.
struct ArmedSave {
    uint32_t target_frame;
    char     path[512];
};
static std::vector<ArmedSave> s_armed_saves;
static bool s_quit_when_all_saved = false;

// Armed save-after-N-accepts (slot 2+ in multi-slot scenes).
// 2026-04-29.
struct ArmedSaveAfterAccept {
    uint32_t scene_canonical;
    int      accept_count;   // 1-based: 1 = after 1st accept, 2 = after 2nd, ...
    int      delay_nmi;      // NMI ticks to wait after the accept before saving
    char     path[512];
};
static std::vector<ArmedSaveAfterAccept> s_armed_after_accept;

// Per-scene accept tracking: updated in check_search_save (every search
// resets the counter and updates the current scene anchor) and in
// notify_accept (each accept increments).
static uint32_t s_current_scene = 0;
static int      s_accept_in_scene = 0;

// Delayed-save: when an accept matches an armed (scene, count) entry with
// delay_nmi > 0, instead of saving immediately we record the pending save
// here and let tick_nmi() count it down.  Only one delayed save can be
// pending at a time (the next accept overwrites — should not happen for
// well-formed scans).
static bool     s_pending_save_active   = false;
static int      s_pending_save_remain   = 0;
static char     s_pending_save_path[512] = { 0 };

// 2026-05-03 pomeriggio: "save at next search complete" pending state.
// When delay_nmi == -1 in arm_save_after_accept, we set this flag instead
// of using s_pending_save_remain. The next pre_search captures the target
// frame; tick_nmi monitors current_frame and saves when VLDP reaches it.
static bool     s_save_at_search_active        = false;
static uint32_t s_save_at_search_target_frame  = 0;  // 0 = not yet captured
static uint32_t s_save_at_search_frame_at_accept = 0; // frame at the triggering accept beep
static char     s_save_at_search_path[512]     = { 0 };

// Minimum search jump (in frames) required to consider a pre_search the
// "real" sub-state transition vs. an intra-scene small seek. The ROM may
// emit small pre_search calls for housekeeping right after the accept;
// only seeks of >= MIN_SEARCH_JUMP frames from the accept point count as
// the genuine sub-state transition we want to align the save with.
static const uint32_t SAVE_AT_SEARCH_MIN_JUMP = 50;

// Armed load state.
static char     s_load_path[512]      = { 0 };
static bool     s_load_armed_flag     = false;
static int32_t  s_test_frame_offset   = 0;
static char     s_test_input          = '\0';
static uint32_t s_test_timeout_ms     = 5000;

// 2026-04-30: chain test parameters (Approach D).  Stored as parallel
// arrays for simplicity.  arm_load() populates a chain of length 1.
// arm_load_chain() populates the full chain.
#define SAVE_STATE_MAX_CHAIN 16
static int      s_test_chain_count    = 0;
static int32_t  s_test_chain_offsets[SAVE_STATE_MAX_CHAIN] = { 0 };
static char     s_test_chain_inputs[SAVE_STATE_MAX_CHAIN]  = { 0 };

bool save(const char* filename,
          const uint8_t* cpumem,
          uint32_t cpumem_size,
          uint32_t disc_frame)
{
    FILE* f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "[save_state] save: cannot open %s for writing\n", filename);
        return false;
    }

    if (fwrite(MAGIC, 1, 8, f) != 8) goto fail;
    if (fwrite(&cpumem_size, 4, 1, f) != 1) goto fail;
    if (fwrite(&disc_frame, 4, 1, f) != 1) goto fail;
    if (fwrite(&g_context, sizeof(g_context), 1, f) != 1) goto fail;
    if (fwrite(cpumem, 1, cpumem_size, f) != cpumem_size) goto fail;

    fclose(f);
    fprintf(stderr, "[save_state] saved %u bytes cpumem + Z80 ctx to %s (frame=%u)\n",
            cpumem_size, filename, disc_frame);
    fflush(stderr);
    return true;

fail:
    fclose(f);
    fprintf(stderr, "[save_state] save: write error on %s\n", filename);
    return false;
}

bool load(const char* filename,
          uint8_t* cpumem,
          uint32_t cpumem_size,
          uint32_t* out_disc_frame)
{
    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "[save_state] load: cannot open %s\n", filename);
        return false;
    }

    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, MAGIC, 8) != 0) {
        fclose(f);
        fprintf(stderr, "[save_state] load: bad magic in %s\n", filename);
        return false;
    }

    uint32_t saved_size = 0, frame = 0;
    if (fread(&saved_size, 4, 1, f) != 1) goto fail;
    if (fread(&frame, 4, 1, f) != 1) goto fail;

    if (saved_size != cpumem_size) {
        fclose(f);
        fprintf(stderr, "[save_state] load: cpumem size mismatch (file=%u expected=%u)\n",
                saved_size, cpumem_size);
        return false;
    }

    if (fread(&g_context, sizeof(g_context), 1, f) != 1) goto fail;
    if (fread(cpumem, 1, cpumem_size, f) != cpumem_size) goto fail;

    fclose(f);
    if (out_disc_frame) *out_disc_frame = frame;
    fprintf(stderr, "[save_state] loaded from %s (frame=%u, %u bytes cpumem)\n",
            filename, frame, cpumem_size);
    fflush(stderr);
    return true;

fail:
    fclose(f);
    fprintf(stderr, "[save_state] load: read error on %s\n", filename);
    return false;
}

// ─── Triggered save-on-search ─────────────────────────────────────────────

void arm_save_on_search(uint32_t target_frame, const char* path, bool quit_after_save)
{
    if (path == NULL || path[0] == '\0') {
        // Disarm all targets.
        s_armed_saves.clear();
        s_quit_when_all_saved = false;
        return;
    }
    ArmedSave a;
    a.target_frame = target_frame;
    strncpy(a.path, path, sizeof(a.path) - 1);
    a.path[sizeof(a.path) - 1] = '\0';
    s_armed_saves.push_back(a);
    if (quit_after_save) s_quit_when_all_saved = true;  // sticky
    fprintf(stderr, "[save_state] armed: frame %u -> '%s' (pending=%d, quit_when_empty=%s)\n",
            target_frame, path, (int)s_armed_saves.size(),
            s_quit_when_all_saved ? "yes" : "no");
    fflush(stderr);
}

bool has_armed_saves() { return !s_armed_saves.empty(); }
int  armed_saves_pending() { return (int)s_armed_saves.size(); }

bool check_search_save(uint32_t search_to_frame, uint8_t* cpumem, uint32_t cpumem_size)
{
    // Track scene context for after-accept matching.  The anchor is only
    // updated when the search target matches a scene we're tracking for
    // slot 2+ purposes — this avoids spurious resets on intra-scene
    // frame seeks (the ROM does many small pre_search calls during normal
    // playback within a scene).
    //
    // 2026-04-29 design fix: previously we reset on every search, which
    // meant the per-scene accept counter never advanced past 0 because
    // every frame increment was a new "scene".
    bool is_tracked_scene = false;
    for (size_t i = 0; i < s_armed_after_accept.size(); ++i) {
        if (s_armed_after_accept[i].scene_canonical == search_to_frame) {
            is_tracked_scene = true;
            break;
        }
    }
    if (is_tracked_scene) {
        s_current_scene = search_to_frame;
        s_accept_in_scene = 0;
        fprintf(stderr, "[save_state] scene anchor set: %u (accept counter reset)\n",
                search_to_frame);
        fflush(stderr);
    }

    // 2026-05-03: capture next-search target for save-at-search-complete mode.
    // Need the search to be a "genuine sub-state transition", NOT a small
    // intra-scene seek emitted by the ROM right after the accept (which
    // would set the target = current frame and trigger an immediate save
    // at the accept beep frame, defeating the purpose).
    // Heuristic: target_frame must be at least SAVE_AT_SEARCH_MIN_JUMP
    // frames away from the accept-trigger frame. For DL scenes, sub-state
    // transitions are typically 80-200 frames past the accept point.
    if (s_save_at_search_active && s_save_at_search_target_frame == 0
        && search_to_frame != s_current_scene) {
        uint32_t jump = (search_to_frame > s_save_at_search_frame_at_accept)
                        ? (search_to_frame - s_save_at_search_frame_at_accept)
                        : (s_save_at_search_frame_at_accept - search_to_frame);
        if (jump >= SAVE_AT_SEARCH_MIN_JUMP) {
            s_save_at_search_target_frame = search_to_frame;
            fprintf(stderr, "[save_state] save-at-search target captured: frame %u (jump %u from accept frame %u)\n",
                    search_to_frame, jump, s_save_at_search_frame_at_accept);
            fflush(stderr);
        } else {
            fprintf(stderr, "[save_state] save-at-search: ignoring small seek to %u (jump %u < %u, not the real sub-state)\n",
                    search_to_frame, jump, SAVE_AT_SEARCH_MIN_JUMP);
            fflush(stderr);
        }
    }

    if (s_armed_saves.empty()) return false;

    // Find a matching armed target.
    int hit_idx = -1;
    for (size_t i = 0; i < s_armed_saves.size(); ++i) {
        if (s_armed_saves[i].target_frame == search_to_frame) {
            hit_idx = (int)i;
            break;
        }
    }
    if (hit_idx < 0) return false;

    if (cpumem == NULL || cpumem_size == 0) {
        fprintf(stderr, "[save_state] check_search_save: cpumem is NULL — skip\n");
        return false;
    }

    ArmedSave hit = s_armed_saves[hit_idx];  // copy before erase
    fprintf(stderr, "[save_state] hit: frame %u matches armed target — saving to '%s' (remaining=%d)\n",
            search_to_frame, hit.path, (int)s_armed_saves.size() - 1);
    fflush(stderr);

    bool ok = save(hit.path, cpumem, cpumem_size, search_to_frame);

    // Consume this target so a re-search to the same frame does not save again.
    s_armed_saves.erase(s_armed_saves.begin() + hit_idx);

    if (ok && s_quit_when_all_saved
           && s_armed_saves.empty()
           && s_armed_after_accept.empty()) {
        fprintf(stderr, "[save_state] all armed targets consumed — requesting graceful quit\n");
        fflush(stderr);
        set_quitflag();
    }
    return ok;
}

// ─── Triggered save-after-N-accepts ────────────────────────────────────────

void arm_save_after_accept(uint32_t scene_canonical,
                           int accept_count,
                           const char* path,
                           bool quit_after_save,
                           int delay_nmi)
{
    if (path == NULL || path[0] == '\0') return;
    if (accept_count <= 0) {
        fprintf(stderr, "[save_state] arm_save_after_accept: invalid count %d (must be >= 1)\n",
                accept_count);
        return;
    }
    // 2026-05-03: -1 is now a sentinel for "save at next search complete";
    // negative values < -1 are still invalid → clamp to 0.
    if (delay_nmi < -1) delay_nmi = 0;
    ArmedSaveAfterAccept a;
    a.scene_canonical = scene_canonical;
    a.accept_count = accept_count;
    a.delay_nmi = delay_nmi;
    strncpy(a.path, path, sizeof(a.path) - 1);
    a.path[sizeof(a.path) - 1] = '\0';
    s_armed_after_accept.push_back(a);
    if (quit_after_save) s_quit_when_all_saved = true;
    fprintf(stderr, "[save_state] armed: scene %u + %d accepts (delay=%d nmi) -> '%s' (after_accept_pending=%d)\n",
            scene_canonical, accept_count, delay_nmi, path,
            (int)s_armed_after_accept.size());
    fflush(stderr);
}

bool notify_accept(uint8_t* cpumem, uint32_t cpumem_size, uint32_t current_frame)
{
    s_accept_in_scene++;
    if (s_armed_after_accept.empty()) return false;

    // Find a matching armed (scene, count) pair.
    int hit_idx = -1;
    for (size_t i = 0; i < s_armed_after_accept.size(); ++i) {
        if (s_armed_after_accept[i].scene_canonical == s_current_scene &&
            s_armed_after_accept[i].accept_count    == s_accept_in_scene) {
            hit_idx = (int)i;
            break;
        }
    }
    if (hit_idx < 0) return false;

    if (cpumem == NULL || cpumem_size == 0) {
        fprintf(stderr, "[save_state] notify_accept: cpumem is NULL — skip\n");
        return false;
    }

    ArmedSaveAfterAccept hit = s_armed_after_accept[hit_idx];

    if (hit.delay_nmi == -1) {
        // 2026-05-03: "save at next search complete" mode.
        // Set flag pending; next pre_search captures target frame; tick_nmi
        // saves when VLDP arrives at that frame.
        if (s_save_at_search_active) {
            fprintf(stderr, "[save_state] WARN: save-at-search already pending, overwriting (path=%s)\n",
                    s_save_at_search_path);
        }
        s_save_at_search_active = true;
        s_save_at_search_target_frame = 0;  // captured at next pre_search
        s_save_at_search_frame_at_accept = current_frame;
        strncpy(s_save_at_search_path, hit.path, sizeof(s_save_at_search_path) - 1);
        s_save_at_search_path[sizeof(s_save_at_search_path) - 1] = '\0';
        fprintf(stderr, "[save_state] hit: scene %u, accept #%d at frame %u — SAVE-AT-NEXT-SEARCH armed -> '%s'\n",
                s_current_scene, s_accept_in_scene, current_frame, hit.path);
        fflush(stderr);
        s_armed_after_accept.erase(s_armed_after_accept.begin() + hit_idx);
        return true;
    }

    if (hit.delay_nmi == 0) {
        // Immediate save (original behaviour).
        fprintf(stderr, "[save_state] hit: scene %u, accept #%d at frame %u — saving to '%s' (remaining=%d)\n",
                s_current_scene, s_accept_in_scene, current_frame, hit.path,
                (int)s_armed_after_accept.size() - 1);
        fflush(stderr);

        bool ok = save(hit.path, cpumem, cpumem_size, current_frame);
        s_armed_after_accept.erase(s_armed_after_accept.begin() + hit_idx);

        if (ok && s_quit_when_all_saved
               && s_armed_saves.empty()
               && s_armed_after_accept.empty()
               && !s_pending_save_active) {
            fprintf(stderr, "[save_state] all armed targets consumed — requesting graceful quit\n");
            fflush(stderr);
            set_quitflag();
        }
        return ok;
    }

    // Delayed save: arm pending, let tick_nmi count it down.
    if (s_pending_save_active) {
        fprintf(stderr, "[save_state] WARN: delayed save already pending, overwriting (path=%s)\n",
                s_pending_save_path);
    }
    s_pending_save_active = true;
    s_pending_save_remain = hit.delay_nmi;
    strncpy(s_pending_save_path, hit.path, sizeof(s_pending_save_path) - 1);
    s_pending_save_path[sizeof(s_pending_save_path) - 1] = '\0';
    fprintf(stderr, "[save_state] hit: scene %u, accept #%d at frame %u — DELAYED save in %d nmi ticks -> '%s'\n",
            s_current_scene, s_accept_in_scene, current_frame,
            hit.delay_nmi, hit.path);
    fflush(stderr);

    // Remove from armed list NOW; the actual save will happen on tick.
    s_armed_after_accept.erase(s_armed_after_accept.begin() + hit_idx);
    return true;
}

void tick_nmi(uint8_t* cpumem, uint32_t cpumem_size, uint32_t current_frame)
{
    // 2026-05-03: save-at-search-complete mode. Triggered when VLDP reaches
    // the captured target frame from the next pre_search after the accept.
    if (s_save_at_search_active && s_save_at_search_target_frame > 0) {
        if (current_frame >= s_save_at_search_target_frame) {
            if (cpumem == NULL || cpumem_size == 0) {
                fprintf(stderr, "[save_state] tick_nmi: cpumem NULL, skipping save-at-search\n");
                s_save_at_search_active = false;
                s_save_at_search_target_frame = 0;
                return;
            }
            fprintf(stderr, "[save_state] save-at-search firing at frame %u (target was %u) -> '%s'\n",
                    current_frame, s_save_at_search_target_frame, s_save_at_search_path);
            fflush(stderr);
            bool ok = save(s_save_at_search_path, cpumem, cpumem_size, current_frame);
            s_save_at_search_active = false;
            s_save_at_search_target_frame = 0;
            if (ok && s_quit_when_all_saved
                   && s_armed_saves.empty()
                   && s_armed_after_accept.empty()
                   && !s_pending_save_active) {
                fprintf(stderr, "[save_state] all armed targets consumed (after save-at-search) — requesting graceful quit\n");
                fflush(stderr);
                set_quitflag();
            }
            return;
        }
    }

    if (!s_pending_save_active) return;
    if (s_pending_save_remain > 0) {
        s_pending_save_remain--;
        return;
    }
    // Counter reached 0 — perform the save now.
    if (cpumem == NULL || cpumem_size == 0) {
        fprintf(stderr, "[save_state] tick_nmi: cpumem NULL, skipping pending save\n");
        s_pending_save_active = false;
        return;
    }
    fprintf(stderr, "[save_state] delayed save firing at frame %u -> '%s'\n",
            current_frame, s_pending_save_path);
    fflush(stderr);
    bool ok = save(s_pending_save_path, cpumem, cpumem_size, current_frame);
    s_pending_save_active = false;

    if (ok && s_quit_when_all_saved
           && s_armed_saves.empty()
           && s_armed_after_accept.empty()) {
        fprintf(stderr, "[save_state] all armed targets consumed (after delayed save) — requesting graceful quit\n");
        fflush(stderr);
        set_quitflag();
    }
}

// ─── Triggered load ────────────────────────────────────────────────────────

void arm_load(const char* filename,
              int32_t test_frame_offset,
              char test_input,
              uint32_t test_timeout_ms)
{
    if (filename == NULL || filename[0] == '\0') {
        s_load_armed_flag = false;
        s_load_path[0] = '\0';
        s_test_chain_count = 0;
        return;
    }
    strncpy(s_load_path, filename, sizeof(s_load_path) - 1);
    s_load_path[sizeof(s_load_path) - 1] = '\0';
    s_test_frame_offset = test_frame_offset;
    s_test_input = test_input;
    s_test_timeout_ms = test_timeout_ms;
    // Mirror to chain storage (chain of length 1).
    s_test_chain_count = 1;
    s_test_chain_offsets[0] = test_frame_offset;
    s_test_chain_inputs[0]  = test_input;
    s_load_armed_flag = true;
    fprintf(stderr, "[save_state] load armed: file='%s' offset=%+d input='%c' timeout=%u ms\n",
            filename, test_frame_offset,
            test_input ? test_input : '-',
            test_timeout_ms);
    fflush(stderr);
}

void arm_load_chain(const char* filename,
                    const int32_t* offsets,
                    const char* inputs,
                    int n_steps,
                    uint32_t timeout_ms)
{
    if (filename == NULL || filename[0] == '\0' || n_steps <= 0) {
        s_load_armed_flag = false;
        s_load_path[0] = '\0';
        s_test_chain_count = 0;
        return;
    }
    if (n_steps > SAVE_STATE_MAX_CHAIN) {
        fprintf(stderr, "[save_state] arm_load_chain: too many steps %d (max %d)\n",
                n_steps, SAVE_STATE_MAX_CHAIN);
        return;
    }
    strncpy(s_load_path, filename, sizeof(s_load_path) - 1);
    s_load_path[sizeof(s_load_path) - 1] = '\0';
    s_test_timeout_ms = timeout_ms;
    s_test_chain_count = n_steps;
    for (int i = 0; i < n_steps; ++i) {
        s_test_chain_offsets[i] = offsets[i];
        s_test_chain_inputs[i]  = inputs[i];
    }
    // Last step is the "test step" — also expose via single-step getters
    // for backward compatibility (some code paths only read the single one).
    s_test_frame_offset = offsets[n_steps - 1];
    s_test_input        = inputs[n_steps - 1];
    s_load_armed_flag = true;
    fprintf(stderr, "[save_state] load chain armed: file='%s' n_steps=%d timeout=%u ms\n",
            filename, n_steps, timeout_ms);
    for (int i = 0; i < n_steps; ++i) {
        fprintf(stderr, "[save_state]   step %d: offset=%+d input='%c'%s\n",
                i, offsets[i], inputs[i] ? inputs[i] : '-',
                (i == n_steps - 1) ? "  [TEST]" : "  [SETUP]");
    }
    fflush(stderr);
}

bool is_load_armed()
{
    return s_load_armed_flag;
}

bool try_load_armed(uint8_t* cpumem, uint32_t cpumem_size, uint32_t* out_disc_frame)
{
    if (!s_load_armed_flag) return false;
    if (cpumem == NULL || cpumem_size == 0) {
        fprintf(stderr, "[save_state] try_load_armed: cpumem invalid\n");
        return false;
    }
    bool ok = load(s_load_path, cpumem, cpumem_size, out_disc_frame);
    s_load_armed_flag = false;  // disarm after consume
    return ok;
}

int32_t  get_test_frame_offset() { return s_test_frame_offset; }
char     get_test_input()        { return s_test_input; }
uint32_t get_test_timeout_ms()   { return s_test_timeout_ms; }

int     get_test_chain_count() { return s_test_chain_count; }
int32_t get_test_chain_offset(int i) {
    if (i < 0 || i >= s_test_chain_count) return 0;
    return s_test_chain_offsets[i];
}
char    get_test_chain_input(int i) {
    if (i < 0 || i >= s_test_chain_count) return '\0';
    return s_test_chain_inputs[i];
}

} // namespace save_state
