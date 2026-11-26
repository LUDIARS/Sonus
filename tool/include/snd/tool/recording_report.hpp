// Recordings for bug reports and tests (design.md 2.5, 11.3), group P. UI independent
// (Sonus::tool_core): no runtime here. Replaying needs a runtime and lives in the preview
// library (snd/tool/preview/replay_host.hpp, Sonus::tool_preview), which fills the ReplayRun
// structures below; comparing, the expectation files and the bug report folders are here.
//
// DescribeRecording: everything a record stream (protocol §7, 1.1 records included) says,
// without replaying it: how it started (Begin: FromCreation / MidRun, lost starting-state
// items, the override cells and live overrides at the start), the arenas and their commands
// per type, the live data (ReplaceChunk data, structural edits), the LOD changes, the stalls,
// the banks (at the start and registered during it) and the block range. Text() is the summary
// a bug report carries.
//
// Comparing replays (CompareRuns): two replays of one recording agree on the decisions (2.5
// "logic decisions": the decision log, same entries in the same blocks) and, when the 2.5
// conditions hold (ReplayRun::bitExactExpected: no LOD, no stall, nothing skipped ...), on every
// output bit (the digests). When the conditions do not hold, the comparison says why
// (ConditionReasons) and does not ask for bit identity.
// Comparing with the recorded run (CompareWithRecordedRun): what the recording and the live run
// still show of it: the profiler's event log taken while recording (commands the audio thread
// took, playback ends, events) against the replay's decision log, kind and id in order (the
// clocks differ: the recorded run's common clock vs the replay's blocks), and the recorded LOD
// changes / stalls against the replay's.
//
// Expectations for tests (ReplayExpectation): the decision log and, optionally, the output
// digest of a replay, written to a small text file next to the recording. CheckExpectation
// replays nothing: it compares a ReplayRun with an expectation (the preview library's
// CheckRecording replays and calls it).
//
// Bug report (WriteBugReport / ReadBugReport): one folder
//   report.txt     key = value lines: format, the recording file's header values (project and
//                  runtime build hashes, rate, block, platform, contract version), protocol,
//                  the runtime Config used to replay (every numeric snd_config member), the banks
//                  (file, bank id, language, content hash), the comparison results
//   summary.txt    for people: the title, the notes, the description and the comparison
//   notes.txt, description.txt, comparison.txt   BugReport::notes / summary / comparison exactly
//                  (D-185: a report reads back into the same BugReport)
//   recording.snrec  the recording file (recording_file.hpp)
//   banks/<file>   the bank files the recording plays (the project's build)
// It reads back into the same BugReport; the preview library replays it (ReplayBugReport).
#ifndef SND_TOOL_RECORDING_REPORT_HPP
#define SND_TOOL_RECORDING_REPORT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/config.h"
#include "snd/format/livelink_protocol.hpp"
#include "snd/id.hpp"
#include "snd/livelink/replayer.hpp"
#include "snd/tool/recording_file.hpp"

