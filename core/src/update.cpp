// Owner thread: Update() (2.1 publish, flags, callbacks, banks) and the
// queries that read the published snapshot (any thread).
#include <bit>
#include <cstring>

#include "core_impl.hpp"

namespace snd::core {

bool PublishProducer(Impl& im, Producer& pr) noexcept;
bool RecordBankRegister(Impl& im, const cmd::BankRegister& c) noexcept;
bool RecordBankUnload(Impl& im, const cmd::BankUnload& c) noexcept;
bool JobsDoneUpTo(const Impl& im, std::uint32_t ticket) noexcept;

namespace {

void InsertEventPriority(Impl& im, Id id, std::uint32_t priority) noexcept {
  const std::uint32_t mask = im.eventPriorityMask;
  const std::uint64_t v = (static_cast<std::uint64_t>(id) << 32) | priority;
  for (std::uint32_t i = base::MixU32(id) & mask, n = 0; n <= mask; i = (i + 1u) & mask, ++n) {
    const std::uint64_t e = im.eventPriority[i].load(std::memory_order_relaxed);
    if (e == 0u || static_cast<std::uint32_t>(e >> 32) == id) {
      im.eventPriority[i].store(v, std::memory_order_release);
      return;
    }
  }
}

void DeliverEnd(Impl& im, SlotIndex slot, std::uint32_t generation, std::uint32_t reason, std::uint32_t detail) {
  const HandleRef h{slot, generation};
  const bool oneShot = im.oneShot[slot].load(std::memory_order_relaxed) != 0u;
  if (!oneShot && im.callbacks.on_end != nullptr) {
    im.callbacks.on_end(im.callbacks.user, snd_playback_handle{h.Value()}, reason, detail);
  }
  im.playbackSlots.Release(h);
}

void ProcessBanks(Impl& im) noexcept {
  if (im.banks == nullptr) return;
  im.banks->Update();  // PumpIo + Pump (10.3)
  // Registrations that did not fit the arena last time come first (order kept).
  std::uint32_t kept = 0;
  for (std::uint32_t i = 0; i < im.pendingRegCount; ++i) {
    if (kept != 0u || !RecordBankRegister(im, im.pendingRegs[i])) im.pendingRegs[kept++] = im.pendingRegs[i];
  }
  im.pendingRegCount = kept;
  cmd::BankRegister reg{};
  while (im.banks->PopRegistration(&reg)) {
    if (!PrepareBankRecord(im, reg)) {
      // Out of memory for core's per-bank data: drop the bank again.
      im.banks->BeginUnload(reg.bank, SND_UNLOAD_STOP_AND_UNLOAD, false);
      im.banks->Release(reg.bank);
      if (im.callbacks.on_error != nullptr) {
        im.callbacks.on_error(im.callbacks.user, SND_ERR_OUT_OF_BUDGET, 0u, reg.bank.Value());
      }
      continue;
    }
    // Game-side priority table (D-10).
    const BankRecord& rec = *im.records[reg.bank.slot];
    const auto prio = rec.view->Column<bk::col::EVNT_priority>();
    for (std::uint32_t i = 0; i < rec.eventCount; ++i) InsertEventPriority(im, rec.eventIds[i], prio[rec.eventRows[i]]);
    if (im.pendingRegCount != 0u || !RecordBankRegister(im, reg)) im.pendingRegs[im.pendingRegCount++] = reg;
  }
  // Audio -> owner bank events.
  BankEvent e;
  while (im.bankEvents.TryPop(e)) {
    const HandleRef h{e.slot, e.generation};
    switch (e.kind) {
      case BankEventKind::Release:
        for (std::uint32_t i = 0; i < im.cfg.maxBanks; ++i) {
          Impl::ReleaseCandidate& c = im.releaseCandidates[i];
          if (c.used == 0u) {
            c = Impl::ReleaseCandidate{e.slot, e.generation, im.jobEpoch.load(std::memory_order_relaxed), 1u};
            break;
          }
        }
        break;
      case BankEventKind::UnloadRefused:
        im.banks->AbortUnload(h);
        if (im.callbacks.on_error != nullptr) {
          im.callbacks.on_error(im.callbacks.user, SND_ERR_BANK_IN_USE, 0u, h.Value());
        }
        break;
      case BankEventKind::Unregistered: {  // a language drain ended: now really unloading
        base::LockGuard lock(im.gameMutex);
        for (std::uint32_t i = 0; i < im.drainingCount; ++i) {
          if (im.draining[i].slot == h.slot && im.draining[i].generation == h.generation) {
            im.draining[i] = im.draining[--im.drainingCount];
            break;
          }
        }
        im.banks->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false);
        break;
      }
      case BankEventKind::RegisterFailed:
        FreeBankRecord(im, e.slot);
        im.banks->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false);
        im.banks->Release(h);
        if (im.callbacks.on_error != nullptr) {
          im.callbacks.on_error(im.callbacks.user, SND_ERR_OUT_OF_BUDGET, 0u, h.Value());
        }
        break;
    }
  }
  // Release once every source job posted before the unregistration is done (epoch, 3.3).
  for (std::uint32_t i = 0; i < im.cfg.maxBanks; ++i) {
    Impl::ReleaseCandidate& c = im.releaseCandidates[i];
    if (c.used == 0u || !JobsDoneUpTo(im, c.ticket)) continue;
    FreeBankRecord(im, c.slot);
    im.banks->Release(HandleRef{c.slot, c.generation});
    c.used = 0u;
  }
  // Language switches (10.4): drop the old bank once the new one is loaded.
  {
    base::LockGuard lock(im.gameMutex);
    std::uint32_t keep = 0;
    for (std::uint32_t i = 0; i < im.switchCount; ++i) {
      const Impl::PendingSwitch s = im.switches[i];
      const snd_bank_state ns = im.banks->State(s.newBank);
      if (ns == SND_BANK_STATE_LOADED) {
        // Unloaded when its last voice ended (10.4); LOADED for the game until then.
        if (im.banks->State(s.oldBank) == SND_BANK_STATE_LOADED && im.drainingCount < kMaxLanguageSwitches &&
            RecordBankUnload(im, cmd::BankUnload{s.oldBank, kUnloadWhenUnused, 0u})) {
          im.draining[im.drainingCount++] = s.oldBank;
        }
      } else if (ns == SND_BANK_STATE_LOADING) {
        im.switches[keep++] = s;
      }
    }
    im.switchCount = keep;
  }
  // on_bank_loaded, once per load (D-16).
  for (std::uint32_t s = 0; s < im.cfg.maxBanks; ++s) {
    notify::BankSlotState& st = im.bankStates[s];
    const std::uint32_t state = st.state.load(std::memory_order_acquire);
    if ((state != SND_BANK_STATE_LOADED && state != SND_BANK_STATE_FAILED) ||
        st.notified.load(std::memory_order_relaxed) != 0u) {
      continue;
    }
    st.notified.store(1u, std::memory_order_relaxed);
    if (im.callbacks.on_bank_loaded != nullptr) {
      const HandleRef h{s, st.generation.load(std::memory_order_relaxed)};
      im.callbacks.on_bank_loaded(im.callbacks.user, snd_bank_handle{h.Value()},
                                  state == SND_BANK_STATE_LOADED ? SND_OK : st.result.load(std::memory_order_relaxed));
    }
  }
}

}  // namespace

