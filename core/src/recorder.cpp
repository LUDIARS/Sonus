// Game side: command recording into the per-thread arenas (2.1,
// commands.hpp "Recording"), thread registration, and the recording API.
#include <bit>
#include <cmath>
#include <cstring>

#include "core_impl.hpp"
#include "snd/internal/rt_check.hpp"

namespace snd::core {

namespace {

enum class Rec : std::uint8_t { Ok = 0, Rejected = 1, Coalesced = 2, Lost = 3 };

void SetBit(std::uint64_t* words, std::uint32_t slot) noexcept { words[slot >> 6] |= std::uint64_t{1} << (slot & 63u); }

void MarkFallback(ArenaView& av) noexcept { av.header->flags |= cmd::kArenaHasFallback; }

template <class P>
Rec Coalesce(Impl&, ArenaView&, const P&) noexcept {
  return Rec::Lost;  // no compact form (D-11): counted in stats.reserve_overflows
}
template <>
Rec Coalesce<cmd::Stop>(Impl& im, ArenaView& av, const cmd::Stop& c) noexcept {
  if (c.playback.slot >= im.cfg.maxPlaybacks) return Rec::Lost;
  SetBit(av.fallback.stopPlayback, c.playback.slot);
  MarkFallback(av);
  return Rec::Coalesced;
}
template <>
Rec Coalesce<cmd::DestroyEmitter>(Impl& im, ArenaView& av, const cmd::DestroyEmitter& c) noexcept {
  if (c.emitter.slot >= im.cfg.maxEmitters) return Rec::Lost;
  SetBit(av.fallback.destroyEmitter, c.emitter.slot);
  MarkFallback(av);
  return Rec::Coalesced;
}
template <>
Rec Coalesce<cmd::DestroyTransport>(Impl& im, ArenaView& av, const cmd::DestroyTransport& c) noexcept {
  if (c.transport.slot >= im.cfg.maxTransports) return Rec::Lost;
  SetBit(av.fallback.destroyTransport, c.transport.slot);
  MarkFallback(av);
  return Rec::Coalesced;
}
template <>
Rec Coalesce<cmd::DestroyUserBuffer>(Impl& im, ArenaView& av, const cmd::DestroyUserBuffer& c) noexcept {
  if (c.buffer.slot >= im.cfg.maxUserBuffers) return Rec::Lost;
  SetBit(av.fallback.destroyUserBuffer, c.buffer.slot);
  MarkFallback(av);
  return Rec::Coalesced;
}
template <>
Rec Coalesce<cmd::BankUnload>(Impl& im, ArenaView& av, const cmd::BankUnload& c) noexcept {
  if (c.bank.slot >= im.cfg.maxBanks) return Rec::Lost;
  SetBit(av.fallback.unloadBank, c.bank.slot);
  MarkFallback(av);
  return Rec::Coalesced;
}
Rec CoalesceParam(ArenaView& av, std::uint8_t scope, std::uint8_t isSwitch, Id id, HandleRef target,
                  std::uint32_t value) noexcept {
  cmd::OverflowFallback& fb = av.fallback;
  for (std::uint32_t i = 0; i < fb.paramCapacity; ++i) {
    cmd::OverflowParam& p = fb.params[i];
    if (p.used == 0u || (p.scope == scope && p.isSwitch == isSwitch && p.id == id && p.target.slot == target.slot &&
                         p.target.generation == target.generation)) {
      p.scope = scope;
      p.isSwitch = isSwitch;
      p.id = id;
      p.target = target;
      p.value = value;
      p.used = 1u;
      MarkFallback(av);
      return Rec::Coalesced;
    }
  }
  ++fb.lostParams;
  return Rec::Lost;
}
template <>
Rec Coalesce<cmd::SetParam>(Impl&, ArenaView& av, const cmd::SetParam& c) noexcept {
  return CoalesceParam(av, c.scope, 0u, c.param, c.target, std::bit_cast<std::uint32_t>(c.value));
}
template <>
Rec Coalesce<cmd::SetSwitch>(Impl&, ArenaView& av, const cmd::SetSwitch& c) noexcept {
  return CoalesceParam(av, c.scope, 1u, c.group, c.target, c.value);
}

void WriteCommand(std::uint8_t* at, cmd::CommandType type, std::uint32_t size, std::uint32_t seq, const void* payload,
                  std::size_t payloadBytes) noexcept {
  std::memset(at, 0, size);
  cmd::CommandHeader h{static_cast<std::uint16_t>(type), static_cast<std::uint16_t>(size), seq};
  std::memcpy(at, &h, sizeof(h));
  if (payloadBytes != 0u) std::memcpy(at + sizeof(h), payload, payloadBytes);
}

// Appends to the producer's current arena following the delivery class.
Rec RecordRaw(Impl& im, Producer& pr, cmd::CommandType type, std::uint32_t size, const void* payload,
              std::size_t payloadBytes, cmd::Delivery delivery) noexcept {
  ArenaView& av = pr.arenas[pr.cycle.Current()];
  cmd::ArenaHeader& h = *av.header;
  if (h.normalBytes + size <= im.cfg.arenaBytes) {
    WriteCommand(av.normal + h.normalBytes, type, size, pr.seq++, payload, payloadBytes);
    h.normalBytes += size;
    ++h.commandCount;
    return Rec::Ok;
  }
  if ((delivery == cmd::Delivery::MustDeliver || delivery == cmd::Delivery::PlayCritical) &&
      h.reserveBytes + size <= im.cfg.reserveBytes) {
    WriteCommand(av.reserve + h.reserveBytes, type, size, pr.seq++, payload, payloadBytes);
    h.reserveBytes += size;
    ++h.commandCount;
    return Rec::Ok;
  }
  return Rec::Rejected;
}

template <class P>
Rec Record(Impl& im, Producer& pr, const P& payload, cmd::Delivery delivery = cmd::CommandTraits<P>::kDelivery) noexcept {
  using T = cmd::CommandTraits<P>;
  const std::size_t bytes = std::is_empty_v<P> ? 0u : sizeof(P);
  const Rec r = RecordRaw(im, pr, T::kType, T::kSize, &payload, bytes, delivery);
  if (r == Rec::Ok) return r;
  ArenaView& av = pr.arenas[pr.cycle.Current()];
  if (delivery == cmd::Delivery::MustDeliver) {
    const Rec c = Coalesce<P>(im, av, payload);
    if (c == Rec::Lost) im.reserveOverflows.fetch_add(1u, std::memory_order_relaxed);
    if (c == Rec::Coalesced) im.reserveOverflows.fetch_add(1u, std::memory_order_relaxed);
    return c;
  }
  ++av.header->droppedCount;
  av.header->flags |= cmd::kArenaDroppedCommands;
  im.rejectedCommands.fetch_add(1u, std::memory_order_relaxed);
  return Rec::Rejected;
}

snd_result ResultOf(Rec r) noexcept {
  return (r == Rec::Ok || r == Rec::Coalesced) ? SND_OK : SND_ERR_COMMAND_BUFFER_FULL;
}

Producer* ProducerOf(Impl& im) noexcept {
  const std::uint32_t p = CurrentProducer(im);
  return p == kNoSlot ? nullptr : &im.producers[p];
}

snd_when WhenOf(const snd_when* w) noexcept {
  if (w != nullptr) return *w;
  snd_when z{};
  z.kind = static_cast<std::uint8_t>(SND_WHEN_IMMEDIATE);
  return z;
}
bool ValidWhen(const snd_when* w) noexcept {
  return w == nullptr || (w->kind <= SND_WHEN_DELAY_US && (w->flags & ~SND_WHEN_FLAG_PICKUP_ALIGN) == 0u);
}

HandleRef Ref(std::uint64_t v) noexcept { return HandleRef::FromValue(v); }

std::uint32_t PriorityHint(const Impl& im, Id event) noexcept {
  const std::uint32_t mask = im.eventPriorityMask;
  for (std::uint32_t i = base::MixU32(event) & mask, n = 0; n <= mask; i = (i + 1u) & mask, ++n) {
    const std::uint64_t e = im.eventPriority[i].load(std::memory_order_acquire);
    if (e == 0u) return 0u;
    if (static_cast<std::uint32_t>(e >> 32) == event) return static_cast<std::uint32_t>(e);
  }
  return 0u;
}

bool TransportLive(const Impl& im, HandleRef t) noexcept {
  if ((t.slot & SND_TRANSPORT_PLAYBACK_BIT) != 0u) {
    return im.playbackSlots.IsCurrent(HandleRef{t.slot & ~SND_TRANSPORT_PLAYBACK_BIT, t.generation});
  }
  return im.transportSlots.IsCurrent(t);
}

// Records a play-family command; on rejection the handle ends REJECTED at the next Update (D-10).
template <class P>
Rec RecordPlay(Impl& im, Producer& pr, const P& c, Id event, bool oneShot) noexcept {
  im.oneShot[c.playback.slot].store(oneShot ? 1u : 0u, std::memory_order_relaxed);
  cmd::Delivery d = cmd::Delivery::Play;
  if (event != 0u && PriorityHint(im, event) >= im.cfg.criticalPriority) d = cmd::Delivery::PlayCritical;
  const Rec r = Record(im, pr, c, d);
  if (r != Rec::Ok) {
    RejectedPlay rp{c.playback.Value(), oneShot ? 1u : 0u, 0u};
    if (!im.rejected.TryPush(rp)) {
      // Cannot happen: at most maxPlaybacks handles exist. Keep the slot leak-free anyway.
      im.playbackSlots.Release(c.playback);
    }
  }
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Publishing the current arena (Update / Flush / Unregister)
// ---------------------------------------------------------------------------
bool PublishProducer(Impl& im, Producer& pr) noexcept {
  ArenaView& cur = pr.arenas[pr.cycle.Current()];
  if (cur.header->commandCount == 0u && (cur.header->flags & cmd::kArenaHasFallback) == 0u &&
      cur.header->droppedCount == 0u) {
    return true;  // nothing to publish
  }
  cur.header->publishSeq = ++pr.publishSeq;
  if (!pr.cycle.Publish()) return false;  // every other arena is with the audio thread: keep appending
  ArenaView& next = pr.arenas[pr.cycle.Current()];
  cmd::ArenaHeader& h = *next.header;
  const bool hadFallback = (h.flags & cmd::kArenaHasFallback) != 0u;
  h.normalBytes = 0;
  h.reserveBytes = 0;
  h.commandCount = 0;
  h.flags = 0;
  h.droppedCount = 0;
  if (hadFallback) {
    cmd::OverflowFallback& fb = next.fallback;
    std::memset(fb.stopPlayback, 0, sizeof(std::uint64_t) * base::notify::BitWords(im.cfg.maxPlaybacks));
    std::memset(fb.destroyEmitter, 0, sizeof(std::uint64_t) * base::notify::BitWords(im.cfg.maxEmitters));
    std::memset(fb.destroyTransport, 0, sizeof(std::uint64_t) * base::notify::BitWords(im.cfg.maxTransports));
    std::memset(fb.destroyUserBuffer, 0, sizeof(std::uint64_t) * base::notify::BitWords(im.cfg.maxUserBuffers));
    std::memset(fb.unloadBank, 0, sizeof(std::uint64_t) * base::notify::BitWords(im.cfg.maxBanks));
    for (std::uint32_t i = 0; i < fb.paramCapacity; ++i) fb.params[i].used = 0u;
    fb.lostParams = 0;
  }
  return true;
}

// Records a BankRegister from the owner thread (loader path). false: not recorded.
bool RecordBankRegister(Impl& im, const cmd::BankRegister& c) noexcept {
  return Record(im, im.producers[0], c) == Rec::Ok;
}
bool RecordBankUnload(Impl& im, const cmd::BankUnload& c) noexcept {
  Rec r = Record(im, im.producers[0], c);
  return r == Rec::Ok || r == Rec::Coalesced;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
snd_result System::RegisterThread() noexcept {
  Impl& im = *impl_;
  if (CurrentProducer(im) != kNoSlot) return SND_OK;
  for (std::uint32_t p = 1; p < im.cfg.maxThreads; ++p) {
    std::uint32_t expected = 0;
    if (im.producers[p].active.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel)) {
      if (!TlsRegister(im, p)) {
        im.producers[p].active.store(0u, std::memory_order_release);
        return SND_ERR_LIMIT_REACHED;
      }
      return SND_OK;
    }
  }
  return SND_ERR_LIMIT_REACHED;
}

snd_result System::UnregisterThread() noexcept {
  Impl& im = *impl_;
  const std::uint32_t p = CurrentProducer(im);
  if (p == kNoSlot) return SND_ERR_THREAD_NOT_REGISTERED;
  if (p == 0u) return SND_ERR_WRONG_THREAD;
  PublishProducer(im, im.producers[p]);
  TlsUnregister(im);
  im.producers[p].active.store(0u, std::memory_order_release);
  return SND_OK;
}

snd_result System::Flush() noexcept {
  Impl& im = *impl_;
  Producer* pr = ProducerOf(im);
  if (pr == nullptr) return SND_ERR_THREAD_NOT_REGISTERED;
  PublishProducer(im, *pr);
  return SND_OK;
}

// ---------------------------------------------------------------------------
// Recording API
// ---------------------------------------------------------------------------
#define SND_PRODUCER_OR(ret)        \
  Impl& im = *impl_;                \
  Producer* pr = ProducerOf(im);    \
  if (pr == nullptr) return ret;

snd_result System::Suspend() noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  return ResultOf(Record(im, *pr, cmd::Suspend{}));
}
snd_result System::Resume() noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  return ResultOf(Record(im, *pr, cmd::Resume{}));
}

snd_bank_handle System::LoadBankAsync(const char* path) noexcept {
  SND_PRODUCER_OR(snd_bank_handle{0})
  (void)pr;
  if (path == nullptr || im.banks == nullptr) return snd_bank_handle{0};
  snd_result r = SND_OK;
  const HandleRef h = im.banks->RequestLoad(path, &r);
  return snd_bank_handle{r == SND_OK ? h.Value() : 0u};
}

snd_result System::UnloadBank(snd_bank_handle bank, snd_unload_mode mode) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  if (im.banks == nullptr) return SND_ERR_INVALID_HANDLE;
  if (mode != SND_UNLOAD_STOP_AND_UNLOAD && mode != SND_UNLOAD_IF_UNUSED) return SND_ERR_INVALID_ARGUMENT;
  const HandleRef h = Ref(bank.value);
  const snd_bank_state before = im.banks->State(h);
  const snd_result r = im.banks->BeginUnload(h, mode, false);
  if (r != SND_OK) return r;
  if (before != SND_BANK_STATE_LOADED && before != SND_BANK_STATE_LOADING) return SND_OK;
  // A bank that core registered (or is about to) is unregistered at a block boundary.
  if (im.banks->State(h) == SND_BANK_STATE_UNLOADING) {
    return ResultOf(Record(im, *pr, cmd::BankUnload{h, mode, 0u}));
  }
  return SND_OK;
}

