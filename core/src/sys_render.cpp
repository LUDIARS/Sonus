// System 9: Render (design 4 steps 5-6, 2.3, 3.4; D-43, D-44).
//
// Serial part (audio thread):
//   - releases held voices once the RT pool is quiescent (D-43),
//   - initialises Render-owned columns of new voices and the render state of
//     new real assignments (playhead from Voice, resampler pool slot = real
//     slot, voice filter, pan),
//   - copies every per-block input of a voice into its item (ItemInput): rate
//     ratios (pitch x source rate / internal rate, clamped to
//     Config.maxPitchRatio), gain ramp (incl. the steal / devirtualise fade),
//     target pan matrix (basic 3D: azimuth of the nearest listener, spread and
//     2D/3D blend from the distance curves; 2D: balance with constant power),
//     filter target, accumulator segment (one per bus run inside a chunk).
// Chunks (RT workers or the audio thread): source RenderBatch per kind run,
// resampling of source-rate kinds (dsp), the voice filter, gain + pan into
// the chunk's bus accumulator. A chunk touches only its items, its
// accumulators, its thread arena and the render state of its voices.
// After the chunks: late chunks hold their voices (Voice.renderHold) and fade
// their last output (D-15); virtual voices advance (AdvanceVirtual); MixBuses.
//
// Parallel rendering only when IsQuiescent() was true at the block start; the
// set the block uses alternates with the last parallel block (frame.cpp).
#include <cmath>
#include <cstring>
#include <new>

#include "access.hpp"
#include "core_impl.hpp"
#include "snd/platform/clock.hpp"

