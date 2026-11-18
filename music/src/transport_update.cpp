// System 2: UpdateTransport (design 8.1, 8.2; music_if.hpp).
//
//   1. Transport rows: new rows get a time line (standalone: TransportInit;
//      owned by a music playback: Stopped until the music starts), destroyed
//      rows (snd_destroy_transport, owner playback gone) end their
//      reservations with "transport gone" and leave the table.
//   2. Reservations: each new row is resolved once into a musical target tick
//      (musical When kinds, quantised on the transport's current time line)
//      or a clock (IMMEDIATE / CLOCK_SAMPLE / DELAY_US). The lead time (8.2:
//      prime time of the target material) is checked against the late rule.
//   3. Transport changes due in this block (SetTempo / Seek /
//      SetTransportState, "now" included, D-18) are applied in clock order at
//      their exact sample; beats are notified up to each change.
//   4. The other due reservations become FireList entries at their exact
//      clock (the musical target is converted only now, so tempo changes and
//      seeks before the fire time move it, 8.2). Targets already passed (seek
//      forward) take the late rule.
//   5. Beats to the block end, and the Transport columns (position at the
//      block end, as core's minimal transport does).
#include <cmath>
#include <cstring>

#include "music_state.hpp"
#include "snd/base/sparse_set.hpp"

