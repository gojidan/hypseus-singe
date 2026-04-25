#include "explorer.h"
#include "../io/input.h"
#include "../video/video.h"
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
    uint32_t    frame_start;
    int         slot_count;
    SlotInfo    slots[20];
    const char* name;          // short display name for title bar (optional, may be nullptr)
};

static const SceneInfo SCENE_TABLE[] = {
    // Vestibule (small crumbling room)
    { 1887,  2, {{MASK_R,   63}, {MASK_R,  100}}, "Vestibule" },

    // Tentacles & Halberd
    { 2353,  6, {{MASK_B,   79}, {MASK_U,  140}, {MASK_R, 188},
                 {MASK_D,  228}, {MASK_L,  294}, {MASK_U, 345}}, "Tentacles" },

    // Snake Room (snake_room — tentacle_room alternate, row 7 col 2)
    { 3097,  4, {{MASK_B,   53}, {MASK_B,   98}, {MASK_B, 137}, {MASK_U, 159}}, "Snake Room" },

    // Fire Pit (ropes)
    { 3561,  4, {{MASK_R,  108}, {MASK_R,  149}, {MASK_R, 197}, {MASK_R, 242}}, "Fire Pit" },

    // Yellow Brick Road
    { 4139,  9, {{MASK_L,   36}, {MASK_U,   79}, {MASK_R, 135}, {MASK_U, 177},
                 {MASK_L,  230}, {MASK_U,  283}, {MASK_B, 363}, {MASK_R, 400},
                 {MASK_U,  441}}, "Yellow Brick Road" },

    // Bubbling Cauldron / Acid Creature (bubbling_cauldron — tentacle_room row 7 col 3)
    { 5123,  5, {{MASK_U,   56}, {MASK_B, 111}, {MASK_D, 149},
                 {MASK_B,  180}, {MASK_R,  250}}, "Bubbling Cauldron" },

    // Giddy Goons (giddy_goons — flattening_staircase alternate, row 8 col 2)
    // Verified: human run 20260420. Buzz R@100 dropped (not needed to pass scene).
    { 5683,  4, {{MASK_B,   66}, {MASK_R,  111}, {MASK_B, 163}, {MASK_U, 201}}, "Giddy Goons" },

    // YMCA Room (flattening stairs)
    { 6375,  4, {{MASK_L,   49}, {MASK_B,  110}, {MASK_L, 143}, {MASK_L, 210}}, "YMCA Room" },

    // Forge (Smithee — smithee, row 6 col 1)
    // Verified: human run 20260420. Slot 5 B@374: bot fired at 368 (pre-window), missed.
    { 6994,  5, {{MASK_B,   62}, {MASK_B,  129}, {MASK_L, 175},
                 {MASK_B,  235}, {MASK_B,  374}}, "Forge" },

    // Socker Boppers (grim_reaper — row 5 col 3)
    // Verified: human run 20260420. B@221 buzzed (edge of window); B@230 is safe.
    { 8004,  4, {{MASK_U,   77}, {MASK_B,  230}, {MASK_D, 281}, {MASK_U, 350}}, "Socker Boppers" },

    // Breathing Door (wind room)
    { 8709,  1, {{MASK_R,  185}}, "Breathing Door" },

    // Bower (bedroom / closing wall)
    { 9181,  1, {{MASK_U,   6}}, "Bower" },

    // Fire Room (lightning / bench over exit)
    { 9529,  4, {{MASK_R,   78}, {MASK_L,  117}, {MASK_D, 165}, {MASK_L, 213}}, "Fire Room" },

    // Flying Barding
    { 10021, 6, {{MASK_R,   99}, {MASK_L,  132}, {MASK_R, 176}, {MASK_L, 223},
                 {MASK_L,  271}, {MASK_L,  306}}, "Flying Barding" },

    // Chapel (Robot Knight / checkered floor)
    { 10741, 8, {{MASK_R,   85}, {MASK_L,  123}, {MASK_U, 163}, {MASK_L, 213},
                 {MASK_R,  259}, {MASK_L,  288}, {MASK_R, 314}, {MASK_B, 371}}, "Chapel" },

    // Mausoleum (Crypt Creeps)
    { 11489, 6, {{MASK_U,   61}, {MASK_B,  103}, {MASK_U, 139},
                 {MASK_B,  181}, {MASK_L,  226}, {MASK_B, 262}}, "Mausoleum" },

    // Catwalk Bats (catwalk_bats — yellow_brick_road row 9 col 3)
    { 12190, 5, {{MASK_U,   47}, {MASK_U,   76}, {MASK_B, 140},
                 {MASK_R,  177}, {MASK_R,  211}}, "Catwalk Bats" },

    // Fire Pit (reversed)
    // Slot 1 accept at disc 12835. Seq3 noseek window 27-44f from ~12837 = 12864-12881 (midpoint 12872 = +147).
    // Slots 3-4 recalculated from seq4/seq5 noseek starts; previous 156/203/248 were at window boundaries.
    { 12725, 4, {{MASK_L,  110}, {MASK_L,  158}, {MASK_L, 206}, {MASK_L, 248}}, "Fire Pit Rev" },

    // Yellow Brick Road (reversed)
    { 13303, 9, {{MASK_R,   41}, {MASK_U,   78}, {MASK_L, 132}, {MASK_U, 176},
                 {MASK_R,  225}, {MASK_U,  280}, {MASK_B, 363}, {MASK_L, 401},
                 {MASK_U,  444}}, "Yellow Brick Rev" },

    // Giant Bat
    { 14327, 4, {{MASK_B,   17}, {MASK_L,   46}, {MASK_U, 104}, {MASK_B, 163}}, "Giant Bat" },

    // Elevator Floor (always 3-floor version at cycle position 4 — user 2026-04-24)
    // SINGLE input (L), 3 valid floor windows. Cluster analysis from shift sweep:
    //   Floor 1: shift [-20..+4]   (disc ~14983..15007, around offset 156)
    //   Floor 2: shift [+7..+13]   (disc ~15010..15016, around offset 163-169)
    //   Floor 3: shift [+18..+19]  (disc ~15021..15022, around offset 174-175)
    // The bot fires L@156 and floor 1 window catches it (slot 2 was never observed firing).
    // For the runtime dataset, model as 1 slot with 3 alternative windows.
    { 14847, 1, {{MASK_L,  156}}, "Elevator" },

    // Forge Reversed (smithee_reversed — smithee row 6 col 2)
    { 15653, 5, {{MASK_B,   54}, {MASK_B,  128}, {MASK_R, 169},
                 {MASK_B,  239}, {MASK_B,  376}}, "Forge Rev" },

    // Flying Barding (reversed)
    { 16544, 6, {{MASK_L,   95}, {MASK_R,  132}, {MASK_L, 173}, {MASK_R, 222},
                 {MASK_R,  268}, {MASK_R,  303}}, "Flying Barding Rev" },

    // Pot of Gold (Lizard King)
    // Sequence confirmed by user 2026-04-24: L R R R R R U B B B B (11 slots, single U at #7).
    // DL Project reference indicates alternative inputs + optional slots:
    //   #1 U|L   #2-6 R   #7 U|B(sword)   #8 B   #9 D|B|N   #10 B|N   #11 D|B|N   #12 B|N
    //   (N = none / optional slot that can be skipped)
    // Our table captures ONE valid path (user's play style). Full alternatives to be modelled
    // in runtime once we formalise multi-input and optional slots (post-windows phase).
    { 17264,11, {{MASK_L,   43}, {MASK_R,  145}, {MASK_R, 234}, {MASK_R, 282},
                 {MASK_R,  398}, {MASK_R,  448}, {MASK_U, 488},
                 {MASK_B,  540}, {MASK_B,  572}, {MASK_B, 596},
                 {MASK_B,  641}}, "Pot of Gold" },

    // Wizard's Kitchen (Drink Me)
    { 18282, 1, {{MASK_R,   32}}, "Wizard Kitchen" },

    // Mausoleum (reversed)
    // Sequence confirmed by user 2026-04-24: U B U B R B (6 slots, mirrors non-rev Mausoleum).
    // Removed phantom duplicate U@72 that was mis-identified as slot 2 from a sub-seek event.
    { 18662, 6, {{MASK_U,   68}, {MASK_B, 101}, {MASK_U, 137},
                 {MASK_B,  179}, {MASK_R, 218}, {MASK_B, 261}}, "Mausoleum Rev" },

    // Socker Boppers (reversed) / grim_reaper_reversed
    // After U@90: ROM seeks 19614→19628. Seq3@19628 B window is 73-86f (disc 19701-19714).
    // Old B@174 fired at disc 19694 = offset 66 from 19628 → pre-window → buzz.
    // Fixed: B@187 = disc 19707 = offset 79 from 19628, safely inside window 73-86.
    { 19520, 4, {{MASK_U,   90}, {MASK_B,  187}, {MASK_D, 251}, {MASK_U, 307}}, "Socker Boppers Rev" },

    // Note: smithee_reversed (Forge reversed, frame 15653) is already in the table above.
    // 19628 is a Socker Boppers rev sub-seek target — no entry here prevents false scene-switch.

    // Tilting Room (tilting_room — row 11 col 2)
    // Verified: human run 20260420. Visit 1: D@97+U@128 → death (missed L). Visit 2: D@95+U@130+L@164 → success.
    { 20187, 3, {{MASK_D,   98}, {MASK_U,  133}, {MASK_L,  164}}, "Tilting Room" },

    // Throne Room (throne_room — row 11 col 1)
    // Levels.json move "U,R" is split into two ROM windows: U@78 then R@87.
    { 20674, 4, {{MASK_R,   55}, {MASK_U,   78}, {MASK_R,  87}, {MASK_R, 199}}, "Throne Room" },

    // Chapel Reversed (robot_knight_reversed — row 10 col 2)
    // 10 slots across noseek sequences; our log captured all from a cycle-2 visit.
    { 21212, 8, {{MASK_L,   92}, {MASK_R,  129}, {MASK_U, 167}, {MASK_R, 211},
                 {MASK_L,  263}, {MASK_R,  290}, {MASK_L, 315},
                 {MASK_B,  378}}, "Chapel Rev" },

    // Elevator Floor (9-level, reversed)
    { 21959, 1, {{MASK_R,  155}}, "Elevator Rev" },

    // Pirates of the Caribbean
    { 22936,13, {{MASK_L,   16}, {MASK_R,   56}, {MASK_L, 105}, {MASK_R, 151},
                 {MASK_U,  280}, {MASK_U,  344}, {MASK_U, 405}, {MASK_U, 471},
                 {MASK_R,  596}, {MASK_L,  671}, {MASK_R, 735}, {MASK_L, 797},
                 {MASK_R,  913}}, "Pirates" },

    // Mudmen
    // Slot 7 U@442 fired at disc 24820 = ~60f into seq8, but landed in death zone boundary.
    // seq8 UP success window is 46-63f from start (~24762); fixed to U@436 = disc 24814 = ~52f.
    // ROM-verified 20260423: slot 7 window disc 24809-24837 (W=28), center offset 445.
    { 24378, 9, {{MASK_B,  128}, {MASK_U,  217}, {MASK_U, 248}, {MASK_U, 302},
                 {MASK_U,  335}, {MASK_U,  382}, {MASK_U, 445}, {MASK_U, 500},
                 {MASK_U,  620}}, "Mudmen" },

    // Knight & Light (Black Knight / horse)
    // 3-slot data from cycle 3 visit; cycles 1-2 only have 2 slots but the
    // extra press on an already-ended scene is harmless.
    { 25536, 3, {{MASK_L,   74}, {MASK_L,  170}, {MASK_R, 242}}, "Knight & Light" },

    // Boulder Trench (colored balls)
    { 26098, 7, {{MASK_D,  128}, {MASK_D,  178}, {MASK_D, 227}, {MASK_D, 275},
                 {MASK_D,  322}, {MASK_D,  366}, {MASK_U, 411}}, "Boulder Trench" },

    // Three Caves (jagged door / geyser)
    { 26778, 3, {{MASK_U,   58}, {MASK_U,  102}, {MASK_L, 234}}, "Three Caves" },

    // Dragon's Lair (final)
    { 28938,12, {{MASK_L,  108}, {MASK_L,  381}, {MASK_L, 574}, {MASK_L, 582},
                 {MASK_D, 1124}, {MASK_D, 1301},
                 {MASK_R, 1437}, {MASK_U, 1520},
                 {MASK_B, 1556}, {MASK_B, 1644}, {MASK_L,1733}, {MASK_B,1795}}, "Dragon's Lair" },
};

