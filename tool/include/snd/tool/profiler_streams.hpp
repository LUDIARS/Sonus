// The rest of the profiler (design.md 11.1 プロファイラ: CPU, memory tags, streams, load LOD,
// event log), group P. UI independent (Sonus::tool_core).
//
// Profiler (profiler.hpp) keeps what the stage-2 screens show (timing summary, voices, meters,
// memory, log). ProfilerStreams adds the detail views and reads the live link client through its
// update numbers (protocol 1.1 §8.8): each Poll copies only what arrived since the last Poll
// (Copy*Since) and counts what was pushed out of the client before it looked (missed).
//
// Subscription: the UI says which views it shows (SetViews, a ProfilerView mask); Poll sends the
// profiler mask those views need (ProfilerMaskFor) when it changed or the game changed (a new
// session: the client re-sends the last mask itself after a reconnect). Bits a 1.0 game does not
// know (StreamList, BlockDetail) are not asked for (the client also drops them); the views say so
// (Available()).
//
// Views:
//   CPU      BlockTiming history (worst block of each interval): logic, render, budget, load =
//            (logic + render) / budget; and with kViewCpuDetail the BlockDetail history (total /
//            logic / render, voices, chunks, late chunks, held voices, parallel, LOD).
//   Memory   the latest MemoryUsage per snd_mem_tag with its name and used / budget.
//   Streams  the latest StreamList (slot, event, state, stalls) and the stall history.
//   LOD      the current level (last BlockTiming / LodChange) and the history of changes.
//   Log      the event log with a filter (kinds, id, playback slot) and the missed count.
// Every list keeps at most Desc limits (oldest dropped and counted in `dropped`).
// Threads: the UI thread (like Profiler). The client is thread-safe.
#ifndef SND_TOOL_PROFILER_STREAMS_HPP
#define SND_TOOL_PROFILER_STREAMS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/format/livelink_protocol.hpp"

namespace snd::livelink {
class Client;
}

namespace snd::tool {

enum ProfilerView : std::uint32_t {
  kViewVoices = 1u << 0,
  kViewCpu = 1u << 1,        // BlockTiming
  kViewMemory = 1u << 2,
  kViewStreams = 1u << 3,    // StreamList + stalls
  kViewLod = 1u << 4,
  kViewEventLog = 1u << 5,
  kViewMeters = 1u << 6,
  kViewCpuDetail = 1u << 7,  // BlockDetail
};
inline constexpr std::uint32_t kViewAll = 0xFFu;
// The ProfilerMask the views need, for a game speaking protocol minor `peerMinor`.
std::uint32_t ProfilerMaskFor(std::uint32_t views, std::uint16_t peerMinor);

struct CpuPoint {
  std::uint64_t receivedNs = 0;
  livelink::BlockTiming timing{};
  float load = 0.0f;  // (logic + render) / budget (0 when the budget is 0)
};
struct CpuDetailPoint {
  std::uint64_t receivedNs = 0;
  livelink::BlockDetail detail{};
  float load = 0.0f;  // total / budget
};
struct MemoryRow {
  std::uint32_t tag = 0;
  const char* name = "";
  std::uint64_t used = 0;
  std::uint64_t budget = 0;   // 0 = no budget
  float fraction = 0.0f;      // used / budget (0 without a budget)
};
const char* MemTagName(std::uint32_t tag);
struct StallPoint {
  std::uint64_t receivedNs = 0;
  livelink::StreamStall stall{};
};
struct LodPoint {
  std::uint64_t receivedNs = 0;
  livelink::LodChange change{};
};
struct LogPoint {
  std::uint64_t receivedNs = 0;
  livelink::LogEntry entry{};
};

// Event log filter. Kinds: notify::EventType values (1..9), 0x8000 | CommandType (commands the
// audio thread took), 0x4000 (a playback ended). Empty `kinds` = every kind.
struct LogFilter {
  std::vector<std::uint32_t> kinds;
  bool commands = true;    // 0x8000 kinds
  bool events = true;      // notify kinds
  bool ends = true;        // 0x4000
  std::uint32_t id = 0;    // 0 = any event / marker / parameter id
  std::uint32_t slot = ~0u;  // ~0u = any playback slot
  bool Matches(const livelink::LogEntry& e) const;
};
// Short text of a log kind ("Play", "PlaybackEnded", "Marker", ...).
std::string LogKindName(std::uint32_t kind);

// Received / missed per stream (missed = entries pushed out of the client before Poll read them).
struct StreamCount {
  std::uint64_t received = 0;
  std::uint64_t missed = 0;
};

class ProfilerStreams {
 public:
  struct Desc {
    std::uint32_t maxCpu = 4096;
    std::uint32_t maxDetail = 4096;
    std::uint32_t maxLog = 8192;
    std::uint32_t maxStalls = 1024;
    std::uint32_t maxLods = 1024;
    std::uint32_t meterHz = 30;
  };
  ProfilerStreams() : ProfilerStreams(Desc{}) {}
  explicit ProfilerStreams(const Desc& d) : desc_(d) {}

