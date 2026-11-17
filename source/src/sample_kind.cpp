// Sample kind (sample_kind.hpp).
#include "sample_kind.hpp"

#include <new>
#include <thread>

namespace snd::source::detail {

namespace {

// Writes decoded frames into a RingCtx's rings (the job, or the audio thread while no job runs).
class RingSink final : public OutputSink {
 public:
  RingSink(RingCtx& c, std::uint32_t frames, std::uint64_t limit) noexcept
      : c_(c), mask_(frames - 1u), frames_(frames), w_(c.w.load(std::memory_order_relaxed)), limit_(limit) {}
  std::uint32_t Room() const noexcept override {
    const std::uint64_t used = w_ - c_.r.load(std::memory_order_acquire);
    std::uint64_t room = used >= frames_ ? 0u : frames_ - used;
    if (room > limit_) room = limit_;
    return static_cast<std::uint32_t>(room);
  }
  void Put(const float* const* src, std::uint32_t offset, std::uint32_t n) noexcept override {
    for (std::uint32_t i = 0; i < n;) {
      const std::uint32_t at = static_cast<std::uint32_t>(w_ & mask_);
      std::uint32_t k = frames_ - at;
      if (k > n - i) k = n - i;
      for (std::uint32_t ch = 0; ch < c_.channels; ++ch) {
        std::memcpy(c_.ring[ch] + at, src[ch] + offset + i, sizeof(float) * k);
      }
      w_ += k;
      i += k;
    }
    limit_ -= n;
  }
  void Publish() noexcept { c_.w.store(w_, std::memory_order_release); }

 private:
  RingCtx& c_;
  std::uint32_t mask_;
  std::uint32_t frames_;
  std::uint64_t w_;
  std::uint64_t limit_;
};


}  // namespace

// ---------------------------------------------------------------------------
// Layout and lifetime
// ---------------------------------------------------------------------------
void SampleKind::Layout(Carver& c, const SourceLimits& l) noexcept {
  const std::uint32_t ringFrames = NextPow2(l.voiceRingFrames < 256u ? 256u : l.voiceRingFrames);
  const std::uint32_t ctxs = l.maxDecoders;
  if (c.base == nullptr) {
    c.Take<SampleInst>(l.maxVoices);
    c.Take<std::uint32_t>(l.maxVoices);
    c.Take<RingCtx>(ctxs);
    c.Take<std::uint32_t>(ctxs);
    c.Take<std::uint32_t>(ctxs);
    RingPool::Measure(c, ctxs * 2u, ringFrames);
    MeasureCarry(c, ctxs);
    PcmCache::Measure(c, l);
    return;
  }
  inst_ = c.Take<SampleInst>(l.maxVoices);
  std::uint32_t* fi = c.Take<std::uint32_t>(l.maxVoices);
  ctx_ = c.Take<RingCtx>(ctxs);
  std::uint32_t* fc = c.Take<std::uint32_t>(ctxs);
  active_ = c.Take<std::uint32_t>(ctxs);
  if (inst_ == nullptr || fi == nullptr || (ctxs != 0u && (ctx_ == nullptr || fc == nullptr || active_ == nullptr))) {
    c.ok = false;
    return;
  }
  instCap_ = l.maxVoices;
  instFree_.Bind(fi, instCap_);
  for (std::uint32_t i = 0; i < instCap_; ++i) {
    inst_[i] = SampleInst{};
    inst_[i].ctx = kNone;
    inst_[i].entry = kNone;
  }
  if (!rings_.Bind(c, ctxs * 2u, ringFrames)) {
    if (ctxs != 0u) c.ok = false;
  }
  float* carry = c.Take<float>(std::size_t{ctxs} * kMaxChannels * kCarryFrames);
  ctxCap_ = ctxs;
  if (ctxs != 0u) ctxFree_.Bind(fc, ctxs);
  for (std::uint32_t i = 0; i < ctxs; ++i) {
    RingCtx* r = new (&ctx_[i]) RingCtx();
    r->inst = kNone;
    r->kind = this;
    r->jobCounter = 0;
    r->cancel = 0;
    for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
      r->rings[ch] = kNone;
      r->ring[ch] = nullptr;
      r->dec.carry[ch] = carry != nullptr ? carry + (std::size_t{i} * kMaxChannels + ch) * kCarryFrames : nullptr;
    }
  }
  if (!cache_.Bind(c, l)) c.ok = false;
}