static const int SCENE_TABLE_COUNT = (int)(sizeof(SCENE_TABLE) / sizeof(SCENE_TABLE[0]));

// ─── Hard difficulty SCENE_TABLE ──────────────────────────────────────────────
// Derived from 6 human-played Hard games (2026-04-25), median-aggregated offsets.
// Differences from Easy:
//   - Mudmen: 10 slots (slot 10 = U @645, was X on Easy)
//   - Knight & Light: 4 slots (slot 1 = L @9, NEW on Hard)
//   - Pot of Gold: 12 slots (slot 12 = B @638, was N optional)
//   - DL Final: 14 slots (3 X→required slots)
//   - Giant Bat: 5 slots (slot 5 = B @195, NEW)
//   - Many small offset shifts (1-15 frames)
static const SceneInfo SCENE_TABLE_HARD[] = {
    // Vestibule Hard: slot 1 walkthrough "D R" (multi-input). Human played D more often.
    { 1887,  2, {{MASK_D,   61}, {MASK_R,   98}}, "Vestibule" },
    { 2353,  6, {{MASK_B,   77}, {MASK_U,  137}, {MASK_R, 184},
                 {MASK_D,  224}, {MASK_L,  291}, {MASK_U, 333}}, "Tentacles" },
    { 3097,  4, {{MASK_B,   61}, {MASK_B,   98}, {MASK_B, 141}, {MASK_U, 159}}, "Snake Room" },
    { 3561,  4, {{MASK_R,  105}, {MASK_R,  156}, {MASK_R, 201}, {MASK_R, 245}}, "Fire Pit" },
    { 4139,  9, {{MASK_L,   44}, {MASK_U,   85}, {MASK_R, 133}, {MASK_U, 178},
                 {MASK_L,  239}, {MASK_U,  284}, {MASK_B, 366}, {MASK_R, 401},
                 {MASK_U,  454}}, "Yellow Brick Road" },
    { 5123,  5, {{MASK_U,   56}, {MASK_B,  112}, {MASK_D, 155},
                 {MASK_B,  186}, {MASK_R,  253}}, "Bubbling Cauldron" },
    { 5683,  4, {{MASK_B,   63}, {MASK_R,  104}, {MASK_B, 160}, {MASK_U, 196}}, "Giddy Goons" },
    { 6375,  4, {{MASK_L,   53}, {MASK_B,  110}, {MASK_L, 142}, {MASK_L, 209}}, "YMCA Room" },
    { 6994,  5, {{MASK_B,   64}, {MASK_B,  130}, {MASK_L, 174},
                 {MASK_B,  243}, {MASK_B,  375}}, "Forge" },
    { 8004,  4, {{MASK_U,   40}, {MASK_B,  223}, {MASK_D, 285}, {MASK_U, 348}}, "Socker Boppers" },
    { 8709,  1, {{MASK_R,  178}}, "Breathing Door" },
    { 9181,  1, {{MASK_U,   20}}, "Bower" },
    { 9529,  4, {{MASK_R,   73}, {MASK_L,  117}, {MASK_D, 163}, {MASK_L, 212}}, "Fire Room" },
    { 10021, 6, {{MASK_R,   96}, {MASK_L,  132}, {MASK_R, 174}, {MASK_L, 224},
                 {MASK_L,  271}, {MASK_L,  306}}, "Flying Barding" },
    { 10741, 8, {{MASK_R,   90}, {MASK_L,  125}, {MASK_U, 166}, {MASK_L, 214},
                 {MASK_R,  258}, {MASK_L,  291}, {MASK_R, 315}, {MASK_B, 368}}, "Chapel" },
    { 11489, 6, {{MASK_U,   60}, {MASK_B,  102}, {MASK_U, 140},
                 {MASK_B,  181}, {MASK_L,  228}, {MASK_B, 262}}, "Mausoleum" },
    { 12190, 5, {{MASK_U,   59}, {MASK_U,   89}, {MASK_B, 143},
                 {MASK_R,  177}, {MASK_R,  213}}, "Catwalk Bats" },
    { 12725, 4, {{MASK_L,  107}, {MASK_L,  164}, {MASK_L, 209}, {MASK_L, 244}}, "Fire Pit Rev" },
    // YBR Rev Hard: slot 1 walkthrough was R, but human played L 5/7 times (multi-input)
    { 13303, 9, {{MASK_L,   50}, {MASK_U,   87}, {MASK_L, 137}, {MASK_U, 178},
                 {MASK_R,  230}, {MASK_U,  285}, {MASK_B, 364}, {MASK_L, 403},
                 {MASK_U,  447}}, "Yellow Brick Rev" },
    { 14327, 5, {{MASK_B,   10}, {MASK_L,   48}, {MASK_U, 104}, {MASK_B, 163},
                 {MASK_L,  195}}, "Giant Bat" },  // slot 5 NEW on Hard
    { 14847, 1, {{MASK_L,  139}}, "Elevator" },
    { 15653, 5, {{MASK_B,   63}, {MASK_B,  132}, {MASK_R, 179},
                 {MASK_B,  242}, {MASK_B,  375}}, "Forge Rev" },
    { 16544, 6, {{MASK_L,   97}, {MASK_R,  133}, {MASK_L, 175}, {MASK_R, 224},
                 {MASK_R,  271}, {MASK_R,  305}}, "Flying Barding Rev" },
    { 17264,12, {{MASK_L,   43}, {MASK_R,  143}, {MASK_R, 232}, {MASK_R, 282},
                 {MASK_R,  397}, {MASK_R,  449}, {MASK_U, 487}, {MASK_B, 539},
                 {MASK_B,  565}, {MASK_B,  590}, {MASK_B, 613}, {MASK_B, 638}}, "Pot of Gold" },
    { 18282, 1, {{MASK_R,   34}}, "Wizard Kitchen" },
    { 18662, 6, {{MASK_U,   60}, {MASK_B,  101}, {MASK_U, 140},
                 {MASK_B,  182}, {MASK_R,  226}, {MASK_B, 261}}, "Mausoleum Rev" },
    { 19520, 4, {{MASK_U,   55}, {MASK_B,  184}, {MASK_D, 249}, {MASK_U, 306}}, "Socker Boppers Rev" },
    { 20187, 3, {{MASK_D,   95}, {MASK_U,  130}, {MASK_L,  164}}, "Tilting Room" },
    { 20674, 4, {{MASK_R,   48}, {MASK_U,   76}, {MASK_R,  98}, {MASK_R, 197}}, "Throne Room" },
    { 21212, 8, {{MASK_L,   91}, {MASK_R,  126}, {MASK_U, 168}, {MASK_R, 214},
                 {MASK_L,  261}, {MASK_R,  292}, {MASK_L, 317}, {MASK_B, 369}}, "Chapel Rev" },
    { 21959, 1, {{MASK_R,  158}}, "Elevator Rev" },
    // Pirates Hard: ROM sub-seeks 22936→23601 between slot 8 and 9. Slots 9-12 are
    // anchored to 22936 in our table but their actual disc frames come AFTER the
    // sub-seek. ROM-verified offsets from 6 human-played Hard runs:
    //   s9 R@676 (disc 23612), s10 L@734 (disc 23670), s11 R@797 (disc 23733), s12 L@913 (disc 23849)
    { 22936,12, {{MASK_L,   13}, {MASK_R,   59}, {MASK_L, 108}, {MASK_R, 154},
                 {MASK_U,  282}, {MASK_U,  341}, {MASK_U, 401}, {MASK_U, 469},
                 {MASK_L,  676}, {MASK_R,  734}, {MASK_L, 797}, {MASK_R, 913}}, "Pirates" },
    // Mudmen: 10 slots (slot 10 NEW on Hard, X→U)
    { 24378,10, {{MASK_B,  110}, {MASK_U,  217}, {MASK_U, 251}, {MASK_U, 305},
                 {MASK_U,  343}, {MASK_U,  385}, {MASK_U, 445}, {MASK_U, 506},
                 {MASK_U,  628}, {MASK_U,  645}}, "Mudmen" },
    // Knight & Light: 4 slots (slot 1 NEW on Hard, very early L)
    // Note: human samples for slot 1 were disc 5, 9, 11 (median 9). Bot at offset 9
    // buzzed (window starts later). Moving to 12 puts bot solidly inside window.
    { 25536, 4, {{MASK_U,   12}, {MASK_L,   69}, {MASK_L, 172}, {MASK_R, 248}}, "Knight & Light" },
    { 26098, 7, {{MASK_D,  127}, {MASK_D,  173}, {MASK_D, 222}, {MASK_D, 269},
                 {MASK_D,  318}, {MASK_D,  364}, {MASK_U, 409}}, "Boulder Trench" },
    { 26778, 3, {{MASK_U,   66}, {MASK_U,  106}, {MASK_L, 233}}, "Three Caves" },
    // DL Final Hard: 14 slots. Slots 7-8 swapped from initial assumption:
    // s7 disc 1418 was R but human plays U (X→U on Hard). s8 disc 1442 = R (not U).
    { 28938,14, {{MASK_L,  104}, {MASK_L,  375}, {MASK_L, 578}, {MASK_D, 1130},
                 {MASK_U, 1225}, {MASK_D, 1303}, {MASK_U, 1418}, {MASK_R, 1442},
                 {MASK_U, 1522}, {MASK_B, 1556}, {MASK_B, 1644}, {MASK_L, 1724},
                 {MASK_B, 1766}, {MASK_B, 1795}}, "Dragon's Lair" },
};

