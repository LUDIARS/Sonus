// Runtime control of bus effects (stage 3, group 2; fx_control.hpp):
//
// D-130  Parameters of a bus effect change at run time. Each parameter has a control
//        (EffectParamControl): a set value with a linear fade (ApplyCommands, from
//        snd_set_effect_param = SetBusVolume variant 1, or any core system through
//        SetEffectParam) plus a per-block contribution owned by EvaluateDspParams
//        (snapshots / Parameter curves, stage 3 group 3). Render evaluates the value
//        at the end of the block, clamps it to the parameter range and passes the
//        ParamBlock start (last block's end) -> end with the changed mask; the dsp
//        interpolates linearly inside the block (D-26). Standard effects and plugins
//        get the same ParamBlock. The LOD bypass (Bus.effectBypass) does not stop the
//        values: a bypassed chain resumes from the current values.
// D-131  Sidechain of the compressor (and of any standard effect with
//        kEffectHasSidechain) on a bus: the first DUCK row of kind 0 (sidechain, bank
//        row order, rows of the target bus's defining bank) whose target is the bus
//        names the source bus. Its post-fader output of the same block is used when the
//        graph order mixes the source first, otherwise the source's output of the
//        previous block (a buffer per DUCK row, bank record memory, zero at first). The
//        choice follows from Bus.graphOrder only. Channel counts may differ: the
//        detector takes the peak over all channels of the source (dsp EffectSidechain),
//        so the source's channels are passed as they are. The row keeps driving the
//        threshold ducking of EvaluateDspParams too (both apply).
//        Decisions (stage 3 re-check): the key is the source bus AFTER its fader (a muted
//        or silenced key bus causes no compression); with several kind-0 rows on one
//        target, only the first in row order is used.
// D-130 decisions (stage 3 re-check): fade seconds -> frames are truncated (the rule of
//        the bus volume); values set at run time live in the bus's effect instance, so
//        unloading and loading again the bank that defines the bus starts from the bank's
//        values; [init] parameters (kParamInitOnly) are refused with a warning.
#include <cmath>
#include <cstring>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {
float* BusBufferOf(Impl& im, SlotIndex s) noexcept {
  return im.rd.busBuf + std::size_t{s} * kMaxChannels * im.cfg.blockFrames;
}
}  // namespace

EffectInstance* BusEffectInstance(Impl& im, SlotIndex busSlot, std::uint32_t effectIndex) noexcept {
  // Bus.effectRange is written by ApplyCommands and read by Render; the definition
  // (BusDef) tells the bank. Reading the range here does not go through the schema
  // macros because callers belong to several systems: use the bank view instead.
  if (busSlot == kNoSlot || busSlot >= im.cfg.busCap || !base::rows::IsLive(im.tables.Bus.rows, busSlot)) {
    return nullptr;
  }
  const BusDef& def = im.busDefs[busSlot];
  const BankRecord* rec = RegisteredBank(im, def.defBank);
  if (rec == nullptr || def.defRow >= rec->busCount) return nullptr;
  const bk::BankView& v = *rec->view;
  const std::uint32_t first = v.Column<bk::col::BUSS_effectFirst>()[def.defRow];
  if (first == bf::kNone) return nullptr;
  const std::uint32_t count = v.Column<bk::col::BUSS_effectCount>()[def.defRow];
  if (effectIndex >= count || effectIndex >= kMaxBusEffects || first + effectIndex >= rec->effectCount) return nullptr;
  EffectInstance& e = rec->effects[first + effectIndex];
  return e.ok != 0u && e.ctl != nullptr ? &e : nullptr;
}

float ClampEffectParam(const EffectInstance& e, std::uint32_t param, float v) noexcept {
  if (e.plugin != nullptr) return v;
  const dsp::EffectInfo* info = dsp::FindEffect(e.type);
  if (info == nullptr || param >= info->paramCount) return v;
  const dsp::EffectParamInfo& p = info->params[param];
  if (!(v == v)) return p.defaultValue;
  return v < p.minValue ? p.minValue : (v > p.maxValue ? p.maxValue : v);
}

