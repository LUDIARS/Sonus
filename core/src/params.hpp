// Parameter lookup for the systems that read parameter values (4.3):
// Playback scope -> Emitter scope -> Global scope -> PARM default.
//
// Emitter / Playback scopes: the rows of owner o form a chain starting at the
// inline slot o * kParamInlineSlots, linked through `nextSlot` (ApplyCommands
// appends; rows of an owner are only removed together with the owner).
// Switches store the value id in the float's bits.
#ifndef SND_CORE_SRC_PARAMS_HPP
#define SND_CORE_SRC_PARAMS_HPP

#include <bit>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

template <schema::SystemId kSys>
inline bool FindPlaybackParam(const Impl& im, Id id, SlotIndex playback, float* out) noexcept {
  const schema::Tables& t = im.tables;
  if (playback == kNoSlot) return false;
  const schema::TableRows& rows = t.ParamPlayback.rows;
  const Id* ids = SND_R(ParamPlayback_paramId);
  const SlotIndex* next = SND_R(ParamPlayback_nextSlot);
  const float* cur = SND_R(ParamPlayback_current);
  SlotIndex s = playback * kParamInlineSlots;
  for (std::uint32_t guard = 0; guard < rows.capacity && s != kNoSlot; ++guard) {
    const std::uint32_t d = base::rows::DenseOf(rows, s);
    if (d == kNoSlot) return false;
    if (ids[d] == id) {
      *out = cur[d];
      return true;
    }
    s = next[d];
  }
  return false;
}

template <schema::SystemId kSys>
inline bool FindEmitterParam(const Impl& im, Id id, SlotIndex emitter, float* out) noexcept {
  const schema::Tables& t = im.tables;
  if (emitter == kNoSlot) return false;
  const schema::TableRows& rows = t.ParamEmitter.rows;
  const Id* ids = SND_R(ParamEmitter_paramId);
  const SlotIndex* next = SND_R(ParamEmitter_nextSlot);
  const float* cur = SND_R(ParamEmitter_current);
  SlotIndex s = emitter * kParamInlineSlots;
  for (std::uint32_t guard = 0; guard < rows.capacity && s != kNoSlot; ++guard) {
    const std::uint32_t d = base::rows::DenseOf(rows, s);
    if (d == kNoSlot) return false;
    if (ids[d] == id) {
      *out = cur[d];
      return true;
    }
    s = next[d];
  }
  return false;
}

template <schema::SystemId kSys>
inline bool FindGlobalParam(const Impl& im, Id id, float* out) noexcept {
  const schema::Tables& t = im.tables;
  const std::uint32_t slot = im.ap.globalParams.Find(id);
  if (slot == IdMap::kNotFound) return false;
  const std::uint32_t d = base::rows::DenseOf(t.ParamGlobal.rows, slot);
  if (d == kNoSlot) return false;
  if (SND_R(ParamGlobal_paramId)[d] != id) return false;
  *out = SND_R(ParamGlobal_current)[d];
  return true;
}

inline bool FindOverride(const Impl& im, std::uint32_t kind, Id id, std::uint32_t* bits) noexcept {
  for (std::uint32_t i = 0; i < im.ap.overrideCount; ++i) {
    const LiveOverride& o = im.ap.overrides[i];
    if (o.used != 0u && o.kind == kind && o.id == id) {
      *bits = o.value;
      return true;
    }
  }
  return false;
}

// Value of a continuous parameter (or the bits of a switch as float).
template <schema::SystemId kSys>
inline float ParamValueOf(const Impl& im, Id id, SlotIndex playback, SlotIndex emitter, bool* found = nullptr) noexcept {
  std::uint32_t bits = 0;
  if (im.ap.overrideCount != 0u &&
      (FindOverride(im, static_cast<std::uint32_t>(cmd::LiveOverrideKind::Param), id, &bits) ||
       FindOverride(im, static_cast<std::uint32_t>(cmd::LiveOverrideKind::Switch), id, &bits))) {
    if (found != nullptr) *found = true;
    return std::bit_cast<float>(bits);
  }
  float v = 0;
  if (FindPlaybackParam<kSys>(im, id, playback, &v) || FindEmitterParam<kSys>(im, id, emitter, &v) ||
      FindGlobalParam<kSys>(im, id, &v)) {
    if (found != nullptr) *found = true;
    return v;
  }
  const std::uint32_t def = im.paramDefMap.Find(id);
  if (def != IdMap::kNotFound) {
    if (found != nullptr) *found = true;
    const ParamDef& pd = im.paramDefs[def];
    return pd.kind == 1u ? std::bit_cast<float>(static_cast<std::uint32_t>(0)) : pd.defaultValue;
  }
  if (found != nullptr) *found = false;
  return 0.0f;
}

template <schema::SystemId kSys>
inline Id SwitchValueOf(const Impl& im, Id group, SlotIndex playback, SlotIndex emitter) noexcept {
  bool found = false;
  const float f = ParamValueOf<kSys>(im, group, playback, emitter, &found);
  return found ? std::bit_cast<std::uint32_t>(f) : 0u;
}

}  // namespace snd::core

#endif  // SND_CORE_SRC_PARAMS_HPP
