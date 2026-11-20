// Heavy voice effects in the DSP pool (stage 3, group 2; design 4 step 5, 7
// "再生単位のフィルタ" / "空間化"; D-132..D-134).
//
// D-132  A fixed pool of voice-effect slots is allocated at Create
//        (snd_config max_voice_effects slots of voice_effect_bytes, MemTag::DspPool;
//        at most voice_effect_chain stages per voice). A slot belongs to one real
//        assignment of one voice: Render's serial part binds it (RealVoice slot ->
//        VfxPriv::ofReal), copies the block's parameters into it and passes its index
//        in the item (ItemInput::vfxSlot); the voice's chunk runs the chain in place on
//        the voice's own buffers, after the resampler and the standard LP / HP voice
//        filter and before the envelopes (release, D-15 fades, sample-accurate stop)
//        and the pan. Nothing else touches the slot while the voice holds it (D-43).
//        The effects of a voice come from its Event's configuration
//        (snd_set_event_voice_effect) as it is when the voice takes a slot: at its start
//        and again when it comes back from virtual (kinds and values read together, so
//        they always match; a stage removed meanwhile is simply absent, no warning).
//        A voice holding a slot keeps what it has. No per-voice copy of the values is
//        kept: virtualising drops the effect state anyway, so coming back is a new start.
//        The voices of a nested Event use the nested (child) Event's configuration.
//        snd_set_playback_effect_param values of the voice's playback override the
//        Event's: from the first block of a slot (no ramp from the Event's value), and
//        for a sounding voice from the next block, interpolated inside it (D-26).
//        [init] parameters (kParamInitOnly) are read by the effect's init only. A playback
//        value is judged when applied by the kind the playback's Event has set NOW in that
//        stage: an [init] parameter of it is refused with a warning (object = playback).
//        When values are put into a slot that is sounding, [init] parameters of the
//        slot's own kind are skipped (the slot may hold an older kind).
// D-133  No free slot, or a chain whose state does not fit voice_effect_bytes: the voice
//        plays dry (no heavy effects) for this real assignment; never refused. Counted
//        (System::GetVoiceFxStats) and reported once per assignment as a warning
//        (SND_ERR_LIMIT_REACHED, detail SND_END_DETAIL_VOICE_EFFECT_SLOTS, object = the
//        playback handle). A voice that leaves its real slot (virtualised, stolen, ended)
//        gives the slot back; back to real it takes one again with a fresh state (it
//        fades in from 0 over the short fade, D-15, so the reset does not click).
// D-134  Voice effects have no tail of their own: the output ends with the voice (end of
//        the source, end of the stop fade, steal fade). The envelopes are applied after
//        the effects, so a fade-out fades the effect output too. Sounds that need a tail
//        use bus effects.
// Plugins (Config.plugins, D-31: a plugin with the same type id wins) work as voice
// effects too: their state_bytes and init are called on the audio thread when the slot
// is taken, so a plugin used as a voice effect must keep init free of allocation, locks
// and IO (principle 1).
#include <cmath>
#include <cstring>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {

constexpr schema::SystemId kSys = schema::SystemId::Render;

std::uint32_t FindEventEntry(const VoiceFxPriv& x, Id event) noexcept {
  for (std::uint32_t i = 0; i < x.settings; ++i) {
    if (x.events[i].used != 0u && x.events[i].event == event) return i;
  }
  return kNoSlot;
}

const snd_dsp_effect* PluginOf(const Impl& im, Id kind) noexcept {
  for (std::uint32_t p = 0; p < im.config.plugins.effect_count; ++p) {
    if (im.config.plugins.effects[p].effect_type == kind) return &im.config.plugins.effects[p];
  }
  return nullptr;
}

// Fills the stage's parameter values with the kind's defaults (plugins: 0).
void DefaultStage(const Impl& im, VfxStageConfig& sc) noexcept {
  for (float& v : sc.params) v = 0.0f;
  if (PluginOf(im, sc.kind) != nullptr) return;
  if (const dsp::EffectInfo* info = dsp::FindEffect(sc.kind); info != nullptr) dsp::EffectDefaultParams(info->kind, sc.params);
}

// Number of ParamBlock entries of a stage: the standard kind's table, else the values given.
std::uint32_t StageCount(const snd_dsp_effect* plugin, Id kind, std::uint32_t given) noexcept {
  if (plugin == nullptr) {
    if (const dsp::EffectInfo* info = dsp::FindEffect(kind); info != nullptr) return info->paramCount;
  }
  return given < dsp::kMaxEffectParams ? given : dsp::kMaxEffectParams;
}

