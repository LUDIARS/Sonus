// States, transitions and segment placement (design 8.3, 8.5, 8.6).
//
// SetMusicState goes through the state machine of 8.6:
//   Idle          -> a matching rule is looked up; Pending at the rule timing
//                    (or at the fire clock of a When that overrides it).
//   Pending       -> the last request wins (rule looked up again); a request for
//                    the current state cancels the pending transition.
//   Transitioning -> "allowed": a new transition starts from what is audible
//                    now; "after bridge": one request is queued (last wins) and
//                    handled when the transition is complete.
// Executing a transition at tick X: the outgoing segments fade out from X
// (fade in beats at the source tempo; at their musical end they are left to
// ring), planned but unstarted segments are dropped, the bridge segment (if
// any) starts at X, the target state's playlist after it, from the start
// position of the rule (head / same bar / marker / last position).
#include <cmath>

#include "music_state.hpp"

namespace snd::music::detail {

namespace {

double TimingTick(const State& s, const BankTables& tb, const MusicPb& pb, const Timeline& tl, const Rule& r,
                  double now) noexcept {
  double x = now;
  switch (r.timing) {
    case SND_WHEN_IMMEDIATE: return now;
    case SND_WHEN_NEXT_BEAT: return tl.NextBeat(now);
    case SND_WHEN_NEXT_BAR: return tl.NextBar(now);
    case SND_WHEN_BARS_AFTER: return tl.BarsAfter(now, r.timingCount);
    case SND_WHEN_GRID: return tl.NextGrid(now, r.timingCount);
    case SND_WHEN_NEXT_MARKER:
      if (NextMarkerAfter(s, tb, pb, now, r.timingCount, &x)) return x;
      if (SegmentEndAfter(pb, now, &x)) return x;
      return tl.NextBar(now);
    case SND_WHEN_SEGMENT_END:
      if (SegmentEndAfter(pb, now, &x)) return x;
      return tl.NextBar(now);
    default: return tl.NextBar(now);
  }
}

// The grid point after `x` of the rule's timing (late rule "next grid").
bool NextTiming(const State& s, const BankTables& tb, const MusicPb& pb, const Timeline& tl, const Rule& r, double x,
                double* out) noexcept {
  switch (r.timing) {
    case SND_WHEN_IMMEDIATE: return false;
    case SND_WHEN_NEXT_MARKER:
      if (NextMarkerAfter(s, tb, pb, x + 0.5, r.timingCount, out)) return true;
      return SegmentEndAfter(pb, x + 0.5, out);
    case SND_WHEN_SEGMENT_END: return SegmentEndAfter(pb, x + 0.5, out);
    default:
      *out = tl.NextOfKind(r.timing, r.timingCount, x);
      return *out > x;
  }
}

std::uint32_t FirstMaterialPrime(const State& s, const BankTables& tb, std::uint32_t stateRow, const Rule& r) noexcept {
  SegmentInfo si;
  const std::uint32_t seg =
      r.bridge != bf::kNone ? r.bridge : FirstSegment(tb, tb.msta.Has(stateRow) ? tb.msta.U32(bf::col::MSTA_playlist, stateRow) : bf::kNone);
  if (!DescribeSegment(tb, seg, s.rate, &si)) return 0;
  return PrimeOf(s, tb.bank, si.asset, si.pickup);
}

double FirstBpm(const BankTables& tb, const State& s, const Timeline& tl, std::uint32_t seg, double localTick) noexcept {
  if (tl.overrideBpm > 0.0) return tl.overrideBpm;
  SegmentInfo si;
  if (!DescribeSegment(tb, seg, s.rate, &si)) return 120.0;
  double bpm = 120.0;
  std::uint16_t a = 4, b = 4;
  SegmentTempoAt(tb, si, localTick, &bpm, &a, &b);
  return bpm;
}

void SaveLastPosition(MusicPb& pb, Id state, std::uint32_t seg, MusicalTicks offset) noexcept {
  if (state == 0u) return;
  LastPosition* slot = nullptr;
  for (LastPosition& lp : pb.last) {
    if (lp.state == state) slot = &lp;
  }
  if (slot == nullptr) {
    slot = &pb.last[pb.lastNext % kMaxLastPositions];
    ++pb.lastNext;
  }
  slot->state = state;
  slot->seg = seg;
  slot->offset = offset;
  slot->cursor = pb.cursor;
}

const LastPosition* FindLastPosition(const MusicPb& pb, Id state) noexcept {
  for (const LastPosition& lp : pb.last) {
    if (lp.state == state && state != 0u) return &lp;
  }
  return nullptr;
}

std::int32_t FreeInstance(const MusicPb& pb) noexcept {
  for (std::uint32_t i = 0; i < kMaxInstances; ++i) {
    if (pb.inst[i].role == Role::Free) return static_cast<std::int32_t>(i);
  }
  return -1;
}

// The main instance of the current state that the position `tick` is in (or the latest before it).
std::int32_t CurrentMain(const MusicPb& pb, double tick) noexcept {
  std::int32_t best = -1;
  for (std::uint32_t i = 0; i < kMaxInstances; ++i) {
    const Instance& in = pb.inst[i];
    if (in.role != Role::Main || in.stopPending != 0u) continue;
    if (static_cast<double>(in.startTick) > tick + 1e-6) continue;
    if (best < 0 || in.startTick > pb.inst[best].startTick) best = static_cast<std::int32_t>(i);
  }
  return best;
}

}  // namespace

std::int32_t PlanInstance(State& s, const BankTables& tb, MusicPb& pb, Timeline& tl, Role role, std::uint32_t seg,
                          std::uint32_t stateRow, MusicalTicks startTick, MusicalTicks anchorTick, float fadeIn) noexcept {
  const std::int32_t i = FreeInstance(pb);
  SegmentInfo si;
  if (i < 0 || !DescribeSegment(tb, seg, s.rate, &si)) return -1;
  Instance& in = pb.inst[i];
  in = Instance{};
  in.role = role;
  in.phase = InstPhase::Planned;
  in.seg = seg;
  in.segId = tb.mseg.U32(bf::col::MSEG_id, seg);
  in.stateRow = stateRow;
  in.serial = ++pb.serial;
  in.startTick = startTick;
  in.lenTicks = si.lenTicks;
  in.anchorTick = anchorTick > startTick ? anchorTick : startTick;
  in.fadeIn = fadeIn;
  in.ratio = si.ratio;
  for (std::uint8_t& v : in.voices) v = 0xFFu;
  // Tempo points: the segment's tempo from the anchor on (8.1: the transport follows the music).
  const double local0 = static_cast<double>(in.anchorTick - startTick);
  double bpm = 120.0;
  std::uint16_t bpb = 4, unit = 4;
  SegmentTempoAt(tb, si, local0, &bpm, &bpb, &unit);
  tl.AddPoint(in.anchorTick, bpm, bpb, unit);
  for (std::uint32_t k = 0; k < si.tempoCount; ++k) {
    const std::uint32_t r = si.tempoFirst + k;
    const MusicalTicks pt = tb.mtmp.I64(bf::col::MTMP_tick, r);
    if (static_cast<double>(pt) <= local0 || pt >= si.lenTicks) continue;
    tl.AddPoint(startTick + pt, tb.mtmp.F32(bf::col::MTMP_bpm, r),
                static_cast<std::uint16_t>(tb.mtmp.U32(bf::col::MTMP_beatsPerBar, r)),
                static_cast<std::uint16_t>(tb.mtmp.U32(bf::col::MTMP_beatUnit, r)));
  }
  return i;
}

void StartMusic(State& s, const BankTables& tb, MusicPb& pb, ClockSample clock) noexcept {
  pb.started = 1;
  Timeline* tl = TimelineOf(s, pb.transport);
  std::uint32_t row = pb.desired != 0u ? FindStateRow(tb, pb.eventRow, pb.desired) : bf::kNone;
  if (row == bf::kNone) row = InitialStateRow(tb, pb.eventRow);
  pb.desired = 0;
  if (tl == nullptr || !tb.msta.Has(row)) return;
  pb.stateRow = row;
  pb.stateId = tb.msta.U32(bf::col::MSTA_id, row);
  CursorReset(pb.cursor, tb.msta.U32(bf::col::MSTA_playlist, row));
  const std::uint32_t seg = CursorNext(pb.cursor, tb, pb.rng);
  SegmentInfo si;
  if (!DescribeSegment(tb, seg, s.rate, &si)) return;
  // Bar 0 after the pickup: the material starts at `clock` (PICKUP_ALIGN fired earlier, D-21).
  double bpm = 120.0;
  std::uint16_t bpb = 4, unit = 4;
  SegmentTempoAt(tb, si, 0.0, &bpm, &bpb, &unit);
  tl->Reset(s.rate, bpm, bpb, unit);
  const ClockSample barZero = clock + RoundClock(static_cast<double>(si.entry - si.pickup) * si.ratio);
  tl->StartAt(barZero, 0.0);
  pb.head = PlanInstance(s, tb, pb, *tl, Role::Main, seg, row, 0, 0, 0.0f);
}

void HandleSetState(State& s, const BankTables& tb, MusicPb& pb, Id target, ClockSample clock,
                    bool whenOverride) noexcept {
  if (pb.stopping != 0u) return;
  if (pb.started == 0u) {
    pb.desired = target;  // before the music started: the state it starts in
    return;
  }
  Timeline* tl = TimelineOf(s, pb.transport);
  if (tl == nullptr) return;
  const std::uint32_t row = FindStateRow(tb, pb.eventRow, target);
  const std::uint64_t handle = HandleRef{pb.playback, pb.generation}.Value();
  if (row == bf::kNone) {
    PushNote(s, notify::EventType::Warning, static_cast<std::uint32_t>(SND_ERR_NOT_FOUND), handle, clock);
    return;
  }
  switch (pb.phase) {
    case Phase::Idle:
      if (target == pb.stateId) return;
      break;
    case Phase::Pending:
      if (target == pb.stateId) {
        pb.phase = Phase::Idle;  // back to where we are: nothing to do
        return;
      }
      break;
    case Phase::Transitioning:
      if (pb.transInterrupt == Interrupt::AfterBridge) {
        pb.hasQueued = 1;
        pb.queuedTarget = target;
        return;
      }
      if (target == pb.stateId) return;
      break;
  }
  const Rule rule = FindRule(tb, pb.eventRow, pb.stateId, target);
  if (!tl->Playing()) return;
  double x = tl->TickAtClock(static_cast<double>(clock));
  std::uint8_t late = kNoLate;
  std::uint32_t deferrals = 0;
  const std::uint32_t prime = FirstMaterialPrime(s, tb, row, rule);
  if (!whenOverride) {
    x = TimingTick(s, tb, pb, *tl, rule, x);
    // Lead time (8.2): the target material must be primed by then.
    const double earliest = static_cast<double>(s.blockStart) + static_cast<double>(prime);
    for (std::uint32_t guard = 0; guard < 1024u; ++guard) {
      double c = 0.0;
      if (!tl->ClockAtTick(x, &c) || c >= earliest - 0.5) break;
      if (rule.lateRule == static_cast<std::uint8_t>(ReservationLateRule::StartLate)) {
        late = rule.lateRule;
        break;
      }
      if (rule.lateRule == static_cast<std::uint8_t>(ReservationLateRule::Cancel)) {
        ++pb.cancels;
        PushNote(s, notify::EventType::ReservationLate, rule.lateRule, handle, s.blockStart, deferrals);
        return;
      }
      double next = 0.0;
      if (!NextTiming(s, tb, pb, *tl, rule, x, &next)) {
        late = static_cast<std::uint8_t>(ReservationLateRule::StartLate);
        break;
      }
      x = next;
      ++deferrals;
      late = static_cast<std::uint8_t>(ReservationLateRule::NextGrid);
    }
    if (late != kNoLate) PushNote(s, notify::EventType::ReservationLate, late, handle, s.blockStart, deferrals);
    pb.deferrals += deferrals;
  }
  pb.phase = Phase::Pending;
  pb.pendRule = rule;
  pb.pendTarget = target;
  pb.pendTargetRow = row;
  pb.pendTick = x;
  pb.pendPrime = prime;
  pb.pendLate = late;
  pb.pendDeferrals = deferrals;
}

void ExecuteTransition(State& s, const BankTables& tb, MusicPb& pb, Timeline& tl) noexcept {
  const double x = pb.pendTick;
  double cx = 0.0;
  if (!tl.ClockAtTick(x, &cx)) return;
  const Rule rule = pb.pendRule;
  const Id from = pb.stateId;
  const double fadeOut = static_cast<double>(rule.fadeOutBeats) * 60.0 / tl.BpmAt(x);
  // Where we are in the outgoing music (same bar, last position).
  const std::int32_t cur = CurrentMain(pb, x);
  MusicalTicks curOffset = 0;
  std::uint32_t curSeg = bf::kNone;
  if (cur >= 0) {
    curSeg = pb.inst[cur].seg;
    curOffset = static_cast<MusicalTicks>(std::llround(x)) - pb.inst[cur].startTick;
    if (curOffset < 0) curOffset = 0;
    if (curOffset >= pb.inst[cur].lenTicks) curOffset = 0;
    if (pb.inst[cur].stateRow == pb.stateRow) SaveLastPosition(pb, from, curSeg, curOffset);
  }
  // Outgoing.
  const ClockSample stopAt = RoundClock(cx);
  for (Instance& in : pb.inst) {
    if (in.role == Role::Free) continue;
    in.successor = 1;
    if (in.phase == InstPhase::Planned) {
      in.role = Role::Free;  // never started: dropped
      continue;
    }
    if (x >= static_cast<double>(in.EndTick()) - 0.5 && in.stopPending == 0u) continue;  // musically over: the tail rings
    if (in.stopPending != 0u) {
      // Already fading (an interrupted transition): only an earlier end replaces it.
      const double oldEnd = static_cast<double>(in.stopClock) + static_cast<double>(in.stopFade) * s.rate;
      const double newEnd = static_cast<double>(stopAt) + (fadeOut > 0.0 ? fadeOut : 0.005) * s.rate;
      if (newEnd >= oldEnd) continue;
    }
    in.stopPending = 1;
    if (static_cast<double>(in.startTick) >= x - 0.5 && in.voiceClock >= stopAt) {
      in.stopClock = in.voiceClock;  // issued ahead (prime) but not audible yet
      in.stopFade = 0.0f;
    } else {
      in.stopClock = stopAt;
      in.stopFade = static_cast<float>(fadeOut);
    }
    for (std::uint8_t v : in.voices) {
      if (v < kMaxLocalVoices) pb.voice[v].stopSent = 0;
    }
  }
  pb.head = -1;
  const MusicalTicks xt = static_cast<MusicalTicks>(std::llround(x));
  tl.TruncateFrom(xt);
  // Bridge.
  MusicalTicks destStart = xt;
  double fadeInSec = 0.0;
  double destEndFade = 0.0;
  if (rule.bridge != bf::kNone) {
    const double fi = static_cast<double>(rule.fadeInBeats) * 60.0 / FirstBpm(tb, s, tl, rule.bridge, 0.0);
    const std::int32_t b = PlanInstance(s, tb, pb, tl, Role::Bridge, rule.bridge, bf::kNone, xt, xt, static_cast<float>(fi));
    if (b >= 0) {
      pb.inst[b].successor = 1;
      destStart = xt + pb.inst[b].lenTicks;
      destEndFade = fi;
    }
  }
  // Destination state.
  pb.stateRow = pb.pendTargetRow;
  pb.stateId = pb.pendTarget;
  CursorReset(pb.cursor, tb.msta.U32(bf::col::MSTA_playlist, pb.stateRow));
  MusicalTicks startTick = destStart;
  std::uint32_t seg = bf::kNone;
  if (rule.start == StartPosition::LastPosition) {
    if (const LastPosition* lp = FindLastPosition(pb, pb.stateId); lp != nullptr && tb.mseg.Has(lp->seg)) {
      pb.cursor = lp->cursor;
      seg = lp->seg;
      startTick = destStart - lp->offset;
    }
  }
  if (seg == bf::kNone) seg = CursorNext(pb.cursor, tb, pb.rng);
  SegmentInfo dsi;
  if (DescribeSegment(tb, seg, s.rate, &dsi)) {
    if (rule.start == StartPosition::SameBar && cur >= 0) {
      SegmentInfo ssi;
      if (DescribeSegment(tb, curSeg, s.rate, &ssi)) {
        std::uint32_t bar = 0;
        double rest = 0.0;
        SegmentBarOf(tb, ssi, static_cast<double>(curOffset), &bar, &rest);
        const std::uint32_t destBar = bar % (dsi.bars == 0u ? 1u : dsi.bars);
        startTick = destStart - static_cast<MusicalTicks>(std::llround(SegmentBarTick(tb, dsi, destBar) + rest));
      }
    } else if (rule.start == StartPosition::Marker) {
      const std::uint32_t m = FindSegmentMarker(tb, dsi, rule.startMarker);
      if (m != bf::kNone) {
        const double samples =
            (static_cast<double>(tb.mmrk.U64(bf::col::MMRK_frame, m)) - static_cast<double>(dsi.entry)) * dsi.ratio;
        const double off = SegmentTicksForSamples(tb, dsi, samples, s.rate);
        if (off > 0.0) startTick = destStart - static_cast<MusicalTicks>(std::llround(off));
      }
    }
    if (destStart - startTick >= dsi.lenTicks) startTick = destStart;  // past its end: from the head
    if (rule.bridge == bf::kNone) {
      fadeInSec = static_cast<double>(rule.fadeInBeats) * 60.0 /
                  FirstBpm(tb, s, tl, seg, static_cast<double>(destStart - startTick));
      destEndFade = fadeInSec;
    }
    pb.head = PlanInstance(s, tb, pb, tl, Role::Main, seg, pb.stateRow, startTick, destStart, static_cast<float>(fadeInSec));
  }
  double cDest = cx;
  tl.ClockAtTick(static_cast<double>(destStart), &cDest);
  double end = cx + fadeOut * s.rate;
  if (cDest + destEndFade * s.rate > end) end = cDest + destEndFade * s.rate;
  pb.transEnd = RoundClock(end);
  pb.transInterrupt = rule.interrupt;
  pb.phase = Phase::Transitioning;
  ++pb.transitions;
  LogTransition(s, pb, from, pb.stateId, x, cx, pb.pendLate, pb.pendDeferrals);
}

void Relocate(State& s, const BankTables& tb, MusicPb& pb, Timeline& tl) noexcept {
  const double now = tl.TickAtClock(static_cast<double>(pb.seekClock));
  // The segment that was playing (the latest main one).
  std::int32_t ref = -1;
  for (std::uint32_t i = 0; i < kMaxInstances; ++i) {
    const Instance& in = pb.inst[i];
    if (in.role != Role::Main || in.stopPending != 0u) continue;
    if (ref < 0 || in.startTick > pb.inst[ref].startTick) ref = static_cast<std::int32_t>(i);
  }
  if (ref < 0) return;
  const std::uint32_t seg = pb.inst[ref].seg;
  const std::uint32_t stateRow = pb.inst[ref].stateRow;
  const double len = static_cast<double>(pb.inst[ref].lenTicks);
  double o = std::fmod(now - static_cast<double>(pb.inst[ref].startTick), len);
  if (o < 0.0) o += len;
  for (Instance& in : pb.inst) {
    if (in.role == Role::Free) continue;
    if (in.phase == InstPhase::Planned) {
      in.role = Role::Free;
      continue;
    }
    in.successor = 1;
    in.stopPending = 1;
    in.stopClock = pb.seekClock;
    in.stopFade = 0.0f;
    for (std::uint8_t v : in.voices) {
      if (v < kMaxLocalVoices) pb.voice[v].stopSent = 0;
    }
  }
  const MusicalTicks nowT = static_cast<MusicalTicks>(std::llround(now));
  tl.TruncateFrom(nowT);
  pb.head = PlanInstance(s, tb, pb, tl, Role::Main, seg, stateRow, nowT - static_cast<MusicalTicks>(std::llround(o)), nowT,
                         0.0f);
  if (pb.phase == Phase::Transitioning) pb.phase = Phase::Idle;
}

}  // namespace snd::music::detail
