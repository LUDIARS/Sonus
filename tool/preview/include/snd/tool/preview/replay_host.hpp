// The replay host (design.md 2.5, 11.3), group P. Library Sonus::tool_preview.
//
// Replays a recording (recording_file.hpp) in a fresh runtime inside the tool: the same
// composition as the preview engine with a livelink::Replayer installed into core instead of the
// live link. The banks the recording plays are given (the project's build, or a bug report's
// banks/ folder). The runtime Config: the caller's (a bug report's), else ReplayConfigFor (the
// recording's rate, block size and fallback capacities, LOD off, one render thread).
//
// Replay: the banks load first; then blocks are rendered until the next block is the recording's
// first block minus the lead blocks (the recorded block numbers are kept when bank loading took
// fewer blocks: ReplayRun::blockNumbersKept), Start, and Blocks() blocks (+ extraBlocks) are
// rendered. The ReplayRun gets the Replayer's info, conditions, decision log, the OutputDigest of
// the recorded blocks and (keepOutput) the output.
//
// ReplayTwice: two replays in two runtimes and their comparison (recording_report.hpp
// CompareRuns): decisions equal; bits equal when 2.5's conditions hold.
// CheckRecording: replays once and compares with an expectation file (tests, the CLI).
// MakeBugReport / ReplayBugReport: a bug report folder's content from a recording and its banks,
// and its replay.
// WriteWav: the replayed output as a WAV file (float 32). OutputPlayer: plays samples on the
// platform's output device (listening to a replay through the preview output).
// Threads: one thread per call (each replay owns its runtime).
#ifndef SND_TOOL_PREVIEW_REPLAY_HOST_HPP
#define SND_TOOL_PREVIEW_REPLAY_HOST_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "snd/config.h"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/recording_report.hpp"

namespace snd::livelink {
class IStructEditor;
}

namespace snd::tool::preview {

struct ReplayOptions {
  bool haveConfig = false;          // else ReplayConfigFor(recording)
  snd_config config{};
  livelink::IStructEditor* structEditor = nullptr;
  bool applyStartState = true;
  bool keepOutput = false;          // ReplayRun::output
  std::uint32_t extraBlocks = 0;    // rendered after the recording's last block (tails)
  std::uint32_t maxDecisions = 8192;
  std::uint32_t bankTimeoutMs = 10000;
};

// The banks of a build as report banks (files, ids, content hashes, bytes).
std::vector<ReportBank> BanksOf(const DocBuild& build);
// Reads every *.sndb of a directory (non recursive) as report banks.
bool ReadBankDirectory(const std::string& dir, std::vector<ReportBank>* out, std::string* error);

bool Replay(const Recording& recording, const std::vector<ReportBank>& banks, const ReplayOptions& options,
            ReplayRun* out);
ReplayComparison ReplayTwice(const Recording& recording, const std::vector<ReportBank>& banks,
                             const ReplayOptions& options, ReplayRun* first = nullptr, ReplayRun* second = nullptr);

// Replays and checks against an expectation (recording_report.hpp). `run` (optional) gets the replay.
ExpectationResult CheckRecording(const Recording& recording, const std::vector<ReportBank>& banks,
                                 const ReplayExpectation& expected, const ReplayOptions& options = {},
                                 ReplayRun* run = nullptr);
// The same from files: a recording file, a bank directory and an expectation file.
ExpectationResult CheckRecordingFiles(const std::string& recordingPath, const std::string& bankDir,
                                      const std::string& expectationPath, const ReplayOptions& options = {});

// A bug report from a recording: the description, a replay twice (unless replay is false) and its
// comparison, and the recorded run's log when given.
BugReport MakeBugReport(const Recording& recording, const std::vector<ReportBank>& banks, const snd_config* config,
                        const std::string& title, const std::string& notes, bool replay = true,
                        const RecordedRunLog* run = nullptr);
// Replays a report twice (its Config and banks) and checks it against its expectation.
ReplayComparison ReplayBugReport(const BugReport& report, ExpectationResult* againstExpectation = nullptr,
                                 ReplayRun* first = nullptr);

// Writes interleaved float samples as a 32-bit float WAV.
bool WriteWav(const std::string& path, const std::vector<float>& interleaved, std::uint32_t channels,
              std::uint32_t sampleRate, std::string* error);

// Plays samples on the platform's output device (or a silent timer), real time.
class OutputPlayer {
 public:
  OutputPlayer();
  ~OutputPlayer();
  OutputPlayer(const OutputPlayer&) = delete;
  OutputPlayer& operator=(const OutputPlayer&) = delete;
  // false + error if no device opens. Owner thread.
  bool Start(std::vector<float> interleaved, std::uint32_t channels, std::uint32_t sampleRate, bool silent,
             std::string* error);
  void Stop();
  bool Done() const;               // every sample was handed to the device
  std::uint64_t FramesPlayed() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace snd::tool::preview

#endif  // SND_TOOL_PREVIEW_REPLAY_HOST_HPP
