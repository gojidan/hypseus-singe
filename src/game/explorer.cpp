#include "explorer.h"
#include "../io/input.h"
#include <stdint.h>
#include <stdio.h>

// NMI fires every ~32.768 ms (LAIR_IRQ_PERIOD), so ~30 NMIs per second.
// All timing constants below are expressed in NMI ticks.
#define NMI_HZ        30
#define COIN_PRESS    (5  * NMI_HZ / 10)  // hold coin for ~0.5 s then release
#define COIN_WAIT     (3  * NMI_HZ)        // 3 s after coin before START
#define START_PRESS   (5  * NMI_HZ / 10)  // hold start for ~0.5 s then release
#define BOOT_WAIT     (10 * NMI_HZ)        // 10 s for disk seek + logo sequence
#define GAMEOVER_WAIT (10 * NMI_HZ)        // 10 s for game-over screen to clear

namespace explorer {

enum State { ATTRACT, COIN_PRESSED, COIN_WAIT_ST, START_PRESSED, START_WAIT_ST, PLAYING, GAMEOVER };

static bool     s_active       = false;
static uint8_t  s_switch       = 255;   // SWITCH_* constant, 255 = none
static char     s_move_char    = 'N';
static State    s_state        = ATTRACT;
static uint32_t s_nmi          = 0;
static uint32_t s_state_nmi    = 0;
static uint8_t  s_prev_lives   = 255;
static bool     s_move_held    = false;

static void enter_state(State st)
{
    s_state     = st;
    s_state_nmi = s_nmi;
    fprintf(stderr, "[explorer] -> state %d  nmi=%u\n", (int)st, s_nmi);
    fflush(stderr);
}

static void press(uint8_t sw)   { input_enable (sw, -1); }
static void release_(uint8_t sw) { input_disable(sw, -1); }

static void release_move()
{
    if (s_move_held && s_switch != 255) {
        release_(s_switch);
        s_move_held = false;
    }
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
    if (n == 15) return;  // 0xFF & 0x0F = 15 during ROM boot init, ignore

    if (s_state == PLAYING && n == 0 && s_prev_lives > 0 && s_prev_lives != 255) {
        fprintf(stderr, "[explorer] GAME OVER detected (lives 0)\n");
        fflush(stderr);
        release_move();
        enter_state(GAMEOVER);
    }

    s_prev_lives = n;
}

void on_search(uint32_t from, uint32_t to)
{
    // Reserved for future can_pass scene routing.
    (void)from; (void)to;
}

void tick()
{
    if (!s_active) return;
    s_nmi++;

    uint32_t elapsed = s_nmi - s_state_nmi;

    switch (s_state) {

    case ATTRACT:
        // Wait 5 seconds for Hypseus to show attract screen, then insert coin.
        if (elapsed == 5 * NMI_HZ) {
            fprintf(stderr, "[explorer] inserting coin\n");
            fflush(stderr);
            press(SWITCH_COIN1);
            enter_state(COIN_PRESSED);
        }
        break;

    case COIN_PRESSED:
        if (elapsed >= COIN_PRESS) {
            release_(SWITCH_COIN1);
            enter_state(COIN_WAIT_ST);
        }
        break;

    case COIN_WAIT_ST:
        if (elapsed >= COIN_WAIT) {
            fprintf(stderr, "[explorer] pressing START1\n");
            fflush(stderr);
            press(SWITCH_START1);
            enter_state(START_PRESSED);
        }
        break;

    case START_PRESSED:
        if (elapsed >= START_PRESS) {
            release_(SWITCH_START1);
            enter_state(START_WAIT_ST);
        }
        break;

    case START_WAIT_ST:
        // Wait for disk seek + Cinematronics logo before holding move.
        if (elapsed >= BOOT_WAIT) {
            if (s_switch != 255) {
                fprintf(stderr, "[explorer] holding move '%c' (switch %u)\n",
                        s_move_char, (unsigned)s_switch);
                fflush(stderr);
                press(s_switch);
                s_move_held = true;
            }
            s_prev_lives = 255;  // reset so first lives write is clean
            enter_state(PLAYING);
        }
        break;

    case PLAYING:
        // Move is held. on_lives(0) will trigger the transition to GAMEOVER.
        break;

    case GAMEOVER:
        if (elapsed >= GAMEOVER_WAIT) {
            s_prev_lives = 255;
            enter_state(ATTRACT);
        }
        break;
    }
}

} // namespace explorer
