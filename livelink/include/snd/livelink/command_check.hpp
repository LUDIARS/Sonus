// Validation of recorded commands (design.md 11.3; acceptance decisions D5 / R2).
//
// One table (kCommandRules, command_check.cpp) says, per command type, what a
// command must satisfy before ApplyCommands may see it. A rule has two parts:
//   - the game API's entry rules (core/src/recorder.cpp, snd/sonus.h), exactly:
//     a value the API accepts with SND_OK passes, a value the API refuses fails
//     (When kind / flags, finite parameter values, volume >= 0, bpm > 0, beat
//     unit 2/4/8/16, NaN fades of Stop, language names ...). Values the API does
//     not check (transforms, listener contents, Seek positions, When counts, Pause
//     fades ...) pass here too; core must survive them (it does: tested by the
//     replay fuzz with API-acceptable extremes);
//   - invariants of what the recording side itself writes (a handle the game-side
//     allocator issued: slot below the capacity, generation 1 .. 2^31-1 (D-46);
//     scope / target agreement; booleans 0/1; flag masks; fades the API clamped to
//     >= 0; priority hints <= 255). The API never produces anything else, so these
//     refuse only forged / corrupt recordings.
// The live link's own commands (Live*) follow the server's message rules (wire.hpp).
//
// For the api stage: the C API entry can use the same table (RuleOf(type).api is the
// entry rule; proposal in the stage-2 report), so the API and the replay stay equal.
#ifndef SND_LIVELINK_COMMAND_CHECK_HPP
#define SND_LIVELINK_COMMAND_CHECK_HPP

#include <cstdint>

#include "snd/config.h"
#include "snd/internal/commands.hpp"

namespace snd::livelink {

struct CommandLimits {
  std::uint32_t playbacks = 0, emitters = 0, transports = 0, userBuffers = 0, banks = 0, listeners = 0;
};
CommandLimits LimitsOf(const snd_config& config) noexcept;

using CommandPredicate = bool (*)(const std::uint8_t* payload, const CommandLimits& limits) noexcept;
struct CommandRule {
  cmd::CommandType type;
  bool replayable;           // false: points into the recording process / game memory
  CommandPredicate api;      // the game API's entry rule (or the server's, for Live*)
  CommandPredicate shape;    // invariants of what the recording side writes
  const char* description;   // the rule in words
};
// The rule of a command type (nullptr for an unknown type).
const CommandRule* RuleOf(std::uint16_t type) noexcept;

// true if the command (header + payload) is acceptable for ApplyCommands in a replay.
bool CheckCommand(const cmd::CommandHeader& h, const std::uint8_t* payload, const CommandLimits& limits) noexcept;

// 1.2 (D-104): the runtime's reserved ids (SND_ID_RESERVED_FIRST .. LAST, snd/id.h). The
// public snd_set_param_* refuse them; a recorded SetParam carries one only in the form
// snd_set_playback_volume / _pitch record (playback scope, volume >= 0 / pitch > 0).
bool IsReservedId(std::uint32_t id) noexcept;

// Overflow fallback parameter entry (scope, target, finite value for parameters).
bool CheckOverflowParam(const cmd::OverflowParam& p, const CommandLimits& limits) noexcept;

}  // namespace snd::livelink

#endif  // SND_LIVELINK_COMMAND_CHECK_HPP
