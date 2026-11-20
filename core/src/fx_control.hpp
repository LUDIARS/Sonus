// Runtime effect control (stage 3, group 2; included by core_impl.hpp):
//   - D-130: bus effect parameters changed at run time (target value + fade, composed
//     with per-block contributions), ParamBlock start / end / changed mask per block;
//   - D-131: compressor sidechain from the bank's DUCK rows of kind 0 (sidechain);
//   - D-132..D-134: heavy voice effects in a fixed pool of voice-effect slots
//     (snd_config max_voice_effects / voice_effect_chain / voice_effect_bytes /
//     voice_effect_settings), run per real voice after the standard voice filter.
#ifndef SND_CORE_SRC_FX_CONTROL_HPP
#define SND_CORE_SRC_FX_CONTROL_HPP

#include <cstdint>

#include "snd/dsp/effects.hpp"
#include "snd/ext.h"
#include "snd/internal/common.hpp"
#include "snd/internal/schema.hpp"

namespace snd::core {

// Command variants (reserved fields, stage 3 group 2; the shared assignment table of the
// coverage session). A variant of 0 is the command's original meaning.
//  - SetBusVolume.reserved: bits 0-7 variant; variant 1 = bus effect parameter (D-130):
//    bits 8-15 effect index in the bus chain, bits 16-23 parameter index; `volume` is the
//    value and `fadeSeconds` the fade.
//  - SetSwitch.reserved[0] = 1: voice effect of an Event (D-132): scope Global, `group` =
//    event id, `value` = effect kind (0 = remove the stage), reserved[1] = stage,
//    reserved[2] = number of parameter values that follow (SetParam variant 1, Global).
//  - SetParam.reserved[0] = 1: voice effect parameter (D-132): reserved[1] = stage,
//    reserved[2] = parameter index, `value`. Scope Global: of the Event `param` (its
//    configuration for voices that start later). Scope Playback: of the playback `target`.
inline constexpr std::uint32_t kBusVolumeVariantMask = 0xFFu;
inline constexpr std::uint32_t kBusVolumeVariantEffectParam = 1u;
inline constexpr std::uint8_t kSwitchVariantVoiceEffect = 1u;
inline constexpr std::uint8_t kParamVariantVoiceEffect = 1u;
constexpr std::uint32_t BusEffectParamReserved(std::uint32_t effect, std::uint32_t param) noexcept {
  return kBusVolumeVariantEffectParam | ((effect & 0xFFu) << 8) | ((param & 0xFFu) << 16);
}

// ---------------------------------------------------------------------------
// D-130: one runtime-controlled parameter of a bus effect instance.
// value(block end) = clamp(RampValue(set, blockEnd) + contrib) to the parameter range.
// ---------------------------------------------------------------------------
struct EffectParamControl {
  // ApplyCommands (SetEffectParam): the set value; starts at the bank value
  // (EffectInstance::params[k]) with no ramp.
  TimedRamp set;
  // Contributions added to the set value (stage 3 group 3: snapshots SNPV.targetKind 1,
  // Parameter curves CurveTarget::EffectParam). Written by EvaluateDspParams every block
  // (it owns the value, including putting it back to 0); Render only reads it. 0 = none.
  float contrib;
  // Render: the value at the end of the last block (= the next ParamBlock start).
  float applied;
};

// ---------------------------------------------------------------------------
// D-132: voice effects
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kMaxVoiceEffectChain = 4;  // upper bound of snd_config voice_effect_chain

// Effects configured for an Event (snd_set_event_voice_effect). ApplyCommands writes;
// Render reads (at the start of a voice and when it gets a voice-effect slot).
struct VfxStageConfig {
  Id kind;                  // 0 = empty stage
  std::uint32_t paramCount; // values given
  float params[dsp::kMaxEffectParams];
};
struct VfxEventEntry {
  Id event;
  std::uint32_t used;
  VfxStageConfig stage[kMaxVoiceEffectChain];
};
// Values set for a playback (snd_set_playback_effect_param). ApplyCommands writes;
// Render reads every block for the voices of the playback that hold a slot.
struct VfxPlaybackEntry {
  HandleRef playback;
  std::uint32_t used;
  std::uint32_t mask[kMaxVoiceEffectChain];  // bit i: value[stage][i] set
  float value[kMaxVoiceEffectChain][dsp::kMaxEffectParams];
};
// One slot of the voice-effect pool (D-132). Bound to (real slot, assignment, voice).
// Render's serial part writes it; during the chunks only the chunk of the owning voice
// reads it and touches its state memory (D-43: looked up through the real slot).
struct VfxSlot {
  SlotIndex real;
  std::uint32_t assignSeq;
  SlotIndex voice;
  std::uint32_t voiceGen;
  std::uint32_t used;
  std::uint32_t stages;
  std::uint32_t playbackEntry;  // cached VfxPlaybackEntry index (kInvalidSlot = none)
  std::uint32_t channels;
  Id kind[kMaxVoiceEffectChain];
  const snd_dsp_effect* plugin[kMaxVoiceEffectChain];
  void* state[kMaxVoiceEffectChain];
  std::uint32_t count[kMaxVoiceEffectChain];
  std::uint32_t mask[kMaxVoiceEffectChain];
  std::uint32_t cfgStage[kMaxVoiceEffectChain];  // configured stage index (snd_set_*_effect `slot`)
  float base[kMaxVoiceEffectChain][dsp::kMaxEffectParams];   // from the Event at the voice start
  float start[kMaxVoiceEffectChain][dsp::kMaxEffectParams];  // this block's ParamBlock
  float end[kMaxVoiceEffectChain][dsp::kMaxEffectParams];
  snd_render_context rc;
};

struct VoiceFxPriv {
  // Configuration (snd_config, clamped at Create).
  std::uint32_t slotCount = 0;   // max_voice_effects (<= real voices + fade reserve)
  std::uint32_t chain = 0;       // voice_effect_chain (<= kMaxVoiceEffectChain)
  std::uint32_t slotBytes = 0;   // voice_effect_bytes rounded up to 64
  std::uint32_t settings = 0;    // voice_effect_settings: entries of each table below
  VfxEventEntry* events = nullptr;        // [settings]
  VfxPlaybackEntry* playbacks = nullptr;  // [settings]
  VfxSlot* slots = nullptr;               // [slotCount]
  std::uint8_t* memory = nullptr;         // [slotCount * slotBytes], MemTag::DspPool
  std::uint32_t* ofReal = nullptr;        // [realTotal] slot + 1 bound to the real slot (0 = none)
  std::uint32_t* triedAssign = nullptr;   // [realTotal] assignSeq whose acquisition was decided
  std::uint32_t* voiceEntry = nullptr;    // [maxVirtual] event entry at the voice start (kInvalidSlot = none)
  Id* voiceKinds = nullptr;               // [maxVirtual * kMaxVoiceEffectChain] stage kinds at the voice start
  // Statistics (audio thread): voices that got a slot / played dry for want of one (D-133).
  std::uint64_t acquired = 0;
  std::uint64_t dry = 0;
  std::uint64_t dryTooLarge = 0;  // of `dry`: the chain's state did not fit voice_effect_bytes
};

}  // namespace snd::core

#endif  // SND_CORE_SRC_FX_CONTROL_HPP