namespace snd::tool {

// --- describing a recording -------------------------------------------------------------
struct RecordedLod {
  std::uint64_t block = 0;
  std::uint32_t level = 0;
};
struct RecordedStall {
  std::uint64_t block = 0;
  std::uint32_t slot = 0;
};
struct RecordedBank {
  Id bankId = 0;
  std::uint64_t contentHash = 0;
  bool atStart = true;  // false: registered during the recording (a hot reload)
};
struct RecordingDescription {
  RecordingSummary summary;  // recording_file.hpp (arenas, LOD, stalls, bank maps, block range)
  bool hasBegin = false;     // a 1.1 recording (RecordBegin)
  std::uint32_t origin = 0;  // livelink::RecordOrigin (0: none)
  std::uint32_t lostState = 0;
  std::uint64_t beginBlock = 0;
  std::uint32_t startCells = 0;
  std::uint32_t startOverrides = 0;
  std::uint32_t liveData = 0;        // RecordLiveData records
  std::uint32_t replacedChunks = 0;  // ... of table chunks
  std::uint32_t structEdits = 0;     // ... of structural edits (kStructEditTag)
  std::uint64_t commands = 0;        // in every arena (normal + reserve regions)
  std::vector<std::uint32_t> commandsByType;  // [cmd::CommandType::Count]
  std::uint32_t fallbacks = 0;       // arenas with an overflow fallback section
  std::vector<RecordedLod> lods;
  std::vector<RecordedStall> stalls;
  std::vector<RecordedBank> banks;
  // The fallback capacities the recorded runtime used (FallbackHeader of the first arena that
  // has one; all 0 if none): a replay needs the same max_playbacks ... max_banks.
  std::uint32_t capPlaybacks = 0, capEmitters = 0, capTransports = 0, capUserBuffers = 0, capBanks = 0;
  std::string Text() const;  // a summary in words (one item per line)
};
RecordingDescription DescribeRecording(const std::uint8_t* data, std::uint64_t bytes);
inline RecordingDescription DescribeRecording(const Recording& r) {
  return DescribeRecording(r.records.data(), r.records.size());
}

// A runtime Config to replay `r` with when the recorded one is unknown (a game's recording):
// snd_config_init, the file's rate and block size, the fallback capacities the recording shows
// (DescribeRecording), LOD off (2.5), no real-time workers (one thread renders), the live link
// off. A preview recording carries its Config in the bug report instead.
snd_config ReplayConfigFor(const Recording& r);

// Every numeric member of snd_config as "name = value" lines (dotted for the nested ones:
// "memory.stream_pool_bytes"); pointers are left out. ParseConfigLine sets one member from
// such a line (false: unknown name or a malformed value; D-185: integers are digits only (or
// 0x + hex), no sign, and must fit the member's type; floats must be finite; nothing wraps).
std::string ConfigText(const snd_config& c);
bool ParseConfigLine(const std::string& name, const std::string& value, snd_config* c);

// --- replays ------------------------------------------------------------------------------
// One replay (filled by the preview library's ReplayHost).
struct ReplayRun {
  bool ok = false;
  std::int32_t result = 0;  // snd_result of the replay's set-up (Replayer::Create, bank loading)
  std::string error;
  livelink::ReplayInfo info{};
  std::vector<livelink::ReplayDecision> decisions;
  std::uint64_t digest = 0;      // livelink::OutputDigest of the recorded blocks
  std::uint64_t samples = 0;     // samples in the digest
  std::uint64_t blocks = 0;      // blocks rendered after Start (lead blocks included)
  bool finished = false;
  bool bitExactExpected = false;       // Replayer::BitExactExpected
  bool reproducesRecordedRun = false;  // Replayer::ReproducesRecordedRun
  bool blockNumbersKept = true;  // the first recorded block was rendered at its recorded number
  std::vector<float> output;     // interleaved output of the recorded blocks (when asked for)
  std::uint32_t channels = 0;
  std::uint32_t sampleRate = 0;
};

// The 2.5 conditions of a replay in words (ReplayCondition bits).
std::vector<std::string> ConditionReasons(std::uint32_t conditions);

struct ReplayComparison {
  bool decisionsMatch = false;
  std::uint32_t decisionsA = 0, decisionsB = 0;
  std::int64_t firstDecisionDiff = -1;  // index of the first differing entry, -1 if none
  bool bitExactExpected = false;        // both replays expected bit identity
  bool digestsMatch = false;
  bool bitsMatch = false;               // bit identity expected and the digests agree
  bool ok = false;                      // decisions match and (bits match or not expected)
  std::vector<std::string> reasons;     // why bit identity is not expected / what differs
  std::string Text() const;
};
ReplayComparison CompareRuns(const ReplayRun& a, const ReplayRun& b);

// The recorded run as the tool saw it while recording (profiler event log, LOD and stall
// history; any may be empty).
struct RecordedRunLog {
  std::vector<livelink::LogEntry> log;
  std::vector<RecordedLod> lods;
  std::vector<RecordedStall> stalls;
};
struct RecordedRunComparison {
  bool compared = false;      // there was something to compare
  bool logMatches = false;    // the decisions of kinds the log has, in order, equal the log's (kind, id)
  std::uint32_t logEntries = 0, replayEntries = 0;
  std::int64_t firstDiff = -1;
  bool lodsMatch = true;      // recorded LOD changes = the recording's (the replay does not reproduce them)
  bool stallsMatch = true;
  std::vector<std::string> notes;
  std::string Text() const;
};
RecordedRunComparison CompareWithRecordedRun(const ReplayRun& replay, const RecordingDescription& recording,
                                             const RecordedRunLog& run);

// --- expectations (tests) ----------------------------------------------------------------------
struct ReplayExpectation {
  std::vector<livelink::ReplayDecision> decisions;
  bool hasDigest = false;
  std::uint64_t digest = 0;
};
ReplayExpectation ExpectationOf(const ReplayRun& run, bool withDigest);
std::string EncodeExpectation(const ReplayExpectation& e);
bool DecodeExpectation(const std::string& text, ReplayExpectation* out, std::string* error);
bool SaveExpectation(const std::string& path, const ReplayExpectation& e, std::string* error);
bool LoadExpectation(const std::string& path, ReplayExpectation* out, std::string* error);
struct ExpectationResult {
  bool ok = false;
  std::vector<std::string> messages;  // what differs ("decision 3: kind 0x8001 id ... expected ...")
};
ExpectationResult CheckExpectation(const ReplayRun& run, const ReplayExpectation& e);

// --- bug reports -----------------------------------------------------------------------------
struct ReportBank {
  std::string file;  // "<name>[.<language>].sndb"
  Id bankId = 0;
  Id languageId = 0;
  std::uint64_t contentHash = 0;
  std::vector<std::uint8_t> bytes;
};
struct BugReport {
  Recording recording;
  std::vector<ReportBank> banks;
  snd_config config{};            // the runtime Config to replay with
  bool configKnown = false;       // false: ReplayConfigFor (derived), true: the recorded runtime's
  std::uint16_t protocolMajor = 0, protocolMinor = 0;
  std::string title;              // one line from the user
  std::string notes;              // free text
  std::string summary;            // DescribeRecording(...).Text()
  std::string comparison;         // ReplayComparison::Text() (+ recorded run), "" if not replayed
  bool replayed = false;
  bool decisionsMatch = false, bitExactExpected = false, bitsMatch = false;
  ReplayExpectation expectation;  // the first replay's decisions and digest (when replayed)
};
// Writes the folder (created; files in it are replaced). false + error on any failure.
bool WriteBugReport(const std::string& dir, const BugReport& report, std::string* error);
// Reads it back (the recording is checked as LoadRecording does; banks are read from banks/).
bool ReadBugReport(const std::string& dir, BugReport* out, std::string* error);

}  // namespace snd::tool

#endif  // SND_TOOL_RECORDING_REPORT_HPP
