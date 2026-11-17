// Profiler model of the tool (design.md 11.1 プロファイラ, 11.3 記録).
//
// Poll() copies what the live link client buffered (once per UI frame, UI
// thread): block timings of the last history window, the latest voice list
// (sorted by audibility, loudest first), the latest bus meters with peak hold,
// memory usage, the event log, stream stalls and LOD changes. Everything is a
// copy; the views read it without touching the client.
//
// Recording: StartRecording / StopRecording send RecordControl and follow the
// Ack; after the stop is acknowledged and no new RecordData arrived for
// `settleMs`, the recording is Ready and can be saved (SaveRecording writes a
// recording file, recording_file.hpp). LoadRecording reads one back and keeps
// its summary for the view.
#ifndef SND_TOOL_PROFILER_HPP
#define SND_TOOL_PROFILER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/format/livelink_protocol.hpp"
#include "snd/tool/meter_ballistics.hpp"
#include "snd/tool/recording_file.hpp"

namespace snd::livelink {
class Client;
}

namespace snd::tool {

struct TimingPoint {
  std::uint64_t receivedNs = 0;
  livelink::BlockTiming timing{};
};
struct LogLine {
  std::uint64_t receivedNs = 0;
  livelink::LogEntry entry{};
};

struct TimingSummary {
  std::uint32_t samples = 0;
  std::uint64_t lastBlock = 0;
  float lastRenderUs = 0.0f, avgRenderUs = 0.0f, maxRenderUs = 0.0f;
  float lastLogicUs = 0.0f, avgLogicUs = 0.0f;
  float budgetUs = 0.0f;
  float loadPercent = 0.0f;  // (logic + render) / budget of the last sample
  std::uint32_t lod = 0, lateChunks = 0, underruns = 0;  // last sample
  std::uint32_t overBudget = 0;  // samples whose logic + render exceeded the budget
};

struct BusMeter {
  std::uint32_t busId = 0;
  std::uint32_t channels = 0;
  float peak[SND_MAX_CHANNELS] = {};
  float rms[SND_MAX_CHANNELS] = {};
  float holdDb[SND_MAX_CHANNELS] = {};  // peak hold (meter_ballistics.hpp) at the last Poll
  bool clipped = false;
  std::uint64_t receivedNs = 0;
};

enum class RecordingState : std::uint8_t { Idle = 0, Starting = 1, Recording = 2, Stopping = 3, Ready = 4, Failed = 5 };
const char* RecordingStateName(RecordingState s);

class Profiler {
 public:
  struct Desc {
    std::uint32_t maxTimings = 4096;
    std::uint32_t maxLog = 512;
    std::uint32_t maxVoices = 1024;
    std::uint32_t maxMeters = 64;
    std::uint32_t settleMs = 100;
    PeakHoldParams hold;
  };
  Profiler() : Profiler(Desc{}) {}
  explicit Profiler(const Desc& d) : desc_(d) {}

  void Poll(livelink::Client* client, std::uint64_t nowNs);
  void Clear();

  const std::vector<TimingPoint>& timings() const { return timings_; }
  TimingSummary Summary() const;
  const std::vector<livelink::VoiceInfo>& voices() const { return voices_; }
  std::uint32_t realVoices() const;
  const std::vector<BusMeter>& meters() const { return meters_; }
  const BusMeter* FindMeter(std::uint32_t busId) const;
  bool hasMemory() const { return hasMemory_; }
  const livelink::MemoryUsage& memory() const { return memory_; }
  const std::vector<LogLine>& log() const { return log_; }
  std::uint32_t stalls() const { return stalls_; }
  std::uint32_t lodChanges() const { return lods_; }
  std::uint64_t received(livelink::MessageType t) const;
  void ResetClips();

  // --- recording ----------------------------------------------------------------------------
  bool StartRecording(livelink::Client* client);
  bool StopRecording(livelink::Client* client);
  RecordingState recordingState() const { return recState_; }
  std::uint64_t recordedBytes() const { return recBytes_; }
  std::int32_t recordingError() const { return recError_; }
  // Copies the client's recording into a Recording (info from the caller).
  bool TakeRecording(livelink::Client* client, const RecordingInfo& info, Recording* out) const;
  bool SaveRecording(livelink::Client* client, const RecordingInfo& info, const std::string& path,
                     std::string* error);
  bool LoadRecording(const std::string& path, std::string* error);
  const Recording& loaded() const { return loaded_; }
  const RecordingSummary& loadedSummary() const { return loadedSummary_; }
  const std::string& loadedPath() const { return loadedPath_; }

 private:
  void PollRecording(livelink::Client* client, std::uint64_t nowNs);
  Desc desc_;
  std::vector<TimingPoint> timings_;
  std::vector<livelink::VoiceInfo> voices_;
  std::vector<BusMeter> meters_;
  std::vector<std::uint32_t> holdIds_;
  std::vector<PeakHold> holds_;  // holdIds_[i] * SND_MAX_CHANNELS + channel
  livelink::MemoryUsage memory_{};
  bool hasMemory_ = false;
  std::vector<LogLine> log_;
  std::uint32_t stalls_ = 0;
  std::uint32_t lods_ = 0;
  std::uint64_t counts_[8] = {};
  RecordingState recState_ = RecordingState::Idle;
  std::uint32_t recSeq_ = 0;
  std::int32_t recError_ = 0;
  std::uint64_t recBytes_ = 0;
  std::uint64_t recChangeNs_ = 0;
  Recording loaded_;
  RecordingSummary loadedSummary_;
  std::string loadedPath_;
};

}  // namespace snd::tool

#endif  // SND_TOOL_PROFILER_HPP
