// Bus graph mix (design 4 step 6, 4.6, 3.4), the serial end of Render:
//   1. chunk accumulators are added into the bus buffers in chunk order
//      (deterministic, independent of the worker count, 3.4);
//   2. buses in graph order (children and send sources first): effect chain
//      (dsp standard effects / plugins in DSP pool memory), pre-fader sends,
//      gain ramp (Bus.gain -> Bus.gainTarget), post-fader sends, meters,
//      then into the parent (downmixed when the channel counts differ);
//   3. root buses (no parent, not an auxiliary output) into the master output;
//      voices without a bus go straight to the output.
// The LOD bypass (Bus.effectBypass) skips a chain once its tail is done (2.4).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {
constexpr schema::SystemId kSys = schema::SystemId::Render;

float* BusBuffer(Impl& im, SlotIndex s) noexcept {
  return im.rd.busBuf + std::size_t{s} * kMaxChannels * im.cfg.blockFrames;
}

void Downmix(const float* src, std::uint32_t inCh, float* dst, std::uint32_t outCh, std::uint32_t frames, float gain) noexcept {
  if (gain == 0.0f) return;
  if (inCh == outCh) {
    for (std::uint32_t c = 0; c < inCh; ++c) {
      dsp::MixGainRamp(dst + std::size_t{c} * frames, src + std::size_t{c} * frames, frames, gain, gain);
    }
    return;
  }
  dsp::SpeakerLayout a = dsp::SpeakerLayout::Stereo, b = dsp::SpeakerLayout::Stereo;
  PanMatrix m{};
  if (dsp::LayoutForChannels(inCh, &a) && dsp::LayoutForChannels(outCh, &b)) {
    dsp::ComputeDownmixMatrix(a, b, &m);
  } else {
    dsp::ClearPanMatrix(&m);
    for (std::uint32_t i = 0; i < inCh; ++i) m.gain[i][i < outCh ? i : outCh - 1u] = 1.0f;
  }
  dsp::ScalePanMatrix(&m, gain);
  const float* in[kMaxChannels];
  float* out[kMaxChannels];
  for (std::uint32_t c = 0; c < inCh; ++c) in[c] = src + std::size_t{c} * frames;
  for (std::uint32_t c = 0; c < outCh; ++c) out[c] = dst + std::size_t{c} * frames;
  dsp::ApplyPanMatrix(in, inCh, out, outCh, frames, m, m, true);
}

}  // namespace

