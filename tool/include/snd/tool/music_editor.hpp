// Music editor model (design.md 8, 11.1 音楽エディタ; project-format.md 2.x).
//
// TempoMap: a segment's tempo / time-signature points with the conversions the runtime's
// music time line uses (music/src/timeline.cpp, same formulas in double): 960 ticks per
// quarter note, bpm counts quarter notes, a beat is a note of the signature's beat unit
// (3840 / beatUnit ticks), the point in effect at tick t is the last with point.tick <= t
// (the first point for earlier ticks), each point carries the bar position at its tick
// (continuous across signature changes), grids count from the bar line, "next beat /
// bar / grid" fire at the point itself when exactly on it (>=, D-69).
//
// SegmentEditor / PlaylistEditor / MusicEditor edit the objects (undoable). Rules:
// tempo points with distinct ticks (sorted), a change of time signature only on a bar line of
// the signature before it (D-179c), bpm 1e-3 .. 1e5 (D-85), beatsPerBar >= 1,
// beatUnit 1, 2, 4, 8, 16 or 32; marker names unique in the segment, frames inside the
// material; entryFrame < exitFrame <= frames; bars >= 1; stems inside their material's
// channels (a separate-file stem: that file; unreadable: allowed and listed by Problems, D-177d); playlist weights >= 0, repeat >= 1; state names unique and not "*" (the
// wildcard), a state's playlist exists; transitions name states of the event or "*".
// RenameState renames the state in the transitions and the initial state too.
//
// Transition rule table (8.5): Match = the first row that matches from top to bottom (the
// runtime's FindRule; -1 = none: the built-in default, next bar, 1 beat fades);
// Unreachable = rows an earlier row covers (the build's warning, same rule); AutoSort =
// stable sort by specificity (exact from and to, then one exact, then * -> *), refused while a
// row is unreachable (D-179e: sorting could then change which rule matches).
//
// Minimum lookahead (8.2), per reservation target: one block (blockFrames / sampleRate)
// plus the target's prime time:
//   - material in memory (loadForm pcm / compressed / decompressCache): 0;
//   - a stream whose transition is "prepare" (8.3 事前プライム): 0;
//   - a stream started at its head with a preload (preloadMs > 0): 0 (the preload chunk
//     is in memory, 6.1);
//   - a stream started elsewhere (sameBar / marker / lastPosition) whose segment has
//     entryPreload: 0 (8.3 入口の頭出し); otherwise the stream's prime time;
//   - a stream without preload (preloadMs 0): the stream's prime time.
// The stream's prime time (open, seek, first decode) is a setting (MusicSettings::
// streamPrimeMs): it depends on the device, not on the project.
//
// Transition preview (8.7): which rule, the firing tick (the rule's timing from the
// request; nextMarker: the next user marker (by id, 0 = any) else the segment end;
// segmentEnd: the segment's exit), the late rule when the firing comes sooner than the
// minimum lookahead (nextGrid: the next timing point; startLate: the request + lookahead;
// cancel: no transition), fades in seconds (out at the source tempo, in at the target's),
// the bridge and where the target segment starts (head, same bar: the source bar modulo
// the target's bars, beat kept when the signatures match (D-74); marker; lastPosition is
// the head in the preview: no history). Computed on the source state's first segment.
#ifndef SND_TOOL_MUSIC_EDITOR_HPP
#define SND_TOOL_MUSIC_EDITOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/tool/document.hpp"

namespace snd::tool {

inline constexpr double kTicksPerQuarter = 960.0;

struct TempoPointDesc {
  std::uint64_t tick = 0;
  double bpm = 120.0;
  std::uint32_t beatsPerBar = 4;
  std::uint32_t beatUnit = 4;
};

struct MusicalPosition {
  std::int32_t bar = 0;
  std::int32_t beat = 0;
  std::int32_t tick = 0;
};

class TempoMap {
 public:
  // `points` in any order (sorted here); empty -> 120 BPM 4/4.
  explicit TempoMap(std::vector<TempoPointDesc> points = {});
  std::uint32_t PointAt(double tick) const;
  double BeatTicks(std::uint32_t point) const;
  double BarTicks(std::uint32_t point) const;
  double BpmAt(double tick) const;
  double SecondsAtTick(double tick) const;  // from tick 0
  double TickAtSeconds(double seconds) const;
  double FrameAtTick(double tick, double sampleRate) const { return SecondsAtTick(tick) * sampleRate; }
  double TickAtFrame(double frame, double sampleRate) const { return TickAtSeconds(frame / sampleRate); }
  double BarPosition(double tick) const;
  MusicalPosition Musical(double tick) const;
  // D-179d: with a beat longer than a quarter note (beatUnit 1 or 2) the tick within the beat
  // is capped at 959 (snd_musical_time, as the runtime): Musical is then approximate.
  bool MusicalIsApproximate(double tick) const;
  double TickOfMusical(const MusicalPosition& m) const;
  double TickAtBar(double bar) const;
  double NextBeat(double from) const;
  double NextBar(double from) const;
  double BarsAfter(double from, std::uint32_t n) const;
  double NextGrid(double from, std::uint32_t division) const;
  const std::vector<TempoPointDesc>& points() const { return points_; }
  const std::vector<double>& bars() const { return bars_; }

