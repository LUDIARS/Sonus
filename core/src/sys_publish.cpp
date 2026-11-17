// System 10: Publish (2.1, notify.hpp): end flags, release flags, the event
// ring (core warnings + music notifications), the snapshot triple buffer
// (playbacks, transports, buses, stats), and the sources' background work.
#include <bit>
#include <cmath>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {
constexpr schema::SystemId kSys = schema::SystemId::Publish;

snd_musical_time TicksToMusical(MusicalTicks tick, TimeSignature sig) noexcept {
  const std::int64_t beatTicks = static_cast<std::int64_t>(SND_TICKS_PER_BEAT) * 4 / (sig.beatUnit == 0u ? 4 : sig.beatUnit);
  const std::int64_t bpb = sig.beatsPerBar == 0u ? 4 : sig.beatsPerBar;
  if (tick < 0) tick = 0;
  const std::int64_t beats = tick / beatTicks;
  return snd_musical_time{static_cast<std::int32_t>(beats / bpb), static_cast<std::int32_t>(beats % bpb),
                          static_cast<std::int32_t>((tick % beatTicks) * SND_TICKS_PER_BEAT / beatTicks)};
}
}  // namespace

void Publish(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  // 1. Lossless flags.
  const LiveTap& tap = im.desc.liveTap;
  for (std::uint32_t i = 0; i < im.endedCount; ++i) {
    const EndedPlayback& e = im.ended[i];
    if (tap.ended != nullptr) tap.ended(tap.user, e.slot, e.generation, e.reason, e.detail);
    base::notify::PublishEnd(im.endFlags, e.slot,
                             notify::EndRecord{e.generation, static_cast<std::uint16_t>(e.reason),
                                               static_cast<std::uint16_t>(e.detail)});
  }
  for (std::uint32_t i = 0; i < im.releasedEmitterCount; ++i) {
    base::notify::PublishRelease(im.emitterRelease, im.releasedEmitters[i].slot, im.releasedEmitters[i].generation);
  }
  for (std::uint32_t i = 0; i < im.releasedUserBufferCount; ++i) {
    base::notify::PublishRelease(im.userBufferRelease, im.releasedUserBuffers[i].slot,
                                 im.releasedUserBuffers[i].generation);
  }
  const schema::TableRows& tr = t.Transport.rows;
  for (std::uint32_t s = 0; s < im.cfg.maxTransports; ++s) {
    const std::uint32_t live = base::rows::IsLive(tr, s) ? tr.generation[s] : 0u;
    if (im.pub.transportGen[s] != 0u && im.pub.transportGen[s] != live) {
      base::notify::PublishRelease(im.transportRelease, s, im.pub.transportGen[s]);
    }
    im.pub.transportGen[s] = live;
  }
  // 2. Event ring: music notifications, then core's records of this block.
  if (im.music != nullptr && im.eventCount < im.eventCap) {
    im.eventCount += im.music->CollectNotifications(im.events + im.eventCount, im.eventCap - im.eventCount);
  }
  for (std::uint32_t i = 0; i < im.eventCount; ++i) {
    if (tap.event != nullptr) tap.event(tap.user, im.events[i]);
    if (!base::notify::PushEvent(*im.eventRing, im.events[i])) {
      // dropped and counted by the ring (stats.dropped_events)
    }
  }
  im.eventCount = 0;
  // 3. Snapshot.
  snd_stats& st = im.stats;
  st.block_index = im.block + 1u;
  st.clock_sample = im.clock + im.cfg.blockFrames;
  st.playbacks = t.Playback.rows.count;
  st.real_voices = t.RealVoice.rows.count;
  st.virtual_voices = t.Voice.rows.count - (t.RealVoice.rows.count < t.Voice.rows.count ? t.RealVoice.rows.count : t.Voice.rows.count);
  st.streams = t.Stream.rows.count;
  st.lod_level = im.rd.lod;
  // Stream stalls (6.3): the growth of Stream.stallCount (written by the Stream source in Render).
  {
    const schema::TableRows& sr = t.Stream.rows;
    const std::uint32_t* stalls = SND_R(Stream_stallCount);
    for (std::uint32_t s = 0; s < im.cfg.maxStreams; ++s) {
      if (!base::rows::IsLive(sr, s)) continue;
      if (im.pub.streamGen[s] != sr.generation[s]) {  // a new stream in this slot
        im.pub.streamGen[s] = sr.generation[s];
        im.pub.streamStall[s] = 0u;
      }
      const std::uint32_t now = stalls[s];
      if (now > im.pub.streamStall[s]) {
        im.streamStalls += now - im.pub.streamStall[s];
        if (tap.stall != nullptr) tap.stall(tap.user, s, now - im.pub.streamStall[s], im.block);
      }
      im.pub.streamStall[s] = now;
    }
  }
  st.stream_stalls = im.streamStalls;
  st.dropped_events = im.eventRing->dropped.load(std::memory_order_relaxed);
  for (std::uint32_t k = 0; k < SND_MEM_TAG_COUNT; ++k) st.memory_used[k] = 0;
  for (std::uint32_t i = 0; i < im.allocationCount; ++i) {
    const std::uint32_t tag = static_cast<std::uint32_t>(im.allocations[i].tag);
    if (tag < SND_MEM_TAG_COUNT) st.memory_used[tag] += im.allocations[i].bytes;
  }
  st.memory_used[SND_MEM_DSP_POOL] += im.dspReserved.load(std::memory_order_relaxed);
  notify::SnapshotFrame& f = base::notify::BeginSnapshot(*im.triple);
  const std::uint32_t fi = im.triple->back;
  std::uint32_t* written = im.pub.playbackGen + std::size_t{fi} * im.cfg.maxPlaybacks;
  const schema::TableRows& pr = t.Playback.rows;
  const PlaybackStateId* pstate = SND_R(Playback_state);
  const PlaybackLogic* plogic = SND_R(Playback_logic);
  const SlotIndex* ptrans = SND_R(Playback_transportSlot);
  const TransportPosition* tpos = SND_R(Transport_position);
  const TimeSignature* tsig = SND_R(Transport_signature);
  for (std::uint32_t s = 0; s < im.cfg.maxPlaybacks; ++s) {
    const std::uint32_t d = base::rows::DenseOf(pr, s);
    if (d == kNoSlot) {
      if (written[s] != 0u) {
        base::notify::SnapshotStore(&f.playbacks[s], notify::PlaybackSnapshot{});
        written[s] = 0u;
      }
      continue;
    }
    notify::PlaybackSnapshot ps{};
    ps.generation = pr.generation[s];
    ps.state = static_cast<std::uint8_t>(pstate[d]);
    ps.frames = plogic[d].playedFrames;
    ps.transportSlot = ptrans[d];
    const std::uint32_t td = ptrans[d] == kNoSlot ? kNoSlot : base::rows::DenseOf(tr, ptrans[d]);
    if (td != kNoSlot) {
      ps.hasMusical = 1u;
      ps.musical = TicksToMusical(tpos[td].tick, tsig[td]);
    }
    base::notify::SnapshotStore(&f.playbacks[s], ps);
    written[s] = ps.generation;
  }
  const std::uint32_t* tstate = SND_R(Transport_state);
  for (std::uint32_t s = 0; s < im.cfg.maxTransports; ++s) {
    const std::uint32_t d = base::rows::DenseOf(tr, s);
    notify::TransportSnapshot ts{};
    if (d != kNoSlot) {
      ts.generation = tr.generation[s];
      ts.state = tstate[d];
      ts.position = TicksToMusical(tpos[d].tick, tsig[d]);
      ts.beatsPerBar = tsig[d].beatsPerBar;
      ts.bpm = tpos[d].ticksPerSample * im.cfg.sampleRate * 60.0 / SND_TICKS_PER_BEAT;
      ts.clock = tpos[d].anchorClock;
    }
    base::notify::SnapshotStore(&f.transports[s], ts);
  }
  // Buses: index 0 = master (SND_ID_NONE), then every live bus.
  const schema::TableRows& br = t.Bus.rows;
  const Id* bid = SND_R(Bus_busId);
  std::uint32_t nb = 0;
  auto busSnap = [&](SlotIndex s, Id id, std::uint32_t channels) {
    notify::BusSnapshot b{};
    b.busId = id;
    b.channels = channels;
    if (s != kNoSlot) {
      for (std::uint32_t c = 0; c < kMaxChannels; ++c) {
        b.peak[c] = im.rd.peak[s].peak[c];
        b.rms[c] = dsp::RmsMeterValue(im.rd.rms[s], c);
      }
    } else {
      // Output without a master bus: meters from the output buffer.
      for (std::uint32_t c = 0; c < channels && c < kMaxChannels; ++c) {
        b.peak[c] = dsp::Peak(im.rd.outBuf + std::size_t{c} * im.cfg.blockFrames, im.cfg.blockFrames);
        b.rms[c] = static_cast<float>(std::sqrt(dsp::SumOfSquares(im.rd.outBuf + std::size_t{c} * im.cfg.blockFrames,
                                                                  im.cfg.blockFrames) / im.cfg.blockFrames));
      }
    }
    b.loudnessShort = dsp::LoudnessShortTerm(*im.rd.loudness);
    b.loudnessIntegrated = dsp::LoudnessIntegrated(*im.rd.loudness);
    if (nb < f.busCapacity) base::notify::SnapshotStore(&f.buses[nb++], b);
  };
  const SlotIndex master = im.rd.masterSlot != kNoSlot && base::rows::IsLive(br, im.rd.masterSlot) ? im.rd.masterSlot : kNoSlot;
  busSnap(master, master != kNoSlot ? bid[base::rows::DenseOf(br, master)] : 0u,
          master != kNoSlot ? im.busDefs[master].channels : im.cfg.outChannels);
  for (std::uint32_t d = 0; d < br.count; ++d) {
    const SlotIndex s = base::rows::SlotAt(br, d);
    if (s == master) continue;
    busSnap(s, bid[d], im.busDefs[s].channels);
  }
  base::notify::SetFrameHeader(f, im.block + 1u, im.clock + im.cfg.blockFrames, st, nb);
  base::notify::EndSnapshot(*im.triple);
  // 4. Background work of the sources (prefetch, decode, callbacks ahead).
  const source::BlockInfo bi{im.block, im.clock, im.cfg.blockFrames, im.cfg.sampleRate};
  for (std::size_t k = 0; k < static_cast<std::size_t>(SourceKind::Count); ++k) {
    if (im.kinds[k] != nullptr) im.kinds[k]->ScheduleBackground(bi);
  }
}

}  // namespace snd::core