snd_result System::DeclareLanguage(const char* name) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  if (im.banks == nullptr) return SND_ERR_UNSUPPORTED;
  const snd_result r = im.banks->DeclareLanguage(name);
  if (r != SND_OK) return r;
  cmd::DeclareLanguage c{};
  const std::size_t n = std::strlen(name);
  c.language = HashId(std::string_view(name, n));
  c.nameBytes = static_cast<std::uint32_t>(n);
  std::memcpy(c.name, name, n);
  return ResultOf(Record(im, *pr, c));
}

snd_result System::SetLanguage(Id language) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  if (im.banks == nullptr) return SND_ERR_UNSUPPORTED;
  {
    base::LockGuard lock(im.gameMutex);
    snd_result r = SND_OK;
    const std::uint32_t n = im.banks->SetLanguage(language, im.languagePlan, kMaxLanguageSwitches, &r);
    if (r == SND_ERR_NOT_FOUND) return SND_ERR_NOT_FOUND;
    for (std::uint32_t i = 0; i < n && im.switchCount < kMaxLanguageSwitches; ++i) {
      snd_result lr = SND_OK;
      const HandleRef nh = im.banks->RequestLoad(im.languagePlan[i].newPath, &lr);
      if (lr != SND_OK) continue;
      im.switches[im.switchCount++] = Impl::PendingSwitch{im.languagePlan[i].oldBank, nh};
    }
  }
  return ResultOf(Record(im, *pr, cmd::SetLanguage{language, 0u}));
}

