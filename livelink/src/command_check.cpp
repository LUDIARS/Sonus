// The command rule table: the game API's entry rules (core/src/recorder.cpp) and the
// invariants of recorded commands, per command type. See command_check.hpp.
#include "snd/livelink/command_check.hpp"

#include <cmath>
#include <cstring>

#include "snd/livelink/wire.hpp"
#include "snd/types.h"

namespace snd::livelink {

namespace {

using CT = cmd::CommandType;
using L = CommandLimits;

template <class P>
P Load(const std::uint8_t* p) noexcept {
  P v;
  std::memcpy(&v, p, sizeof(P));
  return v;
}

bool Fin(float f) noexcept { return std::isfinite(f); }
bool Fin(const Vec3& v) noexcept { return Fin(v.x) && Fin(v.y) && Fin(v.z); }
bool Bool(std::uint32_t v) noexcept { return v <= 1u; }
bool Zero(const Vec3& v) noexcept { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

// --- recording invariants -----------------------------------------------------------------
bool Handle(HandleRef h, std::uint32_t capacity) noexcept {
  return h.slot < capacity && h.generation != 0u && h.generation < 0x80000000u;
}
bool HandleOrNone(HandleRef h, std::uint32_t capacity) noexcept {
  return (h.slot == 0u && h.generation == 0u) || Handle(h, capacity);
}
bool Transport(HandleRef h, const L& k, bool allowPlaybackForm) noexcept {
  if ((h.slot & SND_TRANSPORT_PLAYBACK_BIT) != 0u) {
    return allowPlaybackForm && Handle(HandleRef{h.slot & ~SND_TRANSPORT_PLAYBACK_BIT, h.generation}, k.playbacks);
  }
  return Handle(h, k.transports);
}
bool Scope(std::uint8_t scope, HandleRef target, const L& k) noexcept {
  switch (static_cast<ParamScope>(scope)) {
    case ParamScope::Global: return target.slot == 0u && target.generation == 0u;
    case ParamScope::Emitter: return Handle(target, k.emitters);
    case ParamScope::Playback: return Handle(target, k.playbacks);
  }
  return false;
}
// Fades / times the API clamps to >= 0 before recording (SetBusVolume ...).
bool Clamped(float s) noexcept { return Fin(s) && s >= 0.0f; }

// --- the game API's entry rules (recorder.cpp) -----------------------------------------------
// ValidWhen: kind <= DELAY_US, flags within PICKUP_ALIGN. Nothing else is checked by the
// API (transport, count, value, offset, reserved): core treats a dead transport as
// IMMEDIATE + warning (D-21).
bool When(const snd_when& w) noexcept {
  return w.kind <= SND_WHEN_DELAY_US && (w.flags & ~SND_WHEN_FLAG_PICKUP_ALIGN) == 0u;
}
bool Volume(float v) noexcept { return Fin(v) && v >= 0.0f; }      // SetBusVolume / SetCategoryVolume
bool Bpm(float b) noexcept { return b > 0.0f && Fin(b); }            // CreateTransport / SetTempo
bool BeatUnit(std::uint32_t u) noexcept { return u == 2u || u == 4u || u == 8u || u == 16u; }

bool True(const std::uint8_t*, const L&) noexcept { return true; }
bool False(const std::uint8_t*, const L&) noexcept { return false; }

constexpr CommandRule kCommandRules[] = {
    {CT::Suspend, true, &True, &True, "no arguments"},
    {CT::Resume, true, &True, &True, "no arguments"},
    {CT::BankRegister, false, &False, &False, "points to the recording process's prepared bank"},
    {CT::BankUnload, true,
     [](const std::uint8_t* p, const L&) noexcept {
       const auto c = Load<cmd::BankUnload>(p);
       return c.mode == SND_UNLOAD_STOP_AND_UNLOAD || c.mode == SND_UNLOAD_IF_UNUSED;
     },
     [](const std::uint8_t* p, const L& k) noexcept { return Handle(Load<cmd::BankUnload>(p).bank, k.banks); },
     "api: mode STOP_AND_UNLOAD / IF_UNUSED; shape: bank handle"},
    {CT::SetLanguage, true, &True, &True, "any language id (the name table is the game side's)"},
    {CT::Play, true,
     [](const std::uint8_t* p, const L&) noexcept { return When(Load<cmd::Play>(p).when); },
     [](const std::uint8_t* p, const L& k) noexcept {
       const auto c = Load<cmd::Play>(p);
       // Play takes any emitter value as given (core ignores a stale one); only PlayAt
       // writes a position (any value), Play / PlayOneShot leave it zero.
       return Handle(c.playback, k.playbacks) && (c.flags & ~(cmd::kPlayOneShot | cmd::kPlayHasPosition)) == 0u &&
              ((c.flags & cmd::kPlayHasPosition) != 0u || Zero(c.position)) && c.priorityHint <= 255u;
     },
     "api: When kind/flags, any emitter value (core ignores one that is not current); shape: playback handle, "
     "flags, position zero without kPlayHasPosition, priority <= 255"},
    {CT::PlaySynth, false, &False, &False, "callbacks of the recording process"},
    {CT::PlayUserBuffer, false, &False, &False, "game memory of the recording process"},
    {CT::PlayInput, false, &False, &False, "device input"},
    {CT::Stop, true,
     [](const std::uint8_t* p, const L&) noexcept {
       const auto c = Load<cmd::Stop>(p);
       return !std::isnan(c.fadeSeconds) && When(c.when);
     },
     [](const std::uint8_t* p, const L& k) noexcept { return Handle(Load<cmd::Stop>(p).playback, k.playbacks); },
     "api: fade not NaN (any other value: < 0 event default, +Inf allowed), When kind/flags; shape: playback"},
    {CT::PauseResume, true, &True,
     [](const std::uint8_t* p, const L& k) noexcept {
       const auto c = Load<cmd::PauseResume>(p);
       return Handle(c.playback, k.playbacks) && Bool(c.pause);
     },
     "api: any fade; shape: playback, pause 0/1"},
    {CT::SetScheduledStop, true,
     [](const std::uint8_t* p, const L&) noexcept { return When(Load<cmd::SetScheduledStop>(p).when); },
     [](const std::uint8_t* p, const L& k) noexcept {
       return Handle(Load<cmd::SetScheduledStop>(p).playback, k.playbacks);
     },
     "api: When kind/flags, any fade; shape: playback"},
    {CT::CreateEmitter, true, &True,
     [](const std::uint8_t* p, const L& k) noexcept { return Handle(Load<cmd::CreateEmitter>(p).emitter, k.emitters); },
     "shape: emitter handle"},
    {CT::DestroyEmitter, true, &True,
     [](const std::uint8_t* p, const L& k) noexcept { return Handle(Load<cmd::DestroyEmitter>(p).emitter, k.emitters); },
     "shape: emitter handle"},
    {CT::SetTransform, true, &True,
     [](const std::uint8_t* p, const L& k) noexcept {
       const auto c = Load<cmd::SetTransform>(p);
       return Handle(c.emitter, k.emitters) &&
              (c.mask & ~(cmd::kTransformPosition | cmd::kTransformForward | cmd::kTransformVelocity)) == 0u;
     },
     "api: any vectors; shape: emitter handle, mask"},
    {CT::SetListener, true,
     [](const std::uint8_t* p, const L& k) noexcept { return Load<cmd::SetListener>(p).index < k.listeners; }, &True,
     "api: index < max_listeners, any listener contents"},
    {CT::SetParam, true, [](const std::uint8_t* p, const L&) noexcept { return Fin(Load<cmd::SetParam>(p).value); },
     [](const std::uint8_t* p, const L& k) noexcept {
       const auto c = Load<cmd::SetParam>(p);
       return Scope(c.scope, c.target, k);
     },
     "api: finite value; shape: scope / target"},
    {CT::SetSwitch, true, &True,
     [](const std::uint8_t* p, const L& k) noexcept {
       const auto c = Load<cmd::SetSwitch>(p);
       return Scope(c.scope, c.target, k);
     },
     "api: any ids; shape: scope / target"},
    {CT::SetBusVolume, true,
     [](const std::uint8_t* p, const L&) noexcept { return Volume(Load<cmd::SetBusVolume>(p).volume); },
     [](const std::uint8_t* p, const L&) noexcept { return Clamped(Load<cmd::SetBusVolume>(p).fadeSeconds); },
     "api: finite volume >= 0, finite fade; shape: fade clamped to >= 0"},
    {CT::ApplySnapshot, true,
     [](const std::uint8_t* p, const L&) noexcept { return Fin(Load<cmd::ApplySnapshot>(p).weight); },
     [](const std::uint8_t* p, const L&) noexcept { return Clamped(Load<cmd::ApplySnapshot>(p).timeSeconds); },
     "api: finite weight, finite time; shape: time clamped to >= 0"},
    {CT::SetCategoryVolume, true,
     [](const std::uint8_t* p, const L&) noexcept { return Volume(Load<cmd::SetCategoryVolume>(p).volume); },
     [](const std::uint8_t* p, const L&) noexcept { return Clamped(Load<cmd::SetCategoryVolume>(p).fadeSeconds); },
     "api: finite volume >= 0, finite fade; shape: fade clamped to >= 0"},
    {CT::PauseCategory, true, &True,
     [](const std::uint8_t* p, const L&) noexcept { return Bool(Load<cmd::PauseCategory>(p).paused); },
     "shape: 0/1"},
    {CT::MuteCategory, true, &True,
     [](const std::uint8_t* p, const L&) noexcept { return Bool(Load<cmd::MuteCategory>(p).muted); }, "shape: 0/1"},
    {CT::SetMusicState, true,
     [](const std::uint8_t* p, const L&) noexcept { return When(Load<cmd::SetMusicState>(p).when); },
     [](const std::uint8_t* p, const L& k) noexcept { return Handle(Load<cmd::SetMusicState>(p).playback, k.playbacks); },
     "api: When kind/flags; shape: playback"},
    {CT::PrepareMusicState, true, &True,
     [](const std::uint8_t* p, const L& k) noexcept {
       return Handle(Load<cmd::PrepareMusicState>(p).playback, k.playbacks);
     },
     "shape: playback"},
    {CT::CreateTransport, true,
     [](const std::uint8_t* p, const L&) noexcept {
       const auto c = Load<cmd::CreateTransport>(p);
       return Bpm(c.desc.bpm) && c.desc.beats_per_bar != 0u && BeatUnit(c.desc.beat_unit);
     },
     [](const std::uint8_t* p, const L& k) noexcept { return Transport(Load<cmd::CreateTransport>(p).transport, k, false); },
     "api: bpm > 0 finite, beats_per_bar >= 1, beat_unit 2/4/8/16; shape: standalone transport handle"},
    {CT::DestroyTransport, true, &True,
     [](const std::uint8_t* p, const L& k) noexcept {
       return Transport(Load<cmd::DestroyTransport>(p).transport, k, false);
     },
     "api: not a playback's transport; shape: transport handle"},
    {CT::SetTransportState, true,
     [](const std::uint8_t* p, const L&) noexcept {
       const auto c = Load<cmd::SetTransportState>(p);
       return c.state <= SND_TRANSPORT_PAUSED && When(c.when);
     },
     [](const std::uint8_t* p, const L& k) noexcept {
       return Transport(Load<cmd::SetTransportState>(p).transport, k, true);
     },
     "api: state STOPPED/PLAYING/PAUSED, When kind/flags; shape: transport handle"},
    {CT::SetTempo, true,
     [](const std::uint8_t* p, const L&) noexcept {
       const auto c = Load<cmd::SetTempo>(p);
       return Bpm(c.bpm) && When(c.when);
     },
     [](const std::uint8_t* p, const L& k) noexcept { return Transport(Load<cmd::SetTempo>(p).transport, k, true); },
     "api: bpm > 0 finite, When kind/flags; shape: transport handle"},
    {CT::SeekTransport, true,
     [](const std::uint8_t* p, const L&) noexcept { return When(Load<cmd::SeekTransport>(p).when); },
     [](const std::uint8_t* p, const L& k) noexcept { return Transport(Load<cmd::SeekTransport>(p).transport, k, true); },
     "api: When kind/flags, any position; shape: transport handle"},
    {CT::CreateUserBuffer, false, &False, &False, "game memory of the recording process"},
    {CT::DestroyUserBuffer, false, &False, &False, "game memory of the recording process"},
    {CT::LiveSetValue, true,
     [](const std::uint8_t* p, const L&) noexcept {
       const auto c = Load<cmd::LiveSetValue>(p);
       if (c.field == kFieldRevertAll || c.field == kFieldSession || c.field == kFieldRevertBegin) return c.objectId == 0u;
       std::uint32_t chunk = 0, column = 0;
       return DecodeField(c.field, &chunk, &column) && c.valueType <= static_cast<std::uint32_t>(cmd::LiveValueType::Id);
     },
     &True, "server: known (chunk, column) field or a live link marker, value type 0..3"},
    {CT::LiveReplaceChunk, false, &False, &False, "data of the recording process"},
    {CT::LiveAudition, true,
     [](const std::uint8_t* p, const L&) noexcept {
       const auto c = Load<cmd::LiveAudition>(p);
       return (c.flags & cmd::kPlayHasPosition) == 0u || Fin(c.position);
     },
     [](const std::uint8_t* p, const L& k) noexcept {
       const auto c = Load<cmd::LiveAudition>(p);
       return Handle(c.playback, k.playbacks) && (c.flags & ~(cmd::kPlayOneShot | cmd::kPlayHasPosition)) == 0u;
     },
     "server: finite position when positioned; shape: playback, flags"},
    {CT::LiveOverride, true,
     [](const std::uint8_t* p, const L&) noexcept {
       const auto c = Load<cmd::LiveOverride>(p);
       if (!Bool(c.enable)) return false;
       if (c.kind == static_cast<std::uint32_t>(cmd::LiveOverrideKind::Param) && c.enable != 0u) {
         float f = 0;
         std::memcpy(&f, &c.value, 4);
         return Fin(f);
       }
       return true;
     },
     [](const std::uint8_t* p, const L& k) noexcept {
       const auto c = Load<cmd::LiveOverride>(p);
       return c.kind <= static_cast<std::uint32_t>(cmd::LiveOverrideKind::Switch) && HandleOrNone(c.target, k.playbacks);
     },
     "server: enable 0/1, finite parameter value; shape: kind, target"},
    {CT::LiveClearOverrides, true, &True, &True, "no arguments"},
    {CT::DeclareLanguage, true,
     [](const std::uint8_t* p, const L&) noexcept {
       const auto c = Load<cmd::DeclareLanguage>(p);  // BankManager::DeclareLanguage's name rule (D-50)
       if (c.nameBytes == 0u || c.nameBytes >= SND_MAX_LANGUAGE_NAME_BYTES || c.name[c.nameBytes] != '\0') return false;
       for (std::uint32_t i = 0; i < c.nameBytes; ++i) {
         const char ch = c.name[i];
         if (ch == '\0' || ch == '/' || ch == '.' || ch == '\\') return false;
       }
       return true;
     },
     &True, "api: 1..31 bytes, no '/', '.', '\\'"},
};
static_assert(sizeof(kCommandRules) / sizeof(kCommandRules[0]) == static_cast<std::size_t>(CT::Count) - 1u,
              "one rule per command type");

}  // namespace

const CommandRule* RuleOf(std::uint16_t type) noexcept {
  if (type == 0u || type >= static_cast<std::uint16_t>(CT::Count)) return nullptr;
  const CommandRule& r = kCommandRules[type - 1u];
  return r.type == static_cast<CT>(type) ? &r : nullptr;
}

CommandLimits LimitsOf(const snd_config& c) noexcept {
  CommandLimits k;
  k.playbacks = c.max_playbacks;
  k.emitters = c.max_emitters;
  k.transports = c.max_transports;
  k.userBuffers = c.max_user_buffers;
  k.banks = c.max_banks;
  k.listeners = c.max_listeners;
  return k;
}

bool CheckOverflowParam(const cmd::OverflowParam& p, const CommandLimits& k) noexcept {
  if (p.isSwitch > 1u || !Scope(p.scope, p.target, k)) return false;
  if (p.isSwitch == 0u) {
    float f = 0;
    std::memcpy(&f, &p.value, 4);
    if (!Fin(f)) return false;  // SetParam's API rule
  }
  return true;
}

bool CheckCommand(const cmd::CommandHeader& h, const std::uint8_t* payload, const CommandLimits& limits) noexcept {
  const cmd::CommandInfo* info = cmd::FindCommandInfo(h.type);
  const CommandRule* rule = RuleOf(h.type);
  if (info == nullptr || rule == nullptr || h.sizeBytes != info->wireSize || !rule->replayable) return false;
  return rule->api(payload, limits) && rule->shape(payload, limits);
}

}  // namespace snd::livelink
