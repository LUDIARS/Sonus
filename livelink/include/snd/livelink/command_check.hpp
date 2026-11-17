// Validation of recorded commands (design.md 11.3; acceptance decision D5).
//
// Commands that reach core through the game API were checked on the way in
// (core/src/recorder.cpp: handles current, When kinds, finite floats, ranges,
// time signatures ...); core's ApplyCommands relies on that. A recording is
// external input (bug reports, files), so the Replayer passes every recorded
// command through the same rules before core sees it. A handle cannot be
// checked against the game-side allocator during a replay; it is checked for
// shape instead (slot below the capacity, generation 1 .. 2^31 - 1, D-46).
//
// Proposal (report to api): the C API entry checks and these should be one
// shared function set (they are the same rules); kept here until the api stage.
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

// true if the command (header + payload) is acceptable for ApplyCommands.
bool CheckCommand(const cmd::CommandHeader& h, const std::uint8_t* payload, const CommandLimits& limits) noexcept;

// Overflow fallback parameter entry (scope, target, finite value for parameters).
bool CheckOverflowParam(const cmd::OverflowParam& p, const CommandLimits& limits) noexcept;

}  // namespace snd::livelink

#endif  // SND_LIVELINK_COMMAND_CHECK_HPP