static const int SCENE_TABLE_HARD_COUNT = (int)(sizeof(SCENE_TABLE_HARD) / sizeof(SCENE_TABLE_HARD[0]));

// ─── Scene alias frames ──────────────────────────────────────────────────────
// On Hard, when Dirk dies, ROM enters scene via start_dead frame instead of
// canonical scene_frame. Marabelli must recognize both as the same scene.
struct SceneAlias {
    uint32_t alias_frame;
    uint32_t canonical_frame;
};
static const SceneAlias SCENE_ALIASES[] = {
    { 2406,  2353 },   // Tentacles start_dead
    { 3144,  3097 },   // Snake Room
    { 3505,  3561 },   // Fire Pit
    { 4174,  4139 }, { 4083, 4139 },   // Yellow Brick Road
    { 5155,  5123 },   // Bubbling Cauldron
    { 6417,  6375 }, { 6283, 6375 },   // YMCA Room
    { 8781,  8709 }, { 8653, 8709 },   // Breathing Door
    { 9190,  9181 }, { 9093, 9181 },   // Bower
    { 9552,  9529 }, { 9473, 9529 },   // Fire Room
    { 12238, 12190 },  // Catwalk Bats
    { 12669, 12725 },  // Fire Pit Rev
    { 13336, 13303 }, { 13127, 13303 }, { 13041, 13303 },  // Yellow Brick Rev
    { 14231, 14327 }, { 13875, 14327 },  // Giant Bat
    { 14791, 14847 },  // Elevator Floor
    { 18226, 18282 },  // Wizard Kitchen
    { 21808, 21959 },  // Elevator Rev
    { 24322, 24378 },  // Mudmen start_dead
    { 26723, 26778 }, { 26830, 26778 },  // Three Caves
    { 28840, 28938 },  // DL Final
};
static const int SCENE_ALIASES_COUNT = (int)(sizeof(SCENE_ALIASES) / sizeof(SCENE_ALIASES[0]));

