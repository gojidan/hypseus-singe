#include "explorer.h"
#include "../io/input.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// NMI fires every ~25-33 ms depending on emulation speed.
// We use ~40 NMIs/s based on observed timing (measured: 150 NMIs in ~3.7 s).
// Timing constants are deliberately generous to be safe on any speed.
#define NMI_HZ        40
#define COIN_HOLD     (NMI_HZ / 4)    // hold coin for ~0.25 s
#define COIN_GAP      (2 * NMI_HZ)    // 2 s between coin and next action
#define START_HOLD    (NMI_HZ / 4)    // hold start for ~0.25 s
#define BOOT_WAIT     (10 * NMI_HZ)   // 10 s: disk seek + logo sequence
#define GAMEOVER_WAIT (10 * NMI_HZ)   // 10 s: wait for attract before restart

// Stuck detection: if the same `to` frame appears this many times in the
// last STUCK_WINDOW searches while PLAYING, we are in an infinite loop
// (e.g. elevator scene with wrong move + infinite lives).
#define STUCK_WINDOW  6
#define STUCK_COUNT   3

namespace explorer {

enum State {
    ATTRACT,        // waiting to insert first coin
    COIN1_PRESS,    // first coin held
    COIN1_WAIT,     // first coin released, short gap
    COIN2_PRESS,    // second coin held
    COIN2_WAIT,     // second coin released, waiting before START
    START_PRESS,    // START1 held
    START_WAIT,     // START1 released, waiting for game to load
    PLAYING,        // move held — waiting for game over or stuck detection
    GAMEOVER        // restarting: waiting before next coin insertion
};

static bool     s_active     = false;
static uint8_t  s_switch     = 255;   // SWITCH_* to hold during PLAYING, 255=none
static char     s_move_char  = 'N';
static State    s_state      = ATTRACT;
static uint32_t s_nmi        = 0;
static uint32_t s_state_nmi  = 0;
static uint8_t  s_prev_lives = 255;
static bool     s_move_held  = false;

// Circular buffer of recent search destinations for stuck detection
static uint32_t s_recent_to[STUCK_WINDOW];
static int      s_recent_idx = 0;

static const Action NO_ACTION = { 255, 255 };

static void enter_state(State st)
{
    s_state     = st;
    s_state_nmi = s_nmi;
    fprintf(stderr, "[explorer] -> state %d  nmi=%u\n", (int)st, s_nmi);
    fflush(stderr);
}

bool init(char move_char)
{
    switch (move_char) {
        case 'U': s_switch = SWITCH_UP;      break;
        case 'L': s_switch = SWITCH_LEFT;    break;
        case 'D': s_switch = SWITCH_DOWN;    break;
        case 'R': s_switch = SWITCH_RIGHT;   break;
        case 'B': s_switch = SWITCH_BUTTON1; break;
        case 'N': s_switch = 255;            break;
        default:
            fprintf(stderr, "[explorer] unknown move '%c'\n", move_char);
            return false;
    }
    s_move_char = move_char;
    s_active    = true;
    memset(s_recent_to, 0, sizeof(s_recent_to));
    fprintf(stderr, "[explorer] armed: move=%c switch=%u\n",
            move_char, (unsigned)s_switch);
    fflush(stderr);
    return true;
}

bool is_active() { return s_active; }

void on_lives(uint8_t n)
{
    if (!s_active) return;
    if (n == 15) return;  // 0xFF & 0x0F = 15 during ROM boot init

    if (s_state == PLAYING && n == 0 && s_prev_lives > 0 && s_prev_lives != 255) {
        fprintf(stderr, "[explorer] GAME OVER (lives 0)\n");
        fflush(stderr);
        s_move_held = false;
        enter_state(GAMEOVER);
    }

    s_prev_lives = n;
}

void on_search(uint32_t from, uint32_t to)
{
    (void)from;
    if (!s_active || s_state != PLAYING) return;

    // Record destination in circular buffer
    s_recent_to[s_recent_idx] = to;
    s_recent_idx = (s_recent_idx + 1) % STUCK_WINDOW;

    // Count how many of the last STUCK_WINDOW searches went to the same frame
    int count = 0;
    for (int i = 0; i < STUCK_WINDOW; i++) {
        if (s_recent_to[i] == to) count++;
    }

    if (count >= STUCK_COUNT) {
        fprintf(stderr, "[explorer] STUCK at frame %u (%d/%d) — forcing restart\n",
                to, count, STUCK_WINDOW);
        fflush(stderr);
        s_move_held = false;
        memset(s_recent_to, 0, sizeof(s_recent_to));
        enter_state(GAMEOVER);
    }
}

Action tick()
{
    if (!s_active) return NO_ACTION;
    s_nmi++;

    uint32_t elapsed = s_nmi - s_state_nmi;
    Action   action  = NO_ACTION;

    switch (s_state) {

    case ATTRACT:
        if (elapsed == 8 * NMI_HZ) {
            fprintf(stderr, "[explorer] inserting coin 1\n");
            fflush(stderr);
            action.press = SWITCH_COIN1;
            enter_state(COIN1_PRESS);
        }
        break;

    case COIN1_PRESS:
        if (elapsed >= COIN_HOLD) {
            action.release = SWITCH_COIN1;
            enter_state(COIN1_WAIT);
        }
        break;

    case COIN1_WAIT:
        if (elapsed >= COIN_GAP) {
            fprintf(stderr, "[explorer] inserting coin 2\n");
            fflush(stderr);
            action.press = SWITCH_COIN1;
            enter_state(COIN2_PRESS);
        }
        break;

    case COIN2_PRESS:
        if (elapsed >= COIN_HOLD) {
            action.release = SWITCH_COIN1;
            enter_state(COIN2_WAIT);
        }
        break;

    case COIN2_WAIT:
        if (elapsed >= COIN_GAP) {
            fprintf(stderr, "[explorer] pressing START1\n");
            fflush(stderr);
            action.press = SWITCH_START1;
            enter_state(START_PRESS);
        }
        break;

    case START_PRESS:
        if (elapsed >= START_HOLD) {
            action.release = SWITCH_START1;
            enter_state(START_WAIT);
        }
        break;

    case START_WAIT:
        if (elapsed >= BOOT_WAIT) {
            if (s_switch != 255) {
                fprintf(stderr, "[explorer] holding move '%c' (switch %u)\n",
                        s_move_char, (unsigned)s_switch);
                fflush(stderr);
                action.press = s_switch;
                s_move_held  = true;
            }
            s_prev_lives = 255;
            memset(s_recent_to, 0, sizeof(s_recent_to));
            enter_state(PLAYING);
        }
        break;

    case PLAYING:
        break;

    case GAMEOVER:
        // Release move on first tick
        if (elapsed == 1 && s_move_held && s_switch != 255) {
            action.release = s_switch;
            s_move_held    = false;
        }
        if (elapsed >= GAMEOVER_WAIT) {
            s_prev_lives = 255;
            enter_state(ATTRACT);
        }
        break;
    }

    return action;
}

} // namespace explorer