std::size_t SampleKind::PoolBytes(const SourceLimits& limits) const noexcept {
  Carver c;
  const_cast<SampleKind*>(this)->Layout(c, limits);
  return c.used;
}

bool SampleKind::Init(void* pool, std::size_t bytes, const SourceLimits& limits, const SourceServices& services) noexcept {
  sv_ = services;
  lim_ = limits;
  Carver c{static_cast<std::uint8_t*>(pool), 0, bytes, true};
  Layout(c, limits);
  if (!c.ok) return false;
  if (limits.maxDecoders != 0u) {
    if (!sh_.EnsureDecoders(limits, services.allocator)) return false;
    decoders_ = true;
  }
  const std::uint32_t half = rings_.frames / 2u;
  const std::uint32_t want = 8u * (limits.blockFrames == 0u ? 256u : limits.blockFrames);
  readyFrames_ = want < half ? want : half;
  live_ = 0;
  activeCount_ = 0;
  block_ = 0;
  return true;
}

void SampleKind::Shutdown() noexcept {
  for (std::uint32_t i = 0; i < ctxCap_; ++i) std::atomic_ref<std::uint32_t>(ctx_[i].cancel).store(1u, std::memory_order_release);
  for (std::uint32_t i = 0; i < ctxCap_; ++i) {
    while (JobBusy(ctx_[i].jobCounter)) std::this_thread::yield();
    if (ctx_[i].inst != kNone && sh_.decodersReady) sh_.decoders.Release(ctx_[i].slot);
    ctx_[i].~RingCtx();
  }
  cache_.Shutdown(sh_);
  ctxCap_ = 0;
  instCap_ = 0;
  live_ = 0;
  if (decoders_) sh_.ReleaseDecoders();
  decoders_ = false;
}

// ---------------------------------------------------------------------------
// Start / Release / Control (audio thread, RunEventLogic)
// ---------------------------------------------------------------------------
void SampleKind::DecodeRing(RingCtx& c, std::uint64_t limit) noexcept {
  ResidentInput in(*c.dec.asset, c.inputOff);
  RingSink sink(c, rings_.frames, limit);
  c.dec.Run(in, sink);
  sink.Publish();
  if (c.dec.error != 0u) c.error.store(c.dec.error, std::memory_order_release);
  if (c.dec.ended) c.ended.store(1u, std::memory_order_release);
}

void SND_CALL SampleKind::RingJob(void* data) {
  RingCtx& c = *static_cast<RingCtx*>(data);
  if (std::atomic_ref<std::uint32_t>(c.cancel).load(std::memory_order_acquire) != 0u) return;
  c.kind->DecodeRing(c, ~std::uint64_t{0});
}

void SampleKind::SeekRing(RingCtx& c, std::uint64_t target, std::uint32_t& inlineBudget) noexcept {
  // No job of c runs (caller's duty) and Render does not read c (instance not primed).
  const std::uint64_t r = c.r.load(std::memory_order_relaxed);
  c.w.store(r, std::memory_order_relaxed);
  c.ended.store(0u, std::memory_order_relaxed);
  c.error.store(0u, std::memory_order_relaxed);
  c.dec.error = 0;
  if (c.inst != kNone) c.dec.loop = inst_[c.inst].loop;  // SetLoop (Control) reaches the decoder here
  std::uint64_t byte = 0;
  c.dec.SeekTo(target, &byte);
  c.inputOff = byte;
  if (c.dec.ended) c.ended.store(1u, std::memory_order_release);
  c.pendingSeek = 0;
  if (inlineBudget != 0u && !c.dec.ended) {
    --inlineBudget;
    DecodeRing(c, readyFrames_);
    Count(sh_, kInlinePrimes);
  }
}

