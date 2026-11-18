// Display names for values the game sends (profiler views, status line).
#ifndef SND_TOOL_NAMES_HPP
#define SND_TOOL_NAMES_HPP

#include <cstdint>
#include <string>

namespace snd::tool {

const char* ResultName(std::int32_t result);          // "SND_OK", "SND_ERR_NOT_FOUND", ...
const char* MemTagName(std::uint32_t tag);            // snd_mem_tag
const char* PlaybackStateName(std::uint32_t state);   // snd_playback_state
const char* SourceKindName(std::uint32_t kind);       // SourceKind
// LogEntry.kind: notify::EventType, or a command type | 0x8000.
std::string LogKindName(std::uint32_t kind);
std::string FormatBytes(std::uint64_t bytes);         // "12.3 MiB"
std::string FormatDb(float db);                       // "-6.0 dB", "-inf dB"
std::string Hex32(std::uint32_t v);                   // "0a1b2c3d"

}  // namespace snd::tool

#endif  // SND_TOOL_NAMES_HPP