// Resolve a (possibly-alias) frame to its canonical scene frame.
// Returns 0 if not a known scene.
static uint32_t resolve_canonical_frame(uint32_t frame)
{
    for (int i = 0; i < SCENE_ALIASES_COUNT; i++) {
        if (SCENE_ALIASES[i].alias_frame == frame)
            return SCENE_ALIASES[i].canonical_frame;
    }
    return frame;
}

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

// Scan mode state
static bool     s_scan        = false;
static uint32_t s_scan_frame  = 0;    // scene to scan
static int      s_scan_slot   = 0;    // 0-based slot index to replace
static uint32_t s_scan_mask   = 0;    // input to test (0 = use SCENE_TABLE mask)
static int32_t  s_scan_delta  = 0;    // current delta (varies each visit)
static int32_t  s_scan_step   = 1;    // delta increment per visit
static int      s_scan_visit  = 0;    // visit counter (for logging)

// Simple mode
static uint32_t s_simple_mask = 0;    // bitmask for simple pulse mode (MASK_*)
static char     s_move_char   = 'N';
static uint32_t s_delay_nmi   = 0;    // NMIs to wait after PLAYING before pulsing
static bool     s_move_held   = false;

// Scene counter (guided/scan mode display)
static int      s_scene_count = 0;

// Difficulty mode for SCENE_TABLE selection
static bool     s_use_hard_table = false;