 private:
  std::vector<TempoPointDesc> points_;
  std::vector<double> bars_;  // bar position at each point
};

// Tap tempo (8.7): least-squares line through the taps (seconds).
class TapTempo {
 public:
  void Tap(double seconds) { taps_.push_back(seconds); }
  void Reset() { taps_.clear(); }
  std::size_t count() const { return taps_.size(); }
  double Bpm() const;            // 0 with fewer than 2 taps
  double OffsetSeconds() const;  // first beat (the fitted line at tap 0), modulo one beat into [0, beat)

 private:
  std::vector<double> taps_;
};

struct GridLine {
  double seconds = 0.0;
  double frame = 0.0;
  std::uint32_t bar = 0;
  std::uint32_t beat = 0;
  bool barLine = false;
};
// Beat grid from `offsetSeconds` over `durationSeconds` of material (8.7 グリッド合わせ).
std::vector<GridLine> BeatGrid(double bpm, double offsetSeconds, std::uint32_t beatsPerBar, std::uint32_t beatUnit,
                               double durationSeconds, double sampleRate);

enum class MarkerKind : std::uint8_t { User = 0, Entry = 1, Exit = 2, Pickup = 3 };
struct MarkerDesc {
  std::string name;
  std::uint64_t frame = 0;
  MarkerKind kind = MarkerKind::User;
};
struct StemDesc {
  std::uint32_t channelFirst = 0;
  std::uint32_t channelCount = 2;
  std::string asset;  // separate-file stem ("" = the segment's material)
};

class SegmentEditor {
 public:
  SegmentEditor(ProjectDocument& doc, std::string guid);
  bool valid() const;
  bool SetAsset(const std::string& assetGuid);
  bool SetBars(std::uint32_t bars);
  bool SetEntryFrame(std::uint64_t frame);
  bool SetExitFrame(std::uint64_t frame);
  bool SetEntryPreload(bool on);
  std::vector<TempoPointDesc> Tempo() const;
  bool AddTempo(const TempoPointDesc& p);
  bool SetTempo(std::uint32_t i, const TempoPointDesc& p);
  bool RemoveTempo(std::uint32_t i);
  std::vector<MarkerDesc> Markers() const;
  bool AddMarker(const MarkerDesc& m);
  bool SetMarker(std::uint32_t i, const MarkerDesc& m);
  bool RemoveMarker(std::uint32_t i);
  bool AddStem(const StemDesc& s);
  bool SetStem(std::uint32_t i, const StemDesc& s);
  bool RemoveStem(std::uint32_t i);
  // The segment's tempo map and material.
  TempoMap Map() const;
  double SampleRate() const;     // of the material (0 if it cannot be read)
  double Frames() const;
  double ExitTick() const;       // exitFrame (else bars) as a tick
  // 8.7: sets one tempo point (bpm, signature) at tick 0, entryFrame = offsetFrame and the
  // bars that fit the material after the entry (at least 1). One undo step.
  bool ApplyGrid(double bpm, std::uint64_t offsetFrame, std::uint32_t beatsPerBar, std::uint32_t beatUnit);
  // Problems the screen shows (warnings): a time signature that changes inside a bar (loaded
  // that way; the editor refuses to make one, D-179c), a stem whose material cannot be read.
  std::vector<std::string> Problems() const;
  // Channels of a stem's material ("" = the segment's), -1 if it cannot be read.
  int StemChannels(const std::string& assetRef) const;

 private:
  bool ValidTempo(const std::vector<TempoPointDesc>& t) const;
  bool WriteTempo(const std::vector<TempoPointDesc>& t);
  ProjectDocument& doc_;
  std::string guid_;
};

enum class PlaylistMode : std::uint8_t { Sequential = 0, Random = 1, Shuffle = 2 };
class PlaylistEditor {
 public:
  PlaylistEditor(ProjectDocument& doc, std::string guid);
  bool valid() const;
  bool SetMode(PlaylistMode m);
  bool SetLoopCount(std::uint32_t n);  // 0 = infinite
  bool AddEntry(const std::string& segmentGuid, double weight = 1.0, std::uint32_t repeat = 1);
  bool SetEntry(std::uint32_t i, const std::string& segmentGuid, double weight, std::uint32_t repeat);
  bool RemoveEntry(std::uint32_t i);
  bool MoveEntry(std::uint32_t from, std::uint32_t to);
  std::uint32_t EntryCount() const;

