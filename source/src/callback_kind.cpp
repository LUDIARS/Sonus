// Callback kind (callback_kind.hpp).
#include "callback_kind.hpp"

#include <chrono>
#include <cstring>
#include <new>
#include <thread>

namespace snd::source::detail {

namespace {

constexpr std::size_t Align64(std::size_t n) noexcept { return (n + 63u) & ~std::size_t{63u}; }

// Monotonic clock for the LowLatency deadline (vDSO, no lock / allocation). source
// does not see platform (D-02), so std::chrono.
std::uint64_t NowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint32_t NextPow2(std::uint32_t v) noexcept {
  std::uint32_t p = 1;
  while (p < v) p <<= 1u;
  return p;
}

std::uint32_t Busy(const CallbackRing& r) noexcept {
  return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(r.done)).load(std::memory_order_acquire);
}

// The C++ wrapper's callback (snd/sonus.hpp SynthCallback), carried as snd_fnptr.
using CppSynth = void (*)(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                          const snd_render_context& ctx);

struct Layout {
  std::size_t inst, rings, freeList, data, total;
};

Layout LayoutOf(std::uint32_t insts, std::uint32_t rings, std::uint32_t ringFrames) noexcept {
  Layout l{};
  l.inst = 0;
  l.rings = Align64(l.inst + sizeof(CallbackInst) * insts);
  l.freeList = Align64(l.rings + sizeof(CallbackRing) * rings);
  l.data = Align64(l.freeList + sizeof(std::uint32_t) * insts);
  l.total = l.data + sizeof(float) * std::size_t{rings} * ringFrames * kMaxChannels;
  return l;
}

struct Sizes {
  std::uint32_t insts, rings, ringFrames;
};
Sizes SizesOf(const SourceLimits& l) noexcept {
  Sizes s{};
  s.insts = l.maxVoices == 0u ? 1u : l.maxVoices;
  const std::uint32_t real = l.maxRealVoices == 0u ? 1u : l.maxRealVoices;
  s.rings = real < CallbackKind::kMaxRings ? real : CallbackKind::kMaxRings;
  const std::uint32_t block = l.blockFrames == 0u ? 256u : l.blockFrames;
  const std::uint32_t ahead = l.callbackAheadBlocks == 0u ? 1u : l.callbackAheadBlocks;
  s.ringFrames = NextPow2((ahead + 2u) * block);
  return s;
}

}  // namespace

std::size_t CallbackKind::PoolBytes(const SourceLimits& limits) const noexcept {
  const Sizes s = SizesOf(limits);
  return LayoutOf(s.insts, s.rings, s.ringFrames).total;
}

bool CallbackKind::Init(void* pool, std::size_t bytes, const SourceLimits& limits,
                        const SourceServices& services) noexcept {
  const Sizes s = SizesOf(limits);
  const Layout l = LayoutOf(s.insts, s.rings, s.ringFrames);
  if (pool == nullptr || bytes < l.total) return false;
  auto* base = static_cast<std::uint8_t*>(pool);
  sv_ = services;
  instCap_ = s.insts;
  ringCap_ = s.rings;
  ringFrames_ = s.ringFrames;
  blockFrames_ = limits.blockFrames == 0u ? 256u : limits.blockFrames;
  sampleRate_ = limits.sampleRate == 0u ? 48000u : limits.sampleRate;
  defaultAhead_ = limits.callbackAheadBlocks == 0u ? 1u : limits.callbackAheadBlocks;
  deadlineNs_ = std::uint64_t{blockFrames_} * 1000000000ull / sampleRate_;
  inst_ = reinterpret_cast<CallbackInst*>(base + l.inst);
  rings_ = reinterpret_cast<CallbackRing*>(base + l.rings);
  freeInst_ = reinterpret_cast<std::uint32_t*>(base + l.freeList);
  float* data = reinterpret_cast<float*>(base + l.data);
  for (std::uint32_t i = 0; i < instCap_; ++i) {
    new (&inst_[i]) CallbackInst();
    freeInst_[i] = instCap_ - 1u - i;
  }
  freeCount_ = instCap_;
  for (std::uint32_t r = 0; r < ringCap_; ++r) {
    CallbackRing* ring = new (&rings_[r]) CallbackRing();
    ring->data = data + std::size_t{r} * ringFrames_ * kMaxChannels;
    ring->inst = kInvalidSlot;
    ring->kind = this;
  }
  live_.store(0, std::memory_order_relaxed);
  return true;
}