bool SampleKind::StartRing(SampleInst& in, std::uint32_t instIndex, std::uint64_t start, std::uint32_t& inlineBudget,
                           std::uint32_t* detail) noexcept {
  ICodec* codec = FindCodec(sv_, in.a.h.codec);
  if (codec == nullptr || (codec::IsBuiltinCodec(in.a.h.codec) && codec::ValidateHeader(in.a.h) != codec::HeaderError::None) ||
      in.a.data == nullptr) {
    *detail = SND_END_DETAIL_DECODE;
    Count(sh_, kRejectedOther);
    return false;
  }
  const std::uint32_t ci = ctxFree_.Pop();
  if (ci == kNone || !sh_.decodersReady) {
    if (ci != kNone) ctxFree_.Push(ci);
    *detail = SND_END_DETAIL_DECODER_POOL_FULL;
    Count(sh_, kRejectedDecoder);
    return false;
  }
  RingCtx& c = ctx_[ci];
  const std::uint32_t ch = in.a.h.channels;
  codec::DecoderSlot ds;
  if (!rings_.Acquire(ch, c.rings)) {
    ctxFree_.Push(ci);
    *detail = SND_END_DETAIL_DECODER_POOL_FULL;
    Count(sh_, kRejectedDecoder);
    return false;
  }
  if (sh_.decoders.Acquire(codec->StateBytes(in.a.h), &ds) != codec::DecoderPoolStatus::Ok) {
    rings_.Release(ch, c.rings);
    ctxFree_.Push(ci);
    *detail = SND_END_DETAIL_DECODER_POOL_FULL;
    Count(sh_, kRejectedDecoder);
    return false;
  }
  c.slot = ds;
  c.channels = ch;
  for (std::uint32_t k = 0; k < ch; ++k) c.ring[k] = rings_.Ring(c.rings[k]);
  c.inst = instIndex;
  c.dec.codec = codec;
  c.dec.state = ds.state;
  c.dec.asset = &in.a;
  c.dec.loop = in.loop;
  c.dec.channels = ch;
  c.r.store(0, std::memory_order_relaxed);
  c.w.store(0, std::memory_order_relaxed);
  std::atomic_ref<std::uint32_t>(c.cancel).store(0u, std::memory_order_relaxed);
  codec->Init(ds.state, in.a.h);
  c.listPos = activeCount_;
  active_[activeCount_++] = ci;
  in.ctx = ci;
  SeekRing(c, start, inlineBudget);
  return true;
}

