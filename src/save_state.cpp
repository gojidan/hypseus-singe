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
    char     path[512];
};
static std::vector<ArmedSaveAfterAccept> s_armed_after_accept;

// Per-scene accept tracking: updated in check_search_save (every search
// resets the counter and updates the current scene anchor) and in
// notify_accept (each accept increments).
static uint32_t s_current_scene = 0;
static int      s_accept_in_scene = 0;

// Armed load state.
static char     s_load_path[512]      = { 0 };
static bool     s_load_armed_flag     = false;
static int32_t  s_test_frame_offset   = 0;
static char     s_test_input          = '\0';
static uint32_t s_test_timeout_ms     = 5000;

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
                           bool quit_after_save)
{
    if (path == NULL || path[0] == '\0') return;
    if (accept_count <= 0) {
        fprintf(stderr, "[save_state] arm_save_after_accept: invalid count %d (must be >= 1)\n",
                accept_count);
        return;
    }
    ArmedSaveAfterAccept a;
    a.scene_canonical = scene_canonical;
    a.accept_count = accept_count;
    strncpy(a.path, path, sizeof(a.path) - 1);
    a.path[sizeof(a.path) - 1] = '\0';
    s_armed_after_accept.push_back(a);
    if (quit_after_save) s_quit_when_all_saved = true;
    fprintf(stderr, "[save_state] armed: scene %u + %d accepts -> '%s' (after_accept_pending=%d)\n",
            scene_canonical, accept_count, path, (int)s_armed_after_accept.size());
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
    fprintf(stderr, "[save_state] hit: scene %u, accept #%d at frame %u — saving to '%s' (remaining=%d)\n",
            s_current_scene, s_accept_in_scene, current_frame, hit.path,
            (int)s_armed_after_accept.size() - 1);
    fflush(stderr);

    bool ok = save(hit.path, cpumem, cpumem_size, current_frame);
    s_armed_after_accept.erase(s_armed_after_accept.begin() + hit_idx);

    if (ok && s_quit_when_all_saved
           && s_armed_saves.empty()
           && s_armed_after_accept.empty()) {
        fprintf(stderr, "[save_state] all armed targets consumed — requesting graceful quit\n");
        fflush(stderr);
        set_quitflag();
    }
    return ok;
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
        return;
    }
    strncpy(s_load_path, filename, sizeof(s_load_path) - 1);
    s_load_path[sizeof(s_load_path) - 1] = '\0';
    s_test_frame_offset = test_frame_offset;
    s_test_input = test_input;
    s_test_timeout_ms = test_timeout_ms;
    s_load_armed_flag = true;
    fprintf(stderr, "[save_state] load armed: file='%s' offset=%+d input='%c' timeout=%u ms\n",
            filename, test_frame_offset,
            test_input ? test_input : '-',
            test_timeout_ms);
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

} // namespace save_state
