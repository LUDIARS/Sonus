// Helpers shared by both halves of the music system.
#include <cmath>

#include "music_state.hpp"
#include "snd/core/core.hpp"

namespace snd::music::detail {

void PushNote(State& s, notify::EventType type, std::uint32_t a, std::uint64_t object, ClockSample clock,
              std::uint32_t b, std::uint16_t detail) noexcept {
  if (s.noteCount >= kNotifyCapacity) {
    ++s.dropped;
    return;
  }
  notify::EventRecord& r = s.notes[s.noteCount++];
  r = notify::EventRecord{};
  r.type = static_cast<std::uint16_t>(type);
  r.detail = detail;
  r.a = a;
  r.object = object;
  r.clock = clock;
  r.b = b;
}

MusicPb* PbOf(State& s, SlotIndex playback) noexcept {
  if (playback >= s.limits.maxPlaybacks) return nullptr;
  const std::uint16_t i = s.pbOfSlot[playback];
  return i == kNoPb ? nullptr : &s.pbs[i];
}

const MusicPb* PbOf(const State& s, SlotIndex playback) noexcept {
  if (playback >= s.limits.maxPlaybacks) return nullptr;
  const std::uint16_t i = s.pbOfSlot[playback];
  return i == kNoPb ? nullptr : &s.pbs[i];
}

Timeline* TimelineOf(State& s, SlotIndex transport) noexcept {
  if (transport >= s.limits.maxTransports) return nullptr;
  Timeline& tl = s.timelines[transport];
  return tl.generation != 0u ? &tl : nullptr;
}

ClockSample RoundClock(double c) noexcept {
  if (!(c > 0.0)) return 0;
  return static_cast<ClockSample>(std::floor(c + 0.5));
}

ClipRef AssetClip(std::uint16_t bank, std::uint32_t asset) noexcept {
  return ClipRef{bank, core::kClipRefAssetTrack, asset};
}

std::uint32_t PrimeOf(const State& s, std::uint16_t bank, std::uint32_t asset, std::uint64_t frame) noexcept {
  if (s.services.primeFrames == nullptr) return 0;
  return s.services.primeFrames(s.services.primeCtx, AssetClip(bank, asset), frame);
}

double StatePickupSamples(const State& s, const BankTables& tb, std::uint32_t stateRow) noexcept {
  if (!tb.msta.Has(stateRow)) return 0.0;
  const std::uint32_t seg = FirstSegment(tb, tb.msta.U32(bf::col::MSTA_playlist, stateRow));
  SegmentInfo si;
  if (!DescribeSegment(tb, seg, s.rate, &si)) return 0.0;
  return static_cast<double>(si.entry - si.pickup) * si.ratio;
}

MusicPb* PbOfTransport(State& s, SlotIndex transport) noexcept {
  for (std::uint32_t i = 0; i < s.pbCount; ++i) {
    if (s.pbs[i].used != 0u && s.pbs[i].transport == transport) return &s.pbs[i];
  }
  return nullptr;
}

std::uint32_t EventPrimeFrames(const State& s, const BankTables& tb, std::uint32_t eventRow) noexcept {
  if (!tb.evnt.Has(eventRow)) return 0;
  std::uint32_t best = 0;
  if ((tb.evnt.U32(bf::col::EVNT_flags, eventRow) & bf::kEventMusic) != 0u) {
    const std::uint32_t st = InitialStateRow(tb, eventRow);
    if (tb.msta.Has(st)) {
      const std::uint32_t seg = FirstSegment(tb, tb.msta.U32(bf::col::MSTA_playlist, st));
      SegmentInfo si;
      if (DescribeSegment(tb, seg, s.rate, &si)) best = PrimeOf(s, tb.bank, si.asset, si.pickup);
    }
    return best;
  }
  const std::uint32_t tf = tb.evnt.U32(bf::col::EVNT_trackFirst, eventRow);
  const std::uint32_t tc = tb.evnt.U32(bf::col::EVNT_trackCount, eventRow);
  for (std::uint32_t k = 0; k < tc && k < 64u; ++k) {
    const std::uint32_t tr = tf + k;
    if (!tb.trak.Has(tr)) break;
    const std::uint32_t cf = tb.trak.U32(bf::col::TRAK_clipFirst, tr);
    const std::uint32_t cc = tb.trak.U32(bf::col::TRAK_clipCount, tr);
    for (std::uint32_t j = 0; j < cc && j < 256u; ++j) {
      const std::uint32_t c = cf + j;
      if (!tb.clip.Has(c)) break;
      if (tb.clip.U32(bf::col::CLIP_kind, c) != static_cast<std::uint32_t>(bf::ClipKind::Asset)) continue;
      const std::uint32_t p = PrimeOf(s, tb.bank, tb.clip.U32(bf::col::CLIP_target, c), 0u);
      if (p > best) best = p;
    }
  }
  return best;
}

double EventPickupSamples(const State& s, const BankTables& tb, std::uint32_t eventRow) noexcept {
  if (!tb.evnt.Has(eventRow)) return 0.0;
  if ((tb.evnt.U32(bf::col::EVNT_flags, eventRow) & bf::kEventMusic) != 0u) {
    return StatePickupSamples(s, tb, InitialStateRow(tb, eventRow));
  }
  const std::uint32_t tr = tb.evnt.U32(bf::col::EVNT_trackFirst, eventRow);
  if (tb.evnt.U32(bf::col::EVNT_trackCount, eventRow) == 0u || !tb.trak.Has(tr)) return 0.0;
  const std::uint32_t c = tb.trak.U32(bf::col::TRAK_clipFirst, tr);
  if (tb.trak.U32(bf::col::TRAK_clipCount, tr) == 0u || !tb.clip.Has(c) ||
      tb.clip.U32(bf::col::CLIP_kind, c) != static_cast<std::uint32_t>(bf::ClipKind::Asset)) {
    return 0.0;
  }
  const std::uint32_t asset = tb.clip.U32(bf::col::CLIP_target, c);
  for (std::uint32_t seg = 0; seg < tb.mseg.Rows(); ++seg) {
    if (tb.mseg.U32(bf::col::MSEG_asset, seg) != asset) continue;
    SegmentInfo si;
    if (DescribeSegment(tb, seg, s.rate, &si)) return static_cast<double>(si.entry - si.pickup) * si.ratio;
  }
  return 0.0;
}

std::int32_t InstanceAt(const MusicPb& pb, double tick) noexcept {
  std::int32_t best = -1;
  for (std::uint32_t i = 0; i < kMaxInstances; ++i) {
    const Instance& in = pb.inst[i];
    if (in.role == Role::Free || in.stopPending != 0u) continue;
    if (static_cast<double>(in.startTick) <= tick + 1e-6 && tick < static_cast<double>(in.EndTick())) {
      if (best < 0 || in.startTick > pb.inst[best].startTick) best = static_cast<std::int32_t>(i);
    }
  }
  return best;
}

bool SegmentEndAfter(const MusicPb& pb, double tick, double* out) noexcept {
  std::int32_t i = InstanceAt(pb, tick);
  if (i < 0) {  // before the first segment (pickup): the next one to begin
    for (std::uint32_t k = 0; k < kMaxInstances; ++k) {
      const Instance& in = pb.inst[k];
      if (in.role == Role::Free || in.stopPending != 0u || static_cast<double>(in.startTick) <= tick) continue;
      if (i < 0 || in.startTick < pb.inst[i].startTick) i = static_cast<std::int32_t>(k);
    }
  }
  if (i < 0) return false;
  *out = static_cast<double>(pb.inst[i].EndTick());
  return true;
}

bool NextMarkerAfter(const State& s, const BankTables& tb, const MusicPb& pb, double tick, Id marker,
                     double* out) noexcept {
  // Search the sounding instance, then the planned ones after it (in time order).
  double best = 0.0;
  bool found = false;
  for (std::uint32_t i = 0; i < kMaxInstances; ++i) {
    const Instance& in = pb.inst[i];
    if (in.role == Role::Free || in.stopPending != 0u) continue;
    if (static_cast<double>(in.EndTick()) <= tick) continue;
    SegmentInfo si;
    if (!DescribeSegment(tb, in.seg, s.rate, &si) || si.markerFirst == bf::kNone) continue;
    for (std::uint32_t k = 0; k < si.markerCount; ++k) {
      const std::uint32_t r = si.markerFirst + k;
      const Id id = tb.mmrk.U32(bf::col::MMRK_id, r);
      if (marker != 0u && id != marker) continue;
      if (marker == 0u && tb.mmrk.U32(bf::col::MMRK_kind, r) != kMarkerUser) continue;
      const std::uint64_t f = tb.mmrk.U64(bf::col::MMRK_frame, r);
      const double samples = (static_cast<double>(f) - static_cast<double>(si.entry)) * si.ratio;
      const double mt = static_cast<double>(in.startTick) + SegmentTicksForSamples(tb, si, samples, s.rate);
      if (mt + 1e-6 < tick) continue;
      if (!found || mt < best) {
        best = mt;
        found = true;
      }
    }
  }
  if (found) *out = best;
  return found;
}

void LogTransition(State& s, const MusicPb& pb, Id from, Id to, double tick, double clock, std::uint8_t late,
                   std::uint32_t deferrals) noexcept {
  MusicTransitionRecord& r = s.log[s.logCount % kLogCapacity];
  r = MusicTransitionRecord{};
  r.playback = pb.playback;
  r.from = from;
  r.to = to;
  r.lateRule = late;
  r.tick = static_cast<MusicalTicks>(std::llround(tick));
  r.clock = RoundClock(clock);
  r.deferrals = deferrals;
  ++s.logCount;
}

}  // namespace snd::music::detail