snd_playback_handle System::Play(Id event, snd_emitter_handle emitter, const snd_when* when) noexcept {
  SND_PRODUCER_OR(snd_playback_handle{0})
  if (!ValidWhen(when)) return snd_playback_handle{0};
  const HandleRef h = im.playbackSlots.Acquire();
  if (h.generation == 0u) return snd_playback_handle{0};
  cmd::Play c{};
  c.playback = h;
  c.emitter = Ref(emitter.value);
  c.event = event;
  c.when = WhenOf(when);
  c.priorityHint = PriorityHint(im, event);
  RecordPlay(im, *pr, c, event, false);
  return snd_playback_handle{h.Value()};
}

snd_result System::PlayOneShot(Id event, snd_emitter_handle emitter, const snd_when* when) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  if (!ValidWhen(when)) return SND_ERR_INVALID_ARGUMENT;
  const HandleRef h = im.playbackSlots.Acquire();
  if (h.generation == 0u) return SND_ERR_LIMIT_REACHED;
  cmd::Play c{};
  c.playback = h;
  c.emitter = Ref(emitter.value);
  c.event = event;
  c.flags = cmd::kPlayOneShot;
  c.when = WhenOf(when);
  c.priorityHint = PriorityHint(im, event);
  return ResultOf(RecordPlay(im, *pr, c, event, true));
}

