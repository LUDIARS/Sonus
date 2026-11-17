// System 2: UpdateTransport (8.1, 8.2). With a music system (music_if.hpp)
// core only calls IMusicSystem::UpdateTransport. Without one, core runs a
// minimal transport here so that every When kind still works:
//   - IMMEDIATE / CLOCK_SAMPLE / DELAY_US reservations fire at their clock;
//   - NEXT_BEAT / NEXT_BAR / BARS_AFTER / GRID quantise on the transport's
//     constant tempo (SetTempo / Seek / SetTransportState apply at their time);
//   - NEXT_MARKER / SEGMENT_END need segments (music module): they fire at
//     once with a warning.
// Transports owned by a playback are destroyed when the playback is gone.
#include <bit>
#include <cmath>
#include <cstring>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {

constexpr schema::SystemId kSys = schema::SystemId::UpdateTransport;
constexpr double kTicksPerBeat = SND_TICKS_PER_BEAT;

double TicksPerSample(float bpm, float pitch, std::uint32_t rate) noexcept {
  return static_cast<double>(bpm) / 60.0 * kTicksPerBeat * pitch / static_cast<double>(rate);
}

void PushFire(Impl& im, SlotIndex reservation, ReservationAction action, HandleRef target,
              const ReservationPayload& payload, ClockSample clock, std::uint64_t seq, std::uint8_t late) noexcept {
  if (im.fire.count >= im.fire.capacity) return;
  music::FireEntry& f = im.fire.entries[im.fire.count++];
  f = music::FireEntry{};
  f.reservation = reservation;
  f.action = action;
  f.lateRuleApplied = late;
  f.target = target;
  f.payload = payload;
  f.fireClock = clock;
  f.sequence = seq;
}

}  // namespace

