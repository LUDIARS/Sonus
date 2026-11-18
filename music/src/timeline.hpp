// The time axis of one transport (design 8.1): tempo map, time signatures and
// the mapping between the common clock and musical ticks (960 per quarter
// note, D-20). Music-private state behind the Transport columns, which
// UpdateTransport writes from it once per block.
//
// Representation: an anchor (clock, tick) plus tempo points sorted by tick.
// The point in effect at tick t is the last point with point.tick <= t (the
// first point for earlier ticks). Each point carries the bar position at its
// tick (`bar`, fractional when the signature changed inside a bar), so bar and
// beat numbers stay continuous across signature changes.
//
// Conventions (same as core's minimal transport, sys_transport.cpp):
//   - bpm counts quarter notes (960 ticks) per minute;
//   - a beat is a note of the signature's beat unit: 3840 / beatUnit ticks
//     (6/8: an eighth = 480 ticks);
//   - grids (snd_when GRID) are note divisions counted from the bar line.
// While Paused / Stopped the position holds at the anchor; clock <-> tick
// conversions of other positions then fail (no time line to follow).
#ifndef SND_MUSIC_SRC_TIMELINE_HPP
#define SND_MUSIC_SRC_TIMELINE_HPP

#include <cstdint>

#include "snd/internal/common.hpp"
#include "snd/types.h"

namespace snd::music::detail {

inline constexpr double kTicksPerQuarter = SND_TICKS_PER_BEAT;
inline constexpr std::uint32_t kMaxTempoPoints = 64;

struct TempoPoint {
  MusicalTicks tick;
  double bpm;
  double bar;  // bar position at `tick`
  std::uint16_t beatsPerBar;
  std::uint16_t beatUnit;
  std::uint32_t reserved;
};

struct Timeline {
  std::uint32_t generation = 0;  // Transport row generation this state belongs to (0 = free)
  std::uint32_t state = SND_TRANSPORT_STOPPED;
  ClockSample anchorClock = 0;
  double anchorTick = 0.0;
  double overrideBpm = 0.0;  // SetTempo (8.1); 0 = follow the tempo points
  double pitch = 1.0;
  double sampleRate = 48000.0;
  ClockSample beatCursor = 0;  // beats before this clock were notified
  MusicalRange loop{0, 0};
  std::uint32_t count = 0;
  TempoPoint points[kMaxTempoPoints];

  void Reset(double rate, double bpm, std::uint16_t beatsPerBar, std::uint16_t beatUnit) noexcept;

  // --- tempo map ---
  std::uint32_t PointAt(double tick) const noexcept;
  double BpmOf(std::uint32_t point) const noexcept;
  double TicksPerSample(std::uint32_t point) const noexcept;
  double BeatTicks(std::uint32_t point) const noexcept;
  double BarTicks(std::uint32_t point) const noexcept;
  double BpmAt(double tick) const noexcept { return BpmOf(PointAt(tick)); }
  // Inserts a point (bar position derived from the previous point). Equal
  // tempo and signature to the point in effect: nothing is added. false when full.
  bool AddPoint(MusicalTicks tick, double bpm, std::uint16_t beatsPerBar, std::uint16_t beatUnit) noexcept;
  // Replaces the map from `tick` on with one point whose bar position is `bar` (a seek
  // rebuilds the current segment's map from its data, D-71).
  void SetPointAt(MusicalTicks tick, double bar, double bpm, std::uint16_t beatsPerBar, std::uint16_t beatUnit) noexcept;
  // Removes points with tick >= `tick` (the first point always stays).
  void TruncateFrom(MusicalTicks tick) noexcept;

  // --- clock <-> ticks (Playing only, except TickAtClock which holds otherwise) ---
  bool Playing() const noexcept { return state == SND_TRANSPORT_PLAYING; }
  double TickAtClock(double clock) const noexcept;
  bool ClockAtTick(double tick, double* clock) const noexcept;

  // --- transport changes at a clock (re-anchor first) ---
  void AnchorAt(ClockSample clock) noexcept;  // keeps the points
  void StartAt(ClockSample clock, double tick) noexcept;
  void PauseAt(ClockSample clock) noexcept;
  void ResumeAt(ClockSample clock) noexcept;
  void StopAt(ClockSample clock) noexcept;  // position back to 0 (8.1 Stopped)
  void SeekAt(ClockSample clock, double tick) noexcept;
  void SetTempoAt(ClockSample clock, double bpm) noexcept;
  // 1.2 (D-88): playback speed of the transport (tempo x pitch), already clamped by core.
  void SetPitchAt(ClockSample clock, double ratio) noexcept;
  // Sound-priority stall (8.4): everything after now happens `samples` later.
  void Delay(ClockSample samples) noexcept { anchorClock += samples; }
  // Block start: re-anchor (the points are kept, see AddPoint for a full table).
  void AdvanceTo(ClockSample clock) noexcept;

  // --- musical positions ---
  double BarPosition(double tick) const noexcept;  // fractional bar index
  snd_musical_time Musical(double tick) const noexcept;
  double TickOfMusical(const snd_musical_time& m) const noexcept;
  double TickAtBar(double bar) const noexcept;  // tick of the bar line `bar` on the whole map
  // Grid points at or after `from` (ties count: from itself when it lies on the grid).
  double NextBeat(double from) const noexcept;
  double NextBar(double from) const noexcept;
  double BarsAfter(double from, std::uint32_t n) const noexcept;  // bar line n bars after the current bar's start
  double NextGrid(double from, std::uint32_t division) const noexcept;
  // The grid point of `kind` strictly after `tick` (late rule "next grid").
  double NextOfKind(std::uint32_t kind, std::uint32_t division, double tick) const noexcept;
};

}  // namespace snd::music::detail

#endif  // SND_MUSIC_SRC_TIMELINE_HPP