// Global-shift mode: track which scenes have been visited at least once.
// Shift is applied ONLY to the first visit of each scene; subsequent visits
// (death-queue retries, later cycles) use the original offset so the game
// can progress past slots whose shifted offset falls outside the ROM window.
static bool     s_scene_visited[128] = {false}; // indexed by scene_table_idx
static bool     s_current_first_visit = false;

// Global-shift mask: list of (scene_frame, slot_idx_0based) pairs that force
// the original offset (delta=0) during -marabelli<N> runs.  Used to bypass
// already-characterised slots so later slots can be tested at extreme shifts.
#define EXPLORER_MASK_MAX 64
static uint32_t s_mask_frame[EXPLORER_MASK_MAX];
static int      s_mask_slot[EXPLORER_MASK_MAX];
static int      s_mask_count = 0;

// Common
static State    s_state       = ATTRACT;
static uint32_t s_nmi         = 0;
static uint32_t s_state_nmi   = 0;
static uint8_t  s_prev_lives  = 255;

// Guided mode scene tracking
static const SceneInfo* s_scene       = nullptr; // current scene entry, null = unknown
static uint32_t         s_scene_start_frame = 0; // CANONICAL scene frame (for slot offset computation)
static uint32_t         s_scene_entry_frame = 0; // ACTUAL frame ROM seeked to (alias or canonical) — for arrival check
static int              s_slot        = 0;        // next slot index to press
static bool             s_waiting_arrival = false; // true during backward seek to scene
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