void MixBuses(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  RenderPriv& rd = im.rd;
  RenderSet& set = rd.sets[rd.currentSet];
  const std::uint32_t frames = im.cfg.blockFrames;
  const schema::TableRows& br = t.Bus.rows;
  // 1. Chunk accumulators in chunk order (late chunks contribute nothing, 2.3).
  for (std::uint32_t c = 0; c < set.chunkCount; ++c) {
    if (rd.parallel != 0u && !im.rt.ChunkDone(c)) continue;
    const ChunkInfo& ci = set.chunks[c];
    for (std::uint32_t s = 0; s < ci.segCount; ++s) {
      const ChunkSeg& seg = set.segs[std::size_t{c} * im.cfg.voicesPerChunk + s];
      const float* src = set.accum + (std::size_t{c} * im.cfg.voicesPerChunk + s) * kMaxChannels * frames;
      float* dst = seg.bus == kNoSlot || !base::rows::IsLive(br, seg.bus) ? rd.outBuf : BusBuffer(im, seg.bus);
      const std::uint32_t dstCh = seg.bus == kNoSlot || !base::rows::IsLive(br, seg.bus) ? im.cfg.outChannels
                                                                                         : im.busDefs[seg.bus].channels;
      Downmix(src, seg.channels, dst, dstCh, frames, 1.0f);
    }
  }
  // 2. Graph order (Bus.graphOrder is a permutation of the live buses).
  std::uint32_t* order = rd.busOrder;
  const std::uint32_t nb = br.count;
  for (std::uint32_t i = 0; i < nb; ++i) order[i] = kNoSlot;
  const std::uint32_t* go = SND_R(Bus_graphOrder);
  for (std::uint32_t d = 0; d < nb; ++d) {
    if (go[d] < nb) order[go[d]] = base::rows::SlotAt(br, d);
  }
  const SlotIndex* parent = SND_R(Bus_parentSlot);
  const RangeU32* sends = SND_R(Bus_sendRange);
  const RangeU32* effects = SND_R(Bus_effectRange);
  const float* gainTarget = SND_R(Bus_gainTarget);
  const std::uint32_t* bypass = SND_R(Bus_effectBypass);
  float* busGain = SND_W(Bus_gain);
  BusPeak* busPeak = SND_W(Bus_peak);
  snd_render_context rc{};
  rc.clock_sample = im.clock;
  rc.sample_rate = im.cfg.sampleRate;
  rc.block_frames = frames;
  rc.transport_beats = -1.0;
  for (auto* e = im.tails; e;) {
    auto* next = e->nextTail;
    float* planes[kMaxChannels];
    std::fill_n(im.tailScratch, std::size_t{e->channels} * frames, 0.0f);
    for (std::uint32_t c = 0; c < e->channels; ++c) planes[c] = im.tailScratch + std::size_t{c} * frames;
    snd_param_block params{e->params, e->params, e->paramCount, 0u, &rc};
    dsp::EffectSlot slot{e->type, e->plugin, e->state, &params, nullptr, 0u, 0u};
    dsp::ProcessEffectChain(&slot, 1, planes, e->channels, frames);
    Downmix(im.tailScratch, e->channels, BusBuffer(im, e->tailBus), im.busDefs[e->tailBus].channels, frames, 1);
    if (dsp::EffectSlotIsTailDone(slot)) FinishEffectTail(im, *e);
    e = next;
  }
  for (std::uint32_t oi = 0; oi < nb; ++oi) {
    const SlotIndex s = order[oi];
    if (s == kNoSlot) continue;
    const std::uint32_t d = base::rows::DenseOf(br, s);
    const BusDef& def = im.busDefs[s];
    const std::uint32_t ch = def.channels == 0u || def.channels > kMaxChannels ? im.cfg.outChannels : def.channels;
    float* buf = BusBuffer(im, s);
    float* chans[kMaxChannels];
    for (std::uint32_t c = 0; c < ch; ++c) chans[c] = buf + std::size_t{c} * frames;
    if (rd.busGen[s] != br.generation[s]) {  // new bus row: start at its target, fresh meters
      rd.busGen[s] = br.generation[s];
      busGain[d] = gainTarget[d];
      dsp::PeakMeterInit(rd.peak[s], ch, 20.0f, im.cfg.sampleRate);
      dsp::RmsMeterInit(rd.rms[s], ch, 0.3f, im.cfg.sampleRate);
      busPeak[d] = BusPeak{};
    }
    const BankRecord* rec = RegisteredBank(im, def.defBank);
    // Effect chain.
    const auto count = def.customChain ? def.chainCount : effects[d].count;
    if (rec != nullptr && count != 0u) {
      dsp::EffectSlot slots[kMaxBusEffects];
      snd_param_block blocks[kMaxBusEffects];
      std::uint32_t n = 0;
      bool tailDone = true;
      for (std::uint32_t k = 0; k < count && n < kMaxBusEffects; ++k) {
        const std::uint32_t x = effects[d].first + k;
        if (!def.customChain && x >= rec->effectCount) break;
        EffectInstance& e = def.customChain ? *def.chain[k] : rec->effects[x];
        if (e.ok == 0u) continue;
        blocks[n] = snd_param_block{e.params, e.params, e.paramCount, 0u, &rc};
        slots[n] = dsp::EffectSlot{e.type, e.plugin, e.state, &blocks[n], nullptr, 0u, 0u};
        if (!dsp::EffectSlotIsTailDone(slots[n])) tailDone = false;
        ++n;
      }
      if (!(bypass[d] != 0u && tailDone) && n != 0u) {
        dsp::ProcessEffectChain(slots, n, chans, ch, frames);
      }
    }
    // Pre-fader sends (graph order: targets come later).
    auto doSends = [&](bool pre, float g) {
      if (rec == nullptr) return;
      for (const auto* send = def.sends; send; send = send->nextSend) {
        if (send->pre == pre) {
          Downmix(buf, ch, BusBuffer(im, send->target), im.busDefs[send->target].channels, frames, g * send->gain);
        }
      }
      const bk::BankView& v = *rec->view;
      for (std::uint32_t k = 0; k < sends[d].count; ++k) {
        const std::uint32_t row = sends[d].first + k;
        if ((v.Column<bk::col::SEND_preFader>()[row] != 0u) != pre) continue;
        const std::uint32_t tb = v.Column<bk::col::SEND_targetBus>()[row];
        if (tb >= rec->busCount) continue;
        const SlotIndex ts = rec->busSlot[tb];
        if (ts == kNoSlot || !base::rows::IsLive(br, ts)) continue;
        if (go[base::rows::DenseOf(br, ts)] <= oi) continue;  // would close a cycle: ignored
        Downmix(buf, ch, BusBuffer(im, ts), im.busDefs[ts].channels, frames, g * DbToGain(v.Column<bk::col::SEND_levelDb>()[row]));
      }
    };
    doSends(true, 1.0f);
    // Fader: ramp from the gain actually applied last block to the target.
    const float g0 = busGain[d], g1 = gainTarget[d];
    for (std::uint32_t c = 0; c < ch; ++c) dsp::CopyGainRamp(chans[c], chans[c], frames, g0, g1);
    busGain[d] = g1;
    doSends(false, 1.0f);
    // Meters.
    dsp::PeakMeterProcess(rd.peak[s], chans, frames);
    dsp::RmsMeterProcess(rd.rms[s], chans, frames);
    BusPeak pk{};
    for (std::uint32_t c = 0; c < ch; ++c) pk.peak[c] = dsp::Peak(chans[c], frames);
    busPeak[d] = pk;
    // Into the parent, or the output for root buses.
    const SlotIndex p = parent[d];
    if (p != kNoSlot && base::rows::IsLive(br, p)) {
      if (go[base::rows::DenseOf(br, p)] > oi) Downmix(buf, ch, BusBuffer(im, p), im.busDefs[p].channels, frames, 1.0f);
    } else if ((def.flags & bf::kBusOutput) == 0u) {
      Downmix(buf, ch, rd.outBuf, im.cfg.outChannels, frames, 1.0f);
    }
  }
  // Master loudness (4.6, dsp meter).
  float* outCh[kMaxChannels];
  for (std::uint32_t c = 0; c < im.cfg.outChannels; ++c) outCh[c] = rd.outBuf + std::size_t{c} * frames;
  dsp::LoudnessMeterProcess(*rd.loudness, outCh, frames);
}

}  // namespace snd::core