bool SampleKind::StartOne(const SourceStartRequest& r, SourceStartResult& res, std::uint32_t& inlineBudget) noexcept {
  res = SourceStartResult{kInvalidSlot, SND_END_DETAIL_NONE, 0, 0};
  if (r.asset == nullptr || r.asset->form == LoadForm::Stream || r.asset->codec.channels == 0u ||
      r.asset->codec.channels > kMaxChannels) {
    Count(sh_, kRejectedOther);
    return false;
  }
  const std::uint32_t ii = instFree_.Pop();
  if (ii == kNone) {
    res.endDetail = SND_END_DETAIL_VOICE_LIMIT;
    return false;
  }
  SampleInst& in = inst_[ii];
  in = SampleInst{};
  in.ctx = kNone;
  in.entry = kNone;
  in.a = CopyAsset(*r.asset);
  in.loop = MakeLoop(in.a, (r.flags & kStartLoop) != 0u, in.a.loopStart, in.a.loopEnd, r.flags >> kStartLoopCountShift);
  in.channels = r.channels == 0u || r.channels > in.a.h.channels ? in.a.h.channels : r.channels;
  in.virt = (r.flags & kStartVirtual) != 0u ? 1u : 0u;
  const std::uint64_t start = WrapInto(in.loop, r.startFrame);
  if (start != r.startFrame) {
    in.hasOverride = 1;
    in.overridePos = start;
  }
  in.renderPos = start;
  const bool pcm = IsPcm(in.a.h.codec) && in.a.data != nullptr &&
                   in.a.dataBytes >= in.a.h.total_frames * PcmFrameBytes(in.a.h) && in.a.h.pre_skip == 0u;
  std::uint32_t detail = SND_END_DETAIL_NONE;
  bool ok = true;
  if (pcm && in.a.form != LoadForm::CompressedResident) {
    in.mode = SampleMode::Direct;
    in.primed = 1;
  } else if (in.a.form == LoadForm::DecompressCache && cache_.Enabled()) {
    ICodec* codec = FindCodec(sv_, in.a.h.codec);
    std::uint32_t e = kNone;
    const CacheResult cr = (codec::IsBuiltinCodec(in.a.h.codec) && codec::ValidateHeader(in.a.h) != codec::HeaderError::None)
                               ? CacheResult::Unsupported
                               : cache_.Acquire(in.a, codec, sh_, block_, &e);
    if (cr == CacheResult::Hit || cr == CacheResult::Started) {
      in.mode = SampleMode::Cache;
      in.entry = e;
      if (cr == CacheResult::Started && inlineBudget != 0u) {
        // Decode the head on the audio thread so the first block plays (bounded).
        --inlineBudget;
        Count(sh_, kInlinePrimes);
        cache_.FillHead(e, start + readyFrames_);
      }
    } else if (cr == CacheResult::Unsupported) {
      detail = SND_END_DETAIL_DECODE;
      ok = false;
      Count(sh_, kRejectedOther);
    } else {
      // Cache full (every page pinned) or no decoder for the fill: per-voice ring (6.2).
      Count(sh_, kCacheFallbacks);
      in.mode = SampleMode::Ring;
      ok = StartRing(in, ii, start, inlineBudget, &detail);
      if (!ok && cr == CacheResult::Full && detail == SND_END_DETAIL_DECODER_POOL_FULL) detail = SND_END_DETAIL_PCM_CACHE_FULL;
    }
  } else {
    in.mode = SampleMode::Ring;
    ok = StartRing(in, ii, start, inlineBudget, &detail);
  }
  if (!ok) {
    instFree_.Push(ii);
    in.state = 0;
    res.endDetail = detail;
    return false;
  }
  in.state = 1;
  ++live_;
  res.sourceSlot = ii;
  res.needsPriming = Ready(in) ? 0u : 1u;
  return true;
}

void SampleKind::Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept {
  std::uint32_t inlineBudget = 8;  // starts primed on the audio thread per call
  for (std::uint32_t i = 0; i < count; ++i) StartOne(requests[i], results[i], inlineBudget);
}

void SampleKind::FreeCtx(std::uint32_t ci) noexcept {
  RingCtx& c = ctx_[ci];
  if (c.inst == kNone) return;
  sh_.decoders.Release(c.slot);  // generation-checked (A1-15)
  c.slot = codec::DecoderSlot{};
  rings_.Release(c.channels, c.rings);
  for (std::uint32_t k = 0; k < kMaxChannels; ++k) c.ring[k] = nullptr;
  c.inst = kNone;
  c.pendingSeek = 0;
  // Remove from the active list (swap with the last).
  const std::uint32_t pos = c.listPos;
  const std::uint32_t last = active_[--activeCount_];
  active_[pos] = last;
  ctx_[last].listPos = pos;
  ctxFree_.Push(ci);
}

void SampleKind::FreeInstance(std::uint32_t i) noexcept {
  SampleInst& in = inst_[i];
  if (in.ctx != kNone) FreeCtx(in.ctx);
  if (in.entry != kNone) cache_.Unpin(in.entry);
  in = SampleInst{};
  in.ctx = kNone;
  in.entry = kNone;
  instFree_.Push(i);
  if (live_ > 0u) --live_;
}

void SampleKind::Release(const SlotIndex* slots, std::uint32_t count) noexcept {
  for (std::uint32_t i = 0; i < count; ++i) {
    const SlotIndex s = slots[i];
    if (s >= instCap_ || inst_[s].state != 1u) continue;
    SampleInst& in = inst_[s];
    if (in.ctx != kNone && JobBusy(ctx_[in.ctx].jobCounter)) {
      // A decode job still uses the context: returned in ScheduleBackground (3.3 epoch).
      std::atomic_ref<std::uint32_t>(ctx_[in.ctx].cancel).store(1u, std::memory_order_release);
      in.state = 2;
      continue;
    }
    FreeInstance(s);
  }
}

