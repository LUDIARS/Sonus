// Sonus public C++ API (design.md 13). FROZEN CONTRACT.
//
// A thin header-only wrapper over the C API in snd/sonus.h: every function
// forwards to exactly one snd_* function, every POD is either an alias of the
// C struct or a layout-identical wrapper, and enums carry the C values. The
// thread rules, error behaviour and invalid-handle behaviour documented in
// snd/sonus.h apply unchanged.
#ifndef SND_SONUS_HPP
#define SND_SONUS_HPP

#include <cstddef>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/ext/codec.hpp"
#include "snd/ext/dsp_effect.hpp"
#include "snd/ext/file_system.hpp"
#include "snd/ext/job_system.hpp"
#include "snd/ext/output_device.hpp"
#include "snd/ext/types.hpp"
#include "snd/id.hpp"
#include "snd/sonus.h"

namespace snd {

inline constexpr std::uint32_t kContractVersion = SND_CONTRACT_VERSION;
inline constexpr std::uint32_t kMaxListeners = SND_MAX_LISTENERS;
inline constexpr std::uint32_t kTicksPerBeat = SND_TICKS_PER_BEAT;

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------
enum class Result : std::int32_t {
  Ok = SND_OK,
  InvalidArgument = SND_ERR_INVALID_ARGUMENT,
  OutOfBudget = SND_ERR_OUT_OF_BUDGET,
  NoDevice = SND_ERR_NO_DEVICE,
  WrongThread = SND_ERR_WRONG_THREAD,
  ThreadNotRegistered = SND_ERR_THREAD_NOT_REGISTERED,
  CommandBufferFull = SND_ERR_COMMAND_BUFFER_FULL,
  InvalidHandle = SND_ERR_INVALID_HANDLE,
  NotFound = SND_ERR_NOT_FOUND,
  BankInUse = SND_ERR_BANK_IN_USE,
  BankDependency = SND_ERR_BANK_DEPENDENCY,
  Io = SND_ERR_IO,
  Corrupt = SND_ERR_CORRUPT,
  VersionMismatch = SND_ERR_VERSION_MISMATCH,
  LimitReached = SND_ERR_LIMIT_REACHED,
  Unsupported = SND_ERR_UNSUPPORTED,
  NotImplemented = SND_ERR_NOT_IMPLEMENTED,
  NotReady = SND_ERR_NOT_READY,
};

enum class PlaybackState : std::uint32_t {
  None = SND_PLAYBACK_STATE_NONE,
  Pending = SND_PLAYBACK_STATE_PENDING,
  Priming = SND_PLAYBACK_STATE_PRIMING,
  Playing = SND_PLAYBACK_STATE_PLAYING,
  Paused = SND_PLAYBACK_STATE_PAUSED,
  Stopping = SND_PLAYBACK_STATE_STOPPING,
  Ended = SND_PLAYBACK_STATE_ENDED,
};

enum class EndReason : std::uint32_t {
  Completed = SND_END_COMPLETED,
  Stopped = SND_END_STOPPED,
  Stolen = SND_END_STOLEN,
  Rejected = SND_END_REJECTED,
  Discarded = SND_END_DISCARDED,
  Error = SND_END_ERROR,
  NotLoaded = SND_END_NOT_LOADED,
};

enum class BankState : std::uint32_t {
  Unloaded = SND_BANK_STATE_UNLOADED,
  Loading = SND_BANK_STATE_LOADING,
  Loaded = SND_BANK_STATE_LOADED,
  Failed = SND_BANK_STATE_FAILED,
  Unloading = SND_BANK_STATE_UNLOADING,
};

enum class UnloadMode : std::uint32_t {
  StopAndUnload = SND_UNLOAD_STOP_AND_UNLOAD,
  IfUnused = SND_UNLOAD_IF_UNUSED,
};

enum class TransportState : std::uint32_t {
  Stopped = SND_TRANSPORT_STOPPED,
  Playing = SND_TRANSPORT_PLAYING,
  Paused = SND_TRANSPORT_PAUSED,
};

enum class SynthMode : std::uint32_t {
  Buffered = SND_SYNTH_BUFFERED,
  LowLatency = SND_SYNTH_LOW_LATENCY,
};

enum class UserBufferMode : std::uint32_t {
  Stream = SND_USER_BUFFER_STREAM,
  Static = SND_USER_BUFFER_STATIC,
};

enum class ListenerMode : std::uint32_t {
  Nearest = SND_LISTENER_NEAREST,
  Weighted = SND_LISTENER_WEIGHTED,
  Split = SND_LISTENER_SPLIT,
};

enum class FocusMode : std::uint32_t {
  Continue = SND_FOCUS_CONTINUE,
  Silence = SND_FOCUS_SILENCE,
  PerCategory = SND_FOCUS_PER_CATEGORY,
};

enum class LogLevel : std::uint32_t {
  Debug = SND_LOG_DEBUG,
  Info = SND_LOG_INFO,
  Warn = SND_LOG_WARN,
  Error = SND_LOG_ERROR,
};

enum class WhenKind : std::uint8_t {
  Immediate = SND_WHEN_IMMEDIATE,
  NextBeat = SND_WHEN_NEXT_BEAT,
  NextBar = SND_WHEN_NEXT_BAR,
  BarsAfter = SND_WHEN_BARS_AFTER,
  Grid = SND_WHEN_GRID,
  NextMarker = SND_WHEN_NEXT_MARKER,
  SegmentEnd = SND_WHEN_SEGMENT_END,
  ClockSample = SND_WHEN_CLOCK_SAMPLE,
  DelayMicroseconds = SND_WHEN_DELAY_US,
};

// ---------------------------------------------------------------------------
// Handles: layout-identical to the C structs, but distinct C++ types.
// ---------------------------------------------------------------------------
#define SND_DECLARE_CPP_HANDLE(Name, CType)                                               \
  struct Name {                                                                         \
    std::uint64_t value = SND_HANDLE_INVALID_VALUE;                                     \
    constexpr bool IsValid() const noexcept { return value != SND_HANDLE_INVALID_VALUE; } \
    constexpr std::uint32_t Slot() const noexcept { return static_cast<std::uint32_t>(value); } \
    constexpr std::uint32_t Generation() const noexcept {                               \
      return static_cast<std::uint32_t>(value >> 32);                                    \
    }                                                                                   \
    constexpr CType ToC() const noexcept { return CType{value}; }                       \
    static constexpr Name FromC(CType c) noexcept { return Name{c.value}; }             \
    friend constexpr bool operator==(Name, Name) noexcept = default;                    \
  };                                                                                    \
  static_assert(sizeof(Name) == sizeof(CType) && alignof(Name) == alignof(CType), #Name)

SND_DECLARE_CPP_HANDLE(PlaybackHandle, snd_playback_handle);
SND_DECLARE_CPP_HANDLE(EmitterHandle, snd_emitter_handle);
SND_DECLARE_CPP_HANDLE(TransportHandle, snd_transport_handle);
SND_DECLARE_CPP_HANDLE(BankHandle, snd_bank_handle);
SND_DECLARE_CPP_HANDLE(UserBufferHandle, snd_user_buffer_handle);
#undef SND_DECLARE_CPP_HANDLE

inline constexpr EmitterHandle kNoEmitter{};

// ---------------------------------------------------------------------------
// PODs shared with C
// ---------------------------------------------------------------------------
using System = snd_system;
using Vec3 = snd_vec3;
using ListenerDesc = snd_listener_desc;
using MusicalTime = snd_musical_time;
using TransportDesc = snd_transport_desc;
using Position = snd_position;
using Meter = snd_meter;
using Stats = snd_stats;
using Timing = snd_timing;
using Callbacks = snd_callbacks;
using UserBufferDesc = snd_user_buffer_desc;
using InputDesc = snd_input_desc;

// "When" (8.2): factories for every kind. Layout = snd_when.
class When {
 public:
  constexpr When() noexcept : w_{} {}
  static constexpr When Immediate() noexcept { return When(); }
  static constexpr When NextBeat(TransportHandle t, std::int32_t offsetTicks = 0) noexcept {
    return Make(WhenKind::NextBeat, t, 0, 0, offsetTicks);
  }
  static constexpr When NextBar(TransportHandle t, std::int32_t offsetTicks = 0) noexcept {
    return Make(WhenKind::NextBar, t, 0, 0, offsetTicks);
  }
  static constexpr When BarsAfter(TransportHandle t, std::uint32_t bars,
                                  std::int32_t offsetTicks = 0) noexcept {
    return Make(WhenKind::BarsAfter, t, 0, bars, offsetTicks);
  }
  // division: 4 = quarter note, 8 = eighth, 16 = sixteenth, 12 = eighth triplets...
  static constexpr When Grid(TransportHandle t, std::uint32_t division,
                             std::int32_t offsetTicks = 0) noexcept {
    return Make(WhenKind::Grid, t, 0, division, offsetTicks);
  }
  static constexpr When NextMarker(TransportHandle t, Id marker = kIdNone) noexcept {
    return Make(WhenKind::NextMarker, t, marker, 0, 0);
  }
  static constexpr When SegmentEnd(TransportHandle t) noexcept {
    return Make(WhenKind::SegmentEnd, t, 0, 0, 0);
  }
  static constexpr When AtClockSample(std::uint64_t clockSample) noexcept {
    return Make(WhenKind::ClockSample, TransportHandle{}, clockSample, 0, 0);
  }
  static constexpr When AfterMicroseconds(std::uint64_t us) noexcept {
    return Make(WhenKind::DelayMicroseconds, TransportHandle{}, us, 0, 0);
  }
  // Align the material's pickup marker (auftakt) to the quantised point (8.2).
  constexpr When WithPickupAlign() const noexcept {
    When w = *this;
    w.w_.flags = static_cast<std::uint8_t>(w.w_.flags | SND_WHEN_FLAG_PICKUP_ALIGN);
    return w;
  }
  constexpr WhenKind Kind() const noexcept { return static_cast<WhenKind>(w_.kind); }
  constexpr const snd_when& ToC() const noexcept { return w_; }

 private:
  static constexpr When Make(WhenKind k, TransportHandle t, std::uint64_t value,
                             std::uint32_t count, std::int32_t offsetTicks) noexcept {
    When w;
    w.w_.transport = t.ToC();
    w.w_.value = value;
    w.w_.offset_ticks = offsetTicks;
    w.w_.count = count;
    w.w_.kind = static_cast<std::uint8_t>(k);
    return w;
  }
  snd_when w_;
};
static_assert(sizeof(When) == sizeof(snd_when));

// Synthesis callback (5.2). Same ABI position as snd_synth_callback but with a reference.
using SynthCallback = void (*)(void* user, float* const* out, std::uint32_t channels,
                               std::uint32_t frames, const RenderContext& ctx);

struct SynthDesc {
  SynthCallback callback = nullptr;
  void* user = nullptr;
  std::uint32_t channels = 1;
  SynthMode mode = SynthMode::Buffered;  // LowLatency is opt-in
  std::uint32_t aheadBlocks = 0;         // 0 = Config.tuning.callbackAheadBlocks
  std::uint32_t priority = 0;
  Id bus = kIdNone;
  Id eventTemplate = kIdNone;
  float volume = 1.0f;
};

// ---------------------------------------------------------------------------
// Config (13.2). Fields generated from snd/config.h; see there for meaning.
// ---------------------------------------------------------------------------
#define SND_CONFIG_CPP_FIELD(c, cpp, ct, cppt, d) cppt cpp = static_cast<cppt>(d);
struct MemoryConfig { SND_CONFIG_MEMORY(SND_CONFIG_CPP_FIELD) };
struct CommandsConfig { SND_CONFIG_COMMANDS(SND_CONFIG_CPP_FIELD) };
struct JobsConfig {
  SND_CONFIG_JOBS(SND_CONFIG_CPP_FIELD)
  IJobSystem* external = nullptr;  // normal-worker jobs only (2.3)
};
struct TuningConfig { SND_CONFIG_TUNING(SND_CONFIG_CPP_FIELD) };
struct DevReserveConfig { SND_CONFIG_DEV_RESERVE(SND_CONFIG_CPP_FIELD) };
struct LiveLinkConfig { SND_CONFIG_LIVELINK(SND_CONFIG_CPP_FIELD) };
struct OutputConfig {
  IOutputDevice* device = nullptr;  // nullptr = platform default
  std::uint32_t deviceIndex = 0;
};
struct PluginConfig {
  const CodecPlugin* codecs = nullptr;
  std::uint32_t codecCount = 0;
  const EffectPlugin* effects = nullptr;
  std::uint32_t effectCount = 0;
};
inline constexpr std::uint32_t kMaxPluginsPerKind = 32;  // C++ wrapper limit per call

struct Config {
  SND_CONFIG_TOP(SND_CONFIG_CPP_FIELD)
  MemoryConfig memory;
  CommandsConfig commands;
  JobsConfig jobs;
  TuningConfig tuning;
  DevReserveConfig devReserve;
  LiveLinkConfig liveLink;
  OutputConfig output;
  PluginConfig plugins;
};
#undef SND_CONFIG_CPP_FIELD

namespace detail {
// Converts the scalar fields (pointers are bridged by Create()).
inline void ToC(const Config& in, snd_config* out) noexcept {
  snd_config_init(out);
#define SND_TO_C_TOP(c, cpp, ct, cppt, d) out->c = static_cast<ct>(in.cpp);
#define SND_TO_C_MEMORY(c, cpp, ct, cppt, d) out->memory.c = static_cast<ct>(in.memory.cpp);
#define SND_TO_C_COMMANDS(c, cpp, ct, cppt, d) out->commands.c = static_cast<ct>(in.commands.cpp);
#define SND_TO_C_JOBS(c, cpp, ct, cppt, d) out->jobs.c = static_cast<ct>(in.jobs.cpp);
#define SND_TO_C_TUNING(c, cpp, ct, cppt, d) out->tuning.c = static_cast<ct>(in.tuning.cpp);
#define SND_TO_C_DEV(c, cpp, ct, cppt, d) out->dev_reserve.c = static_cast<ct>(in.devReserve.cpp);
#define SND_TO_C_LL(c, cpp, ct, cppt, d) out->livelink.c = static_cast<ct>(in.liveLink.cpp);
  SND_CONFIG_TOP(SND_TO_C_TOP)
  SND_CONFIG_MEMORY(SND_TO_C_MEMORY)
  SND_CONFIG_COMMANDS(SND_TO_C_COMMANDS)
  SND_CONFIG_JOBS(SND_TO_C_JOBS)
  SND_CONFIG_TUNING(SND_TO_C_TUNING)
  SND_CONFIG_DEV_RESERVE(SND_TO_C_DEV)
  SND_CONFIG_LIVELINK(SND_TO_C_LL)
#undef SND_TO_C_TOP
#undef SND_TO_C_MEMORY
#undef SND_TO_C_COMMANDS
#undef SND_TO_C_JOBS
#undef SND_TO_C_TUNING
#undef SND_TO_C_DEV
#undef SND_TO_C_LL
  out->output.device_index = in.output.deviceIndex;
}
constexpr Result R(snd_result r) noexcept { return static_cast<Result>(r); }
constexpr const snd_when* W(const When& w) noexcept { return &w.ToC(); }
}  // namespace detail

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------
inline std::uint32_t ContractVersion() noexcept { return snd_contract_version(); }

// allocator / fileSystem may be nullptr (built-in defaults).
inline Result Create(const Config& cfg, IAllocator* allocator, IFileSystem* fileSystem,
                     System** out) noexcept {
  snd_config c;
  detail::ToC(cfg, &c);
  snd_job_system jobs{};
  if (cfg.jobs.external != nullptr) {
    jobs = ToC(*cfg.jobs.external);
    c.jobs.external = &jobs;
  }
  snd_output_device device{};
  if (cfg.output.device != nullptr) {
    device = ToC(*cfg.output.device);
    c.output.device = &device;
  }
  if (cfg.plugins.codecCount > kMaxPluginsPerKind || cfg.plugins.effectCount > kMaxPluginsPerKind) {
    if (out != nullptr) *out = nullptr;
    return Result::InvalidArgument;
  }
  snd_codec codecs[kMaxPluginsPerKind] = {};
  snd_dsp_effect effects[kMaxPluginsPerKind] = {};
  for (std::uint32_t i = 0; i < cfg.plugins.codecCount; ++i) {
    codecs[i] = ToC(*cfg.plugins.codecs[i].codec, cfg.plugins.codecs[i].fourcc);
  }
  for (std::uint32_t i = 0; i < cfg.plugins.effectCount; ++i) {
    effects[i] = ToC(*cfg.plugins.effects[i].effect, cfg.plugins.effects[i].effectType);
  }
  c.plugins.codecs = cfg.plugins.codecCount ? codecs : nullptr;
  c.plugins.codec_count = cfg.plugins.codecCount;
  c.plugins.effects = cfg.plugins.effectCount ? effects : nullptr;
  c.plugins.effect_count = cfg.plugins.effectCount;
  snd_allocator a{};
  snd_file_system f{};
  if (allocator != nullptr) a = ToC(*allocator);
  if (fileSystem != nullptr) f = ToC(*fileSystem);
  // snd_create copies every table, so the temporaries above may die afterwards.
  return detail::R(snd_create(&c, allocator ? &a : nullptr, fileSystem ? &f : nullptr, out));
}
inline void Destroy(System* sys) noexcept { snd_destroy(sys); }
inline Result Update(System* sys) noexcept { return detail::R(snd_update(sys)); }
inline Result RegisterThread(System* sys) noexcept { return detail::R(snd_register_thread(sys)); }
inline Result UnregisterThread(System* sys) noexcept { return detail::R(snd_unregister_thread(sys)); }
inline Result Flush(System* sys) noexcept { return detail::R(snd_flush(sys)); }
inline Result Suspend(System* sys) noexcept { return detail::R(snd_suspend(sys)); }
inline Result Resume(System* sys) noexcept { return detail::R(snd_resume(sys)); }
inline Result SetCallbacks(System* sys, const Callbacks* callbacks) noexcept {
  return detail::R(snd_set_callbacks(sys, callbacks));
}

// ---------------------------------------------------------------------------
// Banks
// ---------------------------------------------------------------------------
inline BankHandle LoadBankAsync(System* sys, const char* path) noexcept {
  return BankHandle::FromC(snd_load_bank_async(sys, path));
}
inline Result UnloadBank(System* sys, BankHandle bank,
                         UnloadMode mode = UnloadMode::StopAndUnload) noexcept {
  return detail::R(snd_unload_bank(sys, bank.ToC(), static_cast<snd_unload_mode>(mode)));
}
inline BankState GetBankState(const System* sys, BankHandle bank) noexcept {
  return static_cast<BankState>(snd_get_bank_state(sys, bank.ToC()));
}
inline Result SetLanguage(System* sys, Id language) noexcept {
  return detail::R(snd_set_language(sys, language));
}
inline Result GetBankChunk(const System* sys, BankHandle bank, std::uint32_t fourcc,
                           const void** data, std::uint64_t* bytes) noexcept {
  return detail::R(snd_get_bank_chunk(sys, bank.ToC(), fourcc, data, bytes));
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------
inline PlaybackHandle Play(System* sys, Id event, EmitterHandle emitter = kNoEmitter,
                           const When& when = When::Immediate()) noexcept {
  return PlaybackHandle::FromC(snd_play(sys, event, emitter.ToC(), detail::W(when)));
}
inline Result PlayOneShot(System* sys, Id event, EmitterHandle emitter = kNoEmitter,
                          const When& when = When::Immediate()) noexcept {
  return detail::R(snd_play_one_shot(sys, event, emitter.ToC(), detail::W(when)));
}
inline Result PlayAt(System* sys, Id event, const Vec3& position,
                     const When& when = When::Immediate()) noexcept {
  return detail::R(snd_play_at(sys, event, &position, detail::W(when)));
}
inline Result Stop(System* sys, PlaybackHandle p, float fadeSeconds = -1.0f,
                   const When& when = When::Immediate()) noexcept {
  return detail::R(snd_stop(sys, p.ToC(), fadeSeconds, detail::W(when)));
}
inline Result Pause(System* sys, PlaybackHandle p, float fadeSeconds = 0.0f) noexcept {
  return detail::R(snd_pause(sys, p.ToC(), fadeSeconds));
}
inline Result Resume(System* sys, PlaybackHandle p, float fadeSeconds = 0.0f) noexcept {
  return detail::R(snd_resume_playback(sys, p.ToC(), fadeSeconds));
}
inline Result SetScheduledStop(System* sys, PlaybackHandle p, const When& when,
                               float fadeSeconds = -1.0f) noexcept {
  return detail::R(snd_set_scheduled_stop(sys, p.ToC(), detail::W(when), fadeSeconds));
}
inline TransportHandle GetPlaybackTransport(PlaybackHandle p) noexcept {
  return TransportHandle::FromC(snd_get_playback_transport(p.ToC()));
}

// ---------------------------------------------------------------------------
// 3D
// ---------------------------------------------------------------------------
inline EmitterHandle CreateEmitter(System* sys) noexcept {
  return EmitterHandle::FromC(snd_create_emitter(sys));
}
inline Result DestroyEmitter(System* sys, EmitterHandle e) noexcept {
  return detail::R(snd_destroy_emitter(sys, e.ToC()));
}
inline Result SetTransform(System* sys, EmitterHandle e, const Vec3& position, const Vec3& forward,
                           const Vec3& velocity) noexcept {
  return detail::R(snd_set_transform(sys, e.ToC(), &position, &forward, &velocity));
}
inline Result SetListener(System* sys, std::uint32_t index, const ListenerDesc& l) noexcept {
  return detail::R(snd_set_listener(sys, index, &l));
}

// ---------------------------------------------------------------------------
// Parameters (Global / Emitter / Playback scope)
// ---------------------------------------------------------------------------
inline Result SetParam(System* sys, Id param, float value) noexcept {
  return detail::R(snd_set_param_global(sys, param, value));
}
inline Result SetParam(System* sys, EmitterHandle e, Id param, float value) noexcept {
  return detail::R(snd_set_param_emitter(sys, e.ToC(), param, value));
}
inline Result SetParam(System* sys, PlaybackHandle p, Id param, float value) noexcept {
  return detail::R(snd_set_param_playback(sys, p.ToC(), param, value));
}
inline Result SetSwitch(System* sys, Id group, Id value) noexcept {
  return detail::R(snd_set_switch_global(sys, group, value));
}
inline Result SetSwitch(System* sys, EmitterHandle e, Id group, Id value) noexcept {
  return detail::R(snd_set_switch_emitter(sys, e.ToC(), group, value));
}
inline Result SetSwitch(System* sys, PlaybackHandle p, Id group, Id value) noexcept {
  return detail::R(snd_set_switch_playback(sys, p.ToC(), group, value));
}

// ---------------------------------------------------------------------------
// Mix
// ---------------------------------------------------------------------------
inline Result SetBusVolume(System* sys, Id bus, float volume, float fadeSeconds = 0.0f) noexcept {
  return detail::R(snd_set_bus_volume(sys, bus, volume, fadeSeconds));
}
inline Result ApplySnapshot(System* sys, Id snapshot, float weight, float timeSeconds) noexcept {
  return detail::R(snd_apply_snapshot(sys, snapshot, weight, timeSeconds));
}
inline Result SetCategoryVolume(System* sys, Id category, float volume,
                                float fadeSeconds = 0.0f) noexcept {
  return detail::R(snd_set_category_volume(sys, category, volume, fadeSeconds));
}
inline Result PauseCategory(System* sys, Id category, bool paused) noexcept {
  return detail::R(snd_pause_category(sys, category, paused ? 1 : 0));
}
inline Result MuteCategory(System* sys, Id category, bool muted) noexcept {
  return detail::R(snd_mute_category(sys, category, muted ? 1 : 0));
}

// ---------------------------------------------------------------------------
// Music
// ---------------------------------------------------------------------------
inline Result SetMusicState(System* sys, PlaybackHandle music, Id state,
                            const When& when = When::Immediate()) noexcept {
  return detail::R(snd_set_music_state(sys, music.ToC(), state, detail::W(when)));
}
inline Result PrepareMusicState(System* sys, PlaybackHandle music, Id state) noexcept {
  return detail::R(snd_prepare_music_state(sys, music.ToC(), state));
}
inline TransportHandle CreateTransport(System* sys, const TransportDesc& desc) noexcept {
  return TransportHandle::FromC(snd_create_transport(sys, &desc));
}
inline Result DestroyTransport(System* sys, TransportHandle t) noexcept {
  return detail::R(snd_destroy_transport(sys, t.ToC()));
}
inline Result SetTransportState(System* sys, TransportHandle t, TransportState state,
                                const When& when = When::Immediate()) noexcept {
  return detail::R(snd_set_transport_state(sys, t.ToC(), static_cast<snd_transport_state>(state),
                                           detail::W(when)));
}
inline Result SetTempo(System* sys, TransportHandle t, float bpm,
                       const When& when = When::Immediate()) noexcept {
  return detail::R(snd_set_tempo(sys, t.ToC(), bpm, detail::W(when)));
}
inline Result SeekTransport(System* sys, TransportHandle t, const MusicalTime& position,
                            const When& when = When::Immediate()) noexcept {
  return detail::R(snd_seek_transport(sys, t.ToC(), &position, detail::W(when)));
}

// ---------------------------------------------------------------------------
// Dynamic generation
// ---------------------------------------------------------------------------
inline PlaybackHandle PlaySynth(System* sys, const SynthDesc& desc,
                                EmitterHandle emitter = kNoEmitter,
                                const When& when = When::Immediate()) noexcept {
  snd_synth_desc d{};
  d.callback = nullptr;
  // Carried through the C ABI as a generic function pointer; api casts it back
  // to snd::SynthCallback (a round-trip cast is well defined).
  d.cpp_callback = reinterpret_cast<snd_fnptr>(desc.callback);
  d.user = desc.user;
  d.channels = desc.channels;
  d.mode = static_cast<snd_synth_mode>(desc.mode);
  d.ahead_blocks = desc.aheadBlocks;
  d.priority = desc.priority;
  d.bus = desc.bus;
  d.event_template = desc.eventTemplate;
  d.volume = desc.volume;
  return PlaybackHandle::FromC(snd_play_synth(sys, &d, emitter.ToC(), detail::W(when)));
}
inline UserBufferHandle CreateUserBuffer(System* sys, const UserBufferDesc& desc) noexcept {
  return UserBufferHandle::FromC(snd_create_user_buffer(sys, &desc));
}
inline Result DestroyUserBuffer(System* sys, UserBufferHandle b) noexcept {
  return detail::R(snd_destroy_user_buffer(sys, b.ToC()));
}
inline bool IsUserBufferReleased(const System* sys, UserBufferHandle b) noexcept {
  return snd_get_user_buffer_released(sys, b.ToC()) != 0;
}
inline std::uint32_t WriteUserBuffer(System* sys, UserBufferHandle b, const float* const* channels,
                                     std::uint32_t frames) noexcept {
  return snd_write_user_buffer(sys, b.ToC(), channels, frames);
}
inline PlaybackHandle PlayUserBuffer(System* sys, UserBufferHandle b, Id bus = kIdNone,
                                     EmitterHandle emitter = kNoEmitter,
                                     const When& when = When::Immediate()) noexcept {
  return PlaybackHandle::FromC(
      snd_play_user_buffer(sys, b.ToC(), bus, emitter.ToC(), detail::W(when)));
}
inline PlaybackHandle PlayInput(System* sys, const InputDesc& desc,
                                EmitterHandle emitter = kNoEmitter,
                                const When& when = When::Immediate()) noexcept {
  return PlaybackHandle::FromC(snd_play_input(sys, &desc, emitter.ToC(), detail::W(when)));
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
inline PlaybackState GetPlaybackState(const System* sys, PlaybackHandle p) noexcept {
  return static_cast<PlaybackState>(snd_get_playback_state(sys, p.ToC()));
}
inline Result GetPosition(const System* sys, PlaybackHandle p, Position* out) noexcept {
  return detail::R(snd_get_position(sys, p.ToC(), out));
}
inline Result GetPosition(const System* sys, TransportHandle t, Position* out) noexcept {
  return detail::R(snd_get_transport_position(sys, t.ToC(), out));
}
inline Result GetMeter(const System* sys, Id bus, Meter* out) noexcept {
  return detail::R(snd_get_meter(sys, bus, out));
}
inline Result GetStats(const System* sys, Stats* out) noexcept {
  return detail::R(snd_get_stats(sys, out));
}

}  // namespace snd

#endif  // SND_SONUS_HPP