bool EffectParamIsInitOnly(const snd_dsp_effect* plugin, Id type, std::uint32_t param) noexcept {
  if (plugin != nullptr) return false;
  const dsp::EffectInfo* info = dsp::FindEffect(type);
  return info != nullptr && param < info->paramCount && (info->params[param].flags & dsp::kParamInitOnly) != 0u;
}

Id PlaybackEventId(Impl& im, HandleRef playback) noexcept {
  constexpr schema::SystemId kSys = schema::SystemId::ApplyCommands;
  schema::Tables& t = im.tables;
  const std::uint32_t d = base::rows::DenseOf(t.Playback.rows, playback);
  return d == kNoSlot ? 0u : SND_W(Playback_eventId)[d];
}

void SetEffectParam(Impl& im, EffectInstance& e, std::uint32_t param, float value, float fadeSeconds) noexcept {
  if (e.ctl == nullptr || param >= e.paramCount || !std::isfinite(value)) return;
  if (EffectParamIsInitOnly(e.plugin, e.type, param)) return;  // the dsp reads it at init only
  value = ClampEffectParam(e, param, value);
  TimedRamp& r = e.ctl[param].set;
  const float from = RampValue(r, im.clock);
  float s = std::isnan(fadeSeconds) ? 0.0f : fadeSeconds;
  s = s > 86400.0f ? 86400.0f : s;
  const std::uint32_t frames = s > 0.0f ? static_cast<std::uint32_t>(static_cast<double>(s) * im.cfg.sampleRate) : 0u;
  r = TimedRamp{from, value, im.clock, frames, 0u};
}

std::uint32_t StepEffectParams(Impl& im, EffectInstance& e) noexcept {
  if (e.ctl == nullptr) return 0u;
  const ClockSample end = im.clock + im.cfg.blockFrames;
  std::uint32_t mask = 0;
  for (std::uint32_t k = 0; k < e.paramCount; ++k) {
    EffectParamControl& c = e.ctl[k];
    const float v = ClampEffectParam(e, k, RampValue(c.set, end) + c.contrib);
    e.blockStart[k] = c.applied;
    e.blockEnd[k] = v;
    if (v != c.applied) mask |= k < 31u ? (1u << k) : (1u << 31);
    c.applied = v;
  }
  return mask;
}

void ApplyBusEffectParam(Impl& im, const cmd::SetBusVolume& c) noexcept {
  const std::uint32_t effect = (c.reserved >> cmd::kBusVolumeEffectIndexShift) & cmd::kBusVolumeIndexMask;
  const std::uint32_t param = (c.reserved >> cmd::kBusVolumeParamIndexShift) & cmd::kBusVolumeIndexMask;
  const Id id = c.bus == 0u && im.rd.masterSlot != kNoSlot ? im.busDefs[im.rd.masterSlot].id : c.bus;
  const std::uint32_t slot = im.busMap.Find(id);
  if (slot == IdMap::kNotFound || !base::rows::IsLive(im.tables.Bus.rows, slot)) {
    PostWarning(im, SND_ERR_NOT_FOUND, 0u, c.bus);
    return;
  }
  EffectInstance* e = BusEffectInstance(im, slot, effect);
  if (e == nullptr || param >= e->paramCount) {
    PostWarning(im, SND_ERR_NOT_FOUND, 0u, c.bus);
    return;
  }
  if (EffectParamIsInitOnly(e->plugin, e->type, param)) {  // read at init only: refused, warned
    PostWarning(im, SND_ERR_INVALID_ARGUMENT, 0u, c.bus);
    return;
  }
  SetEffectParam(im, *e, param, c.volume, c.fadeSeconds);
}