void CallbackKind::Shutdown() noexcept {
  if (rings_ == nullptr) return;
  // Jobs that did not start skip the callback; running ones finish (no audio thread
  // runs any more, the job system still drains, core pumped its queue before this).
  for (std::uint32_t r = 0; r < ringCap_; ++r) {
    std::atomic_ref<std::uint32_t>(rings_[r].cancel).store(1u, std::memory_order_release);
  }
  for (std::uint32_t r = 0; r < ringCap_; ++r) {
    while (Busy(rings_[r]) != 0u) std::this_thread::yield();
  }
  for (std::uint32_t i = 0; i < instCap_; ++i) inst_[i].~CallbackInst();
  for (std::uint32_t r = 0; r < ringCap_; ++r) rings_[r].~CallbackRing();
  inst_ = nullptr;
  rings_ = nullptr;
  freeInst_ = nullptr;
  freeCount_ = 0;
  live_.store(0, std::memory_order_relaxed);
}

std::uint32_t CallbackKind::AcquireRing(std::uint32_t inst, std::uint64_t pos) noexcept {
  for (std::uint32_t r = 0; r < ringCap_; ++r) {
    CallbackRing& ring = rings_[r];
    if (ring.inst != kInvalidSlot || Busy(ring) != 0u) continue;
    ring.inst = inst;
    ring.written.store(pos, std::memory_order_relaxed);
    ring.consumed.store(pos, std::memory_order_relaxed);
    return r;
  }
  return kInvalidSlot;
}

void CallbackKind::FreeInst(std::uint32_t i) noexcept {
  CallbackInst& in = inst_[i];
  const std::uint32_t r = in.ring.load(std::memory_order_relaxed);
  if (r != kInvalidSlot) rings_[r].inst = kInvalidSlot;
  in.ring.store(kInvalidSlot, std::memory_order_release);
  std::atomic_ref<std::uint32_t>(in.state).store(0u, std::memory_order_release);
  freeInst_[freeCount_++] = i;
  live_.fetch_sub(1u, std::memory_order_relaxed);
}

void CallbackKind::Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SourceStartRequest& rq = requests[k];
    SourceStartResult& res = results[k];
    res = SourceStartResult{kInvalidSlot, SND_END_DETAIL_NONE, 0u, 0u};
    const auto* p = static_cast<const CallbackParams*>(rq.params);
    if (p == nullptr || (p->desc.callback == nullptr && p->desc.cpp_callback == nullptr) || p->desc.channels == 0u) {
      continue;  // core ends it UNSUPPORTED / ERROR
    }
    if (freeCount_ == 0u) {
      res.endDetail = SND_END_DETAIL_VOICE_LIMIT;
      continue;
    }
    const std::uint32_t i = freeInst_[--freeCount_];
    CallbackInst& in = inst_[i];
    in.desc = p->desc;
    in.playback = rq.playback;
    in.channels = p->desc.channels < kMaxChannels ? p->desc.channels : kMaxChannels;
    std::uint32_t ahead = p->desc.ahead_blocks != 0u ? p->desc.ahead_blocks : defaultAhead_;
    std::uint64_t af = std::uint64_t{ahead} * blockFrames_;
    if (af + 2u * blockFrames_ > ringFrames_) af = ringFrames_ - 2u * blockFrames_;  // the ring's own limit
    if (af < blockFrames_) af = blockFrames_;
    in.aheadFrames = static_cast<std::uint32_t>(af);
    in.seq.store(0, std::memory_order_relaxed);
    in.refBeats.store(-1.0, std::memory_order_relaxed);
    in.refBps.store(0.0, std::memory_order_relaxed);
    in.demote.store(0, std::memory_order_relaxed);
    in.muted.store(0, std::memory_order_relaxed);
    in.called.store(0, std::memory_order_relaxed);
    in.waiting = 1;
    in.played = 0;
    in.pos.store(0, std::memory_order_relaxed);
    in.virt = (rq.flags & kStartVirtual) != 0u ? 1u : 0u;
    in.ring.store(kInvalidSlot, std::memory_order_relaxed);
    const bool low = p->desc.mode == SND_SYNTH_LOW_LATENCY || (rq.flags & kStartLowLatency) != 0u;
    in.lowLatency.store(low ? 1u : 0u, std::memory_order_relaxed);
    if (!low && in.virt == 0u) {
      const std::uint32_t r = AcquireRing(i, 0u);
      if (r == kInvalidSlot) rejected_.fetch_add(1u, std::memory_order_relaxed);  // waits (D-97)
      in.ring.store(r, std::memory_order_relaxed);
    }
    // Publishes the instance to the RT workers of this block (they read `state` first).
    std::atomic_ref<std::uint32_t>(in.state).store(1u, std::memory_order_release);
    live_.fetch_add(1u, std::memory_order_relaxed);
    res.sourceSlot = i;
  }
}

