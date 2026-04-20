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
// last STUCK_WINDOW searches while PLAYING, we are in an infinite loop.
#define STUCK_WINDOW  6
#define STUCK_COUNT   3

// Pulse timing for simple mode: ROM is edge-triggered — it registers the
// falling edge (press), not the level.  We press for PULSE_PRESS NMIs then
// release for the remainder of PULSE_PERIOD.
#define PULSE_PRESS   5
#define PULSE_PERIOD  20

// Switch bitmask helpers
#define MASK_U  (1u << SWITCH_UP)
#define MASK_L  (1u << SWITCH_LEFT)
#define MASK_D  (1u << SWITCH_DOWN)
#define MASK_R  (1u << SWITCH_RIGHT)
#define MASK_B  (1u << SWITCH_BUTTON1)

// ─── Guided mode scene table ─────────────────────────────────────────────────
// Data sourced from real ROM log (lair_rom_log_20260419_192104.ndjson).
// frame_offset = disc frames after the scene-start seek where the ROM opens
// the acceptance window.  switch_mask = button(s) to press.

struct SlotInfo {
    uint32_t mask;         // bitmask of SWITCH_* to press
    int32_t  frame_offset; // disc frames after scene_start to start pressing
};

struct SceneInfo {
    uint32_t frame_start;
    int      slot_count;
    SlotInfo slots[20];
};