float ClampStage(const snd_dsp_effect* plugin, Id kind, std::uint32_t param, float v) noexcept {
  if (plugin != nullptr) return v;
  const dsp::EffectInfo* info = dsp::FindEffect(kind);
  if (info == nullptr || param >= info->paramCount) return v;
  const dsp::EffectParamInfo& p = info->params[param];
  return v < p.minValue ? p.minValue : (v > p.maxValue ? p.maxValue : v);
}

// Event id of a voice (0: none, e.g. dynamic sources).
Id EventOfVoice(const Impl& im, const ClipRef& cr, const VoiceStatic& vs) noexcept {
  const BankRecord* br = RegisteredBank(im, cr.bankSlot);
  if (br == nullptr || vs.eventRow >= br->view->Rows(bf::chunk::EVNT)) return 0u;
  return br->view->Column<bk::col::EVNT_id>()[vs.eventRow];
}

void FreeSlot(VoiceFxPriv& x, std::uint32_t s) noexcept {
  VfxSlot& sl = x.slots[s];
  if (sl.real < kInvalidSlot && x.ofReal[sl.real] == s + 1u) x.ofReal[sl.real] = 0u;
  sl.used = 0u;
}

std::uint32_t PlaybackEntry(Impl& im, VfxSlot& sl, SlotIndex pb) noexcept {
  VoiceFxPriv& x = im.vfx;
  const schema::TableRows& pr = im.tables.Playback.rows;
  if (pb == kNoSlot || !base::rows::IsLive(pr, pb)) return kNoSlot;
  const std::uint32_t gen = pr.generation[pb];
  if (sl.playbackEntry < x.settings) {
    const VfxPlaybackEntry& e = x.playbacks[sl.playbackEntry];
    if (e.used != 0u && e.playback.slot == pb && e.playback.generation == gen) return sl.playbackEntry;
  }
  for (std::uint32_t i = 0; i < x.settings; ++i) {
    const VfxPlaybackEntry& e = x.playbacks[i];
    if (e.used != 0u && e.playback.slot == pb && e.playback.generation == gen) return sl.playbackEntry = i;
  }
  return kNoSlot;
}