snd_result System::PlayAt(Id event, const snd_vec3* position, const snd_when* when) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  if (position == nullptr || !ValidWhen(when)) return SND_ERR_INVALID_ARGUMENT;
  const HandleRef h = im.playbackSlots.Acquire();
  if (h.generation == 0u) return SND_ERR_LIMIT_REACHED;
  cmd::Play c{};
  c.playback = h;
  c.event = event;
  c.flags = cmd::kPlayOneShot | cmd::kPlayHasPosition;
  c.when = WhenOf(when);
  c.position = *position;
  c.priorityHint = PriorityHint(im, event);
  return ResultOf(RecordPlay(im, *pr, c, event, true));
}

snd_result System::Stop(snd_playback_handle playback, float fadeSeconds, const snd_when* when) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(playback.value);
  if (!im.playbackSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  if (!ValidWhen(when) || std::isnan(fadeSeconds)) return SND_ERR_INVALID_ARGUMENT;
  return ResultOf(Record(im, *pr, cmd::Stop{h, fadeSeconds, 0u, WhenOf(when)}));
}

snd_result System::Pause(snd_playback_handle playback, float fadeSeconds) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(playback.value);
  if (!im.playbackSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  return ResultOf(Record(im, *pr, cmd::PauseResume{h, fadeSeconds, 1u}));
}
snd_result System::ResumePlayback(snd_playback_handle playback, float fadeSeconds) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(playback.value);
  if (!im.playbackSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  return ResultOf(Record(im, *pr, cmd::PauseResume{h, fadeSeconds, 0u}));
}