static const SceneInfo SCENE_TABLE[] = {
    // Vestibule (small crumbling room)
    { 1887,  2, {{MASK_R,   63}, {MASK_R,  100}} },

    // Tentacles & Halberd
    { 2353,  6, {{MASK_B,   71}, {MASK_U,  136}, {MASK_R, 181},
                 {MASK_D,  225}, {MASK_L,  291}, {MASK_U, 342}} },

    // Snake Room (snake_room — tentacle_room alternate, row 7 col 2)
    { 3097,  4, {{MASK_B,   53}, {MASK_B,   98}, {MASK_B, 137}, {MASK_U, 159}} },

    // Fire Pit (ropes)
    { 3561,  4, {{MASK_R,  108}, {MASK_R,  152}, {MASK_R, 197}, {MASK_R, 242}} },

    // Yellow Brick Road
    { 4139,  9, {{MASK_L,   36}, {MASK_U,   86}, {MASK_R, 142}, {MASK_U, 183},
                 {MASK_L,  236}, {MASK_U,  283}, {MASK_B, 363}, {MASK_R, 400},
                 {MASK_U,  441}} },

    // Bubbling Cauldron / Acid Creature (bubbling_cauldron — tentacle_room row 7 col 3)
    // Note: first two slots are a buzz B followed immediately by the correct U.
    { 5123,  6, {{MASK_B,   58}, {MASK_U,   64}, {MASK_B, 116}, {MASK_D, 154},
                 {MASK_B,  183}, {MASK_R,  250}} },

    // Giddy Goons (giddy_goons — flattening_staircase alternate, row 8 col 2)
    // Note: logged sequence B,R,R,B,U — differs from levels.json U,U+L,B,R,R.
    //       The log is authoritative for the guided explorer.
    { 5683,  5, {{MASK_B,   58}, {MASK_R,   97}, {MASK_R, 107},
                 {MASK_B,  155}, {MASK_U,  197}} },

    // YMCA Room (flattening stairs)
    { 6375,  4, {{MASK_L,   49}, {MASK_B,  110}, {MASK_L, 143}, {MASK_L, 210}} },

    // Forge (Smithee)
    { 6994,  5, {{MASK_B,   65}, {MASK_B,  129}, {MASK_L, 175},
                 {MASK_B,  242}, {MASK_B,  386}} },

    // Socker Boppers (Grim Reaper)
    { 8004,  4, {{MASK_U,   73}, {MASK_B,  221}, {MASK_D, 289}, {MASK_U, 349}} },

    // Breathing Door (wind room)
    { 8709,  1, {{MASK_R,  180}} },

    // Bower (bedroom / closing wall)
    { 9181,  1, {{MASK_U,   14}} },

    // Fire Room (lightning / bench over exit)
    { 9529,  4, {{MASK_R,   78}, {MASK_L,  117}, {MASK_D, 165}, {MASK_L, 213}} },

    // Flying Barding
    { 10021, 6, {{MASK_R,   96}, {MASK_L,  132}, {MASK_R, 176}, {MASK_L, 223},
                 {MASK_L,  271}, {MASK_L,  306}} },

    // Chapel (Robot Knight / checkered floor)
    { 10741, 8, {{MASK_R,   89}, {MASK_L,  123}, {MASK_U, 163}, {MASK_L, 213},
                 {MASK_R,  259}, {MASK_L,  288}, {MASK_R, 314}, {MASK_B, 371}} },

    // Mausoleum (Crypt Creeps)
    { 11489, 6, {{MASK_U,   61}, {MASK_B,  103}, {MASK_U, 139},
                 {MASK_B,  181}, {MASK_L,  226}, {MASK_B, 262}} },

    // Catwalk Bats (catwalk_bats — yellow_brick_road row 9 col 3)
    { 12190, 5, {{MASK_U,   47}, {MASK_U,   87}, {MASK_B, 140},
                 {MASK_R,  177}, {MASK_R,  211}} },

    // Fire Pit (reversed)
    { 12725, 4, {{MASK_L,  110}, {MASK_L,  156}, {MASK_L, 203}, {MASK_L, 248}} },

    // Yellow Brick Road (reversed)
    { 13303, 9, {{MASK_R,   41}, {MASK_U,   89}, {MASK_L, 141}, {MASK_U, 185},
                 {MASK_R,  234}, {MASK_U,  284}, {MASK_B, 368}, {MASK_L, 406},
                 {MASK_U,  449}} },

    // Giant Bat
    { 14327, 4, {{MASK_B,   17}, {MASK_L,   46}, {MASK_U, 104}, {MASK_B, 163}} },

    // Elevator Floor (3-level and 9-level share this frame start)
    { 14847, 1, {{MASK_L,  156}} },

    // Forge Reversed (smithee_reversed — smithee row 6 col 2)
    { 15653, 5, {{MASK_B,   62}, {MASK_B,  131}, {MASK_R, 172},
                 {MASK_B,  242}, {MASK_B,  376}} },

    // Flying Barding (reversed)
    { 16544, 6, {{MASK_L,   95}, {MASK_R,  132}, {MASK_L, 173}, {MASK_R, 222},
                 {MASK_R,  268}, {MASK_R,  303}} },

    // Pot of Gold (Lizard King)
    { 17264,12, {{MASK_L,   43}, {MASK_R,  145}, {MASK_R, 234}, {MASK_R, 282},
                 {MASK_R,  398}, {MASK_R,  448}, {MASK_U, 488}, {MASK_B, 530},
                 {MASK_B,  540}, {MASK_B,  572}, {MASK_B, 596}, {MASK_B, 641}} },

    // Wizard's Kitchen (Drink Me)
    { 18282, 1, {{MASK_R,   32}} },

    // Mausoleum (reversed) — 8 ROM windows (3 initial UP buzz slots then correct sequence)
    { 18662, 8, {{MASK_U,   56}, {MASK_U,   68}, {MASK_U,  72}, {MASK_B, 101},
                 {MASK_U,  137}, {MASK_B,  179}, {MASK_R, 218}, {MASK_B, 261}} },

    // Socker Boppers (reversed)
    // Disc seeks internally from ~19614 to 19628 after slot 0; NMI elapsed computed
    // from ms timestamps (not disc frame delta) to account for seek latency.
    { 19520, 4, {{MASK_U,   90}, {MASK_B,  174}, {MASK_D, 242}, {MASK_U, 298}} },

    // Note: smithee_reversed (Forge reversed, frame 15653) is already in the table above.
    // 19628 is a Socker Boppers rev sub-seek target — no entry here prevents false scene-switch.

    // Tilting Room (tilting_room — row 11 col 2) — PROVISIONAL: DirkSimple-derived,
    // not verified by ROM log. Our reference run played throne_room for this row,
    // so explorerG will never seek here. Offsets: D@90 (enter_room 3768ms→90fr),
    // U@119 (jumps_back 885-1540ms midpoint), L@139 (jumps_forward 492-1049ms midpoint).
    { 20187, 3, {{MASK_D,   90}, {MASK_U,  119}, {MASK_L,  139}} },

    // Throne Room (throne_room — row 11 col 1)
    // Levels.json move "U,R" is split into two ROM windows: U@78 then R@87.
    { 20674, 4, {{MASK_R,   52}, {MASK_U,   78}, {MASK_R,  87}, {MASK_R, 199}} },

    // Chapel Reversed (robot_knight_reversed — row 10 col 2)
    // 10 slots across noseek sequences; our log captured all from a cycle-2 visit.
    { 21212,10, {{MASK_L,   92}, {MASK_R,  129}, {MASK_U, 167}, {MASK_R, 211},
                 {MASK_L,  253}, {MASK_L,  263}, {MASK_R, 290}, {MASK_L, 315},
                 {MASK_B,  365}, {MASK_B,  378}} },

    // Elevator Floor (9-level, reversed)
    { 21959, 1, {{MASK_R,  155}} },

    // Pirates of the Caribbean
    { 22936,13, {{MASK_L,   16}, {MASK_R,   60}, {MASK_L, 109}, {MASK_R, 155},
                 {MASK_U,  284}, {MASK_U,  344}, {MASK_U, 405}, {MASK_U, 471},
                 {MASK_R,  596}, {MASK_L,  671}, {MASK_R, 735}, {MASK_L, 797},
                 {MASK_R,  913}} },

    // Mudmen
    { 24378, 9, {{MASK_B,  128}, {MASK_U,  217}, {MASK_U, 245}, {MASK_U, 302},
                 {MASK_U,  335}, {MASK_U,  382}, {MASK_U, 442}, {MASK_U, 500},
                 {MASK_U,  620}} },

    // Knight & Light (Black Knight / horse)
    // 3-slot data from cycle 3 visit; cycles 1-2 only have 2 slots but the
    // extra press on an already-ended scene is harmless.
    { 25536, 3, {{MASK_L,   77}, {MASK_L,  176}, {MASK_R, 247}} },

    // Boulder Trench (colored balls)
    { 26098, 7, {{MASK_D,  128}, {MASK_D,  174}, {MASK_D, 223}, {MASK_D, 271},
                 {MASK_D,  322}, {MASK_D,  366}, {MASK_U, 411}} },

    // Three Caves (jagged door / geyser)
    { 26778, 3, {{MASK_U,   58}, {MASK_U,  102}, {MASK_L, 234}} },

    // Dragon's Lair (final)
    { 28938,17, {{MASK_L,  108}, {MASK_L,  381}, {MASK_L, 574}, {MASK_L, 582},
                 {MASK_D, 1124}, {MASK_R, 1219}, {MASK_U,1230}, {MASK_D,1301},
                 {MASK_R, 1421}, {MASK_R, 1429}, {MASK_R,1437}, {MASK_U,1520},
                 {MASK_B, 1556}, {MASK_B, 1644}, {MASK_L,1733}, {MASK_B,1772},
                 {MASK_B, 1783}} },
};