// Takes a slot for voice `v` in real slot `r` (assignment `seq`); 0 = none (dry or no effects).
std::uint32_t Acquire(Impl& im, SlotIndex v, std::uint32_t d, SlotIndex r, std::uint32_t seq,
                      const VoiceStatic& vs) noexcept {
  schema::Tables& t = im.tables;
  VoiceFxPriv& x = im.vfx;
  // The Event's configuration as it is now (kinds and values together): read every time
  // the voice takes a slot (start, back from virtual). No configuration: no effect.
  const Id eventId = EventOfVoice(im, SND_R(Voice_clipRef)[d], vs);
  if (eventId == 0u) return 0u;
  const std::uint32_t entry = FindEventEntry(x, eventId);
  if (entry == kNoSlot) return 0u;
  const VfxEventEntry* ev = &x.events[entry];
  Id kinds[kMaxVoiceEffectChain] = {};
  for (std::uint32_t k = 0; k < x.chain && k < kMaxVoiceEffectChain; ++k) kinds[k] = ev->stage[k].kind;
  std::uint32_t wanted = 0;
  for (std::uint32_t k = 0; k < x.chain; ++k) wanted += kinds[k] != 0u ? 1u : 0u;
  if (wanted == 0u) return 0u;
  const SlotIndex pb = SND_R(Voice_playbackSlot)[d];
  const schema::TableRows& pr = t.Playback.rows;
  const std::uint64_t pbHandle =
      pb != kNoSlot && base::rows::IsLive(pr, pb) ? HandleRef{pb, pr.generation[pb]}.Value() : 0u;
  std::uint32_t s = kNoSlot;
  for (std::uint32_t i = 0; i < x.slotCount; ++i) {
    if (x.slots[i].used == 0u) {
      s = i;
      break;
    }
  }
  if (s == kNoSlot) {  // D-133: dry
    ++x.dry;
    PostWarning(im, SND_ERR_LIMIT_REACHED, SND_END_DETAIL_VOICE_EFFECT_SLOTS, pbHandle);
    return 0u;
  }
  VfxSlot& sl = x.slots[s];
  std::uint8_t* mem = x.memory + std::size_t{s} * x.slotBytes;
  const std::uint32_t channels = vs.channels == 0u ? 1u : (vs.channels > kMaxChannels ? kMaxChannels : vs.channels);
  // Size every stage first: all of the chain fits, or the voice plays dry (D-133).
  snd_effect_desc descs[kMaxVoiceEffectChain];
  std::uint32_t bytes[kMaxVoiceEffectChain] = {};
  std::uint32_t used = 0;
  sl.stages = 0;
  for (std::uint32_t k = 0; k < x.chain; ++k) {
    if (kinds[k] == 0u) continue;
    const std::uint32_t n = sl.stages;
    const snd_dsp_effect* plugin = PluginOf(im, kinds[k]);
    const VfxStageConfig& sc = ev->stage[k];
    const std::uint32_t count = StageCount(plugin, sc.kind, sc.paramCount);
    for (std::uint32_t i = 0; i < dsp::kMaxEffectParams; ++i) sl.base[n][i] = i < count ? ClampStage(plugin, sc.kind, i, sc.params[i]) : 0.0f;
    snd_effect_desc& dd = descs[n];
    dd = snd_effect_desc{};
    dd.effect_type = sc.kind;
    dd.channels = channels;
    dd.max_frames = im.cfg.blockFrames;
    dd.param_count = count;
    dd.initial_params = sl.base[n];
    dd.sample_rate = im.cfg.sampleRate;
    std::uint32_t b = plugin != nullptr ? plugin->state_bytes(plugin->user, &dd) : dsp::EffectStateBytes(dd, im.cfg.sampleRate);
    if (b == 0u) continue;  // unknown kind: the stage is skipped
    b = (b + 63u) & ~63u;
    if (std::uint64_t{used} + b > x.slotBytes) {  // D-133: the chain does not fit the slot
      ++x.dry;
      ++x.dryTooLarge;
      PostWarning(im, SND_ERR_LIMIT_REACHED, SND_END_DETAIL_VOICE_EFFECT_SLOTS, pbHandle);
      return 0u;
    }
    bytes[n] = b;
    used += b;
    sl.kind[n] = sc.kind;
    sl.plugin[n] = plugin;
    sl.count[n] = count;
    sl.cfgStage[n] = k;
    ++sl.stages;
  }
  if (sl.stages == 0u) return 0u;
  std::uint32_t off = 0;
  std::uint32_t live = 0;
  for (std::uint32_t n = 0; n < sl.stages; ++n) {
    void* state = mem + off;
    off += bytes[n];
    bool ok = true;
    if (sl.plugin[n] != nullptr) {
      sl.plugin[n]->init(sl.plugin[n]->user, state, &descs[n], im.cfg.sampleRate);
    } else {
      ok = dsp::EffectInit(state, bytes[n], descs[n], im.cfg.sampleRate);
    }
    if (!ok) continue;
    // Compact the stages that initialised.
    sl.kind[live] = sl.kind[n];
    sl.plugin[live] = sl.plugin[n];
    sl.count[live] = sl.count[n];
    sl.cfgStage[live] = sl.cfgStage[n];
    sl.state[live] = state;
    if (live != n) std::memcpy(sl.base[live], sl.base[n], sizeof(sl.base[n]));
    ++live;
  }
  sl.stages = live;
  if (live == 0u) return 0u;
  sl.used = 1u;
  sl.real = r;
  sl.assignSeq = seq;
  sl.voice = v;
  sl.voiceGen = vs.generation;
  sl.channels = channels;
  sl.playbackEntry = kNoSlot;
  for (std::uint32_t n = 0; n < live; ++n) {
    for (std::uint32_t i = 0; i < dsp::kMaxEffectParams; ++i) sl.end[n][i] = sl.base[n][i];
  }
  sl.fresh = 1u;  // first block: start = end = the target (playback values included, no ramp)
  x.ofReal[r] = s + 1u;
  ++x.acquired;
  return s + 1u;
}

}  // namespace