 private:
  ProjectDocument& doc_;
  std::string guid_;
};

enum class Timing : std::uint8_t { Immediate = 0, NextBeat = 1, NextBar = 2, BarsAfter = 3, Grid = 4, NextMarker = 5, SegmentEnd = 6 };
enum class StartPosition : std::uint8_t { Head = 0, SameBar = 1, Marker = 2, LastPosition = 3 };
enum class LateRule : std::uint8_t { NextGrid = 0, StartLate = 1, Cancel = 2 };

struct TransitionRule {
  std::string from = "*";
  std::string to = "*";
  Timing timing = Timing::NextBar;
  std::uint32_t timingCount = 0;
  double fadeOutBeats = 0.0;
  double fadeInBeats = 0.0;
  std::string bridge;  // segment GUID, "" = none
  StartPosition start = StartPosition::Head;
  std::string startMarker;
  bool interruptAfterBridge = false;
  LateRule late = LateRule::NextGrid;
  bool prepare = false;
};

struct StingerDesc {
  std::string name;
  std::string event;  // GUID
  Timing timing = Timing::NextBeat;
  std::uint32_t timingCount = 0;
  bool stack = false;  // overlap: replace (false) / stack
};

struct MusicSettings {
  std::uint32_t sampleRate = 48000;  // Config.sampleRate
  std::uint32_t blockFrames = 256;   // Config.blockSize
  double streamPrimeMs = 30.0;       // a stream's open + seek + first decode on the target device
};

struct LookaheadEntry {
  std::string kind;        // "state", "bridge", "stinger"
  std::string name;        // state / stinger name, bridge segment name
  std::string assetGuid;   // the material that is primed ("" if none)
  std::string reason;      // "memory", "prepare", "preload", "entryPreload", "stream", "none"
  double primeMs = 0.0;
  double lookaheadMs = 0.0;  // one block + primeMs
};

struct TransitionPreview {
  bool valid = false;        // the states exist
  int rule = -1;             // row used, -1 = the built-in default
  bool cancelled = false;    // late rule "cancel"
  bool lateApplied = false;  // the firing was moved by the late rule
  double requestTick = 0.0;
  double fireTick = 0.0;     // on the source segment's map
  double requestSeconds = 0.0;
  double fireSeconds = 0.0;
  MusicalPosition fireAt;
  double lookaheadMs = 0.0;
  double fadeOutSeconds = 0.0;
  double fadeInSeconds = 0.0;
  std::string bridge;        // segment GUID
  double bridgeSeconds = 0.0;
  std::string targetSegment;  // GUID (first entry of the target's playlist)
  double targetStartSeconds = 0.0;  // when the target is heard (fire + bridge)
  double targetStartTick = 0.0;     // position in the target segment
};

class MusicEditor {
 public:
  MusicEditor(ProjectDocument& doc, std::string eventGuid);
  bool valid() const;
  // --- states ----------------------------------------------------------------------------
  std::vector<std::string> States() const;
  bool AddState(const std::string& name, const std::string& playlistGuid);
  bool RemoveState(const std::string& name);  // refused while a transition or the initial state names it
  bool RenameState(const std::string& from, const std::string& to);
  bool SetStatePlaylist(const std::string& name, const std::string& playlistGuid);
  bool SetInitialState(const std::string& name);
  // --- transitions -----------------------------------------------------------------------
  std::vector<TransitionRule> Rules() const;
  bool AddRule(const TransitionRule& r, std::uint32_t at);
  bool SetRule(std::uint32_t i, const TransitionRule& r);
  bool RemoveRule(std::uint32_t i);
  bool MoveRule(std::uint32_t from, std::uint32_t to);
  // Refused (with the reason) while some row is unreachable (D-179e).
  bool AutoSort(std::string* why = nullptr);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> Unreachable() const;  // (row, earlier row covering it)
  int Match(const std::string& from, const std::string& to) const;
  // --- stingers --------------------------------------------------------------------------
  bool AddStinger(const StingerDesc& s);
  bool SetStinger(std::uint32_t i, const StingerDesc& s);
  bool RemoveStinger(std::uint32_t i);
  // --- derived ---------------------------------------------------------------------------
  std::vector<LookaheadEntry> MinimumLookahead(const MusicSettings& s) const;
  TransitionPreview Preview(const std::string& from, const std::string& to, double requestSeconds,
                            const MusicSettings& s) const;

 private:
  const json::Value* Music() const;
  bool ValidRule(const TransitionRule& r) const;
  json::Value RuleJson(const TransitionRule& r, const json::Value* keep) const;
  std::string FirstSegmentOf(const std::string& state) const;
  ProjectDocument& doc_;
  std::string guid_;
};

}  // namespace snd::tool

#endif  // SND_TOOL_MUSIC_EDITOR_HPP