void CallbackKind::Release(const SlotIndex* sourceSlots, std::uint32_t count) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SlotIndex i = sourceSlots[k];
    if (i >= instCap_ || inst_[i].state != 1u) continue;
    std::atomic_ref<std::uint32_t>(inst_[i].state).store(2u, std::memory_order_release);  // freed by ScheduleBackground
  }
}

// Seek / loop do not apply to generated sound; pause needs nothing (core does not render
// a paused voice). A virtual voice gives its ring back, a real one wants one (D-97);
// ScheduleBackground does both once no job of the ring runs.
void CallbackKind::Control(const SourceControlOp* ops, std::uint32_t count) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SourceControlOp& op = ops[k];
    if (op.sourceSlot >= instCap_ || inst_[op.sourceSlot].state != 1u) continue;
    if (op.op == ControlOp::Virtualize) inst_[op.sourceSlot].virt = 1u;
    if (op.op == ControlOp::Devirtualize) inst_[op.sourceSlot].virt = 0u;
  }
}

void CallbackKind::QueryReady(const SlotIndex*, std::uint32_t count, std::uint8_t* ready) const noexcept {
  for (std::uint32_t k = 0; k < count; ++k) ready[k] = 1u;
}

void CallbackKind::Call(CallbackInst& in, float* const* out, std::uint32_t channels, std::uint32_t frames,
                        std::uint64_t clock, double beats, std::uint32_t flags) noexcept {
  snd_render_context ctx{};
  ctx.clock_sample = clock;
  ctx.playback = snd_playback_handle{in.playback.Value()};
  ctx.param_start = nullptr;
  ctx.param_end = nullptr;
  ctx.param_count = 0;
  ctx.sample_rate = sampleRate_;
  ctx.block_frames = blockFrames_;
  if (in.called.load(std::memory_order_relaxed) == 0u) {
    flags |= SND_RENDER_FLAG_FIRST_BLOCK;
    in.called.store(1u, std::memory_order_relaxed);
  }
  ctx.flags = flags;
  ctx.transport_beats = beats;
  calls_.fetch_add(1u, std::memory_order_relaxed);
  if (in.desc.callback != nullptr) {
    in.desc.callback(in.desc.user, out, channels, frames, &ctx);
  } else if (in.desc.cpp_callback != nullptr) {
    reinterpret_cast<CppSynth>(in.desc.cpp_callback)(in.desc.user, out, channels, frames, ctx);
  }
}

void CallbackKind::RenderBatch(const source::RenderBatch& batch, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t k = 0; k < batch.count; ++k) {
    const RenderItem& it = batch.items[k];
    VoiceRenderStatus& st = status[k];
    st = VoiceRenderStatus{};
    if (it.sourceSlot >= instCap_) continue;
    CallbackInst& in = inst_[it.sourceSlot];
    if (std::atomic_ref<std::uint32_t>(in.state).load(std::memory_order_acquire) != 1u) continue;
    const std::uint32_t n = it.framesRequested;
    const std::uint32_t off = it.frameOffset;
    const std::uint32_t ch = it.channels < in.channels ? it.channels : in.channels;
    const std::uint64_t pos = it.playhead->frame;
    const std::uint64_t clock = batch.block.clock + off;
    const double bps = it.transportBeats >= 0.0 ? it.transportBeatsPerSample : 0.0;
    const double beats = it.transportBeats >= 0.0 ? it.transportBeats + static_cast<double>(off) * bps : -1.0;
    // Reference point for the render-ahead job (seqlock: odd while writing). No fences
    // (TSan does not model them): the odd mark is an acq_rel RMW, the fields are
    // release stores, so a reader that sees a new field also sees the odd mark.
    const std::uint32_t s0 = in.seq.fetch_add(1u, std::memory_order_acq_rel);
    in.refPos.store(pos, std::memory_order_release);
    in.refClock.store(clock, std::memory_order_release);
    in.refBeats.store(beats, std::memory_order_release);
    in.refBps.store(bps, std::memory_order_release);
    in.seq.store(s0 + 2u, std::memory_order_release);
    it.playhead->frame = pos + n;
    in.pos.store(pos + n, std::memory_order_relaxed);
    st.framesRendered = n;
    if (in.muted.load(std::memory_order_acquire) != 0u) continue;  // demoted, no ring yet: silence
    float* outs[kMaxChannels];
    for (std::uint32_t c = 0; c < ch; ++c) outs[c] = it.out[c] + off;
    if (in.lowLatency.load(std::memory_order_acquire) != 0u) {
      const std::uint64_t t0 = NowNs();
      Call(in, outs, ch, n, clock, beats, SND_RENDER_FLAG_LOW_LATENCY);
      if (NowNs() - t0 > deadlineNs_) {
        // 5.2: muted from the next block, demoted to Buffered (ScheduleBackground).
        in.muted.store(1u, std::memory_order_release);
        in.demote.store(1u, std::memory_order_release);
      }
      continue;
    }
    const std::uint32_t ri = in.ring.load(std::memory_order_acquire);
    if (ri == kInvalidSlot) {  // waiting for a ring (D-97): silence, then a full ring again
      in.waiting = 1;
      continue;
    }
    CallbackRing& ring = rings_[ri];
    const std::uint64_t w = ring.written.load(std::memory_order_acquire);
    const std::uint64_t avail = w > pos ? w - pos : 0u;
    if (in.waiting != 0u && avail >= in.aheadFrames) in.waiting = 0;
    if (in.waiting == 0u) {
      const std::uint64_t take = avail < n ? avail : n;
      for (std::uint64_t f = 0; f < take;) {
        const std::uint64_t at = (pos + f) & (ringFrames_ - 1u);
        std::uint64_t len = take - f;
        if (len > ringFrames_ - at) len = ringFrames_ - at;
        for (std::uint32_t c = 0; c < ch; ++c) {
          std::memcpy(outs[c] + f, RingChannel(ring, c) + at, sizeof(float) * len);
        }
        f += len;
      }
      if (take != 0u) in.played = 1;
      if (take < n) {  // 5.2: silence, warning (starved), then wait for a full ring
        in.waiting = 1;
        if (in.played != 0u) {
          st.starved = 1;
          underruns_.fetch_add(1u, std::memory_order_relaxed);
        }
      }
    }
    ring.consumed.store(pos + n, std::memory_order_release);
  }
}

