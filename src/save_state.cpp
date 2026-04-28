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
#include "cpu/m80_internal.h"  // for struct m80_context and extern g_context

#include <stdio.h>
#include <string.h>

extern struct m80_context g_context;

namespace save_state {

static const char MAGIC[8] = { 'H','Y','P','S','V','0','1','\0' };

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

} // namespace save_state
