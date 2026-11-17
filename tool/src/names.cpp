// Display names.
#include "snd/tool/names.hpp"

#include <cstdio>

#include "snd/internal/commands.hpp"
#include "snd/internal/notify.hpp"
#include "snd/tool/mixer.hpp"
#include "snd/types.h"

namespace snd::tool {

const char* ResultName(std::int32_t r) {
  switch (r) {
    case SND_OK: return "SND_OK";
    case SND_ERR_INVALID_ARGUMENT: return "SND_ERR_INVALID_ARGUMENT";
    case SND_ERR_OUT_OF_BUDGET: return "SND_ERR_OUT_OF_BUDGET";
    case SND_ERR_NO_DEVICE: return "SND_ERR_NO_DEVICE";
    case SND_ERR_WRONG_THREAD: return "SND_ERR_WRONG_THREAD";
    case SND_ERR_THREAD_NOT_REGISTERED: return "SND_ERR_THREAD_NOT_REGISTERED";
    case SND_ERR_COMMAND_BUFFER_FULL: return "SND_ERR_COMMAND_BUFFER_FULL";
    case SND_ERR_INVALID_HANDLE: return "SND_ERR_INVALID_HANDLE";
    case SND_ERR_NOT_FOUND: return "SND_ERR_NOT_FOUND";
    case SND_ERR_BANK_IN_USE: return "SND_ERR_BANK_IN_USE";
    case SND_ERR_BANK_DEPENDENCY: return "SND_ERR_BANK_DEPENDENCY";
    case SND_ERR_IO: return "SND_ERR_IO";
    case SND_ERR_CORRUPT: return "SND_ERR_CORRUPT";
    case SND_ERR_VERSION_MISMATCH: return "SND_ERR_VERSION_MISMATCH";
    case SND_ERR_LIMIT_REACHED: return "SND_ERR_LIMIT_REACHED";
    case SND_ERR_UNSUPPORTED: return "SND_ERR_UNSUPPORTED";
    case SND_ERR_NOT_IMPLEMENTED: return "SND_ERR_NOT_IMPLEMENTED";
    case SND_ERR_NOT_READY: return "SND_ERR_NOT_READY";
    case SND_ERR_BANK_ALREADY_LOADED: return "SND_ERR_BANK_ALREADY_LOADED";
    default: return "SND_ERR_?";
  }
}

const char* MemTagName(std::uint32_t tag) {
  static const char* const kNames[SND_MEM_TAG_COUNT] = {"tables",      "frame arena", "thread arena", "stream pool",
                                                        "decoder pool", "dsp pool",    "pcm cache",    "bank",
                                                        "command arena", "notify",     "livelink",     "misc"};
  return tag < SND_MEM_TAG_COUNT ? kNames[tag] : "?";
}

const char* PlaybackStateName(std::uint32_t s) {
  switch (s) {
    case SND_PLAYBACK_STATE_NONE: return "none";
    case SND_PLAYBACK_STATE_PENDING: return "pending";
    case SND_PLAYBACK_STATE_PRIMING: return "priming";
    case SND_PLAYBACK_STATE_PLAYING: return "playing";
    case SND_PLAYBACK_STATE_PAUSED: return "paused";
    case SND_PLAYBACK_STATE_STOPPING: return "stopping";
    case SND_PLAYBACK_STATE_ENDED: return "ended";
    default: return "?";
  }
}

const char* SourceKindName(std::uint32_t k) {
  static const char* const kNames[] = {"sample", "stream", "oscillator", "noise", "granular",
                                       "patch",  "callback", "user buffer", "input"};
  return k < sizeof(kNames) / sizeof(kNames[0]) ? kNames[k] : "?";
}

std::string LogKindName(std::uint32_t kind) {
  if ((kind & 0x8000u) != 0u) {
    static const char* const kCommands[] = {
        "Invalid",
#define SND_TOOL_CMD_NAME(T, P, D, O) #T,
        SND_COMMANDS(SND_TOOL_CMD_NAME)
#undef SND_TOOL_CMD_NAME
    };
    const std::uint32_t c = kind & 0x7FFFu;
    if (c < sizeof(kCommands) / sizeof(kCommands[0])) return std::string("cmd ") + kCommands[c];
    return "cmd " + std::to_string(c);
  }
  switch (static_cast<notify::EventType>(kind)) {
    case notify::EventType::Beat: return "beat";
    case notify::EventType::Marker: return "marker";
    case notify::EventType::Log: return "log";
    case notify::EventType::Error: return "error";
    case notify::EventType::Warning: return "warning";
    case notify::EventType::StreamStall: return "stream stall";
    case notify::EventType::Underrun: return "underrun";
    case notify::EventType::LodChange: return "lod change";
    case notify::EventType::ReservationLate: return "reservation late";
  }
  return "event " + std::to_string(kind);
}

std::string FormatBytes(std::uint64_t b) {
  char buf[32];
  if (b < 1024u) {
    std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
  } else if (b < (1024u * 1024u)) {
    std::snprintf(buf, sizeof buf, "%.1f KiB", static_cast<double>(b) / 1024.0);
  } else {
    std::snprintf(buf, sizeof buf, "%.1f MiB", static_cast<double>(b) / (1024.0 * 1024.0));
  }
  return buf;
}

std::string FormatDb(float db) {
  if (db <= kFaderOffDb) return "-inf dB";
  char buf[24];
  std::snprintf(buf, sizeof buf, "%+.1f dB", static_cast<double>(db));
  return buf;
}

std::string Hex32(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%08x", v);
  return b;
}

}  // namespace snd::tool