snd_result System::SetScheduledStop(snd_playback_handle playback, const snd_when* when, float fadeSeconds) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(playback.value);
  if (!im.playbackSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  if (!ValidWhen(when)) return SND_ERR_INVALID_ARGUMENT;
  return ResultOf(Record(im, *pr, cmd::SetScheduledStop{h, fadeSeconds, 0u, WhenOf(when)}));
}

snd_emitter_handle System::CreateEmitter() noexcept {
  SND_PRODUCER_OR(snd_emitter_handle{0})
  const HandleRef h = im.emitterSlots.Acquire();
  if (h.generation == 0u) return snd_emitter_handle{0};
  Record(im, *pr, cmd::CreateEmitter{h});
  return snd_emitter_handle{h.Value()};
}
snd_result System::DestroyEmitter(snd_emitter_handle emitter) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(emitter.value);
  if (!im.emitterSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  return ResultOf(Record(im, *pr, cmd::DestroyEmitter{h}));
}
snd_result System::SetTransform(snd_emitter_handle emitter, const snd_vec3* position, const snd_vec3* forward,
                                const snd_vec3* velocity) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(emitter.value);
  if (!im.emitterSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  cmd::SetTransform c{};
  c.emitter = h;
  if (position != nullptr) {
    c.position = *position;
    c.mask |= cmd::kTransformPosition;
  }
  if (forward != nullptr) {
    c.forward = *forward;
    c.mask |= cmd::kTransformForward;
  }
  if (velocity != nullptr) {
    c.velocity = *velocity;
    c.mask |= cmd::kTransformVelocity;
  }
  return ResultOf(Record(im, *pr, c));
}
snd_result System::SetListener(std::uint32_t index, const snd_listener_desc* listener) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  if (listener == nullptr || index >= im.cfg.maxListeners) return SND_ERR_INVALID_ARGUMENT;
  return ResultOf(Record(im, *pr, cmd::SetListener{index, 0u, *listener}));
}