namespace snd::core {

namespace {

constexpr schema::SystemId kSys = schema::SystemId::Render;
constexpr float kPi = 3.14159265358979f;

struct RenderCtx {
  Impl* im;
  RenderSet* set;
  source::BlockInfo block;
  std::uint32_t lod;
  std::uint32_t reserved;
};

float* SegBuffer(const Impl& im, RenderSet& set, std::uint32_t chunk, std::uint32_t seg) noexcept {
  return set.accum + (std::size_t{chunk} * im.cfg.voicesPerChunk + seg) * kMaxChannels * im.cfg.blockFrames;
}

std::uint8_t* ResamplerState(const Impl& im, SlotIndex real) noexcept {
  return im.rd.resamplerPool + im.cfg.resamplerSlotBytes * real;
}

// Identity-ish routing for channel counts the dsp layouts do not cover.
void PlainMatrix(std::uint32_t in, std::uint32_t out, PanMatrix* m) noexcept {
  dsp::ClearPanMatrix(m);
  for (std::uint32_t i = 0; i < in; ++i) m->gain[i][i < out ? i : out - 1u] = 1.0f;
}

void PanFor(const Impl& im, const VoiceStatic& vs, float pan, std::uint32_t outChannels, const Vec3* direction, float spread,
            float blend3d, PanMatrix* m) noexcept {
  dsp::SpeakerLayout outLayout = dsp::SpeakerLayout::Stereo;
  const bool outKnown = dsp::LayoutForChannels(outChannels, &outLayout);
  dsp::SpeakerLayout inLayout = dsp::SpeakerLayout::Stereo;
  const bool inKnown = dsp::LayoutForChannels(vs.channels, &inLayout);
  if (!outKnown || !inKnown) {
    PlainMatrix(vs.channels, outChannels, m);
    return;
  }
  PanMatrix p2{};
  dsp::ComputeStereoPanMatrix(vs.channels, outLayout, pan, dsp::PanLaw::ConstantPower, &p2);
  if (direction == nullptr || blend3d <= 0.0f) {
    *m = p2;
    return;
  }
  const float azimuth = std::atan2(direction->x, direction->z);
  PanMatrix p3{};
  dsp::ComputePanMatrix(vs.channels, outLayout, azimuth, spread, &p3);
  if (blend3d >= 1.0f) {
    *m = p3;
    return;
  }
  dsp::LerpPanMatrix(p2, p3, blend3d, m);
  (void)im;
}

void ScaleMatrix(const PanMatrix& in, float g, PanMatrix* out) noexcept {
  *out = in;
  dsp::ScalePanMatrix(out, g);
}

// --- chunk function (RT workers / audio thread) ---------------------------------
void PostProcessItem(Impl& im, RenderSet& set, const source::RenderItem& it, ItemInput& in, std::uint32_t chunk,
                     float* const* voiceOut, schema::Tables& t) noexcept {
  const std::uint32_t frames = im.cfg.blockFrames;
  const SlotIndex r = it.realSlot;
  // D-132: heavy voice effects run after the voice filter and before the envelopes, so the
  // envelopes (stop / steal fades, sample-accurate stop) also end the effect output (D-134).
  const bool vfx = in.vfxSlot != 0u;
  if (vfx) {
    if (in.filterActive != 0u) {
      dsp::VoiceFilterProcess(t.RealVoice.col.filterState[r], voiceOut, in.channels, frames, in.filter, im.cfg.sampleRate);
    }
    VfxProcess(im, in.vfxSlot - 1u, voiceOut, in.channels);
  }
  // Release envelope (sample accurate): 1 before the release starts, linear to 0 at stopClock.
  if (in.releaseActive != 0u) {
    for (std::uint32_t f = 0; f < frames; ++f) {
      float e = (static_cast<float>(in.releaseStop) - static_cast<float>(f)) / in.releaseFrames;
      if (e >= 1.0f) continue;
      e = e < 0.0f ? 0.0f : e;
      for (std::uint32_t c = 0; c < in.channels; ++c) voiceOut[c][f] *= e;
    }
  }
  // Assignment fade (D-15: steal fade out, devirtualise / hold-release fade in), per sample.
  if (in.fadeActive != 0u) {
    for (std::uint32_t f = 0; f < frames; ++f) {
      float e = in.fade0 + in.fadeDelta * static_cast<float>(f + 1u);
      e = e < 0.0f ? 0.0f : (e > 1.0f ? 1.0f : e);
      for (std::uint32_t c = 0; c < in.channels; ++c) voiceOut[c][f] *= e;
    }
  }
  // Sample-accurate stop: silence after frameEnd.
  if (in.frameEnd < frames) {
    for (std::uint32_t c = 0; c < in.channels; ++c) std::memset(voiceOut[c] + in.frameEnd, 0, sizeof(float) * (frames - in.frameEnd));
  }
  if (in.filterActive != 0u && !vfx) {
    dsp::VoiceFilterProcess(t.RealVoice.col.filterState[r], voiceOut, in.channels, frames, in.filter, im.cfg.sampleRate);
  }
  PanMatrix from{}, to{};
  const PanMatrix& prev = (in.flags & kItemInitPan) != 0u ? in.pan : t.RealVoice.col.panMatrix[r];
  ScaleMatrix(prev, in.gain0, &from);
  ScaleMatrix(in.pan, in.gain1, &to);
  float* seg = SegBuffer(im, set, chunk, in.segment);
  float* outPtr[kMaxChannels];
  for (std::uint32_t c = 0; c < in.outChannels; ++c) outPtr[c] = seg + std::size_t{c} * frames;
  dsp::ApplyPanMatrix(voiceOut, in.channels, outPtr, in.outChannels, frames, from, to, true);
  for (std::uint32_t o = 0; o < in.outChannels; ++o) {
    float v = 0.0f;
    for (std::uint32_t i = 0; i < in.channels; ++i) v += voiceOut[i][frames - 1u] * to.gain[i][o];
    in.tail[o] = v;
  }
  t.RealVoice.col.panMatrix[r] = in.pan;
  in.status.late = 0u;
  t.RealVoice.col.renderStatus[r] = in.status;
}

void RenderRun(Impl& im, RenderSet& set, std::uint32_t chunk, std::uint32_t first, std::uint32_t count,
               source::Scratch& scratch, std::uint32_t generation, const source::BlockInfo& block,
               std::uint32_t lod) noexcept {
  schema::Tables& t = im.tables;
  const SourceKind kind = static_cast<SourceKind>(set.inputs[first].kind);
  source::ISourceKind* src = im.kinds[static_cast<std::size_t>(kind)];
  const std::uint32_t frames = im.cfg.blockFrames;
  std::uint32_t i = first;
  while (i < first + count) {
    // Sub-batch: as many items as the thread arena holds at once.
    const std::size_t mark = scratch.used;
    std::uint32_t j = i;
    for (; j < first + count; ++j) {
      source::RenderItem& it = set.items[j];
      ItemInput& in = set.inputs[j];
      float** ptrs = static_cast<float**>(scratch.Take(sizeof(float*) * kMaxChannels, 16));
      if (ptrs == nullptr) break;
      bool ok = true;
      if ((in.flags & kItemSourceRate) != 0u) {
        std::uint8_t* rs = ResamplerState(im, it.realSlot);
        const std::uint32_t outFrames = frames - in.frameOffset;
        const std::uint32_t n = dsp::ResamplerInputFrames(rs, outFrames, in.ratio0, in.ratio1);
        const std::uint32_t pad = dsp::ResamplerPadFrames(dsp::ResamplerGetQuality(rs), im.cfg.maxPitchRatio);
        for (std::uint32_t c = 0; c < in.channels && ok; ++c) {
          float* b = static_cast<float*>(scratch.Take(sizeof(float) * (pad + n)));
          if (b == nullptr) {
            ok = false;
            break;
          }
          std::memset(b + pad, 0, sizeof(float) * n);
          ptrs[c] = b + pad;
        }
        it.frameOffset = 0;
        it.framesRequested = n;
      } else {
        for (std::uint32_t c = 0; c < in.channels && ok; ++c) {
          float* b = static_cast<float*>(scratch.Take(sizeof(float) * frames));
          if (b == nullptr) {
            ok = false;
            break;
          }
          std::memset(b, 0, sizeof(float) * frames);
          ptrs[c] = b;
        }
        it.frameOffset = in.frameOffset;
        it.framesRequested = frames - in.frameOffset;
      }
      if (!ok) break;
      it.channels = in.channels;
      it.out = ptrs;
      it.playhead = &t.RealVoice.col.playhead[it.realSlot];
    }
    if (j == i) {  // not even one item fits: silence, reported as starved (3.3)
      set.inputs[i].status = VoiceRenderStatus{0, 1, 0, 0, 0};
      t.RealVoice.col.renderStatus[set.items[i].realSlot] = set.inputs[i].status;
      for (std::uint32_t o = 0; o < kMaxChannels; ++o) set.inputs[i].tail[o] = 0.0f;
      scratch.used = mark;
      ++i;
      continue;
    }
    VoiceRenderStatus* status = static_cast<VoiceRenderStatus*>(scratch.Take(sizeof(VoiceRenderStatus) * (j - i), 16));
    if (status == nullptr) {
      scratch.used = mark;
      for (std::uint32_t k = i; k < j; ++k) {
        set.inputs[k].status = VoiceRenderStatus{0, 1, 0, 0, 0};
        t.RealVoice.col.renderStatus[set.items[k].realSlot] = set.inputs[k].status;
      }
      i = j;
      continue;
    }
    for (std::uint32_t k = 0; k < j - i; ++k) status[k] = VoiceRenderStatus{};
    if (src != nullptr) {
      source::RenderBatch b{};
      b.kind = kind;
      b.lodLevel = static_cast<std::uint8_t>(lod);
      b.count = j - i;
      b.items = set.items + i;
      b.block = block;
      b.scratch = &scratch;
      b.chunkIndex = chunk;
      b.chunkGeneration = generation;
      src->RenderBatch(b, status);
    }
    for (std::uint32_t k = i; k < j; ++k) {
      source::RenderItem& it = set.items[k];
      ItemInput& in = set.inputs[k];
      in.status = status[k - i];
      const std::size_t itemMark = scratch.used;
      float* voiceOut[kMaxChannels];
      if ((in.flags & kItemSourceRate) != 0u) {
        bool ok = true;
        for (std::uint32_t c = 0; c < in.channels; ++c) {
          voiceOut[c] = static_cast<float*>(scratch.Take(sizeof(float) * frames));
          if (voiceOut[c] == nullptr) ok = false;
        }
        if (!ok) {
          scratch.used = itemMark;
          in.status.starved = 1u;
          t.RealVoice.col.renderStatus[it.realSlot] = in.status;
          continue;
        }
        std::uint8_t* rs = ResamplerState(im, it.realSlot);
        const std::uint32_t pad = dsp::ResamplerPadFrames(dsp::ResamplerGetQuality(rs), im.cfg.maxPitchRatio);
        float* inBase[kMaxChannels];
        float* outAt[kMaxChannels];
        for (std::uint32_t c = 0; c < in.channels; ++c) {
          inBase[c] = it.out[c] - pad;
          if (in.frameOffset != 0u) std::memset(voiceOut[c], 0, sizeof(float) * in.frameOffset);
          outAt[c] = voiceOut[c] + in.frameOffset;
        }
        dsp::ResamplerProcess(rs, inBase, it.framesRequested, outAt, frames - in.frameOffset, in.ratio0, in.ratio1);
      } else {
        for (std::uint32_t c = 0; c < in.channels; ++c) voiceOut[c] = it.out[c];
      }
      PostProcessItem(im, set, it, in, chunk, voiceOut, t);
      scratch.used = itemMark;
    }
    scratch.used = mark;
    i = j;
  }
}

void ChunkFn(void* p, const jobs::RtChunkContext& c) noexcept {
  RenderCtx& ctx = *static_cast<RenderCtx*>(p);
  Impl& im = *ctx.im;
  RenderSet& set = *ctx.set;
  const ChunkInfo& ci = set.chunks[c.chunkIndex];
  // Zero this chunk's accumulator segments.
  for (std::uint32_t s = 0; s < ci.segCount; ++s) {
    const ChunkSeg& seg = set.segs[c.chunkIndex * im.cfg.voicesPerChunk + s];
    std::memset(SegBuffer(im, set, c.chunkIndex, s), 0, sizeof(float) * seg.channels * im.cfg.blockFrames);
  }
  source::Scratch& scratch = *c.scratch;
  const std::size_t base = scratch.used;
  std::uint32_t i = ci.firstItem;
  const std::uint32_t end = ci.firstItem + ci.itemCount;
  while (i < end) {
    std::uint32_t j = i + 1u;
    while (j < end && set.inputs[j].kind == set.inputs[i].kind) ++j;
    RenderRun(im, set, c.chunkIndex, i, j - i, scratch, c.generation, ctx.block, ctx.lod);
    scratch.used = base;
    i = j;
  }
}

// Initial render state of a new real assignment (serial: the voice is in no running chunk).
void InitRenderState(Impl& im, SlotIndex r, SlotIndex v, std::uint32_t vd, const VoiceStatic& vs, bool sourceRate) noexcept {
  schema::Tables& t = im.tables;
  const Playhead ph = SND_W(Voice_playhead)[vd];
  t.RealVoice.col.playhead[r] = ph;
  t.RealVoice.col.renderStatus[r] = VoiceRenderStatus{};
  dsp::VoiceFilterInit(t.RealVoice.col.filterState[r], SND_R(Voice_filterTarget)[vd]);
  if (sourceRate) {
    const dsp::ResamplerQuality q = im.rd.lod >= 2u ? dsp::kResamplerLight : dsp::kResamplerHigh;
    dsp::ResamplerInit(ResamplerState(im, r), q, vs.channels, im.cfg.maxPitchRatio);
  }
  im.rd.stateChannels[r] = vs.channels;
  // The previous owner's last output must not be faded into a bus by this voice.
  for (std::uint32_t o = 0; o < kMaxChannels; ++o) im.rd.tail[std::size_t{r} * kMaxChannels + o] = 0.0f;
  im.rd.tailBus[r] = kNoSlot;
  im.rd.tailChannels[r] = 0u;
  // Fresh starts begin at full level (sample-accurate start); devirtualised voices fade in (D-15).
  const bool fresh = ph.frame == vs.startFrame && ph.loopCount == 0u && ph.fraction == 0u;
  im.rd.fade[r] = fresh ? 1.0f : 0.0f;
  (void)v;
}

// Adds this block's part of the D-15 fade of a held voice's last output.
void FadeTail(Impl& im, SlotIndex r, std::uint32_t frames) noexcept {
  RenderPriv& rd = im.rd;
  schema::Tables& t = im.tables;
  const std::uint32_t total = im.cfg.shortFadeFrames == 0u ? 1u : im.cfg.shortFadeFrames;
  std::uint32_t left = rd.tailFadeLeft[r];
  const std::uint32_t n = left < frames ? left : frames;
  SlotIndex bus = rd.tailFadeBus[r];
  if (bus != kNoSlot && !base::rows::IsLive(t.Bus.rows, bus)) bus = kNoSlot;  // freed meanwhile
  float* dst = bus == kNoSlot ? rd.outBuf : rd.busBuf + std::size_t{bus} * kMaxChannels * frames;
  const float* tail = rd.tailFade + std::size_t{r} * kMaxChannels;
  const std::uint32_t done = total - left;
  for (std::uint32_t o = 0; o < rd.tailFadeChannels[r] && o < kMaxChannels; ++o) {
    const float v0 = tail[o];
    for (std::uint32_t f = 0; f < n; ++f) {
      dst[std::size_t{o} * frames + f] += v0 * (1.0f - static_cast<float>(done + f + 1u) / static_cast<float>(total));
    }
  }
  rd.tailFadeLeft[r] = left - n;
}

}  // namespace

void Render(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  RenderPriv& rd = im.rd;
  RenderSet& set = rd.sets[rd.currentSet];
  const std::uint32_t frames = im.cfg.blockFrames;
  const ClockSample blockEnd = im.clock + frames;
  const schema::TableRows& vr = t.Voice.rows;
  Playhead* vPlayhead = SND_W(Voice_playhead);
  VoiceRenderStatus* vStatus = SND_W(Voice_renderStatus);
  VoiceRenderHold* vHold = SND_W(Voice_renderHold);
  const SlotIndex* vReal = SND_R(Voice_realSlot);
  im.profile.heldVoices = 0;

  // a) New voices: Render-owned columns. Held voices: release once quiescent (D-43).
  for (std::uint32_t d = 0; d < vr.count; ++d) {
    const SlotIndex v = base::rows::SlotAt(vr, d);
    const VoiceStatic& vs = im.ev.vs[v];
    if (rd.voiceGen[v] != vs.generation) {
      rd.voiceGen[v] = vs.generation;
      vPlayhead[d] = Playhead{vs.startFrame, 0u, 0u};
      vStatus[d] = VoiceRenderStatus{};
      vHold[d] = VoiceRenderHold{};
      continue;
    }
    if (vHold[d].held != 0u) {
      if (im.blockQuiescent) {
        const SlotIndex r = vReal[d];
        if (r != kNoSlot && r < im.cfg.realTotal) {
          // D-43: the state is what the late chunk wrote; the voice also missed the
          // blocks it was held (and this one): advance it like a virtual voice to
          // catch up with the clock, then fade in from 0 (D-15) from the next block.
          const std::uint64_t missed = im.block > rd.holdBlock[v] ? im.block - rd.holdBlock[v] : 0u;
          source::ISourceKind* src = im.kinds[static_cast<std::size_t>(SND_R(Voice_sourceKind)[d])];
          const SlotIndex srcSlot = SND_R(Voice_sourceSlot)[d];
          if (missed != 0u && src != nullptr && srcSlot != kNoSlot) {
            float ratio = SND_R(Voice_pitch)[d].end *
                          ((vs.flags & kVsSourceRate) != 0u ? vs.sourceRate / static_cast<float>(im.cfg.sampleRate) : 1.0f);
            ratio = ratio > im.cfg.maxPitchRatio ? im.cfg.maxPitchRatio : ratio;
            const source::AdvanceItem item{v, srcSlot, static_cast<std::uint32_t>(missed * frames), ratio,
                                           &t.RealVoice.col.playhead[r]};
            VoiceRenderStatus st{};
            src->AdvanceVirtual(&item, 1u, &st);
            t.RealVoice.col.renderStatus[r] = st;
          }
          vPlayhead[d] = t.RealVoice.col.playhead[r];
          vStatus[d] = t.RealVoice.col.renderStatus[r];
          vStatus[d].late = 0u;
          rd.fade[r] = 0.0f;
        }
        vHold[d] = VoiceRenderHold{};
      } else {
        ++im.profile.heldVoices;
      }
    }
  }
  if (im.blockQuiescent) {
    rd.sets[0].inUseLate = 0u;
    rd.sets[1].inUseLate = 0u;
  }

  // b) Serial preparation of the items.
  VfxReleaseStale(im);  // D-133: slots of voices that left their real assignment go back
  const float rate = static_cast<float>(im.cfg.sampleRate);
  for (std::uint32_t c = 0; c < set.chunkCount; ++c) set.chunks[c].segCount = 0;
  for (std::uint32_t i = 0; i < set.itemCount; ++i) {
    source::RenderItem& it = set.items[i];
    ItemInput& in = set.inputs[i];
    const SlotIndex r = it.realSlot;
    const SlotIndex v = it.voiceSlot;
    const std::uint32_t d = base::rows::DenseOf(vr, v);
    const VoiceStatic& vs = im.ev.vs[v];
    const SourceKind kind = SND_R(Voice_sourceKind)[d];
    const bool sourceRate = (vs.flags & kVsSourceRate) != 0u;
    const RealVoiceAssign& as = SND_R(RealVoice_assign)[r];
    in = ItemInput{};
    in.kind = static_cast<std::uint32_t>(kind);
    in.channels = vs.channels;
    if (rd.lastAssign[r] != as.assignSeq) {
      rd.lastAssign[r] = as.assignSeq;
      InitRenderState(im, r, v, d, vs, sourceRate);
      in.flags |= kItemInitPan;
    }
    // Bus and accumulator segment.
    SlotIndex bus = SND_R(Voice_busSlot)[d];
    if (bus != kNoSlot && !base::rows::IsLive(t.Bus.rows, bus)) bus = rd.masterSlot;
    in.outChannels = bus == kNoSlot ? im.cfg.outChannels : im.busDefs[bus].channels;
    const std::uint32_t chunk = i / im.cfg.voicesPerChunk;
    ChunkInfo& ci = set.chunks[chunk];
    ChunkSeg* segs = set.segs + std::size_t{chunk} * im.cfg.voicesPerChunk;
    if (ci.segCount == 0u || segs[ci.segCount - 1u].bus != bus) segs[ci.segCount++] = ChunkSeg{bus, in.outChannels};
    in.segment = ci.segCount - 1u;
    // Timing (4.4 sample-accurate start; stop of a released voice).
    const VoiceLifecycle& lc = SND_R(Voice_lifecycle)[d];
    in.frameOffset = lc.startClock > im.clock ? static_cast<std::uint32_t>(lc.startClock - im.clock) : 0u;
    in.frameEnd = frames;
    if (lc.state == static_cast<std::uint8_t>(VoiceStateId::Releasing)) {
      if (lc.stopClock < blockEnd) {
        in.frameEnd = lc.stopClock > im.clock ? static_cast<std::uint32_t>(lc.stopClock - im.clock) : 0u;
      }
      const float relFrames = lc.releaseSeconds * rate;
      const std::int64_t stop = static_cast<std::int64_t>(lc.stopClock) - static_cast<std::int64_t>(im.clock);
      if (relFrames >= 1.0f && static_cast<float>(stop) - relFrames < static_cast<float>(frames)) {
        in.releaseActive = 1u;
        in.releaseStop = stop;
        in.releaseFrames = relFrames;
      }
    }
    // Rate.
    const Ramp pitch = SND_R(Voice_pitch)[d];
    const float srcRatio = sourceRate ? vs.sourceRate / rate : 1.0f;
    auto clampRatio = [&](float x) noexcept {
      if (!(x > dsp::kResamplerMinRatio)) return dsp::kResamplerMinRatio;
      return x > im.cfg.maxPitchRatio ? im.cfg.maxPitchRatio : x;
    };
    in.ratio0 = clampRatio(pitch.start * srcRatio);
    in.ratio1 = clampRatio(pitch.end * srcRatio);
    if (sourceRate) in.flags |= kItemSourceRate;
    it.pitch = Ramp{in.ratio0, in.ratio1};
    // 1.2 (D-90): followed transport (synth voices), as of this block's first frame.
    it.transportBeats = vs.followTransport.generation != 0u ? vs.transportBeats : -1.0;
    it.transportBeatsPerSample = vs.followTransport.generation != 0u ? vs.transportBeatsPerSample : 0.0;
    // Gain, with the steal fade (out) / devirtualise fade (in).
    const Ramp gain = SND_R(Voice_gain)[d];
    // D-15: the fade lasts shortFadeFrames exactly (it may span blocks), per sample.
    const float perFrame = im.cfg.shortFadeFrames == 0u ? 1.0f : 1.0f / static_cast<float>(im.cfg.shortFadeFrames);
    const float f0 = rd.fade[r];
    const float fadeTarget = as.fadingOut != 0u ? 0.0f : 1.0f;
    if (f0 != fadeTarget) {
      in.fadeActive = 1u;
      in.fade0 = f0;
      in.fadeDelta = as.fadingOut != 0u ? -perFrame : perFrame;
      float f1 = f0 + in.fadeDelta * static_cast<float>(frames);
      rd.fade[r] = f1 < 0.0f ? 0.0f : (f1 > 1.0f ? 1.0f : f1);
    }
    in.gain0 = (in.flags & kItemInitPan) != 0u ? gain.end : gain.start;
    in.gain1 = gain.end;
    t.RealVoice.col.appliedGain[r] = gain.end;
    // Pan (basic 3D or 2D balance).
    const SlotIndex em = SND_R(Voice_emitterSlot)[d];
    const Vec3* dir = nullptr;
    float spread = 0.0f, blend = 1.0f;
    Vec3 dirv{0, 0, 1};
    if ((vs.flags & kVs3D) != 0u && em != kNoSlot && em < im.cfg.maxEmitters) {
      std::uint32_t best = kNoSlot;
      float bestD = 0.0f;
      for (std::uint32_t l = 0; l < im.cfg.maxListeners; ++l) {
        const std::uint32_t s = em * im.cfg.maxListeners + l;
        if (!base::rows::IsLive(t.Spatial.rows, s) || SND_R(Spatial_valid)[s] == 0u) continue;
        if (best == kNoSlot || SND_R(Spatial_distance)[s] < bestD) {
          best = s;
          bestD = SND_R(Spatial_distance)[s];
        }
      }
      if (best != kNoSlot) {
        dirv = SND_R(Spatial_direction)[best];
        dir = &dirv;
        const ClipRef cr = SND_R(Voice_clipRef)[d];
        if (const BankRecord* br = RegisteredBank(im, cr.bankSlot); br != nullptr && vs.eventRow < br->view->Rows(bf::chunk::EVNT)) {
          const std::uint32_t cs = br->view->Column<bk::col::EVNT_curveSpread>()[vs.eventRow];
          const std::uint32_t cb = br->view->Column<bk::col::EVNT_curveBlend3D>()[vs.eventRow];
          if (cs != bf::kNone) spread = EvaluateCurve(*br->view, cs, bestD);
          if (cb != bf::kNone) blend = EvaluateCurve(*br->view, cb, bestD);
          spread = spread < 0.0f ? 0.0f : (spread > 1.0f ? 1.0f : spread);
          blend = blend < 0.0f ? 0.0f : (blend > 1.0f ? 1.0f : blend);
        }
      }
    }
    PanFor(im, vs, im.dp.pan[v], in.outChannels, dir, spread, blend, &in.pan);
    // Filter.
    in.filter = SND_R(Voice_filterTarget)[d];
    const bool targetOpen = in.filter.lowpassHz >= dsp::kVoiceLowpassOffHz && in.filter.highpassHz <= dsp::kVoiceHighpassOffHz;
    in.filterActive = (!targetOpen || !dsp::VoiceFilterIsOpen(t.RealVoice.col.filterState[r], im.cfg.sampleRate)) ? 1u : 0u;
    VfxPrepareItem(im, it, in);  // D-132: voice-effect slot and this block's parameters
  }
  (void)kPi;

  // c) Zero the bus buffers and the output.
  for (std::uint32_t d = 0; d < t.Bus.rows.count; ++d) {
    const SlotIndex s = base::rows::SlotAt(t.Bus.rows, d);
    std::memset(rd.busBuf + std::size_t{s} * kMaxChannels * frames, 0, sizeof(float) * kMaxChannels * frames);
  }
  std::memset(rd.outBuf, 0, sizeof(float) * kMaxChannels * frames);

  // d) Chunks: parallel only when the pool was quiescent at the block start (D-43).
  static_assert(sizeof(RenderCtx) <= sizeof(set.chunkCtx));
  RenderCtx& ctx = *new (set.chunkCtx) RenderCtx{&im, &set, source::BlockInfo{im.block, im.clock, frames, im.cfg.sampleRate}, rd.lod, 0u};
  bool parallel = false;
  std::uint32_t generation = 0;
  im.profile.chunks = set.chunkCount;
  im.profile.lateChunks = 0;
  if (set.chunkCount != 0u) {
    if (im.blockQuiescent && im.rt.Workers() != 0u && set.chunkCount > 1u) {
      const jobs::RtRunResult rr = im.rt.Run(set.chunkCount, &ChunkFn, &ctx);
      parallel = true;
      generation = rr.generation;
      im.profile.lateChunks = rr.late;
    } else {
      source::Scratch& sc = im.rt.CallerScratch();
      for (std::uint32_t c = 0; c < set.chunkCount; ++c) {
        sc.used = 0;
        jobs::RtChunkContext cc{c, 0u, 0u, 0u, &sc};
        ChunkFn(&ctx, cc);
      }
    }
  }
  rd.parallel = parallel ? 1u : 0u;
  if (parallel) rd.lastParallelSet = rd.currentSet;

  // e) After the chunks: copy state of done chunks; hold voices of late ones (D-43).
  for (std::uint32_t c = 0; c < set.chunkCount; ++c) {
    const bool done = !parallel || im.rt.ChunkDone(c);
    const ChunkInfo& ci = set.chunks[c];
    if (!done) {
      set.inUseLate = 1u;
      ++im.stats.late_chunks;
    }
    for (std::uint32_t i = ci.firstItem; i < ci.firstItem + ci.itemCount; ++i) {
      const SlotIndex r = set.items[i].realSlot;
      const std::uint32_t d = base::rows::DenseOf(vr, set.items[i].voiceSlot);
      if (d == kNoSlot) continue;
      float* tail = rd.tail + std::size_t{r} * kMaxChannels;
      if (done) {
        vPlayhead[d] = t.RealVoice.col.playhead[r];
        vStatus[d] = set.inputs[i].status;
        for (std::uint32_t o = 0; o < kMaxChannels; ++o) tail[o] = set.inputs[i].tail[o];
        rd.tailBus[r] = set.segs[std::size_t{c} * im.cfg.voicesPerChunk + set.inputs[i].segment].bus;
        rd.tailChannels[r] = set.inputs[i].outChannels;
        continue;
      }
      vHold[d] = VoiceRenderHold{1u, {0, 0, 0}, generation};
      rd.holdBlock[set.items[i].voiceSlot] = im.block;
      ++im.profile.heldVoices;
      vStatus[d].late = 1u;
      // D-15: fade the voice's last output to silence instead of cutting it (below).
      float* from = rd.tailFade + std::size_t{r} * kMaxChannels;
      for (std::uint32_t o = 0; o < kMaxChannels; ++o) {
        from[o] = tail[o];
        tail[o] = 0.0f;
      }
      rd.tailFadeBus[r] = rd.tailBus[r];
      rd.tailFadeChannels[r] = rd.tailChannels[r];
      rd.tailFadeLeft[r] = im.cfg.shortFadeFrames == 0u ? 1u : im.cfg.shortFadeFrames;
    }
  }
  // Tail fades of held voices (D-15, shortFadeFrames long, continued across blocks).
  for (SlotIndex r = 0; r < im.cfg.realTotal; ++r) {
    if (rd.tailFadeLeft[r] != 0u) FadeTail(im, r, frames);
  }
  im.profile.renderedVoices = set.itemCount;
  im.profile.parallel = rd.parallel;
  im.profile.renderSet = rd.currentSet;

  // f) Virtual voices: advance without rendering (4.4), batched per kind.
  {
    source::AdvanceItem* adv = im.frameArena.AllocateArray<source::AdvanceItem>(vr.count == 0u ? 1u : vr.count);
    VoiceRenderStatus* st = im.frameArena.AllocateArray<VoiceRenderStatus>(vr.count == 0u ? 1u : vr.count);
    std::uint32_t* dens = im.frameArena.AllocateArray<std::uint32_t>(vr.count == 0u ? 1u : vr.count);
    if (adv != nullptr && st != nullptr && dens != nullptr) {
      for (std::size_t k = 0; k < static_cast<std::size_t>(SourceKind::Count); ++k) {
        source::ISourceKind* src = im.kinds[k];
        if (src == nullptr) continue;
        std::uint32_t n = 0;
        for (std::uint32_t d = 0; d < vr.count; ++d) {
          if (static_cast<std::size_t>(SND_R(Voice_sourceKind)[d]) != k) continue;
          if (vReal[d] != kNoSlot || vHold[d].held != 0u) continue;
          const VoiceLifecycle& lc = SND_R(Voice_lifecycle)[d];
          if ((lc.flags & kVoicePausedFlag) != 0u || lc.startClock >= blockEnd) continue;
          const SlotIndex srcSlot = SND_R(Voice_sourceSlot)[d];
          if (srcSlot == kNoSlot) continue;
          const SlotIndex v = base::rows::SlotAt(vr, d);
          const VoiceStatic& vs = im.ev.vs[v];
          const std::uint32_t off = lc.startClock > im.clock ? static_cast<std::uint32_t>(lc.startClock - im.clock) : 0u;
          float ratio = SND_R(Voice_pitch)[d].end * ((vs.flags & kVsSourceRate) != 0u ? vs.sourceRate / rate : 1.0f);
          ratio = ratio > im.cfg.maxPitchRatio ? im.cfg.maxPitchRatio : ratio;
          adv[n] = source::AdvanceItem{v, srcSlot, frames - off, ratio, &vPlayhead[d]};
          st[n] = VoiceRenderStatus{};
          dens[n] = d;
          ++n;
        }
        if (n == 0u) continue;
        src->AdvanceVirtual(adv, n, st);
        for (std::uint32_t i = 0; i < n; ++i) vStatus[dens[i]] = st[i];
      }
    }
  }

  // g) Buses.
  MixBuses(im);
}

}  // namespace snd::core