// ---------------------------------------------------------------------------
// D-131: sidechain sources (Render, MixBuses)
// ---------------------------------------------------------------------------
bool FindSidechain(Impl& im, const BankRecord& rec, SlotIndex s, std::uint32_t orderIndex,
                   SidechainSource* out) noexcept {
  constexpr schema::SystemId kSys = schema::SystemId::Render;
  schema::Tables& t = im.tables;
  out->count = 0;
  out->prevRow = kNoSlot;
  if (rec.duckCount == 0u || rec.sidechainIndex == nullptr) return false;
  const bk::BankView& v = *rec.view;
  const schema::TableRows& br = t.Bus.rows;
  const std::uint32_t* go = SND_R(Bus_graphOrder);
  const std::uint32_t frames = im.cfg.blockFrames;
  for (std::uint32_t k = 0; k < rec.duckCount; ++k) {
    if (rec.sidechainIndex[k] == kNoSlot) continue;
    const std::uint32_t tb = v.Column<bk::col::DUCK_targetBus>()[k];
    if (tb >= rec.busCount || rec.busSlot[tb] != s) continue;
    const std::uint32_t sb = v.Column<bk::col::DUCK_source>()[k];
    if (sb >= rec.busCount) continue;
    const SlotIndex src = rec.busSlot[sb];
    if (src == kNoSlot || !base::rows::IsLive(br, src)) continue;
    const std::uint32_t ch = im.busDefs[src].channels == 0u || im.busDefs[src].channels > kMaxChannels
                                 ? im.cfg.outChannels
                                 : im.busDefs[src].channels;
    if (src != s && go[base::rows::DenseOf(br, src)] < orderIndex) {
      const float* buf = BusBufferOf(im, src);  // mixed earlier this block (post-fader)
      for (std::uint32_t c = 0; c < ch; ++c) out->channels[c] = buf + std::size_t{c} * frames;
      out->count = ch;
      return true;
    }
    // Mixed later (or the bus itself): last block's output of the source.
    const float* buf = rec.sidechainPrev + std::size_t{rec.sidechainIndex[k]} * kMaxChannels * frames;
    const std::uint32_t pch = rec.sidechainChannels[k] == 0u ? ch : rec.sidechainChannels[k];
    for (std::uint32_t c = 0; c < pch && c < kMaxChannels; ++c) out->channels[c] = buf + std::size_t{c} * frames;
    out->count = pch;
    out->prevRow = k;
    return true;
  }
  return false;
}

void StoreSidechainSources(Impl& im, SlotIndex s, std::uint32_t orderIndex, const float* buf, std::uint32_t ch) noexcept {
  constexpr schema::SystemId kSys = schema::SystemId::Render;
  schema::Tables& t = im.tables;
  const schema::TableRows& br = t.Bus.rows;
  const std::uint32_t* go = SND_R(Bus_graphOrder);
  const std::uint32_t frames = im.cfg.blockFrames;
  // Every DUCK kind-0 row whose source is `s` and whose target is mixed before it (or is
  // `s` itself) reads this output next block. The rows live in the target bus's defining
  // bank; walk the duck rules (few) and their banks.
  for (std::uint32_t b = 0; b < im.cfg.maxBanks; ++b) {
    const BankAudio& ba = im.ap.banks[b];
    if (ba.state != BankAudioState::Registered || ba.rec == nullptr) continue;
    BankRecord& rec = *ba.rec;
    if (rec.sidechainPrev == nullptr) continue;
    const bk::BankView& v = *rec.view;
    for (std::uint32_t k = 0; k < rec.duckCount; ++k) {
      if (rec.sidechainIndex[k] == kNoSlot) continue;
      const std::uint32_t sb = v.Column<bk::col::DUCK_source>()[k];
      const std::uint32_t tb = v.Column<bk::col::DUCK_targetBus>()[k];
      if (sb >= rec.busCount || tb >= rec.busCount || rec.busSlot[sb] != s) continue;
      const SlotIndex tgt = rec.busSlot[tb];
      if (tgt == kNoSlot || !base::rows::IsLive(br, tgt)) continue;
      if (tgt != s && go[base::rows::DenseOf(br, tgt)] > orderIndex) continue;  // reads this block's output
      float* dst = rec.sidechainPrev + std::size_t{rec.sidechainIndex[k]} * kMaxChannels * frames;
      std::memcpy(dst, buf, sizeof(float) * ch * frames);
      rec.sidechainChannels[k] = ch;
    }
  }
}

}  // namespace snd::core
