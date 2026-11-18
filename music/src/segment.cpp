#include "segment.hpp"

#include <cmath>

#include "timeline.hpp"

namespace snd::music::detail {

namespace {

constexpr std::uint32_t kMaxBarsWalk = 1u << 16;

struct SegTempo {
  double tick;
  double bpm;
  std::uint16_t bpb;
  std::uint16_t unit;
};

SegTempo PointOf(const BankTables& tb, const SegmentInfo& s, std::uint32_t k) noexcept {
  const std::uint32_t r = s.tempoFirst + k;
  SegTempo p{static_cast<double>(tb.mtmp.I64(bf::col::MTMP_tick, r)), tb.mtmp.F32(bf::col::MTMP_bpm, r),
             static_cast<std::uint16_t>(tb.mtmp.U32(bf::col::MTMP_beatsPerBar, r)),
             static_cast<std::uint16_t>(tb.mtmp.U32(bf::col::MTMP_beatUnit, r))};
  if (!(p.bpm > 0.0)) p.bpm = 120.0;
  if (p.bpb == 0u) p.bpb = 4;
  if (p.unit == 0u) p.unit = 4;
  return p;
}

std::uint32_t PointIndexAt(const BankTables& tb, const SegmentInfo& s, double tick) noexcept {
  std::uint32_t i = 0;
  while (i + 1u < s.tempoCount && PointOf(tb, s, i + 1u).tick <= tick + 1e-9) ++i;
  return i;
}

SegTempo TempoAt(const BankTables& tb, const SegmentInfo& s, double tick) noexcept {
  if (s.tempoCount == 0u) return SegTempo{0.0, 120.0, 4, 4};
  return PointOf(tb, s, PointIndexAt(tb, s, tick));
}

double BarTicksOf(const SegTempo& p) noexcept { return 4.0 * kTicksPerQuarter / p.unit * p.bpb; }

std::uint32_t MarkerOfKind(const BankTables& tb, const SegmentInfo& s, std::uint32_t kind) noexcept {
  if (s.markerFirst == bf::kNone) return bf::kNone;
  for (std::uint32_t k = 0; k < s.markerCount; ++k) {
    const std::uint32_t r = s.markerFirst + k;
    if (!tb.mmrk.Has(r)) break;
    if (tb.mmrk.U32(bf::col::MMRK_kind, r) == kind) return r;
  }
  return bf::kNone;
}

}  // namespace

bool DescribeSegment(const BankTables& tb, std::uint32_t seg, double outRate, SegmentInfo* out) noexcept {
  SegmentInfo s;
  if (!tb.mseg.Has(seg)) {
    *out = s;
    return false;
  }
  s.row = seg;
  s.asset = tb.mseg.U32(bf::col::MSEG_asset, seg);
  if (!tb.asst.Has(s.asset)) {
    *out = s;
    return false;
  }
  s.channels = tb.asst.U32(bf::col::ASST_channels, s.asset);
  s.assetRate = tb.asst.U32(bf::col::ASST_sampleRate, s.asset);
  if (s.assetRate == 0u) s.assetRate = 48000;
  s.ratio = outRate / static_cast<double>(s.assetRate);
  s.totalFrames = tb.asst.U64(bf::col::ASST_totalFrames, s.asset);
  s.soundPriority = (tb.asst.U32(bf::col::ASST_flags, s.asset) & bf::kAssetStallSoundPriority) != 0u ? 1u : 0u;
  s.tempoFirst = tb.mseg.U32(bf::col::MSEG_tempoFirst, seg);
  s.tempoCount = tb.mseg.U32(bf::col::MSEG_tempoCount, seg);
  if (s.tempoFirst >= tb.mtmp.Rows() || s.tempoCount > tb.mtmp.Rows() - s.tempoFirst) s.tempoCount = 0;
  s.markerFirst = tb.mseg.U32(bf::col::MSEG_markerFirst, seg);
  s.markerCount = tb.mseg.U32(bf::col::MSEG_markerCount, seg);
  if (s.markerFirst == bf::kNone || s.markerFirst >= tb.mmrk.Rows() || s.markerCount > tb.mmrk.Rows() - s.markerFirst) {
    s.markerFirst = bf::kNone;
    s.markerCount = 0;
  }
  // Entry (bar 0): the column, else the entry marker.
  s.entry = tb.mseg.U64(bf::col::MSEG_entryFrame, seg);
  if (s.entry == 0u) {
    const std::uint32_t m = MarkerOfKind(tb, s, kMarkerEntry);
    if (m != bf::kNone) s.entry = tb.mmrk.U64(bf::col::MMRK_frame, m);
  }
  const std::uint32_t pk = MarkerOfKind(tb, s, kMarkerPickup);
  s.pickup = pk != bf::kNone ? tb.mmrk.U64(bf::col::MMRK_frame, pk) : 0u;
  if (s.pickup > s.entry) s.pickup = s.entry;
  // Length in bars (8.7: inferred from the exit when the bank says 0).
  s.bars = tb.mseg.U32(bf::col::MSEG_bars, seg);
  std::uint64_t exitCol = tb.mseg.U64(bf::col::MSEG_exitFrame, seg);
  if (exitCol == 0u) {
    const std::uint32_t m = MarkerOfKind(tb, s, kMarkerExit);
    if (m != bf::kNone) exitCol = tb.mmrk.U64(bf::col::MMRK_frame, m);
  }
  if (s.bars == 0u) {
    const std::uint64_t endFrame = exitCol > s.entry ? exitCol : s.totalFrames;
    const SegTempo p = TempoAt(tb, s, 0.0);
    const double barSamples = BarTicksOf(p) / (p.bpm / 60.0 * kTicksPerQuarter) * outRate;
    const double samples = static_cast<double>(endFrame > s.entry ? endFrame - s.entry : 0u) * s.ratio;
    const double b = std::floor(samples / barSamples + 0.5);
    s.bars = b < 1.0 ? 1u : static_cast<std::uint32_t>(b);
  }
  if (s.bars > kMaxBarsWalk) s.bars = kMaxBarsWalk;
  s.lenTicks = static_cast<MusicalTicks>(std::llround(SegmentBarTick(tb, s, s.bars)));
  if (s.lenTicks <= 0) s.lenTicks = static_cast<MusicalTicks>(4.0 * kTicksPerQuarter);
  s.exit = exitCol != 0u ? exitCol
                         : s.entry + static_cast<std::uint64_t>(
                                         std::llround(SegmentSamples(tb, s, static_cast<double>(s.lenTicks), outRate) / s.ratio));
  s.valid = true;
  *out = s;
  return true;
}

void SegmentTempoAt(const BankTables& tb, const SegmentInfo& s, double tick, double* bpm, std::uint16_t* beatsPerBar,
                    std::uint16_t* beatUnit) noexcept {
  const SegTempo p = TempoAt(tb, s, tick);
  *bpm = p.bpm;
  *beatsPerBar = p.bpb;
  *beatUnit = p.unit;
}

double SegmentSamples(const BankTables& tb, const SegmentInfo& s, double tick, double outRate) noexcept {
  if (tick <= 0.0) {
    const SegTempo p = TempoAt(tb, s, 0.0);
    return tick / (p.bpm / 60.0 * kTicksPerQuarter) * outRate;
  }
  double samples = 0.0;
  double t = 0.0;
  const std::uint32_t n = s.tempoCount == 0u ? 1u : s.tempoCount;
  for (std::uint32_t i = 0; i < n && t < tick; ++i) {
    const SegTempo p = s.tempoCount == 0u ? SegTempo{0.0, 120.0, 4, 4} : PointOf(tb, s, i);
    const double next = (i + 1u < n) ? PointOf(tb, s, i + 1u).tick : tick;
    const double to = next < tick ? next : tick;
    if (to > t) samples += (to - t) / (p.bpm / 60.0 * kTicksPerQuarter) * outRate;
    if (to > t) t = to;
  }
  return samples;
}

double SegmentTicksForSamples(const BankTables& tb, const SegmentInfo& s, double samples, double outRate) noexcept {
  if (samples <= 0.0) {
    const SegTempo p = TempoAt(tb, s, 0.0);
    return samples / outRate * (p.bpm / 60.0 * kTicksPerQuarter);
  }
  double t = 0.0;
  const std::uint32_t n = s.tempoCount == 0u ? 1u : s.tempoCount;
  for (std::uint32_t i = 0; i < n; ++i) {
    const SegTempo p = s.tempoCount == 0u ? SegTempo{0.0, 120.0, 4, 4} : PointOf(tb, s, i);
    const double tps = p.bpm / 60.0 * kTicksPerQuarter / outRate;
    if (i + 1u < n) {
      const double next = PointOf(tb, s, i + 1u).tick;
      const double need = (next - t) / tps;
      if (samples > need) {
        samples -= need;
        t = next;
        continue;
      }
    }
    return t + samples * tps;
  }
  return t;
}

double SegmentBarTick(const BankTables& tb, const SegmentInfo& s, std::uint32_t bar) noexcept {
  double t = 0.0;
  for (std::uint32_t b = 0; b < bar && b < kMaxBarsWalk; ++b) t += BarTicksOf(TempoAt(tb, s, t));
  return t;
}

void SegmentBarOf(const BankTables& tb, const SegmentInfo& s, double tick, std::uint32_t* bar, double* rest) noexcept {
  double t = 0.0;
  std::uint32_t b = 0;
  for (; b < kMaxBarsWalk; ++b) {
    const double len = BarTicksOf(TempoAt(tb, s, t));
    if (tick < t + len - 1e-6) break;
    t += len;
  }
  *bar = b;
  *rest = tick - t < 0.0 ? 0.0 : tick - t;
}

std::uint32_t FindSegmentMarker(const BankTables& tb, const SegmentInfo& s, Id id) noexcept {
  if (s.markerFirst == bf::kNone) return bf::kNone;
  for (std::uint32_t k = 0; k < s.markerCount; ++k) {
    const std::uint32_t r = s.markerFirst + k;
    if (tb.mmrk.U32(bf::col::MMRK_id, r) == id) return r;
  }
  return bf::kNone;
}

}  // namespace snd::music::detail