void ApplyVoiceFxSwitch(Impl& im, const cmd::SetSwitch& c) noexcept {
  VoiceFxPriv& x = im.vfx;
  const std::uint32_t stage = c.reserved[1];
  if (x.events == nullptr || stage >= x.chain || c.group == 0u) return;
  std::uint32_t e = FindEventEntry(x, c.group);
  if (c.value == 0u) {  // remove the stage
    if (e == kNoSlot) return;
    x.events[e].stage[stage] = VfxStageConfig{};
    bool any = false;
    for (std::uint32_t k = 0; k < kMaxVoiceEffectChain; ++k) any = any || x.events[e].stage[k].kind != 0u;
    if (!any) x.events[e].used = 0u;
    return;
  }
  if (e == kNoSlot) {
    for (std::uint32_t i = 0; i < x.settings && e == kNoSlot; ++i) {
      if (x.events[i].used == 0u) e = i;
    }
    if (e == kNoSlot) {
      PostWarning(im, SND_ERR_LIMIT_REACHED, 0u, c.group);
      return;
    }
    x.events[e] = VfxEventEntry{};
    x.events[e].event = c.group;
    x.events[e].used = 1u;
  }
  VfxStageConfig& sc = x.events[e].stage[stage];
  sc = VfxStageConfig{};
  sc.kind = c.value;
  DefaultStage(im, sc);
  const std::uint32_t given = c.reserved[2];
  sc.paramCount = given < dsp::kMaxEffectParams ? given : dsp::kMaxEffectParams;
}

void ApplyVoiceFxParam(Impl& im, const cmd::SetParam& c) noexcept {
  VoiceFxPriv& x = im.vfx;
  const std::uint32_t stage = c.reserved[1];
  const std::uint32_t k = c.reserved[2];
  if (x.events == nullptr || stage >= x.chain || k >= dsp::kMaxEffectParams || !std::isfinite(c.value)) return;
  if (static_cast<ParamScope>(c.scope) == ParamScope::Global) {
    const std::uint32_t e = FindEventEntry(x, c.param);
    if (e == kNoSlot) return;
    VfxStageConfig& sc = x.events[e].stage[stage];
    if (sc.kind == 0u) return;
    sc.params[k] = c.value;
    if (k + 1u > sc.paramCount) sc.paramCount = k + 1u;
    return;
  }
  if (static_cast<ParamScope>(c.scope) != ParamScope::Playback) return;
  const schema::TableRows& pr = im.tables.Playback.rows;
  if (!base::rows::IsCurrent(pr, c.target)) return;
  // An [init] parameter of the stage's kind (in the playback's Event configuration) cannot
  // change at run time: refused with a warning (object = playback).
  if (const std::uint32_t ev = FindEventEntry(x, PlaybackEventId(im, c.target)); ev != kNoSlot) {
    const Id kind = x.events[ev].stage[stage].kind;
    if (kind != 0u && EffectParamIsInitOnly(PluginOf(im, kind), kind, k)) {
      PostWarning(im, SND_ERR_INVALID_ARGUMENT, 0u, c.target.Value());
      return;
    }
  }
  std::uint32_t e = kNoSlot, freeEntry = kNoSlot;
  for (std::uint32_t i = 0; i < x.settings; ++i) {
    VfxPlaybackEntry& pe = x.playbacks[i];
    if (pe.used != 0u && pe.playback.slot == c.target.slot && pe.playback.generation == c.target.generation) {
      e = i;
      break;
    }
    if (freeEntry == kNoSlot && (pe.used == 0u || !base::rows::IsCurrent(pr, pe.playback))) freeEntry = i;
  }
  if (e == kNoSlot) {
    if (freeEntry == kNoSlot) {
      PostWarning(im, SND_ERR_LIMIT_REACHED, 0u, c.target.Value());
      return;
    }
    e = freeEntry;
    x.playbacks[e] = VfxPlaybackEntry{};
    x.playbacks[e].playback = c.target;
    x.playbacks[e].used = 1u;
  }
  x.playbacks[e].mask[stage] |= 1u << k;
  x.playbacks[e].value[stage][k] = c.value;
}

void VfxReleaseStale(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  VoiceFxPriv& x = im.vfx;
  const schema::TableRows& vr = t.Voice.rows;
  const VoiceRenderHold* hold = SND_W(Voice_renderHold);
  const SlotIndex* real = SND_R(Voice_realSlot);
  const RealVoiceAssign* assign = SND_R(RealVoice_assign);
  for (std::uint32_t s = 0; s < x.slotCount; ++s) {
    VfxSlot& sl = x.slots[s];
    if (sl.used == 0u) continue;
    bool keep = false;
    if (sl.voice < im.cfg.maxVirtual && base::rows::IsLive(vr, sl.voice) && im.ev.vs[sl.voice].generation == sl.voiceGen) {
      const std::uint32_t d = base::rows::DenseOf(vr, sl.voice);
      // D-43: a held voice keeps its state untouched (a late chunk may still run it).
      keep = hold[d].held != 0u || (real[d] == sl.real && assign[sl.real].assignSeq == sl.assignSeq);
    }
    if (!keep) FreeSlot(x, s);
  }
}