snd_result System::Update() noexcept {
  Impl& im = *impl_;
  if (!IsOwnerThread(im)) return SND_ERR_WRONG_THREAD;
  PumpServices();
  ProcessBanks(im);
  PublishProducer(im, im.producers[0]);
  // Retiring producers (UnregisterThread with an unpublished arena).
  for (std::uint32_t p = 1; p < im.cfg.maxThreads; ++p) {
    Producer& pr = im.producers[p];
    std::uint32_t expected = 2u;  // 2 -> 3: the thread cannot take it back meanwhile
    if (!pr.active.compare_exchange_strong(expected, 3u, std::memory_order_acq_rel)) continue;
    pr.active.store(PublishProducer(im, pr) ? 0u : 2u, std::memory_order_release);
  }

  // Ends (lossless, D-16) and slot returns (D-46).
  base::notify::ConsumeEnds(im.endFlags, [&](SlotIndex slot, const notify::EndRecord& rec) {
    DeliverEnd(im, slot, rec.generation, rec.reason, rec.detail);
  });
  RejectedPlay rp;
  while (im.rejected.TryPop(rp)) {
    const HandleRef h = HandleRef::FromValue(rp.handle);
    DeliverEnd(im, h.slot, h.generation, SND_END_REJECTED, SND_END_DETAIL_COMMAND_OVERFLOW);
  }
  base::notify::ConsumeReleases(im.emitterRelease, [&](SlotIndex slot, std::uint32_t gen) {
    im.emitterSlots.Release(HandleRef{slot, gen});
  });
  base::notify::ConsumeReleases(im.transportRelease, [&](SlotIndex slot, std::uint32_t gen) {
    im.transportSlots.Release(HandleRef{slot, gen});
  });
  base::notify::ConsumeReleases(im.userBufferRelease, [&](SlotIndex slot, std::uint32_t gen) {
    source::UserBufferControl& ub = im.userBuffers[slot];
    std::uint32_t expected = gen;
    ub.generation.compare_exchange_strong(expected, 0u, std::memory_order_acq_rel);
    im.userBufferSlots.Release(HandleRef{slot, gen});
  });

  // Event ring (beats, markers, logs, errors).
  base::notify::SwapFront(*im.triple);
  const base::notify::FrameHeader fh = [&] {
    base::notify::FrameHeader h{};
    base::notify::ReadFront(*im.triple, [&](const notify::SnapshotFrame& f) { h = base::notify::GetFrameHeader(f); });
    return h;
  }();
  const double rate = static_cast<double>(im.cfg.sampleRate);
  base::notify::DrainEvents(*im.eventRing, [&](const notify::EventRecord& r) {
    const snd_callbacks& cb = im.callbacks;
    snd_timing timing{r.clock, (static_cast<double>(r.clock) - static_cast<double>(fh.clock)) / rate +
                                   im.outputLatency.load(std::memory_order_relaxed) +
                                   double(im.config.render_ahead_blocks) * im.cfg.blockFrames / rate};
    switch (static_cast<notify::EventType>(r.type)) {
      case notify::EventType::Beat:
        if (cb.on_beat != nullptr) {
          const snd_musical_time pos{static_cast<std::int32_t>(r.a), static_cast<std::int32_t>(r.b), 0};
          cb.on_beat(cb.user, snd_transport_handle{r.object}, &pos, &timing);
        }
        break;
      case notify::EventType::Marker:
        if (cb.on_marker != nullptr) cb.on_marker(cb.user, snd_playback_handle{r.object}, r.a, &timing);
        break;
      case notify::EventType::Log:
        if (cb.on_log != nullptr) {
          cb.on_log(cb.user, r.a, reinterpret_cast<const char*>(static_cast<std::uintptr_t>(r.object)), r.arg0, r.arg1);
        }
        break;
      case notify::EventType::Error:
      case notify::EventType::Warning:
      case notify::EventType::ReservationLate:
        if (cb.on_error != nullptr) cb.on_error(cb.user, static_cast<snd_result>(r.a), r.detail, r.object);
        break;
      default:
        break;
    }
  });
  return SND_OK;
}