static const int SCENE_TABLE_COUNT = (int)(sizeof(SCENE_TABLE) / sizeof(SCENE_TABLE[0]));

// ─── State ───────────────────────────────────────────────────────────────────

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

static bool     s_active      = false;
static bool     s_guided      = false;
static int32_t  s_delta_frames= 0;    // guided mode: signed frame offset per slot

// Simple mode
static uint32_t s_simple_mask = 0;    // bitmask for simple pulse mode (MASK_*)
static char     s_move_char   = 'N';
static uint32_t s_delay_nmi   = 0;    // NMIs to wait after PLAYING before pulsing
static bool     s_move_held   = false;

// Common
static State    s_state       = ATTRACT;
static uint32_t s_nmi         = 0;
static uint32_t s_state_nmi   = 0;
static uint8_t  s_prev_lives  = 255;

// Guided mode scene tracking
static const SceneInfo* s_scene       = nullptr; // current scene entry, null = unknown
static uint32_t         s_scene_start_frame = 0; // disc frame when scene seek fired
static int              s_slot        = 0;        // next slot index to press
static uint32_t         s_held_mask   = 0;        // bitmask currently held in guided mode
static uint32_t         s_hold_end_nmi = 0;       // NMI at which to release held buttons

// Stuck detection (simple mode)
static uint32_t s_recent_to[STUCK_WINDOW];
static int      s_recent_idx = 0;

static const Action NO_ACTION = { 0, 0 };

// ─── Helpers ─────────────────────────────────────────────────────────────────

static void enter_state(State st)
{
    s_state     = st;
    s_state_nmi = s_nmi;
    fprintf(stderr, "[explorer] -> state %d  nmi=%u\n", (int)st, s_nmi);
    fflush(stderr);
}

// Look up a frame_start in the scene table; returns null if not found.
static const SceneInfo* find_scene(uint32_t frame)
{
    for (int i = 0; i < SCENE_TABLE_COUNT; i++) {
        if (SCENE_TABLE[i].frame_start == frame)
            return &SCENE_TABLE[i];
    }
    return nullptr;
}

// Compute the target disc frame for a slot.
// scene_start + offset + delta, clamped to ≥ scene_start.
static uint32_t slot_target_frame(uint32_t scene_start, int32_t offset, int32_t delta)
{
    int64_t t = (int64_t)scene_start + offset + delta;
    if (t < (int64_t)scene_start) t = scene_start;
    return (uint32_t)t;
}

// ─── Public API ──────────────────────────────────────────────────────────────