snd_result System::SetParamGlobal(Id param, float value) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  if (!std::isfinite(value)) return SND_ERR_INVALID_ARGUMENT;
  cmd::SetParam c{};
  c.scope = static_cast<std::uint8_t>(ParamScope::Global);
  c.param = param;
  c.value = value;
  return ResultOf(Record(im, *pr, c));
}
snd_result System::SetParamEmitter(snd_emitter_handle emitter, Id param, float value) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(emitter.value);
  if (!im.emitterSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  if (!std::isfinite(value)) return SND_ERR_INVALID_ARGUMENT;
  cmd::SetParam c{};
  c.scope = static_cast<std::uint8_t>(ParamScope::Emitter);
  c.param = param;
  c.target = h;
  c.value = value;
  return ResultOf(Record(im, *pr, c));
}
snd_result System::SetParamPlayback(snd_playback_handle playback, Id param, float value) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(playback.value);
  if (!im.playbackSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  if (!std::isfinite(value)) return SND_ERR_INVALID_ARGUMENT;
  cmd::SetParam c{};
  c.scope = static_cast<std::uint8_t>(ParamScope::Playback);
  c.param = param;
  c.target = h;
  c.value = value;
  return ResultOf(Record(im, *pr, c));
}
snd_result System::SetSwitchGlobal(Id group, Id value) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  cmd::SetSwitch c{};
  c.scope = static_cast<std::uint8_t>(ParamScope::Global);
  c.group = group;
  c.value = value;
  return ResultOf(Record(im, *pr, c));
}
snd_result System::SetSwitchEmitter(snd_emitter_handle emitter, Id group, Id value) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(emitter.value);
  if (!im.emitterSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  cmd::SetSwitch c{};
  c.scope = static_cast<std::uint8_t>(ParamScope::Emitter);
  c.group = group;
  c.target = h;
  c.value = value;
  return ResultOf(Record(im, *pr, c));
}
snd_result System::SetSwitchPlayback(snd_playback_handle playback, Id group, Id value) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(playback.value);
  if (!im.playbackSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  cmd::SetSwitch c{};
  c.scope = static_cast<std::uint8_t>(ParamScope::Playback);
  c.group = group;
  c.target = h;
  c.value = value;
  return ResultOf(Record(im, *pr, c));
}

snd_result System::SetBusVolume(Id bus, float volume, float fadeSeconds) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  if (!std::isfinite(volume) || volume < 0.0f || !std::isfinite(fadeSeconds)) return SND_ERR_INVALID_ARGUMENT;
  return ResultOf(Record(im, *pr, cmd::SetBusVolume{bus, volume, fadeSeconds < 0 ? 0 : fadeSeconds, 0u}));
}
snd_result System::ApplySnapshot(Id snapshot, float weight, float timeSeconds) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  if (!std::isfinite(weight) || !std::isfinite(timeSeconds)) return SND_ERR_INVALID_ARGUMENT;
  return ResultOf(Record(im, *pr, cmd::ApplySnapshot{snapshot, weight, timeSeconds < 0 ? 0 : timeSeconds, 0u}));
}
snd_result System::SetCategoryVolume(Id category, float volume, float fadeSeconds) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  if (!std::isfinite(volume) || volume < 0.0f || !std::isfinite(fadeSeconds)) return SND_ERR_INVALID_ARGUMENT;
  return ResultOf(Record(im, *pr, cmd::SetCategoryVolume{category, volume, fadeSeconds < 0 ? 0 : fadeSeconds, 0u}));
}
snd_result System::PauseCategory(Id category, bool paused) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  return ResultOf(Record(im, *pr, cmd::PauseCategory{category, paused ? 1u : 0u}));
}
snd_result System::MuteCategory(Id category, bool muted) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  return ResultOf(Record(im, *pr, cmd::MuteCategory{category, muted ? 1u : 0u}));
}