void UpdateTransport(Impl& im) noexcept {
  im.fire.count = 0;
  im.musicFrame.block = source::BlockInfo{im.block, im.clock, im.cfg.blockFrames, im.cfg.sampleRate};
  im.musicFrame.lookaheadFrames = im.cfg.blockFrames;
  im.musicFrame.outputLatencySeconds = im.desc.outputLatencySeconds;
  if (im.music != nullptr) {
    im.music->UpdateTransport(im.musicFrame, im.tables, im.fire);
    return;
  }
  schema::Tables& t = im.tables;
  const ClockSample blockEnd = im.clock + im.cfg.blockFrames;
  // --- transports ---
  schema::TableRows& trows = SND_ROWS(Transport);
  for (std::uint32_t d = 0; d < trows.count;) {
    const SlotIndex s = base::rows::SlotAt(trows, d);
    const TransportLifetime life = SND_R(Transport_lifetime)[d];
    bool destroy = life.destroyRequested != 0u;
    if (life.createdFromPlayback != 0u) {
      const SlotIndex owner = SND_R(Transport_ownerPlayback)[d];
      destroy = destroy || owner == kNoSlot || !base::rows::IsLive(t.Playback.rows, owner);
    }
    if (destroy) {
      // Reservations on it end with TRANSPORT_GONE (snd_destroy_transport).
      schema::TableRows& rr = SND_ROWS(Reservation);
      for (std::uint32_t rd = 0; rd < rr.count;) {
        if (SND_R(Reservation_transportSlot)[rd] == s) {
          const SlotIndex rs = base::rows::SlotAt(rr, rd);
          PushFire(im, rs, SND_R(Reservation_action)[rd], SND_R(Reservation_target)[rd], SND_R(Reservation_payload)[rd],
                   im.clock, SND_R(Reservation_sequence)[rd], kFireTransportGone);
          RemoveReservationRow(t, rs);
          continue;
        }
        ++rd;
      }
      im.tp.transportGen[s] = 0u;
      RemoveTransportRow(t, s);
      continue;
    }
    if (im.tp.transportGen[s] != trows.generation[s]) {  // new row
      im.tp.transportGen[s] = trows.generation[s];
      const TransportInit init = SND_R(Transport_initial)[d];
      SND_W(Transport_tempoMap)[d] = TempoMapRef{kNoBank, 0, 0, 0, init.bpm};
      SND_W(Transport_signature)[d] = init.signature;
      SND_W(Transport_state)[d] = init.startPlaying != 0u ? SND_TRANSPORT_PLAYING : SND_TRANSPORT_STOPPED;
      SND_W(Transport_pitch)[d] = 1.0f;
      SND_W(Transport_loopRange)[d] = MusicalRange{0, 0};
      SND_W(Transport_position)[d] =
          TransportPosition{0, 0.0, im.clock, TicksPerSample(init.bpm, 1.0f, im.cfg.sampleRate)};
    }
    ++d;
  }
  // --- reservations ---
  schema::TableRows& rr = SND_ROWS(Reservation);
  for (std::uint32_t d = 0; d < rr.count;) {
    const SlotIndex s = base::rows::SlotAt(rr, d);
    const std::uint64_t seq = SND_R(Reservation_sequence)[d];
    ResolvedTime& res = SND_W(Reservation_resolved)[d];
    if (im.tp.seenSeq[s] != seq) {  // new row: resolve the When into a clock (kept in `tick`)
      im.tp.seenSeq[s] = seq;
      res = ResolvedTime{static_cast<MusicalTicks>(im.clock), 0u, 1u};
      const ReservationWhen w = SND_R(Reservation_when)[d];
      const SlotIndex ts = SND_R(Reservation_transportSlot)[d];
      const std::uint32_t td = ts == kNoSlot ? kNoSlot : base::rows::DenseOf(t.Transport.rows, ts);
      switch (w.kind) {
        case SND_WHEN_CLOCK_SAMPLE:
          res.tick = static_cast<MusicalTicks>(w.value);
          break;
        case SND_WHEN_DELAY_US:
          res.tick = static_cast<MusicalTicks>(im.clock + static_cast<ClockSample>(
                                                              static_cast<double>(w.value) * im.cfg.sampleRate / 1e6 + 0.5));
          break;
        case SND_WHEN_NEXT_BEAT:
        case SND_WHEN_NEXT_BAR:
        case SND_WHEN_BARS_AFTER:
        case SND_WHEN_GRID: {
          if (td == kNoSlot) break;
          const TransportPosition pos = SND_W(Transport_position)[td];
          const TimeSignature sig = SND_W(Transport_signature)[td];
          const double beatTicks = kTicksPerBeat * 4.0 / (sig.beatUnit == 0u ? 4.0 : sig.beatUnit);
          const double barTicks = beatTicks * (sig.beatsPerBar == 0u ? 4u : sig.beatsPerBar);
          double grid = beatTicks;
          if (w.kind == SND_WHEN_NEXT_BAR || w.kind == SND_WHEN_BARS_AFTER) grid = barTicks;
          if (w.kind == SND_WHEN_GRID) grid = kTicksPerBeat * 4.0 / (w.count == 0u ? 4u : w.count);
          const double cur = static_cast<double>(pos.tick) + pos.tickFraction;
          double target = std::ceil(cur / grid - 1e-9) * grid;
          if (w.kind == SND_WHEN_BARS_AFTER) target = std::floor(cur / grid) * grid + grid * (w.count == 0u ? 1u : w.count);
          target += w.offsetTicks;
          const double tps = pos.ticksPerSample > 0.0 ? pos.ticksPerSample : 1e-9;
          const double samples = (target - cur) / tps;
          res.tick = static_cast<MusicalTicks>(im.clock + static_cast<ClockSample>(samples > 0.0 ? samples + 0.5 : 0.0));
          break;
        }
        case SND_WHEN_NEXT_MARKER:
        case SND_WHEN_SEGMENT_END:
          PostWarning(im, SND_ERR_UNSUPPORTED, 0u, SND_R(Reservation_target)[d].Value());
          break;
        default:
          break;
      }
    }
    const ClockSample fireAt = static_cast<ClockSample>(res.tick);
    if (fireAt >= blockEnd) {
      ++d;
      continue;
    }
    const ClockSample at = fireAt < im.clock ? im.clock : fireAt;
    if (fireAt < im.clock) {  // a time already gone (e.g. PlayScheduled in the past): now, with a warning
      PostEvent(im, notify::EventType::ReservationLate, static_cast<std::uint32_t>(ReservationLateRule::StartLate),
                SND_R(Reservation_target)[d].Value(), 0, static_cast<std::uint32_t>(im.clock - fireAt));
    }
    const ReservationAction action = SND_R(Reservation_action)[d];
    const ReservationPayload payload = SND_R(Reservation_payload)[d];
    const SlotIndex ts = SND_R(Reservation_transportSlot)[d];
    const std::uint32_t td = ts == kNoSlot ? kNoSlot : base::rows::DenseOf(t.Transport.rows, ts);
    switch (action) {
      case ReservationAction::SetTempo:
        if (td != kNoSlot) {
          float bpm = 120.0f;
          std::memcpy(&bpm, payload.bytes, 4);
          SND_W(Transport_tempoMap)[td].baseBpm = bpm;
          SND_W(Transport_position)[td].ticksPerSample = TicksPerSample(bpm, SND_W(Transport_pitch)[td], im.cfg.sampleRate);
        }
        break;
      case ReservationAction::Seek:
        if (td != kNoSlot) {
          snd_musical_time mt{};
          std::memcpy(&mt, payload.bytes, sizeof(mt));
          const TimeSignature sig = SND_W(Transport_signature)[td];
          std::int64_t beatTicks = static_cast<std::int64_t>(kTicksPerBeat) * 4 / (sig.beatUnit == 0u ? 4 : sig.beatUnit);
    if (beatTicks < 1) beatTicks = 1;  // defensive: a beat unit above 4 * kTicksPerBeat (bad input)
          TransportPosition& pos = SND_W(Transport_position)[td];
          pos.tick = (static_cast<std::int64_t>(mt.bar) * sig.beatsPerBar + mt.beat) * beatTicks + mt.tick;
          pos.tickFraction = 0.0;
        }
        break;
      case ReservationAction::SetTransportState:
        if (td != kNoSlot) {
          std::uint32_t st = 0;
          std::memcpy(&st, payload.bytes, 4);
          SND_W(Transport_state)[td] = st;
          if (st == SND_TRANSPORT_STOPPED) {
            SND_W(Transport_position)[td].tick = 0;
            SND_W(Transport_position)[td].tickFraction = 0.0;
          }
        }
        break;
      default:
        PushFire(im, s, action, SND_R(Reservation_target)[d], payload, at, SND_R(Reservation_sequence)[d], 0xFFu);
        break;
    }
    RemoveReservationRow(t, s);
  }
  // Same-clock entries fire in sequence order (2.5): stable insertion sort by (clock, sequence).
  for (std::uint32_t i = 1; i < im.fire.count; ++i) {
    const music::FireEntry e = im.fire.entries[i];
    std::uint32_t j = i;
    while (j > 0u && (im.fire.entries[j - 1u].fireClock > e.fireClock ||
                      (im.fire.entries[j - 1u].fireClock == e.fireClock && im.fire.entries[j - 1u].sequence > e.sequence))) {
      im.fire.entries[j] = im.fire.entries[j - 1u];
      --j;
    }
    im.fire.entries[j] = e;
  }
  // Advance playing transports to the end of the block.
  for (std::uint32_t d = 0; d < trows.count; ++d) {
    TransportPosition& pos = SND_W(Transport_position)[d];
    pos.ticksPerSample = TicksPerSample(SND_W(Transport_tempoMap)[d].baseBpm, SND_W(Transport_pitch)[d], im.cfg.sampleRate);
    pos.anchorClock = blockEnd;
    if (SND_W(Transport_state)[d] != SND_TRANSPORT_PLAYING) continue;
    double f = pos.tickFraction + pos.ticksPerSample * im.cfg.blockFrames;
    const double whole = std::floor(f);
    pos.tick += static_cast<MusicalTicks>(whole);
    pos.tickFraction = f - whole;
    // Beat notifications of the minimal transport.
    const TimeSignature sig = SND_W(Transport_signature)[d];
    std::int64_t beatTicks = static_cast<std::int64_t>(kTicksPerBeat) * 4 / (sig.beatUnit == 0u ? 4 : sig.beatUnit);
    if (beatTicks < 1) beatTicks = 1;  // defensive: a beat unit above 4 * kTicksPerBeat (bad input)
    const std::int64_t before = pos.tick - static_cast<MusicalTicks>(whole);
    const std::int64_t firstBeat = (before + beatTicks - 1) / beatTicks;
    const std::int64_t lastBeat = pos.tick / beatTicks;
    for (std::int64_t b = firstBeat; b <= lastBeat && b * beatTicks >= before && b * beatTicks < pos.tick; ++b) {
      const SlotIndex s = base::rows::SlotAt(trows, d);
      const std::uint32_t bpb = sig.beatsPerBar == 0u ? 4u : sig.beatsPerBar;
      std::uint64_t handle = HandleRef{s, trows.generation[s]}.Value();
      const SlotIndex owner = SND_R(Transport_ownerPlayback)[d];
      if (SND_R(Transport_lifetime)[d].createdFromPlayback != 0u && owner != kNoSlot &&
          base::rows::IsLive(t.Playback.rows, owner)) {
        handle = HandleRef{owner | SND_TRANSPORT_PLAYBACK_BIT, t.Playback.rows.generation[owner]}.Value();
      }
      PostEvent(im, notify::EventType::Beat, static_cast<std::uint32_t>(b / bpb), handle, 0,
                static_cast<std::uint32_t>(b % bpb));
    }
  }
}

}  // namespace snd::core