void VfxPrepareItem(Impl& im, const source::RenderItem& it, ItemInput& in) noexcept {
  schema::Tables& t = im.tables;
  VoiceFxPriv& x = im.vfx;
  in.vfxSlot = 0u;
  if (x.events == nullptr || x.chain == 0u) return;
  const SlotIndex r = it.realSlot;
  const SlotIndex v = it.voiceSlot;
  const std::uint32_t seq = SND_R(RealVoice_assign)[r].assignSeq;
  std::uint32_t cur = x.ofReal[r];
  if (cur != 0u) {
    const VfxSlot& sl = x.slots[cur - 1u];
    if (sl.used == 0u || sl.voice != v || sl.assignSeq != seq) {
      FreeSlot(x, cur - 1u);
      cur = 0u;
    }
  }
  const std::uint32_t d = base::rows::DenseOf(t.Voice.rows, v);
  if (cur == 0u) {
    if (x.triedAssign[r] == seq || d == kNoSlot) return;
    x.triedAssign[r] = seq;  // decided once per real assignment (D-133)
    cur = Acquire(im, v, d, r, seq, im.ev.vs[v]);
    if (cur == 0u) return;
  }
  VfxSlot& sl = x.slots[cur - 1u];
  // This block's parameters: the Event's values, overridden by the playback's (D-26 ramp).
  const SlotIndex pb = SND_R(Voice_playbackSlot)[d];
  const std::uint32_t pe = PlaybackEntry(im, sl, pb);
  for (std::uint32_t n = 0; n < sl.stages; ++n) {
    const std::uint32_t cfgStage = sl.cfgStage[n];
    std::uint32_t mask = 0;
    for (std::uint32_t i = 0; i < sl.count[n]; ++i) {
      float target = sl.base[n][i];
      if (pe != kNoSlot && (x.playbacks[pe].mask[cfgStage] & (1u << i)) != 0u &&
          !EffectParamIsInitOnly(sl.plugin[n], sl.kind[n], i)) {
        target = ClampStage(sl.plugin[n], sl.kind[n], i, x.playbacks[pe].value[cfgStage][i]);
      }
      sl.start[n][i] = sl.fresh != 0u ? target : sl.end[n][i];
      sl.end[n][i] = target;
      if (target != sl.start[n][i]) mask |= i < 31u ? (1u << i) : (1u << 31);
    }
    sl.mask[n] = mask;
  }
  sl.fresh = 0u;
  sl.rc = snd_render_context{};
  sl.rc.clock_sample = im.clock;
  sl.rc.playback = snd_playback_handle{pb != kNoSlot && base::rows::IsLive(t.Playback.rows, pb)
                                           ? HandleRef{pb, t.Playback.rows.generation[pb]}.Value()
                                           : 0u};
  sl.rc.sample_rate = im.cfg.sampleRate;
  sl.rc.block_frames = im.cfg.blockFrames;
  sl.rc.transport_beats = -1.0;
  in.vfxSlot = cur;
}

void VfxProcess(Impl& im, std::uint32_t slot, float* const* voiceOut, std::uint32_t channels) noexcept {
  VfxSlot& sl = im.vfx.slots[slot];
  dsp::EffectSlot slots[kMaxVoiceEffectChain];
  snd_param_block blocks[kMaxVoiceEffectChain];
  for (std::uint32_t n = 0; n < sl.stages; ++n) {
    blocks[n] = snd_param_block{sl.start[n], sl.end[n], sl.count[n], sl.mask[n], &sl.rc};
    slots[n] = dsp::EffectSlot{sl.kind[n], sl.plugin[n], sl.state[n], &blocks[n], nullptr, 0u, 0u};
  }
  dsp::ProcessEffectChain(slots, sl.stages, voiceOut, channels < sl.channels ? channels : sl.channels,
                          im.cfg.blockFrames);
}

}  // namespace snd::core