namespace snd::music {

namespace {

using namespace detail;
constexpr schema::SystemId kSys = schema::SystemId::UpdateTransport;

#define SND_MW(col) (::snd::schema::WriteColumn<kSys, ::snd::schema::ColumnId::col>(t))
#define SND_MR(col) (::snd::schema::ReadColumn<kSys, ::snd::schema::ColumnId::col>(t))

void RemoveTransportRow(schema::Tables& t, SlotIndex slot) noexcept {
  schema::TransportTable& tb = schema::TableTraits<schema::TableId::Transport>::Get(t);
  (void)schema::CreatorRows<kSys, schema::TableId::Transport>(t);
  const base::rows::RemoveResult res = base::rows::Remove(tb.rows, slot);
  if (res.Moved()) {
#define SND_MOVE_COLUMN(T, name, type, temp, writer) base::rows::MoveElement(tb.col.name, res.dense, res.movedFrom);
    SND_COLS_Transport(SND_MOVE_COLUMN)
#undef SND_MOVE_COLUMN
  }
}

void RemoveReservationRow(schema::Tables& t, SlotIndex slot) noexcept {
  schema::ReservationTable& tb = schema::TableTraits<schema::TableId::Reservation>::Get(t);
  (void)schema::CreatorRows<kSys, schema::TableId::Reservation>(t);
  const base::rows::RemoveResult res = base::rows::Remove(tb.rows, slot);
  if (res.Moved()) {
#define SND_MOVE_COLUMN(T, name, type, temp, writer) base::rows::MoveElement(tb.col.name, res.dense, res.movedFrom);
    SND_COLS_Reservation(SND_MOVE_COLUMN)
#undef SND_MOVE_COLUMN
  }
}

bool IsMusicalKind(std::uint32_t k) noexcept { return k >= SND_WHEN_NEXT_BEAT && k <= SND_WHEN_SEGMENT_END; }
bool IsTransportAction(ReservationAction a) noexcept {
  return a == ReservationAction::SetTempo || a == ReservationAction::Seek || a == ReservationAction::SetTransportState;
}

void PushFire(FireList& fire, SlotIndex r, ReservationAction action, HandleRef target, const ReservationPayload& payload,
              ClockSample clock, std::uint64_t seq, std::uint8_t late) noexcept {
  if (fire.entries == nullptr || fire.count >= fire.capacity) return;
  FireEntry& f = fire.entries[fire.count++];
  f = FireEntry{};
  f.reservation = r;
  f.action = action;
  f.lateRuleApplied = late;
  f.target = target;
  f.payload = payload;
  f.fireClock = clock;
  f.sequence = seq;
}

// Handle under which beats of transport `slot` are reported (playback-owned: D-05).
std::uint64_t TransportHandle(const schema::Tables& t, SlotIndex slot) noexcept {
  const schema::TableRows& rows = t.Transport.rows;
  const std::uint32_t d = base::rows::DenseOf(rows, slot);
  if (d == kInvalidSlot) return 0;
  const TransportLifetime life = SND_MR(Transport_lifetime)[d];
  const SlotIndex owner = SND_MR(Transport_ownerPlayback)[d];
  if (life.createdFromPlayback != 0u && owner != kInvalidSlot && base::rows::IsLive(t.Playback.rows, owner)) {
    return HandleRef{owner | SND_TRANSPORT_PLAYBACK_BIT, t.Playback.rows.generation[owner]}.Value();
  }
  return HandleRef{slot, rows.generation[slot]}.Value();
}

// Beats in [tl.beatCursor, to): one record per beat (bar line when beat == 0).
void EmitBeats(State& s, Timeline& tl, std::uint64_t handle, ClockSample to) noexcept {
  if (to <= tl.beatCursor) return;
  const ClockSample from = tl.beatCursor;
  tl.beatCursor = to;
  if (!tl.Playing() || handle == 0u) return;
  double b = tl.NextBeat(tl.TickAtClock(static_cast<double>(from)));
  for (std::uint32_t guard = 0; guard < 256u; ++guard) {
    double c = 0.0;
    if (!tl.ClockAtTick(b, &c)) break;
    const ClockSample at = RoundClock(c);  // rounded on both bounds: a beat on a block edge is sent once
    if (at >= to) break;
    if (b >= -1e-6 && at >= from) {
      const snd_musical_time m = tl.Musical(b);
      PushNote(s, notify::EventType::Beat, static_cast<std::uint32_t>(m.bar), handle, at,
               static_cast<std::uint32_t>(m.beat));
    }
    b = tl.NextBeat(b + 0.5);
  }
}

struct Resolver {
  State& s;
  schema::Tables& t;
};

// The target playback's bank tables (s.tb2) and event row; false if not loaded.
bool TargetEvent(State& s, const schema::Tables& t, HandleRef target, std::uint32_t* eventRow, EventRef* refOut) noexcept {
  const std::uint32_t pd = base::rows::DenseOf(t.Playback.rows, target);
  if (pd == kInvalidSlot) return false;
  const EventRef ref = SND_MR(Playback_eventRef)[pd];
  if (ref.bankSlot == kUnresolvedBank) return false;
  if (s.tb2.bank != ref.bankSlot || s.tb2.evnt.Rows() == 0u) {
    if (!s.tb2.Bind(s.services, ref.bankSlot)) return false;
  }
  if (!s.tb2.evnt.Has(ref.eventIndex)) return false;
  *eventRow = ref.eventIndex;
  if (refOut != nullptr) *refOut = ref;
  return true;
}

// Lead time (8.2): prime frames of what the reservation will start.
std::uint32_t LeadFrames(State& s, const schema::Tables& t, ReservationAction action, HandleRef target,
                         const ReservationPayload& payload) noexcept {
  std::uint32_t ev = 0;
  switch (action) {
    case ReservationAction::Play:
    case ReservationAction::Stinger:
      return TargetEvent(s, t, target, &ev, nullptr) ? EventPrimeFrames(s, s.tb2, ev) : 0u;
    case ReservationAction::SetMusicState: {
      if (!TargetEvent(s, t, target, &ev, nullptr)) return 0;
      Id state = 0;
      std::memcpy(&state, payload.bytes, 4);
      const std::uint32_t row = FindStateRow(s.tb2, ev, state);
      if (!s.tb2.msta.Has(row)) return 0;
      SegmentInfo si;
      if (!DescribeSegment(s.tb2, FirstSegment(s.tb2, s.tb2.msta.U32(bf::col::MSTA_playlist, row)), s.rate, &si)) return 0;
      return PrimeOf(s, s.tb2.bank, si.asset, si.pickup);
    }
    default:
      return 0;
  }
}

// Keeps a grid target as a musical position (bar line + ticks into the bar).
void Pin(const Timeline& tl, ResState& rs) noexcept {
  if (rs.barBased == 0u) return;
  const double base = rs.tick - rs.offset;
  rs.bar = std::floor(tl.BarPosition(base) + 1e-7);
  rs.inBar = base - tl.TickAtBar(rs.bar);
}
// The target tick on the map in effect now.
void Refresh(const Timeline& tl, ResState& rs) noexcept {
  if (rs.barBased != 0u) rs.tick = tl.TickAtBar(rs.bar) + rs.inBar + rs.offset;
}

// Applies the late rule until the target clears `earliest` (8.2 / 8.5). Musical kinds only.
void ApplyLateRule(State& s, Timeline& tl, ResState& rs, std::uint32_t kind, std::uint32_t count, Id marker,
                   const MusicPb* owner, double earliest) noexcept {
  for (std::uint32_t guard = 0; guard < 1024u; ++guard) {
    double c = 0.0;
    if (!tl.ClockAtTick(rs.tick, &c)) return;
    c -= static_cast<double>(rs.pickup);
    if (c >= earliest - 0.5) return;
    switch (static_cast<ReservationLateRule>(rs.lateRule)) {
      case ReservationLateRule::StartLate:
        rs.late = static_cast<std::uint8_t>(ReservationLateRule::StartLate);
        return;
      case ReservationLateRule::Cancel:
        rs.late = static_cast<std::uint8_t>(ReservationLateRule::Cancel);
        rs.cancel = 1;
        return;
      case ReservationLateRule::NextGrid:
      default: {
        double next = rs.tick;
        if (kind == SND_WHEN_NEXT_MARKER || kind == SND_WHEN_SEGMENT_END) {
          bool ok = false;
          if (owner != nullptr) {
            ok = kind == SND_WHEN_NEXT_MARKER ? NextMarkerAfter(s, s.tb, *owner, rs.tick + 0.5, marker, &next)
                                              : SegmentEndAfter(*owner, rs.tick + 0.5, &next);
          }
          if (!ok) {  // nothing further on the time line: start late
            rs.late = static_cast<std::uint8_t>(ReservationLateRule::StartLate);
            return;
          }
        } else {
          next = tl.NextOfKind(kind, count, rs.tick);
        }
        if (next <= rs.tick) {
          rs.late = static_cast<std::uint8_t>(ReservationLateRule::StartLate);
          return;
        }
        rs.tick = next;
        ++rs.deferrals;
        rs.late = static_cast<std::uint8_t>(ReservationLateRule::NextGrid);
        break;
      }
    }
  }
}

void Resolve(State& s, schema::Tables& t, std::uint32_t d, SlotIndex r) noexcept {
  ResState& rs = s.res[r];
  rs = ResState{};
  rs.seq = SND_MR(Reservation_sequence)[d];
  rs.late = kNoLate;
  const ReservationWhen w = SND_MR(Reservation_when)[d];
  const ReservationAction action = SND_MR(Reservation_action)[d];
  const HandleRef target = SND_MR(Reservation_target)[d];
  const ReservationPayload payload = SND_MR(Reservation_payload)[d];
  const SlotIndex ts = SND_MR(Reservation_transportSlot)[d];
  rs.lateRule = w.lateRule <= 2u ? w.lateRule : 0u;
  // SetMusicState: the matching rule's late rule (8.5 "間に合わない時").
  std::uint32_t ev = 0;
  if (action == ReservationAction::SetMusicState) {
    if (const MusicPb* pb = PbOf(s, target.slot); pb != nullptr && pb->started != 0u && TargetEvent(s, t, target, &ev, nullptr)) {
      Id to = 0;
      std::memcpy(&to, payload.bytes, 4);
      rs.lateRule = FindRule(s.tb2, ev, pb->stateId, to).lateRule;
    }
  }
  // PICKUP_ALIGN (D-21): start earlier by the material's pickup.
  if ((w.flags & SND_WHEN_FLAG_PICKUP_ALIGN) != 0u && action == ReservationAction::Play &&
      TargetEvent(s, t, target, &ev, nullptr)) {
    rs.pickup = RoundClock(EventPickupSamples(s, s.tb2, ev));
  }
  Timeline* tl = IsMusicalKind(w.kind) ? TimelineOf(s, ts) : nullptr;
  if (!IsMusicalKind(w.kind) || tl == nullptr) {
    rs.isClock = 1;
    switch (w.kind) {
      case SND_WHEN_CLOCK_SAMPLE: rs.clock = w.value; break;
      case SND_WHEN_DELAY_US:
        rs.clock = s.blockStart + RoundClock(static_cast<double>(w.value) * s.rate / 1e6);
        break;
      default: rs.clock = s.blockStart; break;  // IMMEDIATE ("now"), or a musical kind without a transport
    }
    rs.clock = rs.clock > rs.pickup ? rs.clock - rs.pickup : 0u;
    SND_MW(Reservation_resolved)[d] = ResolvedTime{static_cast<MusicalTicks>(rs.clock), 0u, 1u};
    return;
  }
  const double now = tl->TickAtClock(static_cast<double>(s.blockStart));
  MusicPb* owner = PbOfTransport(s, ts);
  bool ownerTables = false;
  if (owner != nullptr && (w.kind == SND_WHEN_NEXT_MARKER || w.kind == SND_WHEN_SEGMENT_END)) {
    ownerTables = s.tb.bank == owner->bank || s.tb.Bind(s.services, owner->bank);
  }
  double tick = now;
  switch (w.kind) {
    case SND_WHEN_NEXT_BEAT: tick = tl->NextBeat(now); break;
    case SND_WHEN_NEXT_BAR: tick = tl->NextBar(now); break;
    case SND_WHEN_BARS_AFTER: tick = tl->BarsAfter(now, w.count); break;
    case SND_WHEN_GRID: tick = tl->NextGrid(now, w.count); break;
    case SND_WHEN_NEXT_MARKER:
    case SND_WHEN_SEGMENT_END: {
      bool ok = false;
      if (owner != nullptr && ownerTables) {
        ok = w.kind == SND_WHEN_NEXT_MARKER
                 ? NextMarkerAfter(s, s.tb, *owner, now, static_cast<Id>(w.value), &tick)
                 : SegmentEndAfter(*owner, now, &tick);
      }
      if (!ok) {  // no segment on this transport: at once, with a warning (as core's minimal transport)
        PushNote(s, notify::EventType::Warning, static_cast<std::uint32_t>(SND_ERR_UNSUPPORTED), target.Value(),
                 s.blockStart);
        tick = now;
      }
      break;
    }
    default: break;
  }
  rs.offset = static_cast<double>(w.offsetTicks);
  rs.tick = tick + rs.offset;
  rs.barBased = (w.kind == SND_WHEN_NEXT_BEAT || w.kind == SND_WHEN_NEXT_BAR || w.kind == SND_WHEN_BARS_AFTER ||
                 w.kind == SND_WHEN_GRID)
                    ? 1u
                    : 0u;
  if (tl->Playing()) {
    const double earliest =
        static_cast<double>(s.blockStart) + static_cast<double>(LeadFrames(s, t, action, target, payload));
    ApplyLateRule(s, *tl, rs, w.kind, w.count, static_cast<Id>(w.value), owner, earliest);
    if (rs.late != kNoLate) {
      PushNote(s, notify::EventType::ReservationLate, rs.late, target.Value(), s.blockStart, rs.deferrals);
    }
    if (owner != nullptr && rs.deferrals != 0u) owner->deferrals += rs.deferrals;
  }
  Pin(*tl, rs);
  SND_MW(Reservation_resolved)[d] =
      ResolvedTime{static_cast<MusicalTicks>(std::llround(rs.tick)), rs.deferrals, 1u};
}

// Stingers with overlap "replace" (8.6): a newer reservation of the same
// stinger event on the same transport cancels the older pending ones.
void ReplaceStingers(State& s, schema::Tables& t, std::uint32_t dNew) noexcept {
  if (SND_MR(Reservation_action)[dNew] != ReservationAction::Play) return;
  const HandleRef target = SND_MR(Reservation_target)[dNew];
  std::uint32_t ev = 0;
  EventRef ref{};
  if (!TargetEvent(s, t, target, &ev, &ref)) return;
  if ((s.tb2.evnt.U32(bf::col::EVNT_flags, ev) & bf::kEventStinger) == 0u) return;
  bool replace = false;
  for (std::uint32_t r = 0; r < s.tb2.mstg.Rows(); ++r) {
    if (s.tb2.mstg.U32(bf::col::MSTG_event, r) == ev) replace = replace || s.tb2.mstg.U32(bf::col::MSTG_overlap, r) == 0u;
  }
  if (!replace) return;
  const SlotIndex ts = SND_MR(Reservation_transportSlot)[dNew];
  const std::uint64_t seqNew = SND_MR(Reservation_sequence)[dNew];
  const schema::TableRows& rr = t.Reservation.rows;
  for (std::uint32_t d = 0; d < rr.count; ++d) {
    if (d == dNew || SND_MR(Reservation_action)[d] != ReservationAction::Play) continue;
    if (SND_MR(Reservation_transportSlot)[d] != ts || SND_MR(Reservation_sequence)[d] >= seqNew) continue;
    const std::uint32_t pd = base::rows::DenseOf(t.Playback.rows, SND_MR(Reservation_target)[d]);
    if (pd == kInvalidSlot) continue;
    const EventRef other = SND_MR(Playback_eventRef)[pd];
    if (other.bankSlot != ref.bankSlot || other.eventIndex != ref.eventIndex) continue;
    ResState& o = s.res[base::rows::SlotAt(rr, d)];
    o.cancel = 1;
    o.late = static_cast<std::uint8_t>(ReservationLateRule::Cancel);
  }
}

// Fire clock of a resolved reservation; false while its transport does not play.
bool FireClock(State& s, const schema::Tables& t, std::uint32_t d, ResState& rs, double* out) noexcept {
  if (rs.isClock != 0u || rs.cancel != 0u) {
    *out = rs.cancel != 0u ? static_cast<double>(s.blockStart) : static_cast<double>(rs.clock);
    return true;
  }
  Timeline* tl = TimelineOf(s, SND_MR(Reservation_transportSlot)[d]);
  if (tl == nullptr) {
    *out = static_cast<double>(s.blockStart);
    return true;
  }
  Refresh(*tl, rs);
  double c = 0.0;
  if (!tl->ClockAtTick(rs.tick, &c)) return false;
  *out = c - static_cast<double>(rs.pickup);
  return true;
}

void ApplyTransportAction(State& s, schema::Tables& t, std::uint32_t d, ClockSample at) noexcept {
  const SlotIndex ts = SND_MR(Reservation_transportSlot)[d];
  Timeline* tl = TimelineOf(s, ts);
  if (tl == nullptr) return;
  EmitBeats(s, *tl, TransportHandle(t, ts), at);
  const ReservationPayload p = SND_MR(Reservation_payload)[d];
  switch (SND_MR(Reservation_action)[d]) {
    case ReservationAction::SetTempo: {
      float bpm = 0.0f;
      std::memcpy(&bpm, p.bytes, 4);
      if (bpm > 0.0f && bpm < 10000.0f) tl->SetTempoAt(at, bpm);
      break;
    }
    case ReservationAction::Seek: {
      snd_musical_time m{};
      std::memcpy(&m, p.bytes, sizeof m);
      tl->SeekAt(at, tl->TickOfMusical(m));
      if (MusicPb* pb = PbOfTransport(s, ts); pb != nullptr) {
        pb->seekPending = 1;
        pb->seekClock = at;
        pb->seekTarget = m;
      }
      break;
    }
    case ReservationAction::SetTransportState: {
      std::uint32_t st = 0;
      std::memcpy(&st, p.bytes, 4);
      if (st == SND_TRANSPORT_PLAYING) {
        tl->ResumeAt(at);
      } else if (st == SND_TRANSPORT_PAUSED) {
        tl->PauseAt(at);
      } else if (st == SND_TRANSPORT_STOPPED) {
        tl->StopAt(at);
      }
      break;
    }
    default: break;
  }
}

}  // namespace

void MusicSystem::UpdateTransport(const MusicFrame& frame, schema::Tables& t, FireList& fire) noexcept {
  fire.count = 0;
  if (s_ == nullptr) return;
  State& s = *s_;
  s.block = frame.block.index;
  s.blockStart = frame.block.clock;
  s.blockFrames = frame.block.frames != 0u ? frame.block.frames : s.limits.blockFrames;
  const ClockSample blockEnd = s.BlockEnd();
  const ClockSample dueLimit = s.blockStart + (frame.lookaheadFrames != 0u ? frame.lookaheadFrames : s.blockFrames);
  s.tb.bank = 0xFFFFu;   // bank tables may have changed between blocks (load / unload)
  s.tb2.bank = 0xFFFFu;
  s.tb.evnt.Bind(0, nullptr, 0);
  s.tb2.evnt.Bind(0, nullptr, 0);

  // 1. Transports.
  schema::TableRows& trows = schema::CreatorRows<kSys, schema::TableId::Transport>(t);
  for (std::uint32_t d = 0; d < trows.count;) {
    const SlotIndex slot = base::rows::SlotAt(trows, d);
    const std::uint32_t gen = trows.generation[slot];
    const TransportLifetime life = SND_MR(Transport_lifetime)[d];
    const SlotIndex owner = SND_MR(Transport_ownerPlayback)[d];
    bool destroy = life.destroyRequested != 0u;
    if (life.createdFromPlayback != 0u) {
      destroy = destroy || owner == kInvalidSlot || !base::rows::IsLive(t.Playback.rows, owner);
    }
    if (slot >= s.limits.maxTransports) {
      ++d;
      continue;
    }
    Timeline& tl = s.timelines[slot];
    if (destroy) {
      schema::TableRows& rr = schema::CreatorRows<kSys, schema::TableId::Reservation>(t);
      for (std::uint32_t rd = 0; rd < rr.count;) {
        if (SND_MR(Reservation_transportSlot)[rd] != slot) {
          ++rd;
          continue;
        }
        const SlotIndex rs = base::rows::SlotAt(rr, rd);
        PushFire(fire, rs, SND_MR(Reservation_action)[rd], SND_MR(Reservation_target)[rd],
                 SND_MR(Reservation_payload)[rd], s.blockStart, SND_MR(Reservation_sequence)[rd], kLateTransportGone);
        if (rs < s.limits.maxReservations) s.res[rs] = ResState{};
        RemoveReservationRow(t, rs);
      }
      tl.generation = 0;
      RemoveTransportRow(t, slot);
      continue;
    }
    if (tl.generation != gen) {
      tl = Timeline{};
      if (life.createdFromPlayback != 0u) {
        tl.Reset(s.rate, 120.0, 4, 4);  // Stopped until its music starts (StartMusic)
      } else {
        const TransportInit init = SND_MR(Transport_initial)[d];
        tl.Reset(s.rate, init.bpm, init.signature.beatsPerBar, init.signature.beatUnit);
        if (init.startPlaying != 0u) tl.StartAt(s.blockStart, 0.0);
      }
      tl.generation = gen;
      tl.beatCursor = s.blockStart;
    }
    tl.AdvanceTo(s.blockStart);
    ++d;
  }

  // 2. Resolve new reservations.
  schema::TableRows& rr = schema::CreatorRows<kSys, schema::TableId::Reservation>(t);
  for (std::uint32_t d = 0; d < rr.count; ++d) {
    const SlotIndex r = base::rows::SlotAt(rr, d);
    if (r >= s.limits.maxReservations) continue;
    if (s.res[r].seq != SND_MR(Reservation_sequence)[d]) {
      Resolve(s, t, d, r);
      ReplaceStingers(s, t, d);
    }
  }

  // 3. Transport changes due in this block, in (clock, sequence) order.
  std::uint32_t nDue = 0;
  for (std::uint32_t d = 0; d < rr.count; ++d) {
    const SlotIndex r = base::rows::SlotAt(rr, d);
    if (r >= s.limits.maxReservations || !IsTransportAction(SND_MR(Reservation_action)[d])) continue;
    double c = 0.0;
    if (FireClock(s, t, d, s.res[r], &c) && c < static_cast<double>(dueLimit)) s.due[nDue++] = r;
  }
  auto clockOf = [&](SlotIndex r) {
    double c = 0.0;
    const std::uint32_t d = base::rows::DenseOf(rr, r);
    FireClock(s, t, d, s.res[r], &c);
    return c < static_cast<double>(s.blockStart) ? s.blockStart : RoundClock(c);
  };
  for (std::uint32_t i = 1; i < nDue; ++i) {  // stable insertion sort (2.5)
    const SlotIndex x = s.due[i];
    const ClockSample cx = clockOf(x);
    std::uint32_t j = i;
    while (j > 0u) {
      const SlotIndex y = s.due[j - 1u];
      const ClockSample cy = clockOf(y);
      if (cy < cx || (cy == cx && s.res[y].seq <= s.res[x].seq)) break;
      s.due[j] = y;
      --j;
    }
    s.due[j] = x;
  }
  for (std::uint32_t i = 0; i < nDue; ++i) {
    const SlotIndex r = s.due[i];
    const std::uint32_t d = base::rows::DenseOf(rr, r);
    if (d == kInvalidSlot) continue;
    ApplyTransportAction(s, t, d, clockOf(r));
    s.res[r] = ResState{};
    RemoveReservationRow(t, r);
  }

  // 4. Other due reservations -> FireList.
  for (std::uint32_t d = 0; d < rr.count;) {
    const SlotIndex r = base::rows::SlotAt(rr, d);
    if (r >= s.limits.maxReservations) {
      ++d;
      continue;
    }
    ResState& rs = s.res[r];
    double c = 0.0;
    // Transport changes are applied in step 3 only (one that became due through an
    // earlier change of this block takes effect at the next block start).
    // Stingers fire one block early: core creates their playback in the next block's
    // ApplyCommands with the exact start clock (sample accurate, D-76).
    const ClockSample limit =
        SND_MR(Reservation_action)[d] == ReservationAction::Stinger ? dueLimit + s.blockFrames : dueLimit;
    if (IsTransportAction(SND_MR(Reservation_action)[d]) || !FireClock(s, t, d, rs, &c) ||
        c >= static_cast<double>(limit)) {
      ++d;
      continue;
    }
    const ReservationAction action = SND_MR(Reservation_action)[d];
    const HandleRef target = SND_MR(Reservation_target)[d];
    ClockSample at = c < static_cast<double>(s.blockStart) ? s.blockStart : RoundClock(c);
    if (rs.cancel == 0u && c < static_cast<double>(s.blockStart) - 0.5) {
      // The target passed without firing (seek forward, a clock in the past): late rule.
      if (rs.isClock == 0u) {
        Timeline* tl = TimelineOf(s, SND_MR(Reservation_transportSlot)[d]);
        const ReservationWhen w = SND_MR(Reservation_when)[d];
        MusicPb* owner = tl != nullptr ? PbOfTransport(s, SND_MR(Reservation_transportSlot)[d]) : nullptr;
        if (owner != nullptr) s.tb.Bind(s.services, owner->bank);
        if (tl != nullptr) {
          ApplyLateRule(s, *tl, rs, w.kind, w.count, static_cast<Id>(w.value), owner, static_cast<double>(s.blockStart));
          Pin(*tl, rs);
        }
        if (rs.late == static_cast<std::uint8_t>(ReservationLateRule::NextGrid)) {
          PushNote(s, notify::EventType::ReservationLate, rs.late, target.Value(), s.blockStart, rs.deferrals);
          SND_MW(Reservation_resolved)[d] =
              ResolvedTime{static_cast<MusicalTicks>(std::llround(rs.tick)), rs.deferrals, 1u};
          if (FireClock(s, t, d, rs, &c) && c >= static_cast<double>(dueLimit)) {
            ++d;
            continue;  // moved to a later grid point
          }
          at = c < static_cast<double>(s.blockStart) ? s.blockStart : RoundClock(c);
        } else {
          PushNote(s, notify::EventType::ReservationLate, rs.late, target.Value(), s.blockStart, rs.deferrals);
        }
      } else {
        rs.late = static_cast<std::uint8_t>(ReservationLateRule::StartLate);
        PushNote(s, notify::EventType::ReservationLate, rs.late, target.Value(), s.blockStart,
                 static_cast<std::uint32_t>(s.blockStart - RoundClock(c)));
      }
    }
    std::uint8_t late = rs.late;
    if (rs.cancel != 0u) {
      late = static_cast<std::uint8_t>(ReservationLateRule::Cancel);
      at = s.blockStart;
    }
    // A cancelled SetMusicState / Stinger never reaches the music logic (nothing to end).
    const bool drop = rs.cancel != 0u && action != ReservationAction::Play;
    if (!drop) {
      PushFire(fire, r, action, target, SND_MR(Reservation_payload)[d], at, SND_MR(Reservation_sequence)[d], late);
    } else if (MusicPb* pb = PbOf(s, target.slot); pb != nullptr) {
      ++pb->cancels;
    }
    rs = ResState{};
    RemoveReservationRow(t, r);
  }
  // Same-clock entries fire in sequence order (2.5).
  for (std::uint32_t i = 1; i < fire.count; ++i) {
    const FireEntry e = fire.entries[i];
    std::uint32_t j = i;
    while (j > 0u && (fire.entries[j - 1u].fireClock > e.fireClock ||
                      (fire.entries[j - 1u].fireClock == e.fireClock && fire.entries[j - 1u].sequence > e.sequence))) {
      fire.entries[j] = fire.entries[j - 1u];
      --j;
    }
    fire.entries[j] = e;
  }

  // 5. Beats to the block end, loop range, columns.
  for (std::uint32_t d = 0; d < trows.count; ++d) {
    const SlotIndex slot = base::rows::SlotAt(trows, d);
    if (slot >= s.limits.maxTransports) continue;
    Timeline& tl = s.timelines[slot];
    const std::uint64_t handle = TransportHandle(t, slot);
    if (tl.Playing() && tl.loop.end > tl.loop.begin) {
      double c = 0.0;
      if (tl.ClockAtTick(static_cast<double>(tl.loop.end), &c) && c >= static_cast<double>(s.blockStart) &&
          c < static_cast<double>(blockEnd)) {
        EmitBeats(s, tl, handle, RoundClock(c));
        tl.SeekAt(RoundClock(c), static_cast<double>(tl.loop.begin));
      }
    }
    EmitBeats(s, tl, handle, blockEnd);
    const double endTick = tl.TickAtClock(static_cast<double>(blockEnd));
    const std::uint32_t pi = tl.PointAt(endTick);
    const MusicPb* pb = PbOfTransport(s, slot);
    SND_MW(Transport_tempoMap)[d] = TempoMapRef{pb != nullptr ? pb->bank : kUnresolvedBank,
                                                static_cast<std::uint16_t>(tl.overrideBpm > 0.0 ? 1u : 0u), 0u,
                                                tl.count, static_cast<float>(tl.BpmOf(pi))};
    SND_MW(Transport_signature)[d] = TimeSignature{tl.points[pi].beatsPerBar, tl.points[pi].beatUnit};
    const double whole = std::floor(endTick);
    SND_MW(Transport_position)[d] =
        TransportPosition{static_cast<MusicalTicks>(whole), endTick - whole, blockEnd, tl.TicksPerSample(pi)};
    SND_MW(Transport_state)[d] = tl.state;
    SND_MW(Transport_pitch)[d] = static_cast<float>(tl.pitch);
    SND_MW(Transport_loopRange)[d] = tl.loop;
  }
}

}  // namespace snd::music