snd_result System::SetMusicState(snd_playback_handle music, Id state, const snd_when* when) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(music.value);
  if (!im.playbackSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  if (!ValidWhen(when)) return SND_ERR_INVALID_ARGUMENT;
  return ResultOf(Record(im, *pr, cmd::SetMusicState{h, state, 0u, WhenOf(when)}));
}
snd_result System::PrepareMusicState(snd_playback_handle music, Id state) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(music.value);
  if (!im.playbackSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  return ResultOf(Record(im, *pr, cmd::PrepareMusicState{h, state, 0u}));
}
snd_transport_handle System::CreateTransport(const snd_transport_desc* desc) noexcept {
  SND_PRODUCER_OR(snd_transport_handle{0})
  if (desc == nullptr || !(desc->bpm > 0.0f) || !std::isfinite(desc->bpm) || desc->beats_per_bar == 0u ||
      (desc->beat_unit != 2u && desc->beat_unit != 4u && desc->beat_unit != 8u && desc->beat_unit != 16u)) {
    return snd_transport_handle{0};
  }
  const HandleRef h = im.transportSlots.Acquire();
  if (h.generation == 0u) return snd_transport_handle{0};
  Record(im, *pr, cmd::CreateTransport{h, *desc});
  return snd_transport_handle{h.Value()};
}
snd_result System::DestroyTransport(snd_transport_handle transport) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(transport.value);
  if ((h.slot & SND_TRANSPORT_PLAYBACK_BIT) != 0u) return SND_ERR_INVALID_ARGUMENT;
  if (!im.transportSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  return ResultOf(Record(im, *pr, cmd::DestroyTransport{h}));
}
snd_result System::SetTransportState(snd_transport_handle transport, snd_transport_state state,
                                     const snd_when* when) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(transport.value);
  if (!TransportLive(im, h)) return SND_ERR_INVALID_HANDLE;
  if (state > SND_TRANSPORT_PAUSED || !ValidWhen(when)) return SND_ERR_INVALID_ARGUMENT;
  return ResultOf(Record(im, *pr, cmd::SetTransportState{h, state, 0u, WhenOf(when)}));
}
snd_result System::SetTempo(snd_transport_handle transport, float bpm, const snd_when* when) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(transport.value);
  if (!TransportLive(im, h)) return SND_ERR_INVALID_HANDLE;
  if (!(bpm > 0.0f) || !std::isfinite(bpm) || !ValidWhen(when)) return SND_ERR_INVALID_ARGUMENT;
  return ResultOf(Record(im, *pr, cmd::SetTempo{h, bpm, 0u, WhenOf(when)}));
}
snd_result System::SeekTransport(snd_transport_handle transport, const snd_musical_time* position,
                                 const snd_when* when) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(transport.value);
  if (!TransportLive(im, h)) return SND_ERR_INVALID_HANDLE;
  if (position == nullptr || !ValidWhen(when)) return SND_ERR_INVALID_ARGUMENT;
  return ResultOf(Record(im, *pr, cmd::SeekTransport{h, *position, 0u, WhenOf(when)}));
}

snd_playback_handle System::PlaySynth(const snd_synth_desc* desc, snd_emitter_handle emitter,
                                      const snd_when* when) noexcept {
  SND_PRODUCER_OR(snd_playback_handle{0})
  if (desc == nullptr || (desc->callback == nullptr && desc->cpp_callback == nullptr) || desc->channels == 0u ||
      desc->channels > kMaxChannels || !ValidWhen(when)) {
    return snd_playback_handle{0};
  }
  const HandleRef h = im.playbackSlots.Acquire();
  if (h.generation == 0u) return snd_playback_handle{0};
  cmd::PlaySynth c{};
  c.playback = h;
  c.emitter = Ref(emitter.value);
  c.when = WhenOf(when);
  c.desc = *desc;
  RecordPlay(im, *pr, c, desc->event_template, false);
  return snd_playback_handle{h.Value()};
}

