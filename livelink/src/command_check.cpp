// Validation of recorded commands: the game API's entry rules (core/src/recorder.cpp)
// applied to command payloads, plus handle shape checks.
#include "snd/livelink/command_check.hpp"

#include <cmath>
#include <cstring>

#include "snd/livelink/wire.hpp"
#include "snd/types.h"

namespace snd::livelink {

namespace {

template <class P>
P Load(const std::uint8_t* p) noexcept {
  P v;
  std::memcpy(&v, p, sizeof(P));
  return v;
}

bool Fin(float f) noexcept { return std::isfinite(f); }
bool Fin(const Vec3& v) noexcept { return Fin(v.x) && Fin(v.y) && Fin(v.z); }
bool Bool(std::uint32_t v) noexcept { return v <= 1u; }

// A live handle: slot below the capacity, generation 1 .. 2^31-1 (D-46).
bool Handle(HandleRef h, std::uint32_t capacity) noexcept {
  return h.slot < capacity && h.generation != 0u && h.generation < 0x80000000u;
}
bool HandleOrNone(HandleRef h, std::uint32_t capacity) noexcept {
  return (h.slot == 0u && h.generation == 0u) || Handle(h, capacity);
}
// Transport handle: a standalone transport, or a music playback's (SND_TRANSPORT_PLAYBACK_BIT).
bool Transport(HandleRef h, const CommandLimits& k, bool allowPlaybackForm) noexcept {
  if ((h.slot & SND_TRANSPORT_PLAYBACK_BIT) != 0u) {
    return allowPlaybackForm && Handle(HandleRef{h.slot & ~SND_TRANSPORT_PLAYBACK_BIT, h.generation}, k.playbacks);
  }
  return Handle(h, k.transports);
}

bool When(const snd_when& w, const CommandLimits& k) noexcept {
  if (w.kind > SND_WHEN_DELAY_US || (w.flags & ~SND_WHEN_FLAG_PICKUP_ALIGN) != 0u) return false;
  for (std::uint8_t r : w.reserved) {
    if (r != 0u) return false;
  }
  const bool musical = w.kind >= SND_WHEN_NEXT_BEAT && w.kind <= SND_WHEN_SEGMENT_END;
  if (musical && w.transport.value != 0u && !Transport(HandleRef::FromValue(w.transport.value), k, true)) return false;
  if (w.kind == SND_WHEN_BARS_AFTER && (w.count == 0u || w.count > 4096u)) return false;
  if (w.kind == SND_WHEN_GRID && (w.count == 0u || w.count > 256u)) return false;
  if (w.kind == SND_WHEN_DELAY_US && w.value > 3600ull * 1000000ull) return false;  // an hour at most
  if (w.offset_ticks < -1000000 || w.offset_ticks > 1000000) return false;
  return true;
}

bool Fade(float s) noexcept { return Fin(s) && s <= 3600.0f; }  // < 0 = the event's default
bool Volume(float v) noexcept { return Fin(v) && v >= 0.0f && v <= 1.0e6f; }
bool Seconds(float s) noexcept { return Fin(s) && s >= 0.0f && s <= 3600.0f; }
bool Bpm(float b) noexcept { return Fin(b) && b > 0.0f && b <= 10000.0f; }
bool BeatUnit(std::uint32_t u) noexcept { return u == 2u || u == 4u || u == 8u || u == 16u; }

bool Scope(std::uint8_t scope, HandleRef target, const CommandLimits& k) noexcept {
  switch (static_cast<ParamScope>(scope)) {
    case ParamScope::Global: return target.slot == 0u && target.generation == 0u;
    case ParamScope::Emitter: return Handle(target, k.emitters);
    case ParamScope::Playback: return Handle(target, k.playbacks);
  }
  return false;
}

}  // namespace

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
    if (!Fin(f)) return false;
  }
  return true;
}