void SampleKind::Control(const SourceControlOp* ops, std::uint32_t count) noexcept {
  for (std::uint32_t i = 0; i < count; ++i) {
    const SourceControlOp& op = ops[i];
    if (op.sourceSlot >= instCap_ || inst_[op.sourceSlot].state != 1u) continue;
    SampleInst& in = inst_[op.sourceSlot];
    std::uint64_t target = kNone;
    switch (op.op) {
      case ControlOp::Seek:
        target = op.frame;
        break;
      case ControlOp::Virtualize:
        in.virt = 1;
        break;
      case ControlOp::Devirtualize:
        in.virt = 0;
        if (in.a.virtualReturn == 2u) {
          in.kill = 1;
        } else {
          target = in.a.virtualReturn == 1u ? 0u : op.frame;
          if (in.mode == SampleMode::Direct || in.mode == SampleMode::Cache) {
            if (in.a.virtualReturn == 1u) {
              in.hasOverride = 1;
              in.overridePos = 0;
            }
            target = kNone;
          }
        }
        break;
      case ControlOp::SetLoop: {
        const bool loop = op.frame2 != 0u || op.frame != 0u;
        in.loop = MakeLoop(in.a, loop, op.frame, op.frame2, in.loop.count);
        if (in.mode == SampleMode::Ring) target = in.renderPos;  // the decoder takes the loop at the seek
        break;
      }
      case ControlOp::Pause:
      case ControlOp::Resume:
        break;
    }
    if (target == kNone) continue;
    target = WrapInto(in.loop, target);
    in.hasOverride = 1;
    in.overridePos = target;
    in.renderPos = target;
    if (in.mode == SampleMode::Ring) {
      RingCtx& c = ctx_[in.ctx];
      c.pendingSeek = 1;
      c.seekTarget = target;
      in.primed = 0;
    }
  }
}

bool SampleKind::Ready(const SampleInst& in) const noexcept {
  switch (in.mode) {
    case SampleMode::Direct:
      return true;
    case SampleMode::Ring: {
      const RingCtx& c = ctx_[in.ctx];
      if (c.pendingSeek != 0u) return false;
      if (c.ended.load(std::memory_order_acquire) != 0u || c.error.load(std::memory_order_acquire) != 0u) return true;
      return c.w.load(std::memory_order_acquire) - c.r.load(std::memory_order_acquire) >= readyFrames_;
    }
    case SampleMode::Cache: {
      if (cache_.Error(in.entry) != 0u || cache_.Finished(in.entry)) return true;
      const std::uint64_t filled = cache_.Filled(in.entry);
      const std::uint64_t need = in.renderPos + readyFrames_;
      return filled >= (need < cache_.Frames(in.entry) ? need : cache_.Frames(in.entry));
    }
  }
  return true;
}

void SampleKind::QueryReady(const SlotIndex* slots, std::uint32_t count, std::uint8_t* ready) const noexcept {
  for (std::uint32_t i = 0; i < count; ++i) {
    const SlotIndex s = slots[i];
    ready[i] = (s >= instCap_ || inst_[s].state != 1u || inst_[s].virt != 0u || Ready(inst_[s])) ? 1u : 0u;
  }
}