snd_result System::SetCallbacks(const snd_callbacks* callbacks) noexcept {
  Impl& im = *impl_;
  if (!IsOwnerThread(im)) return SND_ERR_WRONG_THREAD;
  im.callbacks = callbacks != nullptr ? *callbacks : snd_callbacks{};
  return SND_OK;
}

// ---------------------------------------------------------------------------
// User buffers (5.1)
// ---------------------------------------------------------------------------
std::uint32_t System::WriteUserBuffer(snd_user_buffer_handle buffer, const float* const* channels,
                                      std::uint32_t frames) noexcept {
  Impl& im = *impl_;
  const HandleRef h = HandleRef::FromValue(buffer.value);
  if (channels == nullptr || !im.userBufferSlots.IsCurrent(h)) return 0;
  source::UserBufferControl& ub = im.userBuffers[h.slot];
  if (ub.generation.load(std::memory_order_acquire) != h.generation) return 0;
  const std::uint64_t w = ub.writePos.load(std::memory_order_relaxed);
  std::uint64_t n = frames;
  if (ub.mode == SND_USER_BUFFER_STREAM) {
    const std::uint64_t r = ub.readPos.load(std::memory_order_acquire);
    const std::uint64_t used = w - r;
    const std::uint64_t space = used >= ub.frames ? 0u : ub.frames - used;
    if (n > space) n = space;
  } else {
    // Static: rewrite from the start (only while stopped, 5.1); capacity bound.
    if (n > ub.frames) n = ub.frames;
  }
  for (std::uint32_t c = 0; c < ub.channelCount; ++c) {
    if (channels[c] == nullptr) return 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::uint64_t pos = ub.mode == SND_USER_BUFFER_STREAM ? (w + i) % ub.frames : i;
      ub.channels[c][pos] = channels[c][i];
    }
  }
  ub.writePos.store(ub.mode == SND_USER_BUFFER_STREAM ? w + n : n, std::memory_order_release);
  return static_cast<std::uint32_t>(n);
}