bool CheckCommand(const cmd::CommandHeader& h, const std::uint8_t* p, const CommandLimits& k) noexcept {
  using CT = cmd::CommandType;
  const cmd::CommandInfo* info = cmd::FindCommandInfo(h.type);
  if (info == nullptr || h.sizeBytes != info->wireSize) return false;
  switch (static_cast<CT>(h.type)) {
    case CT::Suspend:
    case CT::Resume:
    case CT::LiveClearOverrides:
      return true;
    case CT::SetLanguage:
      return true;
    case CT::DeclareLanguage: {
      const auto c = Load<cmd::DeclareLanguage>(p);
      if (c.nameBytes == 0u || c.nameBytes >= SND_MAX_LANGUAGE_NAME_BYTES || c.name[c.nameBytes] != '\0') return false;
      for (std::uint32_t i = 0; i < c.nameBytes; ++i) {
        const char ch = c.name[i];
        if (ch == '\0' || ch == '/' || ch == '.' || ch == '\\') return false;
      }
      return true;
    }
    case CT::BankUnload: {
      const auto c = Load<cmd::BankUnload>(p);
      return Handle(c.bank, k.banks) && (c.mode == SND_UNLOAD_STOP_AND_UNLOAD || c.mode == SND_UNLOAD_IF_UNUSED);
    }
    case CT::Play: {
      const auto c = Load<cmd::Play>(p);
      return Handle(c.playback, k.playbacks) && HandleOrNone(c.emitter, k.emitters) &&
             (c.flags & ~(cmd::kPlayOneShot | cmd::kPlayHasPosition)) == 0u && When(c.when, k) && Fin(c.position) &&
             c.priorityHint <= 255u;
    }
    case CT::Stop: {
      const auto c = Load<cmd::Stop>(p);
      return Handle(c.playback, k.playbacks) && Fade(c.fadeSeconds) && When(c.when, k);
    }
    case CT::PauseResume: {
      const auto c = Load<cmd::PauseResume>(p);
      return Handle(c.playback, k.playbacks) && Fade(c.fadeSeconds) && Bool(c.pause);
    }
    case CT::SetScheduledStop: {
      const auto c = Load<cmd::SetScheduledStop>(p);
      return Handle(c.playback, k.playbacks) && Fade(c.fadeSeconds) && When(c.when, k);
    }
    case CT::CreateEmitter: return Handle(Load<cmd::CreateEmitter>(p).emitter, k.emitters);
    case CT::DestroyEmitter: return Handle(Load<cmd::DestroyEmitter>(p).emitter, k.emitters);
    case CT::SetTransform: {
      const auto c = Load<cmd::SetTransform>(p);
      return Handle(c.emitter, k.emitters) &&
             (c.mask & ~(cmd::kTransformPosition | cmd::kTransformForward | cmd::kTransformVelocity)) == 0u &&
             Fin(c.position) && Fin(c.forward) && Fin(c.velocity);
    }
    case CT::SetListener: {
      const auto c = Load<cmd::SetListener>(p);
      const snd_listener_desc& l = c.listener;
      return c.index < k.listeners && Fin(l.position) && Fin(l.forward) && Fin(l.up) && Fin(l.velocity) &&
             Fin(l.weight) && l.weight >= 0.0f && Bool(l.enabled);
    }
    case CT::SetParam: {
      const auto c = Load<cmd::SetParam>(p);
      return Scope(c.scope, c.target, k) && Fin(c.value);
    }
    case CT::SetSwitch: {
      const auto c = Load<cmd::SetSwitch>(p);
      return Scope(c.scope, c.target, k);
    }
    case CT::SetBusVolume: {
      const auto c = Load<cmd::SetBusVolume>(p);
      return Volume(c.volume) && Seconds(c.fadeSeconds);
    }
    case CT::ApplySnapshot: {
      const auto c = Load<cmd::ApplySnapshot>(p);
      return Fin(c.weight) && c.weight >= 0.0f && c.weight <= 1.0f && Seconds(c.timeSeconds);
    }
    case CT::SetCategoryVolume: {
      const auto c = Load<cmd::SetCategoryVolume>(p);
      return Volume(c.volume) && Seconds(c.fadeSeconds);
    }
    case CT::PauseCategory: return Bool(Load<cmd::PauseCategory>(p).paused);
    case CT::MuteCategory: return Bool(Load<cmd::MuteCategory>(p).muted);
    case CT::SetMusicState: {
      const auto c = Load<cmd::SetMusicState>(p);
      return Handle(c.playback, k.playbacks) && When(c.when, k);
    }
    case CT::PrepareMusicState: return Handle(Load<cmd::PrepareMusicState>(p).playback, k.playbacks);
    case CT::CreateTransport: {
      const auto c = Load<cmd::CreateTransport>(p);
      return Transport(c.transport, k, false) && Bpm(c.desc.bpm) && c.desc.beats_per_bar >= 1u &&
             c.desc.beats_per_bar <= 64u && BeatUnit(c.desc.beat_unit) && Bool(c.desc.start_playing);
    }
    case CT::DestroyTransport: return Transport(Load<cmd::DestroyTransport>(p).transport, k, false);
    case CT::SetTransportState: {
      const auto c = Load<cmd::SetTransportState>(p);
      return Transport(c.transport, k, true) && c.state <= SND_TRANSPORT_PAUSED && When(c.when, k);
    }
    case CT::SetTempo: {
      const auto c = Load<cmd::SetTempo>(p);
      return Transport(c.transport, k, true) && Bpm(c.bpm) && When(c.when, k);
    }
    case CT::SeekTransport: {
      const auto c = Load<cmd::SeekTransport>(p);
      const snd_musical_time& m = c.position;
      return Transport(c.transport, k, true) && m.bar >= 0 && m.bar < (1 << 20) && m.beat >= 0 && m.beat < 64 &&
             m.tick >= 0 && m.tick < static_cast<std::int32_t>(SND_TICKS_PER_BEAT) && When(c.when, k);
    }
    case CT::LiveSetValue: {
      const auto c = Load<cmd::LiveSetValue>(p);
      std::uint32_t chunk = 0, column = 0;
      if (c.field == kFieldRevertAll) return c.objectId == 0u;
      return DecodeField(c.field, &chunk, &column) && c.valueType <= static_cast<std::uint32_t>(cmd::LiveValueType::Id);
    }
    case CT::LiveAudition: {
      const auto c = Load<cmd::LiveAudition>(p);
      return Handle(c.playback, k.playbacks) && (c.flags & ~(cmd::kPlayOneShot | cmd::kPlayHasPosition)) == 0u &&
             Fin(c.position);
    }
    case CT::LiveOverride: {
      const auto c = Load<cmd::LiveOverride>(p);
      if (c.kind > static_cast<std::uint32_t>(cmd::LiveOverrideKind::Switch) || !Bool(c.enable)) return false;
      if (c.kind == static_cast<std::uint32_t>(cmd::LiveOverrideKind::Param)) {
        float f = 0;
        std::memcpy(&f, &c.value, 4);
        if (c.enable != 0u && !Fin(f)) return false;
      }
      return HandleOrNone(c.target, k.playbacks);
    }
    // Pointers into the recording process / game memory: never replayable.
    case CT::BankRegister:
    case CT::LiveReplaceChunk:
    case CT::PlaySynth:
    case CT::PlayInput:
    case CT::PlayUserBuffer:
    case CT::CreateUserBuffer:
    case CT::DestroyUserBuffer:
    case CT::Invalid:
    case CT::Count:
      return false;
  }
  return false;
}

}  // namespace snd::livelink