snd_user_buffer_handle System::CreateUserBuffer(const snd_user_buffer_desc* desc) noexcept {
  SND_PRODUCER_OR(snd_user_buffer_handle{0})
  if (desc == nullptr || desc->channels == nullptr || desc->channel_count == 0u || desc->channel_count > kMaxChannels ||
      desc->frames == 0u || desc->mode > SND_USER_BUFFER_STATIC) {
    return snd_user_buffer_handle{0};
  }
  for (std::uint32_t c = 0; c < desc->channel_count; ++c) {
    if (desc->channels[c] == nullptr) return snd_user_buffer_handle{0};
  }
  const HandleRef h = im.userBufferSlots.Acquire();
  if (h.generation == 0u) return snd_user_buffer_handle{0};
  source::UserBufferControl& ub = im.userBuffers[h.slot];
  for (std::uint32_t c = 0; c < kMaxChannels; ++c) ub.channels[c] = c < desc->channel_count ? desc->channels[c] : nullptr;
  ub.channelCount = desc->channel_count;
  ub.frames = desc->frames;
  ub.sampleRate = desc->sample_rate == 0u ? im.cfg.sampleRate : desc->sample_rate;
  ub.mode = desc->mode;
  ub.loop = desc->loop;
  ub.writePos.store(desc->mode == SND_USER_BUFFER_STATIC ? desc->frames : 0u, std::memory_order_relaxed);
  ub.readPos.store(0u, std::memory_order_relaxed);
  ub.generation.store(h.generation, std::memory_order_release);
  Record(im, *pr, cmd::CreateUserBuffer{h, *desc});
  return snd_user_buffer_handle{h.Value()};
}
snd_result System::DestroyUserBuffer(snd_user_buffer_handle buffer) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const HandleRef h = Ref(buffer.value);
  if (!im.userBufferSlots.IsCurrent(h)) return SND_ERR_INVALID_HANDLE;
  return ResultOf(Record(im, *pr, cmd::DestroyUserBuffer{h}));
}
snd_playback_handle System::PlayUserBuffer(snd_user_buffer_handle buffer, Id bus, snd_emitter_handle emitter,
                                           const snd_when* when) noexcept {
  SND_PRODUCER_OR(snd_playback_handle{0})
  const HandleRef b = Ref(buffer.value);
  if (!im.userBufferSlots.IsCurrent(b) || !ValidWhen(when)) return snd_playback_handle{0};
  const HandleRef h = im.playbackSlots.Acquire();
  if (h.generation == 0u) return snd_playback_handle{0};
  cmd::PlayUserBuffer c{};
  c.playback = h;
  c.emitter = Ref(emitter.value);
  c.buffer = b;
  c.bus = bus;
  c.when = WhenOf(when);
  RecordPlay(im, *pr, c, 0u, false);
  return snd_playback_handle{h.Value()};
}
snd_playback_handle System::PlayInput(const snd_input_desc* desc, snd_emitter_handle emitter,
                                      const snd_when* when) noexcept {
  SND_PRODUCER_OR(snd_playback_handle{0})
  if (desc == nullptr || desc->channels == 0u || desc->channels > kMaxChannels || !ValidWhen(when)) {
    return snd_playback_handle{0};
  }
  const HandleRef h = im.playbackSlots.Acquire();
  if (h.generation == 0u) return snd_playback_handle{0};
  cmd::PlayInput c{};
  c.playback = h;
  c.emitter = Ref(emitter.value);
  c.desc = *desc;
  c.when = WhenOf(when);
  RecordPlay(im, *pr, c, 0u, false);
  return snd_playback_handle{h.Value()};
}

snd_result System::RecordLive(cmd::CommandType type, const void* payload, std::uint32_t payloadBytes) noexcept {
  SND_PRODUCER_OR(SND_ERR_THREAD_NOT_REGISTERED)
  const cmd::CommandInfo* info = cmd::FindCommandInfo(static_cast<std::uint16_t>(type));
  if (info == nullptr || info->origin != cmd::Origin::LiveLink) return SND_ERR_INVALID_ARGUMENT;
  if (payloadBytes + sizeof(cmd::CommandHeader) > info->wireSize || (payloadBytes != 0u && payload == nullptr)) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  if (type == cmd::CommandType::LiveAudition) {
    // Audition plays get a game-side playback slot like any play (one-shot).
    cmd::LiveAudition c{};
    std::memcpy(&c, payload, payloadBytes < sizeof(c) ? payloadBytes : sizeof(c));
    const HandleRef h = im.playbackSlots.Acquire();
    if (h.generation == 0u) return SND_ERR_LIMIT_REACHED;
    c.playback = h;
    c.flags |= cmd::kPlayOneShot;
    im.oneShot[h.slot].store(1u, std::memory_order_relaxed);
    const Rec r = Record(im, *pr, c);
    if (r != Rec::Ok) {
      im.playbackSlots.Release(h);
      return SND_ERR_COMMAND_BUFFER_FULL;
    }
    return SND_OK;
  }
  const Rec r = RecordRaw(im, *pr, type, info->wireSize, payload, payloadBytes, info->delivery);
  if (r == Rec::Ok) return SND_OK;
  im.reserveOverflows.fetch_add(1u, std::memory_order_relaxed);
  return SND_ERR_COMMAND_BUFFER_FULL;
}

#undef SND_PRODUCER_OR

}  // namespace snd::core