bool init(char move_char, uint32_t delay_sec)
{
    s_guided = false;
    switch (move_char) {
        case 'U': s_simple_mask = MASK_U; break;
        case 'L': s_simple_mask = MASK_L; break;
        case 'D': s_simple_mask = MASK_D; break;
        case 'R': s_simple_mask = MASK_R; break;
        case 'B': s_simple_mask = MASK_B; break;
        case 'N': s_simple_mask = 0;      break;
        default:
            fprintf(stderr, "[explorer] unknown move '%c'\n", move_char);
            return false;
    }
    s_move_char  = move_char;
    s_delay_nmi  = delay_sec * NMI_HZ;
    s_active     = true;
    s_move_held  = false;
    memset(s_recent_to, 0, sizeof(s_recent_to));
    fprintf(stderr, "[explorer] simple mode: move=%c mask=0x%x delay=%us (%u NMIs)\n",
            move_char, s_simple_mask, delay_sec, s_delay_nmi);
    fflush(stderr);
    return true;
}

bool init_guided(int32_t delta_frames)
{
    s_guided            = true;
    s_delta_frames      = delta_frames;
    s_active            = true;
    s_move_held         = false;
    s_scene             = nullptr;
    s_scene_start_frame = 0;
    s_slot              = 0;
    s_held_mask         = 0;
    memset(s_recent_to, 0, sizeof(s_recent_to));
    fprintf(stderr, "[explorer] guided mode: delta=%+d frames  scenes=%d\n",
            delta_frames, SCENE_TABLE_COUNT);
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
        s_held_mask = 0;
        s_scene     = nullptr;
        enter_state(GAMEOVER);
    }

    s_prev_lives = n;
}

void on_search(uint32_t from, uint32_t to)
{
    (void)from;
    if (!s_active) return;

    // Stuck detection (simple mode)
    if (!s_guided && s_state == PLAYING) {
        s_recent_to[s_recent_idx] = to;
        s_recent_idx = (s_recent_idx + 1) % STUCK_WINDOW;

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
            return;
        }
    }

    // Guided mode: identify scene from table
    if (s_guided && s_state == PLAYING) {
        const SceneInfo* scene = find_scene(to);
        if (scene) {
            s_scene             = scene;
            s_scene_start_frame = to;
            s_slot              = 0;
            s_held_mask         = 0;
            fprintf(stderr, "[explorer] guided: scene frame=%u slots=%d  delta=%+d\n",
                    to, scene->slot_count, s_delta_frames);
            fflush(stderr);
        } else if (s_scene && !s_held_mask) {
            // Sub-seek within the current scene: disc jumped to `to`.
            // Skip slots whose target frame the disc has already passed.
            // Guard: if s_held_mask is set we just pressed this slot — the seek
            // is the ROM's reaction to our correct press, not a skip of an unpressed slot.
            while (s_slot < s_scene->slot_count) {
                uint32_t target = slot_target_frame(s_scene_start_frame,
                                                     s_scene->slots[s_slot].frame_offset,
                                                     s_delta_frames);
                if (to > target) {
                    fprintf(stderr, "[explorer] guided: sub-seek %u->%u skips slot %d (target frame %u)\n",
                            from, to, s_slot, target);
                    fflush(stderr);
                    s_slot++;
                } else break;
            }
            if (s_slot >= s_scene->slot_count) {
                fprintf(stderr, "[explorer] guided: all slots done after sub-seek, waiting for next scene\n");
                fflush(stderr);
                s_scene = nullptr;
            }
        }
    }
}