// ---------------------------------------------------------------------------
// Render (RT workers / audio thread)
// ---------------------------------------------------------------------------
void SampleKind::RenderDirect(SampleInst& in, const RenderItem& it, VoiceRenderStatus& st) noexcept {
  const std::uint64_t total = in.a.h.total_frames;
  std::uint64_t pos = it.playhead->frame;
  std::uint32_t loops = it.playhead->loopCount;
  std::uint32_t written = 0;
  while (written < it.framesRequested) {
    const std::uint64_t end = in.loop.WrapsAfter(loops) ? in.loop.le : total;
    if (pos >= end) {
      if (!in.loop.WrapsAfter(loops)) break;
      pos = in.loop.lb;
      ++loops;
      continue;
    }
    std::uint64_t k = end - pos;
    if (k > it.framesRequested - written) k = it.framesRequested - written;
    ReadPcm(in.a.h, in.a.data, pos, static_cast<std::uint32_t>(k), it.out, it.frameOffset + written, it.channels);
    written += static_cast<std::uint32_t>(k);
    pos += k;
    if (in.loop.WrapsAfter(loops) && pos >= in.loop.le) {
      pos = in.loop.lb;
      ++loops;
    }
  }
  // End: reported by the first call that has nothing left, so the frames the
  // resampler still holds (its look-ahead) are played out first.
  if (written == 0u && !in.loop.WrapsAfter(loops) && pos >= total) st.ended = 1;
  it.playhead->frame = pos;
  it.playhead->loopCount = loops;
  st.framesRendered = written;
}

void SampleKind::RenderCache(SampleInst& in, const RenderItem& it, VoiceRenderStatus& st) noexcept {
  const std::uint32_t e = in.entry;
  const std::uint64_t total = in.a.h.total_frames;
  if (cache_.Error(e) != 0u) {
    st.ended = 1;
    return;
  }
  const std::uint64_t filled = cache_.Filled(e);
  if (in.primed == 0u) {
    if (!Ready(in)) return;  // silence, position kept (Priming)
    in.primed = 1;
  }
  std::uint64_t pos = it.playhead->frame;
  std::uint32_t loops = it.playhead->loopCount;
  std::uint32_t written = 0;
  bool short_ = false;
  while (written < it.framesRequested) {
    const std::uint64_t end = in.loop.WrapsAfter(loops) ? in.loop.le : total;
    if (pos >= end) {
      if (!in.loop.WrapsAfter(loops)) break;
      pos = in.loop.lb;
      ++loops;
      continue;
    }
    if (pos >= filled) {
      short_ = true;
      break;
    }
    std::uint64_t k = (end < filled ? end : filled) - pos;
    if (k > it.framesRequested - written) k = it.framesRequested - written;
    cache_.Read(e, pos, static_cast<std::uint32_t>(k), it.out, it.frameOffset + written, it.channels);
    written += static_cast<std::uint32_t>(k);
    pos += k;
  }
  if (short_) {
    st.starved = 1;
    Count(sh_, kSampleUnderruns);
  } else if (written == 0u && !in.loop.WrapsAfter(loops) && pos >= total) {
    st.ended = 1;
  }
  it.playhead->frame = pos;
  it.playhead->loopCount = loops;
  st.framesRendered = written;
}

void SampleKind::RenderRing(SampleInst& in, const RenderItem& it, VoiceRenderStatus& st) noexcept {
  RingCtx& c = ctx_[in.ctx];
  if (c.pendingSeek != 0u) return;
  const std::uint32_t ended = c.ended.load(std::memory_order_acquire);
  const std::uint32_t err = c.error.load(std::memory_order_acquire);
  const std::uint64_t w = c.w.load(std::memory_order_acquire);
  std::uint64_t r = c.r.load(std::memory_order_relaxed);
  if (in.primed == 0u) {
    if (w - r < readyFrames_ && ended == 0u && err == 0u) return;  // Priming: silence, position kept
    in.primed = 1;
  }
  std::uint64_t avail = w - r;
  std::uint32_t n = it.framesRequested;
  if (avail < n) n = static_cast<std::uint32_t>(avail);
  const std::uint32_t mask = rings_.frames - 1u;
  for (std::uint32_t done = 0; done < n;) {
    const std::uint32_t at = static_cast<std::uint32_t>((r + done) & mask);
    std::uint32_t k = rings_.frames - at;
    if (k > n - done) k = n - done;
    for (std::uint32_t ch = 0; ch < it.channels; ++ch) {
      std::memcpy(it.out[ch] + it.frameOffset + done, c.ring[ch] + at, sizeof(float) * k);
    }
    done += k;
  }
  r += n;
  c.r.store(r, std::memory_order_release);
  std::uint64_t pos = it.playhead->frame;
  std::uint32_t loops = it.playhead->loopCount;
  AdvancePos(in.loop, pos, loops, n);
  it.playhead->frame = pos;
  it.playhead->loopCount = loops;
  st.framesRendered = n;
  if (n < it.framesRequested) {
    if (err != 0u) {
      st.ended = n == 0u ? 1u : 0u;
    } else if (ended != 0u) {
      if (n == 0u) st.ended = 1;
    } else {
      st.starved = 1;
      Count(sh_, kSampleUnderruns);
    }
  }
}