  // The views the UI shows (ProfilerView bits). Poll subscribes accordingly.
  void SetViews(std::uint32_t views) { views_ = views; }
  std::uint32_t views() const { return views_; }
  // The mask Poll sent last (0 before).
  std::uint32_t subscribedMask() const { return subscribed_; }
  // Whether the connected game can stream a view (false: a 1.0 game and kViewStreams' list or
  // kViewCpuDetail). `why` (optional) gets the reason.
  bool Available(std::uint32_t view, std::string* why = nullptr) const;

  // UI thread, once per frame. client may be null (offline: nothing changes).
  void Poll(livelink::Client* client, std::uint64_t nowNs);
  void Clear();

  // --- CPU --------------------------------------------------------------------------------
  const std::vector<CpuPoint>& cpu() const { return cpu_; }
  const std::vector<CpuDetailPoint>& cpuDetail() const { return detail_; }
  float lastLoad() const { return cpu_.empty() ? 0.0f : cpu_.back().load; }
  float maxLoad() const;  // over the kept history
  std::uint32_t overBudget() const;  // kept points whose load > 1
  // --- memory -----------------------------------------------------------------------------
  bool hasMemory() const { return hasMemory_; }
  std::vector<MemoryRow> Memory() const;
  std::uint64_t totalUsed() const;
  // --- streams ----------------------------------------------------------------------------
  const std::vector<livelink::StreamInfo>& streams() const { return streams_; }
  std::uint32_t activeStreams() const;
  const std::vector<StallPoint>& stalls() const { return stalls_; }
  // --- LOD --------------------------------------------------------------------------------
  std::uint32_t lodLevel() const { return lod_; }
  const std::vector<LodPoint>& lods() const { return lods_; }
  // --- event log --------------------------------------------------------------------------
  const std::vector<LogPoint>& log() const { return log_; }
  std::vector<LogPoint> Log(const LogFilter& filter) const;
  // --- counts -----------------------------------------------------------------------------
  const StreamCount& cpuCount() const { return cpuCount_; }
  const StreamCount& detailCount() const { return detailCount_; }
  const StreamCount& logCount() const { return logCount_; }
  const StreamCount& stallCount() const { return stallCount_; }
  const StreamCount& lodCount() const { return lodCount_; }
  // Entries dropped from the lists kept here (Desc limits).
  std::uint64_t dropped() const { return dropped_; }
  std::uint16_t peerMinor() const { return peerMinor_; }

 private:
  Desc desc_;
  std::uint32_t views_ = kViewCpu | kViewMemory | kViewLod | kViewEventLog;
  std::uint32_t subscribed_ = 0;
  std::uint32_t subscribedConnects_ = ~0u;
  std::uint16_t peerMinor_ = 0;
  std::vector<CpuPoint> cpu_;
  std::vector<CpuDetailPoint> detail_;
  livelink::MemoryUsage memory_{};
  bool hasMemory_ = false;
  std::vector<livelink::StreamInfo> streams_;
  std::uint64_t streamLists_ = 0, memories_ = 0;
  std::vector<StallPoint> stalls_;
  std::uint32_t lod_ = 0;
  std::vector<LodPoint> lods_;
  std::vector<LogPoint> log_;
  // Update numbers read so far (ClientSequences); reset when the client changes.
  const livelink::Client* client_ = nullptr;
  std::uint64_t seqTimings_ = 0, seqDetails_ = 0, seqLog_ = 0, seqStalls_ = 0, seqLods_ = 0;
  StreamCount cpuCount_, detailCount_, logCount_, stallCount_, lodCount_;
  std::uint64_t dropped_ = 0;
};

}  // namespace snd::tool

#endif  // SND_TOOL_PROFILER_STREAMS_HPP
