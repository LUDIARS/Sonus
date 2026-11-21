// Physics-material audio (design.md 9), C++ wrapper of snd/physics/physics.h.
// Header only: the PODs are the C structs, enumerations mirror the C values. The frozen
// part (PhysicsAudio, Contact, SurfaceWeight, ContactKind, Motion, Create / Destroy /
// SubmitContacts / PlayFootstep) comes from snd/physics.hpp.
#ifndef SND_PHYSICS_PHYSICS_HPP
#define SND_PHYSICS_PHYSICS_HPP

#include <cstdint>

#include "snd/physics.hpp"
#include "snd/physics/physics.h"
#include "snd/sonus.hpp"

namespace snd::phys {

using Config = snd_physics_config;
using ParamIds = snd_physics_param_ids;
using Param = snd_physics_param;
using Record = snd_physics_record;
using Observer = snd_physics_observer;
using CurvePoint = snd_physics_curve_point;
using Curve = snd_physics_curve;
using Curves = snd_physics_curves;
using TableDesc = snd_physics_table_desc;
using MaterialEntry = snd_physics_material_entry;
using Stats = snd_physics_stats;

inline constexpr std::uint16_t kUnmapped = SND_PHYSICS_UNMAPPED;
inline constexpr std::uint16_t kDefaultMaterial = SND_PHYSICS_DEFAULT_MATERIAL;

enum class RecordKind : std::uint32_t {
  Play = SND_PHYSICS_RECORD_PLAY,
  Start = SND_PHYSICS_RECORD_START,
  Update = SND_PHYSICS_RECORD_UPDATE,
  Stop = SND_PHYSICS_RECORD_STOP,
  Fallback = SND_PHYSICS_RECORD_FALLBACK,
};

enum class Source : std::uint32_t {
  Contacts = SND_PHYSICS_SOURCE_CONTACT,  // (not "Contact": that is the snd_contact alias)
  Footstep = SND_PHYSICS_SOURCE_FOOTSTEP,
  PlayAt = SND_PHYSICS_SOURCE_PLAY_AT,
};

enum class Stage : std::uint32_t {
  Pair = SND_PHYSICS_STAGE_PAIR,
  Single = SND_PHYSICS_STAGE_SINGLE,
  Default = SND_PHYSICS_STAGE_DEFAULT,
  Nothing = SND_PHYSICS_STAGE_NOTHING,
  FootMotion = SND_PHYSICS_STAGE_FOOT_MOTION,
  FootAny = SND_PHYSICS_STAGE_FOOT_ANY,
  FootDefault = SND_PHYSICS_STAGE_FOOT_DEFAULT,
  Direct = SND_PHYSICS_STAGE_DIRECT,
};

enum class StopReason : std::uint32_t {
  None = SND_PHYSICS_STOP_NONE,
  Timeout = SND_PHYSICS_STOP_TIMEOUT,
  Evicted = SND_PHYSICS_STOP_EVICTED,
  Destroyed = SND_PHYSICS_STOP_DESTROYED,
  Ended = SND_PHYSICS_STOP_ENDED,
};

// Every contact kind (setters).
inline constexpr ContactKind kAllKinds = static_cast<ContactKind>(SND_PHYSICS_ALL_KINDS);
// The motion-independent footstep column (SetFootstepEvent).
inline constexpr Motion kAnyMotion = static_cast<Motion>(SND_PHYSICS_ANY_MOTION);

inline Config DefaultConfig() noexcept {
  Config c;
  snd_physics_config_init(&c);
  return c;
}
inline TableDesc EmptyTable() noexcept {
  TableDesc d;
  snd_physics_table_desc_init(&d);
  return d;
}

inline Result Create(System* sys, const Config& cfg, IAllocator* allocator, PhysicsAudio** out) noexcept {
  snd_allocator a{};
  if (allocator != nullptr) a = ToC(*allocator);
  return static_cast<Result>(snd_physics_create(sys, &cfg, allocator ? &a : nullptr, out));
}
inline Result PlayAt(PhysicsAudio* p, Id event, const Vec3& position) noexcept {
  return static_cast<Result>(snd_physics_play_at(p, event, &position));
}
inline Result SetObserver(PhysicsAudio* p, Observer observer, void* user) noexcept {
  return static_cast<Result>(snd_physics_set_observer(p, observer, user));
}
inline Result SetTable(PhysicsAudio* p, const TableDesc& desc) noexcept {
  return static_cast<Result>(snd_physics_set_table(p, &desc));
}
inline Result LoadBankTable(PhysicsAudio* p, BankHandle bank, Id table) noexcept {
  return static_cast<Result>(snd_physics_load_bank_table(p, bank.ToC(), table));
}
inline std::uint32_t MaterialCount(const PhysicsAudio* p) noexcept { return snd_physics_material_count(p); }
inline Result SetSingleEvent(PhysicsAudio* p, std::uint16_t material, ContactKind kind, Id event) noexcept {
  return static_cast<Result>(snd_physics_set_single_event(p, material, static_cast<snd_contact_kind>(kind), event));
}
inline Result SetPairEvent(PhysicsAudio* p, std::uint16_t a, std::uint16_t b, ContactKind kind, Id event) noexcept {
  return static_cast<Result>(snd_physics_set_pair_event(p, a, b, static_cast<snd_contact_kind>(kind), event));
}
inline Result SetDefaultEvent(PhysicsAudio* p, ContactKind kind, Id event) noexcept {
  return static_cast<Result>(snd_physics_set_default_event(p, static_cast<snd_contact_kind>(kind), event));
}
inline Result SetFootstepEvent(PhysicsAudio* p, std::uint16_t material, Motion motion, Id event) noexcept {
  return static_cast<Result>(snd_physics_set_footstep_event(p, material, static_cast<snd_motion>(motion), event));
}
inline Result SetCurves(PhysicsAudio* p, std::uint16_t material, const Curves* curves) noexcept {
  return static_cast<Result>(snd_physics_set_curves(p, material, curves));
}
inline Result SetMaterialMap(PhysicsAudio* p, const MaterialEntry* entries, std::uint32_t count) noexcept {
  return static_cast<Result>(snd_physics_set_material_map(p, entries, count));
}
inline Result SetMaterialEntry(PhysicsAudio* p, std::uint16_t gameMaterial, const MaterialEntry& entry) noexcept {
  return static_cast<Result>(snd_physics_set_material_entry(p, gameMaterial, &entry));
}
inline Result SetSurfaceParam(PhysicsAudio* p, std::uint16_t gameMaterial, Id param, float value) noexcept {
  return static_cast<Result>(snd_physics_set_surface_param(p, gameMaterial, param, value));
}
inline Result GetStats(const PhysicsAudio* p, Stats* out) noexcept {
  if (out != nullptr) out->struct_size = static_cast<std::uint32_t>(sizeof(Stats));
  return static_cast<Result>(snd_physics_get_stats(p, out));
}
inline Result ResetStats(PhysicsAudio* p) noexcept { return static_cast<Result>(snd_physics_reset_stats(p)); }

}  // namespace snd::phys

#endif  // SND_PHYSICS_PHYSICS_HPP