void SampleKind::RenderBatch(const snd::source::RenderBatch& b, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t i = 0; i < b.count; ++i) {
    const RenderItem& it = b.items[i];
    VoiceRenderStatus& st = status[i];
    st = VoiceRenderStatus{};
    if (it.sourceSlot >= instCap_ || inst_[it.sourceSlot].state != 1u) {
      st.ended = 1;
      continue;
    }
    SampleInst& in = inst_[it.sourceSlot];
    if (in.kill != 0u) {
      st.ended = 1;
      continue;
    }
    if (in.hasOverride != 0u) {
      it.playhead->frame = in.overridePos;
      it.playhead->fraction = 0;
      in.hasOverride = 0;
    }
    switch (in.mode) {
      case SampleMode::Direct:
        RenderDirect(in, it, st);
        break;
      case SampleMode::Ring:
        RenderRing(in, it, st);
        break;
      case SampleMode::Cache:
        RenderCache(in, it, st);
        break;
    }
    in.renderPos = it.playhead->frame;
  }
}

void SampleKind::AdvanceVirtual(const AdvanceItem* items, std::uint32_t count, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t i = 0; i < count; ++i) {
    status[i] = VoiceRenderStatus{};
    const AdvanceItem& it = items[i];
    if (it.sourceSlot >= instCap_ || inst_[it.sourceSlot].state != 1u) {
      status[i].ended = 1;
      continue;
    }
    SampleInst& in = inst_[it.sourceSlot];
    if (in.hasOverride != 0u) {
      it.playhead->frame = in.overridePos;
      it.playhead->fraction = 0;
      in.hasOverride = 0;
    }
    if (AdvanceVirtualPos(in.loop, in.a.h.total_frames, *it.playhead, it.outputFrames, it.pitch)) status[i].ended = 1;
    in.renderPos = it.playhead->frame;
  }
}

// ---------------------------------------------------------------------------
// Background (audio thread, Publish)
// ---------------------------------------------------------------------------
void SampleKind::ScheduleBackground(const BlockInfo& block) noexcept {
  block_ = block.index;
  std::uint32_t inlineBudget = 4;
  for (std::uint32_t k = 0; k < activeCount_;) {
    const std::uint32_t ci = active_[k];
    RingCtx& c = ctx_[ci];
    SampleInst& in = inst_[c.inst];
    if (JobBusy(c.jobCounter)) {
      ++k;
      continue;
    }
    if (in.state == 2u) {  // released while its job ran
      FreeInstance(c.inst);
      continue;  // active_[k] now holds another context
    }
    ++k;
    if (c.pendingSeek != 0u) {
      SeekRing(c, c.seekTarget, inlineBudget);
      continue;
    }
    if (in.virt != 0u || c.ended.load(std::memory_order_relaxed) != 0u || c.error.load(std::memory_order_relaxed) != 0u) {
      continue;
    }
    const std::uint64_t used = c.w.load(std::memory_order_relaxed) - c.r.load(std::memory_order_acquire);
    if (rings_.frames - used < rings_.frames / 4u) continue;  // full enough
    if (!PostJob(sv_, sh_, &SampleKind::RingJob, &c, &c.jobCounter, &c.cancel, 1u) && sv_.postJob == nullptr) {
      DecodeRing(c, ~std::uint64_t{0});  // no job service: decode here (tests without services)
    }
  }
  cache_.Schedule(sv_, sh_);
}

std::uint32_t SampleKind::PrimeFrames(const SourceAsset&, std::uint64_t) const noexcept { return 0; }

}  // namespace snd::source::detail