std::int32_t System::GetUserBufferReleased(snd_user_buffer_handle buffer) const noexcept {
  const Impl& im = *impl_;
  const HandleRef h = HandleRef::FromValue(buffer.value);
  if (!im.userBufferSlots.Issuable(h)) return 0;
  return im.userBufferSlots.IsCurrent(h) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Queries (any thread; values as of the latest Update, 2.1)
// ---------------------------------------------------------------------------
namespace {
bool ReadPlayback(const Impl& im, HandleRef h, notify::PlaybackSnapshot* out, base::notify::FrameHeader* hdr) noexcept {
  if (h.slot >= im.cfg.maxPlaybacks || h.generation == 0u) return false;
  bool ok = false;
  base::notify::ReadFront(*im.triple, [&](const notify::SnapshotFrame& f) {
    if (hdr != nullptr) *hdr = base::notify::GetFrameHeader(f);
    *out = base::notify::SnapshotLoad(&f.playbacks[h.slot]);
    ok = out->generation == h.generation;
  });
  return ok;
}
}  // namespace

snd_playback_state System::GetPlaybackState(snd_playback_handle playback) const noexcept {
  notify::PlaybackSnapshot s{};
  if (!ReadPlayback(*impl_, HandleRef::FromValue(playback.value), &s, nullptr)) return SND_PLAYBACK_STATE_NONE;
  return s.state;
}

snd_result System::GetPosition(snd_playback_handle playback, snd_position* out) const noexcept {
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  notify::PlaybackSnapshot s{};
  base::notify::FrameHeader hdr{};
  if (!ReadPlayback(*impl_, HandleRef::FromValue(playback.value), &s, &hdr)) return SND_ERR_INVALID_HANDLE;
  *out = snd_position{};
  out->frames = s.frames;
  out->seconds = static_cast<double>(s.frames) / impl_->cfg.sampleRate;
  out->musical = s.musical;
  out->has_musical = s.hasMusical;
  out->clock_sample = hdr.clock;
  return SND_OK;
}

snd_result System::GetTransportPosition(snd_transport_handle transport, snd_position* out) const noexcept {
  const Impl& im = *impl_;
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  HandleRef h = HandleRef::FromValue(transport.value);
  std::uint32_t slot = h.slot;
  std::uint32_t gen = h.generation;
  if ((h.slot & SND_TRANSPORT_PLAYBACK_BIT) != 0u) {
    notify::PlaybackSnapshot ps{};
    if (!ReadPlayback(im, HandleRef{h.slot & ~SND_TRANSPORT_PLAYBACK_BIT, h.generation}, &ps, nullptr) ||
        ps.transportSlot >= im.cfg.maxTransports) {
      return SND_ERR_INVALID_HANDLE;
    }
    slot = ps.transportSlot;
    gen = 0;  // the owner playback's generation was checked
  }
  if (slot >= im.cfg.maxTransports) return SND_ERR_INVALID_HANDLE;
  bool ok = false;
  base::notify::ReadFront(*im.triple, [&](const notify::SnapshotFrame& f) {
    const notify::TransportSnapshot t = base::notify::SnapshotLoad(&f.transports[slot]);
    const base::notify::FrameHeader hdr = base::notify::GetFrameHeader(f);
    ok = t.generation != 0u && (gen == 0u || t.generation == gen);
    *out = snd_position{};
    out->musical = t.position;
    out->has_musical = 1u;
    out->clock_sample = hdr.clock;
  });
  return ok ? SND_OK : SND_ERR_INVALID_HANDLE;
}

snd_result System::GetMeter(Id bus, snd_meter* out) const noexcept {
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  bool found = false, ready = false;
  base::notify::ReadFront(*impl_->triple, [&](const notify::SnapshotFrame& f) {
    const base::notify::FrameHeader hdr = base::notify::GetFrameHeader(f);
    ready = hdr.block != 0u;
    found = false;
    for (std::uint32_t i = 0; i < hdr.busCount; ++i) {
      const notify::BusSnapshot b = base::notify::SnapshotLoad(&f.buses[i]);
      if ((bus == 0u && i == 0u) || (bus != 0u && b.busId == bus)) {
        *out = snd_meter{};
        out->channels = b.channels;
        for (std::uint32_t c = 0; c < kMaxChannels; ++c) {
          out->peak[c] = b.peak[c];
          out->rms[c] = b.rms[c];
        }
        out->loudness_short = b.loudnessShort;
        out->loudness_integrated = b.loudnessIntegrated;
        out->clock_sample = hdr.clock;
        found = true;
        break;
      }
    }
  });
  if (!ready) return SND_ERR_NOT_READY;
  return found ? SND_OK : SND_ERR_NOT_FOUND;
}

snd_result System::GetStats(snd_stats* out) const noexcept {
  const Impl& im = *impl_;
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  bool ready = false;
  base::notify::ReadFront(*im.triple, [&](const notify::SnapshotFrame& f) {
    const base::notify::FrameHeader hdr = base::notify::GetFrameHeader(f);
    *out = hdr.stats;
    ready = hdr.block != 0u;
  });
  out->rejected_commands += im.rejectedCommands.load(std::memory_order_relaxed);
  out->reserve_overflows += im.reserveOverflows.load(std::memory_order_relaxed);
  return ready ? SND_OK : SND_ERR_NOT_READY;
}

snd_bank_state System::GetBankState(snd_bank_handle bank) const noexcept {
  const Impl& im = *impl_;
  if (im.banks == nullptr) return SND_BANK_STATE_UNLOADED;
  return im.banks->State(HandleRef::FromValue(bank.value));
}

snd_result System::GetBankChunk(snd_bank_handle bank, std::uint32_t fourcc, const void** data,
                                std::uint64_t* bytes) const noexcept {
  const Impl& im = *impl_;
  if (!IsOwnerThread(im)) return SND_ERR_WRONG_THREAD;
  if (data == nullptr || bytes == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (im.banks == nullptr) return SND_ERR_INVALID_HANDLE;
  const HandleRef h = HandleRef::FromValue(bank.value);
  const snd_bank_state st = im.banks->State(h);
  if (st == SND_BANK_STATE_UNLOADED && im.banks->Result(h) == SND_ERR_INVALID_HANDLE) return SND_ERR_INVALID_HANDLE;
  if (st != SND_BANK_STATE_LOADED) return SND_ERR_NOT_READY;
  const bk::PreparedBank* pb = im.banks->Prepared(h);
  if (pb == nullptr) return SND_ERR_NOT_READY;
  const bk::ByteSpan s = pb->view.ApplicationChunk(fourcc);
  if (s.data == nullptr) return SND_ERR_NOT_FOUND;
  *data = s.data;
  *bytes = s.bytes;
  return SND_OK;
}

}  // namespace snd::core
