// Segment geometry (design 8.3): where bar 0 is in the material (entry), where
// the pickup starts, the musical length from the segment's own tempo map
// (MTMP rows, segment-local ticks) and conversions between segment ticks and
// output samples. Inference for missing data (8.7): no tempo rows = 120 BPM
// 4/4; no entry column = the entry marker; no exit = the exit marker or the
// end of the last bar; pickup = the pickup marker (else the material start).
#ifndef SND_MUSIC_SRC_SEGMENT_HPP
#define SND_MUSIC_SRC_SEGMENT_HPP

#include <cstdint>

#include "bank_tables.hpp"
#include "snd/internal/common.hpp"

namespace snd::music::detail {

enum MarkerKind : std::uint32_t { kMarkerUser = 0, kMarkerEntry = 1, kMarkerExit = 2, kMarkerPickup = 3 };

struct SegmentInfo {
  std::uint32_t row = bf::kNone;    // MSEG row
  std::uint32_t asset = bf::kNone;  // ASST row
  std::uint32_t channels = 1;
  std::uint32_t assetRate = 48000;
  double ratio = 1.0;               // output samples per material frame
  std::uint64_t entry = 0;          // material frame of bar 0 beat 0
  std::uint64_t exit = 0;           // material frame of the musical end
  std::uint64_t pickup = 0;         // first material frame played (<= entry)
  std::uint64_t totalFrames = 0;
  std::uint32_t tempoFirst = 0, tempoCount = 0;
  std::uint32_t markerFirst = bf::kNone, markerCount = 0;
  std::uint32_t bars = 1;
  MusicalTicks lenTicks = 0;
  std::uint8_t soundPriority = 0;   // ASST stall policy (8.4)
  bool valid = false;
};

bool DescribeSegment(const BankTables& tb, std::uint32_t seg, double outRate, SegmentInfo* out) noexcept;

// Tempo of the segment at segment-local `tick`.
void SegmentTempoAt(const BankTables& tb, const SegmentInfo& s, double tick, double* bpm, std::uint16_t* beatsPerBar,
                    std::uint16_t* beatUnit) noexcept;
// Output samples from bar 0 to segment-local `tick` (>= 0), and the inverse.
double SegmentSamples(const BankTables& tb, const SegmentInfo& s, double tick, double outRate) noexcept;
double SegmentTicksForSamples(const BankTables& tb, const SegmentInfo& s, double samples, double outRate) noexcept;
// Segment-local tick of bar line `bar`.
double SegmentBarTick(const BankTables& tb, const SegmentInfo& s, std::uint32_t bar) noexcept;
// Segment-local tick -> bar index and remainder ticks inside that bar.
void SegmentBarOf(const BankTables& tb, const SegmentInfo& s, double tick, std::uint32_t* bar, double* rest) noexcept;
// MMRK row of marker `id` (kind any) in the segment, or kNone.
std::uint32_t FindSegmentMarker(const BankTables& tb, const SegmentInfo& s, Id id) noexcept;

}  // namespace snd::music::detail

#endif  // SND_MUSIC_SRC_SEGMENT_HPP