Action tick(uint32_t current_disc_frame)
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
            action.press_mask = (1u << SWITCH_COIN1);
            enter_state(COIN1_PRESS);
        }
        break;

    case COIN1_PRESS:
        if (elapsed >= COIN_HOLD) {
            action.release_mask = (1u << SWITCH_COIN1);
            enter_state(COIN1_WAIT);
        }
        break;

    case COIN1_WAIT:
        if (elapsed >= COIN_GAP) {
            fprintf(stderr, "[explorer] inserting coin 2\n");
            fflush(stderr);
            action.press_mask = (1u << SWITCH_COIN1);
            enter_state(COIN2_PRESS);
        }
        break;

    case COIN2_PRESS:
        if (elapsed >= COIN_HOLD) {
            action.release_mask = (1u << SWITCH_COIN1);
            enter_state(COIN2_WAIT);
        }
        break;

    case COIN2_WAIT:
        if (elapsed >= COIN_GAP) {
            fprintf(stderr, "[explorer] pressing START1\n");
            fflush(stderr);
            action.press_mask = (1u << SWITCH_START1);
            enter_state(START_PRESS);
        }
        break;

    case START_PRESS:
        if (elapsed >= START_HOLD) {
            action.release_mask = (1u << SWITCH_START1);
            enter_state(START_WAIT);
        }
        break;

    case START_WAIT:
        if (elapsed >= BOOT_WAIT) {
            if (s_guided) {
                fprintf(stderr, "[explorer] PLAYING (guided, delta=%+d frames)\n",
                        s_delta_frames);
            } else {
                fprintf(stderr, "[explorer] PLAYING: move '%c' (mask 0x%x) delay=%u NMIs"
                        " then pulse every %d NMIs\n",
                        s_move_char, s_simple_mask, s_delay_nmi, PULSE_PERIOD);
            }
            fflush(stderr);
            s_prev_lives        = 255;
            s_scene             = nullptr;
            s_scene_start_frame = 0;
            s_slot              = 0;
            s_held_mask         = 0;
            memset(s_recent_to, 0, sizeof(s_recent_to));
            enter_state(PLAYING);
        }
        break;

    case PLAYING: {
        // Backstop: if stuck in PLAYING for >3 minutes, force restart.
        if (elapsed >= 3 * 60 * NMI_HZ) {
            fprintf(stderr, "[explorer] PLAYING TIMEOUT (%u NMIs) — forcing restart\n", elapsed);
            fflush(stderr);
            s_move_held = false;
            s_held_mask = 0;
            s_scene     = nullptr;
            memset(s_recent_to, 0, sizeof(s_recent_to));
            enter_state(GAMEOVER);
            break;
        }

        if (s_guided) {
            // ── Guided mode ──────────────────────────────────────────────
            // Release any held button when hold_end_nmi is reached.
            if (s_held_mask && s_nmi >= s_hold_end_nmi) {
                action.release_mask = s_held_mask;
                s_held_mask = 0;
                // Advance slot after release
                if (s_scene) s_slot++;
                if (s_scene && s_slot >= s_scene->slot_count) {
                    fprintf(stderr, "[explorer] guided: scene complete, waiting for next search\n");
                    fflush(stderr);
                    s_scene = nullptr;
                }
                break;
            }

            if (s_scene && s_slot < s_scene->slot_count && !s_held_mask) {
                const SlotInfo& slot = s_scene->slots[s_slot];
                uint32_t target_frame = slot_target_frame(s_scene_start_frame,
                                                           slot.frame_offset,
                                                           s_delta_frames);

                // Guard: if the disc is far above the scene start, we are still
                // seeking backward to this scene (current_disc_frame is the
                // pre-seek value, which can be >> target_frame).  Don't fire
                // until the disc has physically arrived at the scene area.
                // 2000 frames ≈ 83 s — comfortably covers the longest DL scene.
                bool disc_arrived = (current_disc_frame >= s_scene_start_frame &&
                                     current_disc_frame < s_scene_start_frame + 2000u);

                if (disc_arrived && current_disc_frame >= target_frame && slot.mask) {
                    action.press_mask = slot.mask;
                    s_held_mask       = slot.mask;
                    s_hold_end_nmi    = s_nmi + PULSE_PRESS;
                    fprintf(stderr, "[explorer] guided: slot %d mask=0x%x @ disc=%u"
                            " (target=%u offset=%d+%d)\n",
                            s_slot, slot.mask, current_disc_frame, target_frame,
                            slot.frame_offset, s_delta_frames);
                    fflush(stderr);
                }
            }
        } else {
            // ── Simple mode ──────────────────────────────────────────────
            if (s_simple_mask && elapsed >= s_delay_nmi) {
                uint32_t phase = (elapsed - s_delay_nmi) % PULSE_PERIOD;
                if (phase == 0) {
                    action.press_mask = s_simple_mask;
                    s_move_held       = true;
                } else if (phase == PULSE_PRESS && s_move_held) {
                    action.release_mask = s_simple_mask;
                    s_move_held         = false;
                }
            }
        }
        break;
    }

    case GAMEOVER:
        // Ensure move is released on first GAMEOVER tick
        if (elapsed == 1) {
            if (s_move_held && s_simple_mask) {
                action.release_mask |= s_simple_mask;
                s_move_held = false;
            }
            if (s_held_mask) {
                action.release_mask |= s_held_mask;
                s_held_mask = 0;
            }
        }
        if (elapsed >= GAMEOVER_WAIT) {
            s_prev_lives = 255;
            s_scene      = nullptr;
            s_slot       = 0;
            enter_state(ATTRACT);
        }
        break;
    }

    return action;
}

} // namespace explorer