void CallbackKind::AdvanceVirtual(const AdvanceItem* items, std::uint32_t count, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    status[k] = VoiceRenderStatus{};
    const AdvanceItem& a = items[k];
    a.playhead->frame += a.outputFrames;
    if (a.sourceSlot < instCap_ && inst_[a.sourceSlot].state == 1u) {
      CallbackInst& in = inst_[a.sourceSlot];
      in.pos.store(a.playhead->frame, std::memory_order_relaxed);
      const std::uint32_t ri = in.ring.load(std::memory_order_relaxed);
      if (ri != kInvalidSlot) rings_[ri].consumed.store(a.playhead->frame, std::memory_order_release);
      in.waiting = 1;  // back from virtual: wait for a full ring again
    }
  }
}

void CallbackKind::ScheduleBackground(const BlockInfo&) noexcept {
  if (inst_ == nullptr) return;
  for (std::uint32_t i = 0; i < instCap_; ++i) {
    CallbackInst& in = inst_[i];
    if (in.state == 0u) continue;
    std::uint32_t ri = in.ring.load(std::memory_order_relaxed);
    if (in.state == 2u) {
      if (ri == kInvalidSlot || Busy(rings_[ri]) == 0u) FreeInst(i);
      continue;
    }
    if (in.demote.load(std::memory_order_acquire) != 0u) {
      if (ri == kInvalidSlot) ri = AcquireRing(i, in.pos.load(std::memory_order_relaxed));
      if (ri != kInvalidSlot) {
        in.ring.store(ri, std::memory_order_release);
        in.demote.store(0u, std::memory_order_relaxed);
        demotions_.fetch_add(1u, std::memory_order_relaxed);
        in.lowLatency.store(0u, std::memory_order_release);
        in.muted.store(0u, std::memory_order_release);  // RenderBatch waits for a full ring (waiting)
      }
      continue;
    }
    if (in.lowLatency.load(std::memory_order_relaxed) != 0u) continue;
    // D-97: only a real voice holds a ring.
    if (in.virt != 0u) {
      if (ri != kInvalidSlot && Busy(rings_[ri]) == 0u) {
        rings_[ri].inst = kInvalidSlot;
        in.ring.store(kInvalidSlot, std::memory_order_release);
      }
      continue;
    }
    if (ri == kInvalidSlot) {
      ri = AcquireRing(i, in.pos.load(std::memory_order_relaxed));
      if (ri == kInvalidSlot) continue;  // still waiting
      in.ring.store(ri, std::memory_order_release);
    }
    CallbackRing& ring = rings_[ri];
    if (Busy(ring) != 0u || in.seq.load(std::memory_order_acquire) == 0u) continue;  // no rendered block yet
    const std::uint64_t r = ring.consumed.load(std::memory_order_acquire);
    const std::uint64_t w = ring.written.load(std::memory_order_relaxed);
    if (w >= r + in.aheadFrames + blockFrames_) continue;  // full enough
    ring.owner.store(in.playback.Value(), std::memory_order_relaxed);
    std::atomic_ref<std::uint32_t>(ring.done).store(1u, std::memory_order_release);
    std::atomic_ref<std::uint32_t>(ring.cancel).store(0u, std::memory_order_relaxed);
    JobDesc job{};
    job.fn = &CallbackKind::JobThunk;
    job.data = &ring;
    job.done_counter = &ring.done;
    job.cancel_flag = &ring.cancel;
    job.priority = 0;
    if (sv_.postJob == nullptr || !sv_.postJob(sv_.jobCtx, job, JobLane::Prefetch)) {
      std::atomic_ref<std::uint32_t>(ring.done).store(0u, std::memory_order_release);  // retried next block
    }
  }
}

