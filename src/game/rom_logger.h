#pragma once

/*
 * rom_logger.h
 *
 * Real-time frame data recorder for Daphne/Hypseus arcade games.
 * Captures laserdisc scene transitions and move timing windows directly
 * from the ROM execution, producing a NDJSON log file that can be used
 * to generate accurate Singe-compatible Lua scripts.
 *
 * Output format: one JSON event per line (NDJSON)
 *   {"e":"session","game":"lair","swA":34,"swB":216,"ms":0}
 *   {"e":"search","from":0,"to":4521,"ms":112}
 *   {"e":"play","f":4521,"ms":113}
 *   {"e":"enable","move":"UP","f":4600,"ms":200}
 *   {"e":"disable","move":"UP","f":4720,"ms":350}
 *   {"e":"pause","f":4890,"ms":500}
 *   {"e":"lives","n":4,"f":4890,"ms":501}
 *   {"e":"sound","snd":"accept","f":4721,"ms":351}
 *   {"e":"sound","snd":"buzz","f":4721,"ms":351}
 *   {"e":"end","ms":99999}
 */

#include <stdint.h>

namespace rom_logger {

// Open log file — call once at game init (lair::init, ace::init, etc.)
// switchA/switchB are the hardware DIP switch banks (difficulty, lives, etc.)
void open(const char* game_name, uint8_t switchA, uint8_t switchB);

// Close and flush the log file — call at shutdown
void close();

// Laserdisc player events
void log_search(uint32_t from_frame, uint32_t to_frame); // scene jump
void log_play(uint32_t frame);                            // playback starts
void log_pause(uint32_t frame);                           // playback paused

// Input window events (ROM opens/closes the acceptance window for a move)
// frame = current laserdisc frame at the moment the ROM sets/clears the input bit
void log_input_enable(uint8_t move, uint32_t frame);  // window opens
void log_input_disable(uint8_t move, uint32_t frame); // window closes

// Player state events
void log_lives(uint8_t lives, uint32_t frame); // lives register updated

// ROM audio feedback — the most direct signal of move acceptance/rejection.
// name = "accept" | "buzz" | "credit"
// "accept" means the ROM has confirmed the move was correct.
// "buzz"   means the ROM has rejected the move (wrong input or wrong timing).
void log_sound(const char* name, uint32_t frame);

// Scoreboard digit update — emitted whenever the ROM writes a score/credits digit.
// player = 1 or 2, pos = digit position 0 (most significant) to 5 (least significant).
// credits_pos = 0 (tens) or 1 (units) when player == 0.
void log_score(uint8_t player, uint8_t pos, uint8_t digit, uint32_t frame);

// Space Ace skill level selected by the player (Ace/Captain/Cadet).
// Emitted when the ROM writes 0xCC to the annunciator LED addresses.
void log_skill(const char* skill, uint32_t frame);

} // namespace rom_logger
