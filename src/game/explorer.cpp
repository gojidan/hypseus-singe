#include "explorer.h"
#include "../io/input.h"
#include <stdint.h>
#include <stdio.h>

// NMI fires every ~32.768 ms (LAIR_IRQ_PERIOD), so ~30 NMIs per second.
// All timing constants below are expressed in NMI ticks.
#define NMI_HZ        30
#define COIN_HOLD     (10 * NMI_HZ / 10)  // hold coin input for ~0.33 s
#define COIN_GAP      (4  * NMI_HZ)        // 4 s after coin before START
#define START_HOLD    (10 * NMI_HZ / 10)  // hold start input for ~0.33 s
#define BOOT_WAIT     (12 * NMI_HZ)        // 12 s for disk seek + logo sequence
#define GAMEOVER_WAIT (12 * NMI_HZ)        // 12 s for game-over screen to clear

namespace explorer {

enum State {
    ATTRACT,        // waiting to insert coin
    COIN_PRESSED,   // coin input held
    COIN_WAIT_ST,   // coin released, waiting before START
    START_PRESSED,  // START1 held
    START_WAIT_ST,  // START1 released, waiting for game to load
    PLAYING,        // move held, waiting for game over
    GAMEOVER        // game over, waiting to restart
};

static bool     s_active     = false;
static uint8_t  s_switch     = 255;   // SWITCH_* to hold during PLAYING, 255=none
static char     s_move_char  = 'N';
static State    s_state      = ATTRACT;
static uint32_t s_nmi        = 0;
static uint32_t s_state_nmi  = 0;
static uint8_t  s_prev_lives = 255;
static bool     s_move_held  = false;

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
        // move release will be emitted by tick() on next GAMEOVER tick
        s_move_held = false;
        enter_state(GAMEOVER);
    }

    s_prev_lives = n;
}

void on_search(uint32_t from, uint32_t to)
{
    (void)from; (void)to;  // reserved for future can_pass routing
}

Action tick()
{
    if (!s_active) return NO_ACTION;
    s_nmi++;

    uint32_t elapsed = s_nmi - s_state_nmi;
    Action   action  = NO_ACTION;

    switch (s_state) {

    case ATTRACT:
        // Wait 8 seconds for ROM to reach attract-mode coin-accept state
        if (elapsed == 8 * NMI_HZ) {
            fprintf(stderr, "[explorer] inserting coin\n");
            fflush(stderr);
            action.press = SWITCH_COIN1;
            enter_state(COIN_PRESSED);
        }
        break;

    case COIN_PRESSED:
        if (elapsed >= COIN_HOLD) {
            action.release = SWITCH_COIN1;
            enter_state(COIN_WAIT_ST);
        }
        break;

    case COIN_WAIT_ST:
        if (elapsed >= COIN_GAP) {
            fprintf(stderr, "[explorer] pressing START1\n");
            fflush(stderr);
            action.press = SWITCH_START1;
            enter_state(START_PRESSED);
        }
        break;

    case START_PRESSED:
        if (elapsed >= START_HOLD) {
            action.release = SWITCH_START1;
            enter_state(START_WAIT_ST);
        }
        break;

    case START_WAIT_ST:
        if (elapsed >= BOOT_WAIT) {
            if (s_switch != 255) {
                fprintf(stderr, "[explorer] holding move '%c' (switch %u)\n",
                        s_move_char, (unsigned)s_switch);
                fflush(stderr);
                action.press = s_switch;
                s_move_held  = true;
            }
            s_prev_lives = 255;
            enter_state(PLAYING);
        }
        break;

    case PLAYING:
        // Move is held. on_lives(0) triggers transition to GAMEOVER.
        break;

    case GAMEOVER:
        // Release move on first GAMEOVER tick if still held
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
