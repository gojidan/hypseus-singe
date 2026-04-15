/*
 * rom_logger.cpp
 *
 * Real-time frame data recorder for Daphne/Hypseus arcade games.
 * See rom_logger.h for full documentation.
 */

#include "rom_logger.h"

#include <SDL.h>   // SDL_GetTicks()
#include <stdio.h>
#include <time.h>

namespace rom_logger {

// -------------------------------------------------------------------------
// Internal state
// -------------------------------------------------------------------------

static FILE*   s_file    = nullptr;
static uint32_t s_t0     = 0;      // SDL tick at session open (for relative ms)

// -------------------------------------------------------------------------
// Move name lookup
// These match the SWITCH_* enum order in src/io/input.h
// -------------------------------------------------------------------------

static const char* switch_name(uint8_t move)
{
    switch (move) {
        case 0:  return "UP";
        case 1:  return "LEFT";
        case 2:  return "DOWN";
        case 3:  return "RIGHT";
        case 4:  return "START1";
        case 5:  return "START2";
        case 6:  return "BUTTON1";   // SWORD in Dragon's Lair
        case 7:  return "BUTTON2";
        case 8:  return "BUTTON3";
        case 9:  return "COIN1";
        case 10: return "COIN2";
        case 11: return "SKILL1";    // Space Ace difficulty
        case 12: return "SKILL2";
        case 13: return "SKILL3";
        default: return "UNKNOWN";
    }
}

static inline uint32_t elapsed_ms()
{
    return SDL_GetTicks() - s_t0;
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

void open(const char* game_name, uint8_t switchA, uint8_t switchB)
{
    if (s_file) {
        fclose(s_file);
        s_file = nullptr;
    }

    // Build filename:  lair_rom_log_20260415_143022.ndjson
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename),
             "%s_rom_log_%04d%02d%02d_%02d%02d%02d.ndjson",
             game_name,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    s_file = fopen(filename, "w");
    if (!s_file) return;

    s_t0 = SDL_GetTicks();

    // Session header — DIP switches encoded as raw byte values.
    // switchA bit layout (Dragon's Lair):
    //   bits 1-0: difficulty  (00=Easy, 01=Medium, 10=Hard, 11=Insane)
    //   bits 3-2: lives       (00=3, 01=4, 10=5, 11=5 with extra)
    // Full mapping is game-specific; raw values let post-processing decide.
    fprintf(s_file,
            "{\"e\":\"session\",\"game\":\"%s\","
            "\"swA\":%u,\"swB\":%u,\"ms\":0}\n",
            game_name, (unsigned)switchA, (unsigned)switchB);
    fflush(s_file);
}

void close()
{
    if (!s_file) return;
    fprintf(s_file, "{\"e\":\"end\",\"ms\":%u}\n", elapsed_ms());
    fclose(s_file);
    s_file = nullptr;
}

void log_search(uint32_t from_frame, uint32_t to_frame)
{
    if (!s_file) return;
    fprintf(s_file,
            "{\"e\":\"search\",\"from\":%u,\"to\":%u,\"ms\":%u}\n",
            from_frame, to_frame, elapsed_ms());
    fflush(s_file);
}

void log_play(uint32_t frame)
{
    if (!s_file) return;
    fprintf(s_file,
            "{\"e\":\"play\",\"f\":%u,\"ms\":%u}\n",
            frame, elapsed_ms());
    fflush(s_file);
}

void log_pause(uint32_t frame)
{
    if (!s_file) return;
    fprintf(s_file,
            "{\"e\":\"pause\",\"f\":%u,\"ms\":%u}\n",
            frame, elapsed_ms());
    fflush(s_file);
}

void log_input_enable(uint8_t move, uint32_t frame)
{
    if (!s_file) return;
    fprintf(s_file,
            "{\"e\":\"enable\",\"move\":\"%s\",\"f\":%u,\"ms\":%u}\n",
            switch_name(move), frame, elapsed_ms());
    fflush(s_file);
}

void log_input_disable(uint8_t move, uint32_t frame)
{
    if (!s_file) return;
    fprintf(s_file,
            "{\"e\":\"disable\",\"move\":\"%s\",\"f\":%u,\"ms\":%u}\n",
            switch_name(move), frame, elapsed_ms());
    fflush(s_file);
}

void log_lives(uint8_t lives, uint32_t frame)
{
    if (!s_file) return;
    fprintf(s_file,
            "{\"e\":\"lives\",\"n\":%u,\"f\":%u,\"ms\":%u}\n",
            (unsigned)lives, frame, elapsed_ms());
    fflush(s_file);
}

} // namespace rom_logger