void CallbackKind::JobThunk(void* data) noexcept {
  auto* ring = static_cast<CallbackRing*>(data);
  ring->kind->RunJob(*ring);
}

// Normal worker: renders from the consumption point up to ahead + 1 block.
void CallbackKind::RunJob(CallbackRing& ring) noexcept {
  if (ring.inst >= instCap_) return;
  CallbackInst& in = inst_[ring.inst];
  if (std::atomic_ref<std::uint32_t>(in.state).load(std::memory_order_acquire) != 1u) return;
  const std::uint64_t r = ring.consumed.load(std::memory_order_acquire);
  std::uint64_t w = ring.written.load(std::memory_order_relaxed);
  if (w < r) w = r;  // frames the render already passed are dropped (5.2)
  const std::uint64_t end = r + in.aheadFrames + blockFrames_;
  // The reference point of the last rendered block (seqlock read).
  std::uint64_t refPos = 0, refClock = 0;
  double refBeats = -1.0, refBps = 0.0;
  for (;;) {
    const std::uint32_t s0 = in.seq.load(std::memory_order_acquire);
    if ((s0 & 1u) != 0u) {
      std::this_thread::yield();
      continue;
    }
    refPos = in.refPos.load(std::memory_order_acquire);
    refClock = in.refClock.load(std::memory_order_acquire);
    refBeats = in.refBeats.load(std::memory_order_acquire);
    refBps = in.refBps.load(std::memory_order_acquire);
    if (in.seq.load(std::memory_order_acquire) == s0) break;
  }
  ring.written.store(w, std::memory_order_release);
  while (w < end) {
    if (std::atomic_ref<std::uint32_t>(ring.cancel).load(std::memory_order_acquire) != 0u) break;
    const std::uint64_t at = w & (ringFrames_ - 1u);
    std::uint64_t len = end - w;
    if (len > blockFrames_) len = blockFrames_;
    if (len > ringFrames_ - at) len = ringFrames_ - at;
    float* outs[kMaxChannels];
    for (std::uint32_t c = 0; c < in.channels; ++c) {
      outs[c] = RingChannel(ring, c) + at;
      std::memset(outs[c], 0, sizeof(float) * len);
    }
    const std::int64_t d = static_cast<std::int64_t>(w) - static_cast<std::int64_t>(refPos);
    const std::uint64_t clock = static_cast<std::uint64_t>(static_cast<std::int64_t>(refClock) + d);
    const double beats = refBeats >= 0.0 ? refBeats + static_cast<double>(d) * refBps : -1.0;
    Call(in, outs, in.channels, static_cast<std::uint32_t>(len), clock, beats, 0u);
    w += len;
    ring.written.store(w, std::memory_order_release);
  }
}

bool CallbackKind::PlaybackBusy(std::uint64_t handle) const noexcept {
  if (rings_ == nullptr || handle == 0u) return false;
  for (std::uint32_t r = 0; r < ringCap_; ++r) {
    // owner is stored before done (release): a busy ring reads its job's owner.
    if (Busy(rings_[r]) != 0u && rings_[r].owner.load(std::memory_order_relaxed) == handle) return true;
  }
  return false;
}

CallbackStats CallbackKind::Stats() const noexcept {
  CallbackStats s;
  s.instances = live_.load(std::memory_order_relaxed);
  s.underruns = underruns_.load(std::memory_order_relaxed);
  s.demotions = demotions_.load(std::memory_order_relaxed);
  s.rejected = rejected_.load(std::memory_order_relaxed);
  s.calls = calls_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace snd::source::detail
