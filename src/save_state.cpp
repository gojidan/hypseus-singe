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

extern struct m80_context g_context;

namespace save_state {

static const char MAGIC[8] = { 'H','Y','P','S','V','0','1','\0' };

// Armed save-on-search state.
static uint32_t s_armed_target_frame = 0;
static char     s_armed_path[512]    = { 0 };
static bool     s_armed_quit_after   = false;
static bool     s_armed              = false;

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
        s_armed = false;
        s_armed_target_frame = 0;
        s_armed_path[0] = '\0';
        s_armed_quit_after = false;
        return;
    }
    s_armed_target_frame = target_frame;
    strncpy(s_armed_path, path, sizeof(s_armed_path) - 1);
    s_armed_path[sizeof(s_armed_path) - 1] = '\0';
    s_armed_quit_after = quit_after_save;
    s_armed = true;
    fprintf(stderr, "[save_state] armed: will save to '%s' on search to frame %u%s\n",
            path, target_frame, quit_after_save ? " (then quit)" : "");
    fflush(stderr);
}

bool check_search_save(uint32_t search_to_frame, uint8_t* cpumem, uint32_t cpumem_size)
{
    if (!s_armed) return false;
    if (search_to_frame != s_armed_target_frame) return false;
    if (cpumem == NULL || cpumem_size == 0) {
        fprintf(stderr, "[save_state] check_search_save: cpumem is NULL — skip\n");
        return false;
    }

    fprintf(stderr, "[save_state] hit: search to %u matches armed target — saving\n",
            search_to_frame);
    fflush(stderr);

    bool ok = save(s_armed_path, cpumem, cpumem_size, search_to_frame);

    // Disarm so we don't save again if the frame is searched twice.
    s_armed = false;

    if (ok && s_armed_quit_after) {
        fprintf(stderr, "[save_state] save complete — requesting graceful quit\n");
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