// Look up a frame_start in the active scene table; returns null if not found.
// Resolves alias frames (start_dead, etc.) to their canonical scene.
static const SceneInfo* find_scene(uint32_t frame)
{
    // Resolve alias frames first
    uint32_t canonical = resolve_canonical_frame(frame);
    const SceneInfo* table = s_use_hard_table ? SCENE_TABLE_HARD : SCENE_TABLE;
    int count = s_use_hard_table ? SCENE_TABLE_HARD_COUNT : SCENE_TABLE_COUNT;
    for (int i = 0; i < count; i++) {
        if (table[i].frame_start == canonical)
            return &table[i];
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
    s_scene_entry_frame = 0;
    s_slot              = 0;
    s_held_mask         = 0;
    s_waiting_arrival   = false;
    memset(s_recent_to, 0, sizeof(s_recent_to));
    fprintf(stderr, "[explorer] guided mode: delta=%+d frames  scenes=%d\n",
            delta_frames, SCENE_TABLE_COUNT);
    fflush(stderr);
    return true;
}

bool add_shift_mask(uint32_t frame, int slot_1based)
{
    if (s_mask_count >= EXPLORER_MASK_MAX) {
        fprintf(stderr, "[mask] full (max %d entries), dropping %u:%d\n",
                EXPLORER_MASK_MAX, frame, slot_1based);
        fflush(stderr);
        return false;
    }
    s_mask_frame[s_mask_count] = frame;
    s_mask_slot[s_mask_count]  = slot_1based - 1;  // 0-based
    s_mask_count++;
    fprintf(stderr, "[mask] added frame=%u slot=%d (count=%d)\n",
            frame, slot_1based, s_mask_count);
    fflush(stderr);
    return true;
}

static bool is_shift_masked(uint32_t scene_frame, int slot_idx_0based)
{
    for (int i = 0; i < s_mask_count; i++) {
        if (s_mask_frame[i] == scene_frame && s_mask_slot[i] == slot_idx_0based)
            return true;
    }
    return false;
}

bool init_guided_hard(int32_t delta_frames)
{
    s_use_hard_table = true;
    bool ok = init_guided(delta_frames);
    fprintf(stderr, "[explorer] HARD difficulty mode active — using SCENE_TABLE_HARD (%d scenes)\n",
            SCENE_TABLE_HARD_COUNT);
    fflush(stderr);
    return ok;
}

bool init_scan(uint32_t frame, int slot, char input_char,
               int32_t start_delta, int32_t step)
{
    if (!init_guided(0)) return false;

    uint32_t mask = 0;
    switch ((char)toupper((unsigned char)input_char)) {
        case 'U': mask = MASK_U; break;
        case 'L': mask = MASK_L; break;
        case 'D': mask = MASK_D; break;
        case 'R': mask = MASK_R; break;
        case 'B': mask = MASK_B; break;
        case '0': mask = 0;      break;  // keep SCENE_TABLE mask
        default:
            fprintf(stderr, "[scan] unknown input '%c'\n", input_char);
            return false;
    }

    s_scan       = true;
    s_scan_frame = frame;
    s_scan_slot  = slot - 1;  // convert to 0-based
    s_scan_mask  = mask;
    s_scan_delta = start_delta;
    s_scan_step  = step;
    s_scan_visit = 0;

    fprintf(stderr, "[scan] scene=%u slot=%d input='%c' start_delta=%+d step=%+d\n",
            frame, slot, input_char, start_delta, step);
    fflush(stderr);
    return true;
}

bool is_active() { return s_active; }

void on_lives(uint8_t n)
{
    if (!s_active) return;
    if (n == 15) return;  // 0xFF & 0x0F = 15 during ROM boot init

    // In guided mode the ROM sends transient lives=0 during death animations;
    // ignore it — guided runs self-terminate via scene/attract logic.
    if (!s_guided && s_state == PLAYING && n == 0 && s_prev_lives > 0 && s_prev_lives != 255) {
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
        // Scan mode: when the target scene is (re)visited, advance the delta.
        if (s_scan && to == s_scan_frame) {
            if (s_scan_visit > 0) s_scan_delta += s_scan_step;
            s_scan_visit++;
            const SceneInfo* sc = find_scene(to);
            int32_t base_off = (sc && s_scan_slot < sc->slot_count)
                               ? sc->slots[s_scan_slot].frame_offset : 0;
            fprintf(stderr, "[scan] visit=%d slot=%d delta=%+d => offset=%d (disc ~%u)\n",
                    s_scan_visit, s_scan_slot + 1, s_scan_delta,
                    base_off + s_scan_delta, to + base_off + s_scan_delta);
            fflush(stderr);
        }

        const SceneInfo* scene = find_scene(to);
        if (scene) {
            s_scene             = scene;
            // Use canonical scene frame so slot offsets (anchored to canonical) work
            // correctly when ROM enters via alias frames (start_dead etc.)
            s_scene_start_frame = scene->frame_start;
            // Track the ACTUAL entry frame ROM seeked to — used for arrival check.
            // Aliases (e.g. start_dead 2406 for Tentacles canonical 2353) land disc
            // at the alias, not canonical, so arrival must be checked against alias.
            s_scene_entry_frame = to;
            s_slot              = 0;
            s_held_mask         = 0;
            s_scene_count++;
            // Track first visit for global-shift mode
            const SceneInfo* base_table = s_use_hard_table ? SCENE_TABLE_HARD : SCENE_TABLE;
            int scene_idx = (int)(scene - base_table);
            if (scene_idx >= 0 && scene_idx < (int)(sizeof(s_scene_visited)/sizeof(s_scene_visited[0]))) {
                s_current_first_visit = !s_scene_visited[scene_idx];
                s_scene_visited[scene_idx] = true;
            } else {
                s_current_first_visit = true;
            }
            // If seeking backward (from > to) the disc hasn't arrived yet —
            // current_disc_frame is still the old high value.  Suppress slot
            // firing until the disc physically arrives at the scene start.
            s_waiting_arrival   = (from > to);
            fprintf(stderr, "[explorer] guided: scene frame=%u slots=%d  delta=%+d%s\n",
                    to, scene->slot_count, s_delta_frames,
                    s_waiting_arrival ? " [await arrival]" : "");
            fflush(stderr);

            // Update window title with current scene info
            char title[160];
            const char* name = scene->name ? scene->name : "?";
            if (s_scan && to == s_scan_frame)
                snprintf(title, sizeof(title), "sc#%d %s (%u) [%d] SCAN d=%+d",
                         s_scene_count, name, to, scene->slot_count, s_scan_delta);
            else
                snprintf(title, sizeof(title), "sc#%d %s (%u) [%d]",
                         s_scene_count, name, to, scene->slot_count);
            video::set_title_extra(title);
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
            s_waiting_arrival   = false;
            memset(s_recent_to, 0, sizeof(s_recent_to));
            // Reset first-visit tracking for global-shift mode
            memset(s_scene_visited, 0, sizeof(s_scene_visited));
            s_current_first_visit = true;
            enter_state(PLAYING);
        }
        break;

    case PLAYING: {
        // Backstop: if stuck in PLAYING for >3 minutes, force restart (free mode only;
        // guided runs can last 30–60 min so we skip this timeout entirely).
        if (!s_guided && elapsed >= 3 * 60 * NMI_HZ) {
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

            // For backward seeks, wait until disc physically arrives at the
            // scene start before evaluating any slots.
            if (s_waiting_arrival) {
                // Check arrival against the ACTUAL seek target (entry_frame), not canonical.
                // When ROM enters via alias frame, disc lands at alias (which may be > canonical).
                if (current_disc_frame <= s_scene_entry_frame + 10u) {
                    s_waiting_arrival = false;
                    fprintf(stderr, "[explorer] guided: arrived at scene %u (disc=%u)\n",
                            s_scene_start_frame, current_disc_frame);
                    fflush(stderr);
                }
            }

            if (s_scene && s_slot < s_scene->slot_count && !s_held_mask && !s_waiting_arrival) {
                const SlotInfo& slot = s_scene->slots[s_slot];

                // Scan mode: override offset delta and/or mask for the target slot.
                bool is_scan_slot = s_scan
                                    && s_scene->frame_start == s_scan_frame
                                    && s_slot == s_scan_slot;
                // Global-shift exemption: Elevator end-of-cycle (21959) and DL Final (28938)
                // are NOT added to the death queue — failing them locks Dirk in an infinite
                // respawn loop.  Keep these scenes at their original offset regardless of shift.
                bool shift_exempt = (s_scene_start_frame == 21959u ||
                                     s_scene_start_frame == 28938u);
                // Apply global shift ONLY to the first visit of each scene.  Retries from
                // the death queue (and subsequent cycle visits) use offset 0 so the game
                // can progress past slots whose shifted offset falls outside the ROM window.
                // Also skip shift for slots in the explicit mask list.
                bool slot_masked = is_shift_masked(s_scene_start_frame, s_slot);
                bool apply_shift = !shift_exempt && !slot_masked && s_current_first_visit;
                int32_t  eff_delta = is_scan_slot ? s_scan_delta
                                   : (apply_shift ? s_delta_frames : 0);
                uint32_t eff_mask  = (is_scan_slot && s_scan_mask) ? s_scan_mask : slot.mask;

                uint32_t target_frame = slot_target_frame(s_scene_start_frame,
                                                           slot.frame_offset,
                                                           eff_delta);

                if (current_disc_frame >= target_frame && eff_mask) {
                    action.press_mask = eff_mask;
                    s_held_mask       = eff_mask;
                    s_hold_end_nmi    = s_nmi + PULSE_PRESS;
                    fprintf(stderr, "[explorer] %s: slot %d mask=0x%x @ disc=%u"
                            " (target=%u offset=%d%+d)\n",
                            is_scan_slot ? "SCAN" : "guided",
                            s_slot, eff_mask, current_disc_frame, target_frame,
                            slot.frame_offset, eff_delta);
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
