// The C API (snd/sonus.h): entry checks and forwarding to the composed runtime.
//
// Entry rules (D-84, D-93): every recording function that has a replayable
// command checks its arguments with the shared rule table of the live link
// (livelink/command_check.hpp, kCommandRules / RuleOf(type).api) on the payload
// the command would carry, before core records it. The API therefore refuses
// exactly what a replay refuses; the table is defined once. A handle that is 0
// is refused first (SND_ERR_INVALID_HANDLE, D-29); core then checks staleness
// (generation) and records. Functions whose commands point into the game's
// memory (PlaySynth, user buffers, PlayInput) are not replayable and keep
// core's own checks.
//
// sys == NULL: recording functions return SND_ERR_INVALID_ARGUMENT (or an
// invalid handle), queries their "nothing" value.
#include <cmath>
#include <cstring>
#include <new>
#include <thread>

#include "config_check.hpp"
#include "snd/base/allocator.hpp"
#include "snd/sonus.h"
#include "system.hpp"

namespace {

using CT = snd::cmd::CommandType;
using snd::HandleRef;

// RuleOf(type).api on the would-be payload; commands that are not replayable pass.
template <class P>
bool EntryOk(const snd_system& s, CT type, const P& payload) noexcept {
  const snd::livelink::CommandRule* r = snd::livelink::RuleOf(static_cast<std::uint16_t>(type));
  if (r == nullptr || !r->replayable) return true;
  return r->api(reinterpret_cast<const std::uint8_t*>(&payload), s.limits);
}

snd_when WhenOf(const snd_when* w) noexcept {
  if (w != nullptr) return *w;
  snd_when z{};
  z.kind = static_cast<std::uint8_t>(SND_WHEN_IMMEDIATE);
  return z;
}

HandleRef Ref(std::uint64_t v) noexcept { return HandleRef::FromValue(v); }

// D-104: what snd_set_playback_volume / _pitch accept (stricter than SetParam's rule).
bool VolumeOk(float v) noexcept { return std::isfinite(v) && v >= 0.0f; }
bool PitchOk(float p) noexcept { return std::isfinite(p) && p > 0.0f; }
// D-104: the reserved ids are the runtime's; snd_set_param_* refuse them (the rule table's
// definition, shared with the replay).
bool ReservedParam(snd_id id) noexcept { return snd::livelink::IsReservedId(id); }

}  // namespace

extern "C" {

uint32_t snd_contract_version(void) { return SND_CONTRACT_VERSION; }

// ---------------------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------------------
snd_result snd_create(const snd_config* cfg, const snd_allocator* allocator, const snd_file_system* file_system,
                      snd_system** out) {
  if (out != nullptr) *out = nullptr;
  if (cfg == nullptr || out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  snd_config c;
  snd_result r = snd::compose::NormalizeConfig(cfg, &c);
  if (r != SND_OK) return r;
  r = snd::compose::ValidateConfig(c, snd::compose::LiveLinkWanted(c));
  if (r != SND_OK) return r;
  if (allocator != nullptr && (allocator->allocate == nullptr || allocator->deallocate == nullptr)) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  // D-103: the root belongs to the built-in file system; with the game's it means nothing.
  if (file_system != nullptr && c.file_system_root != nullptr && c.file_system_root[0] != '\0') {
    return SND_ERR_INVALID_ARGUMENT;
  }
  snd::compose::Note("create:config");
  // The system object itself comes from the game's allocator (3.3).
  snd::CAllocator temp(allocator != nullptr ? *allocator : snd_allocator{});
  snd::IAllocator& a = allocator != nullptr ? static_cast<snd::IAllocator&>(temp) : snd::base::DefaultAllocator();
  void* mem = a.Allocate(sizeof(snd_system), alignof(snd_system) < 64 ? 64 : alignof(snd_system), snd::MemTag::Misc);
  if (mem == nullptr) return SND_ERR_OUT_OF_BUDGET;
  snd_system* sys = new (mem) snd_system();
  sys->config = c;
  // Tables passed by pointer live as long as the system (the C++ wrapper's are temporaries).
  for (std::uint32_t i = 0; i < c.plugins.codec_count; ++i) sys->codecTables[i] = c.plugins.codecs[i];
  for (std::uint32_t i = 0; i < c.plugins.effect_count; ++i) sys->effectTables[i] = c.plugins.effects[i];
  sys->config.plugins.codecs = c.plugins.codec_count != 0u ? sys->codecTables : nullptr;
  sys->config.plugins.effects = c.plugins.effect_count != 0u ? sys->effectTables : nullptr;
  if (c.jobs.external != nullptr) {
    sys->jobTable = *c.jobs.external;
    sys->config.jobs.external = &sys->jobTable;
  }
  if (c.output.device != nullptr) {
    sys->deviceTable = *c.output.device;
    sys->config.output.device = &sys->deviceTable;
  }
  sys->owner = std::this_thread::get_id();
  sys->hasGameAllocator = allocator != nullptr;
  if (allocator != nullptr) sys->gameAllocator = *allocator;
  sys->alloc = allocator != nullptr ? static_cast<snd::IAllocator*>(sys->cAllocator.Emplace(*allocator))
                                    : &snd::base::DefaultAllocator();
  r = snd::compose::Compose(*sys, allocator, file_system);
  sys->config.file_system_root = nullptr;  // copied by the file system; the game's string may go
  if (r != SND_OK) {
    snd_destroy(sys);
    return r;
  }
  *out = sys;
  return SND_OK;
}

void snd_destroy(snd_system* sys) {
  if (sys == nullptr) return;
  snd::compose::Decompose(*sys);
  // Free with the allocator the object came from (the adapter lived inside it).
  const bool game = sys->hasGameAllocator;
  const snd_allocator table = sys->gameAllocator;
  sys->~snd_system();
  if (game) {
    snd::CAllocator a(table);
    a.Free(sys, snd::MemTag::Misc);
  } else {
    snd::base::DefaultAllocator().Free(sys, snd::MemTag::Misc);
  }
  snd::compose::Note("destroy:system");
}

snd_result snd_update(snd_system* sys) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (std::this_thread::get_id() != sys->owner) return SND_ERR_WRONG_THREAD;
  snd::compose::ServiceDevice(*sys);
  const snd_result r = sys->core->Update();
  snd::compose::DeliverHeldEnds(*sys);
  return r;
}
// D-102: the manual device runs its periods on the owner thread.
snd_result snd_advance_output(snd_system* sys, uint32_t periods, float* out, uint32_t out_frames) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (std::this_thread::get_id() != sys->owner) return SND_ERR_WRONG_THREAD;
  if (sys->manual == nullptr) return SND_ERR_UNSUPPORTED;
  if (out != nullptr && static_cast<std::uint64_t>(periods) * sys->blockFrames > out_frames) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  snd::compose::AdvanceManualDevice(*sys, periods, out);
  return SND_OK;
}
snd_result snd_register_thread(snd_system* sys) { return sys ? sys->core->RegisterThread() : SND_ERR_INVALID_ARGUMENT; }
snd_result snd_unregister_thread(snd_system* sys) { return sys ? sys->core->UnregisterThread() : SND_ERR_INVALID_ARGUMENT; }
snd_result snd_flush(snd_system* sys) { return sys ? sys->core->Flush() : SND_ERR_INVALID_ARGUMENT; }

// 2.6. Suspend: core renders silence and holds the common clock from the block that
// applies it; the device keeps running (an instant resume). Resume: the command, and
// on the owner thread AudioDriver::Resume for a device the OS left SUSPENDED (iOS
// interruption without ShouldResume, D-66); from another thread that runs in the
// owner's next snd_update.
snd_result snd_suspend(snd_system* sys) { return sys ? sys->core->Suspend() : SND_ERR_INVALID_ARGUMENT; }
snd_result snd_resume(snd_system* sys) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  const snd_result r = sys->core->Resume();
  if (r != SND_OK) return r;
  if (std::this_thread::get_id() == sys->owner) {
    snd::compose::ResumeDevice(*sys);
  } else {
    sys->resumeRequested.store(1u, std::memory_order_release);
  }
  return SND_OK;
}

snd_result snd_set_callbacks(snd_system* sys, const snd_callbacks* callbacks) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  return snd::compose::SetCallbacks(*sys, callbacks);
}

// ---------------------------------------------------------------------------------------
// Banks
// ---------------------------------------------------------------------------------------
snd_bank_handle snd_load_bank_async(snd_system* sys, const char* path) {
  return sys ? sys->core->LoadBankAsync(path) : snd_bank_handle{0};
}
snd_result snd_unload_bank(snd_system* sys, snd_bank_handle bank, snd_unload_mode mode) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (bank.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::BankUnload, snd::cmd::BankUnload{Ref(bank.value), mode, 0u})) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->UnloadBank(bank, mode);
}
snd_bank_state snd_get_bank_state(const snd_system* sys, snd_bank_handle bank) {
  return sys ? sys->core->GetBankState(bank) : SND_BANK_STATE_UNLOADED;
}
snd_result snd_set_language(snd_system* sys, snd_id language) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (!EntryOk(*sys, CT::SetLanguage, snd::cmd::SetLanguage{language, 0u})) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->SetLanguage(language);
}
// D-50 / A15-3: the name table is the bank manager's (synchronous, under its lock);
// core then records the must-deliver DeclareLanguage command.
snd_result snd_declare_language(snd_system* sys, const char* name) {
  if (sys == nullptr || name == nullptr) return SND_ERR_INVALID_ARGUMENT;
  const std::size_t n = std::strlen(name);
  if (n == 0u || n >= SND_MAX_LANGUAGE_NAME_BYTES) return SND_ERR_INVALID_ARGUMENT;
  snd::cmd::DeclareLanguage c{};
  c.language = snd::HashId(std::string_view(name, n));
  c.nameBytes = static_cast<std::uint32_t>(n);
  std::memcpy(c.name, name, n);
  if (!EntryOk(*sys, CT::DeclareLanguage, c)) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->DeclareLanguage(name);
}
snd_result snd_get_bank_chunk(const snd_system* sys, snd_bank_handle bank, uint32_t fourcc, const void** data,
                              uint64_t* bytes) {
  if (data != nullptr) *data = nullptr;
  if (bytes != nullptr) *bytes = 0;
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (std::this_thread::get_id() != sys->owner) return SND_ERR_WRONG_THREAD;
  return sys->core->GetBankChunk(bank, fourcc, data, bytes);
}

// ---------------------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------------------
snd_playback_handle snd_play(snd_system* sys, snd_id event, snd_emitter_handle emitter, const snd_when* when) {
  if (sys == nullptr) return snd_playback_handle{0};
  snd::cmd::Play c{};
  c.when = WhenOf(when);
  c.emitter = Ref(emitter.value);
  c.event = event;
  if (!EntryOk(*sys, CT::Play, c)) return snd_playback_handle{0};
  return sys->core->Play(event, emitter, when);
}
snd_result snd_play_one_shot(snd_system* sys, snd_id event, snd_emitter_handle emitter, const snd_when* when) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  snd::cmd::Play c{};
  c.when = WhenOf(when);
  c.flags = snd::cmd::kPlayOneShot;
  if (!EntryOk(*sys, CT::Play, c)) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->PlayOneShot(event, emitter, when);
}
snd_result snd_play_at(snd_system* sys, snd_id event, const snd_vec3* position, const snd_when* when) {
  if (sys == nullptr || position == nullptr) return SND_ERR_INVALID_ARGUMENT;
  snd::cmd::Play c{};
  c.when = WhenOf(when);
  c.flags = snd::cmd::kPlayOneShot | snd::cmd::kPlayHasPosition;
  c.position = *position;
  if (!EntryOk(*sys, CT::Play, c)) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->PlayAt(event, position, when);
}
snd_result snd_stop(snd_system* sys, snd_playback_handle playback, float fade_seconds, const snd_when* when) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (playback.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::Stop, snd::cmd::Stop{Ref(playback.value), fade_seconds, 0u, WhenOf(when)})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->Stop(playback, fade_seconds, when);
}
snd_result snd_pause(snd_system* sys, snd_playback_handle playback, float fade_seconds) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (playback.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::PauseResume, snd::cmd::PauseResume{Ref(playback.value), fade_seconds, 1u})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->Pause(playback, fade_seconds);
}
snd_result snd_resume_playback(snd_system* sys, snd_playback_handle playback, float fade_seconds) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (playback.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::PauseResume, snd::cmd::PauseResume{Ref(playback.value), fade_seconds, 0u})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->ResumePlayback(playback, fade_seconds);
}
snd_result snd_set_scheduled_stop(snd_system* sys, snd_playback_handle playback, const snd_when* when,
                                  float fade_seconds) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (playback.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::SetScheduledStop, snd::cmd::SetScheduledStop{Ref(playback.value), fade_seconds, 0u, WhenOf(when)})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SetScheduledStop(playback, when, fade_seconds);
}

// D-05: slot = 0x80000000 | playback slot, generation = the playback's (pure).
snd_transport_handle snd_get_playback_transport(snd_playback_handle playback) {
  if (playback.value == SND_HANDLE_INVALID_VALUE) return snd_transport_handle{0};
  const uint32_t slot = SND_HANDLE_SLOT(playback.value);
  if ((slot & SND_TRANSPORT_PLAYBACK_BIT) != 0u) return snd_transport_handle{0};
  return snd_transport_handle{
      SND_HANDLE_MAKE(slot | SND_TRANSPORT_PLAYBACK_BIT, SND_HANDLE_GENERATION(playback.value))};
}

// ---------------------------------------------------------------------------------------
// 3D
// ---------------------------------------------------------------------------------------
snd_emitter_handle snd_create_emitter(snd_system* sys) { return sys ? sys->core->CreateEmitter() : snd_emitter_handle{0}; }
snd_result snd_destroy_emitter(snd_system* sys, snd_emitter_handle emitter) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (emitter.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  return sys->core->DestroyEmitter(emitter);
}
snd_result snd_set_transform(snd_system* sys, snd_emitter_handle emitter, const snd_vec3* position,
                             const snd_vec3* forward, const snd_vec3* velocity) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (emitter.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  return sys->core->SetTransform(emitter, position, forward, velocity);
}
snd_result snd_set_listener(snd_system* sys, uint32_t index, const snd_listener_desc* listener) {
  if (sys == nullptr || listener == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (!EntryOk(*sys, CT::SetListener, snd::cmd::SetListener{index, 0u, *listener})) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->SetListener(index, listener);
}

// ---------------------------------------------------------------------------------------
// Parameters and switches
// ---------------------------------------------------------------------------------------
namespace {
snd::cmd::SetParam ParamCmd(std::uint8_t scope, snd_id param, std::uint64_t target, float value) noexcept {
  snd::cmd::SetParam c{};
  c.scope = scope;
  c.param = param;
  c.target = Ref(target);
  c.value = value;
  return c;
}
constexpr std::uint8_t kGlobal = static_cast<std::uint8_t>(snd::ParamScope::Global);
constexpr std::uint8_t kEmitter = static_cast<std::uint8_t>(snd::ParamScope::Emitter);
constexpr std::uint8_t kPlayback = static_cast<std::uint8_t>(snd::ParamScope::Playback);
}  // namespace

snd_result snd_set_param_global(snd_system* sys, snd_id param, float value) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (ReservedParam(param) || !EntryOk(*sys, CT::SetParam, ParamCmd(kGlobal, param, 0u, value))) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SetParamGlobal(param, value);
}
snd_result snd_set_param_emitter(snd_system* sys, snd_emitter_handle emitter, snd_id param, float value) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (emitter.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (ReservedParam(param) || !EntryOk(*sys, CT::SetParam, ParamCmd(kEmitter, param, emitter.value, value))) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SetParamEmitter(emitter, param, value);
}
snd_result snd_set_param_playback(snd_system* sys, snd_playback_handle playback, snd_id param, float value) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (playback.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (ReservedParam(param) || !EntryOk(*sys, CT::SetParam, ParamCmd(kPlayback, param, playback.value, value))) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SetParamPlayback(playback, param, value);
}
// D-104: playback-scope SetParam with the reserved ids (handle 0 first, then the value,
// then core's staleness check; D-93).
snd_result snd_set_playback_volume(snd_system* sys, snd_playback_handle playback, float volume) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (playback.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!VolumeOk(volume)) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->SetParamPlayback(playback, snd::core::kParamPlaybackVolume, volume);
}
snd_result snd_set_playback_pitch(snd_system* sys, snd_playback_handle playback, float pitch) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (playback.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!PitchOk(pitch)) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->SetParamPlayback(playback, snd::core::kParamPlaybackPitch, pitch);
}
snd_result snd_set_switch_global(snd_system* sys, snd_id group, snd_id value) {
  return sys ? sys->core->SetSwitchGlobal(group, value) : SND_ERR_INVALID_ARGUMENT;
}
snd_result snd_set_switch_emitter(snd_system* sys, snd_emitter_handle emitter, snd_id group, snd_id value) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (emitter.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  return sys->core->SetSwitchEmitter(emitter, group, value);
}
snd_result snd_set_switch_playback(snd_system* sys, snd_playback_handle playback, snd_id group, snd_id value) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (playback.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  return sys->core->SetSwitchPlayback(playback, group, value);
}

// ---------------------------------------------------------------------------------------
// Mix
// ---------------------------------------------------------------------------------------
snd_result snd_set_bus_volume(snd_system* sys, snd_id bus, float volume, float fade_seconds) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  // The recorded fade is clamped to >= 0 by core; a non-finite fade is refused below.
  if (!std::isfinite(fade_seconds) ||
      !EntryOk(*sys, CT::SetBusVolume, snd::cmd::SetBusVolume{bus, volume, fade_seconds < 0 ? 0 : fade_seconds, 0u})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SetBusVolume(bus, volume, fade_seconds);
}
snd_result snd_apply_snapshot(snd_system* sys, snd_id snapshot, float weight, float time_seconds) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (!std::isfinite(time_seconds) ||
      !EntryOk(*sys, CT::ApplySnapshot, snd::cmd::ApplySnapshot{snapshot, weight, time_seconds < 0 ? 0 : time_seconds, 0u})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->ApplySnapshot(snapshot, weight, time_seconds);
}
snd_result snd_set_category_volume(snd_system* sys, snd_id category, float volume, float fade_seconds) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (!std::isfinite(fade_seconds) ||
      !EntryOk(*sys, CT::SetCategoryVolume,
               snd::cmd::SetCategoryVolume{category, volume, fade_seconds < 0 ? 0 : fade_seconds, 0u})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SetCategoryVolume(category, volume, fade_seconds);
}
snd_result snd_pause_category(snd_system* sys, snd_id category, int32_t paused) {
  return sys ? sys->core->PauseCategory(category, paused != 0) : SND_ERR_INVALID_ARGUMENT;
}
snd_result snd_mute_category(snd_system* sys, snd_id category, int32_t muted) {
  return sys ? sys->core->MuteCategory(category, muted != 0) : SND_ERR_INVALID_ARGUMENT;
}

// ---------------------------------------------------------------------------------------
// Music
// ---------------------------------------------------------------------------------------
snd_result snd_set_music_state(snd_system* sys, snd_playback_handle music, snd_id state, const snd_when* when) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (music.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::SetMusicState,
               snd::cmd::SetMusicState{Ref(music.value), state, snd::cmd::kMusicStateVariantState, WhenOf(when)})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SetMusicState(music, state, when);
}
snd_result snd_prepare_music_state(snd_system* sys, snd_playback_handle music, snd_id state) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (music.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  return sys->core->PrepareMusicState(music, state);
}
snd_result snd_play_stinger(snd_system* sys, snd_playback_handle music, snd_id stinger, const snd_when* when) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (music.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::SetMusicState,
               snd::cmd::SetMusicState{Ref(music.value), stinger, snd::cmd::kMusicStateVariantStinger, WhenOf(when)})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->PlayStinger(music, stinger, when);
}
snd_transport_handle snd_create_transport(snd_system* sys, const snd_transport_desc* desc) {
  if (sys == nullptr || desc == nullptr) return snd_transport_handle{0};
  if (!EntryOk(*sys, CT::CreateTransport, snd::cmd::CreateTransport{HandleRef{}, *desc})) return snd_transport_handle{0};
  return sys->core->CreateTransport(desc);
}
snd_result snd_destroy_transport(snd_system* sys, snd_transport_handle transport) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (transport.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  return sys->core->DestroyTransport(transport);
}
snd_result snd_set_transport_state(snd_system* sys, snd_transport_handle transport, snd_transport_state state,
                                   const snd_when* when) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (transport.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::SetTransportState, snd::cmd::SetTransportState{Ref(transport.value), state, 0u, WhenOf(when)})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SetTransportState(transport, state, when);
}
snd_result snd_set_tempo(snd_system* sys, snd_transport_handle transport, float bpm, const snd_when* when) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (transport.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::SetTempo, snd::cmd::SetTempo{Ref(transport.value), bpm, snd::cmd::kTempoVariantBpm, WhenOf(when)})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SetTempo(transport, bpm, when);
}
snd_result snd_set_transport_pitch(snd_system* sys, snd_transport_handle transport, float pitch, const snd_when* when) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (transport.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::SetTempo,
               snd::cmd::SetTempo{Ref(transport.value), pitch, snd::cmd::kTempoVariantPitch, WhenOf(when)})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SetTransportPitch(transport, pitch, when);
}
snd_result snd_seek_transport(snd_system* sys, snd_transport_handle transport, const snd_musical_time* position,
                              const snd_when* when) {
  if (sys == nullptr || position == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (transport.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  if (!EntryOk(*sys, CT::SeekTransport, snd::cmd::SeekTransport{Ref(transport.value), *position, 0u, WhenOf(when)})) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return sys->core->SeekTransport(transport, position, when);
}

// ---------------------------------------------------------------------------------------
// Dynamic generation (not replayable: core's checks)
// ---------------------------------------------------------------------------------------
snd_playback_handle snd_play_synth(snd_system* sys, const snd_synth_desc* desc, snd_emitter_handle emitter,
                                   const snd_when* when) {
  return sys ? sys->core->PlaySynth(desc, emitter, when) : snd_playback_handle{0};
}
snd_user_buffer_handle snd_create_user_buffer(snd_system* sys, const snd_user_buffer_desc* desc) {
  return sys ? sys->core->CreateUserBuffer(desc) : snd_user_buffer_handle{0};
}
snd_result snd_destroy_user_buffer(snd_system* sys, snd_user_buffer_handle buffer) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (buffer.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  return sys->core->DestroyUserBuffer(buffer);
}
int32_t snd_get_user_buffer_released(const snd_system* sys, snd_user_buffer_handle buffer) {
  return sys ? sys->core->GetUserBufferReleased(buffer) : 0;
}
uint32_t snd_write_user_buffer(snd_system* sys, snd_user_buffer_handle buffer, const float* const* channels,
                               uint32_t frames) {
  return sys ? sys->core->WriteUserBuffer(buffer, channels, frames) : 0u;
}
snd_playback_handle snd_play_user_buffer(snd_system* sys, snd_user_buffer_handle buffer, snd_id bus,
                                         snd_emitter_handle emitter, const snd_when* when) {
  return sys ? sys->core->PlayUserBuffer(buffer, bus, emitter, when) : snd_playback_handle{0};
}
// D-104: the play and its starting values in one go (the same arena: applied in order,
// the values before the playback's first block).
snd_playback_handle snd_play_user_buffer_ex(snd_system* sys, snd_user_buffer_handle buffer, snd_id bus, float volume,
                                            float pitch, snd_emitter_handle emitter, const snd_when* when) {
  if (sys == nullptr || !VolumeOk(volume) || !PitchOk(pitch)) return snd_playback_handle{0};
  const snd_playback_handle h = sys->core->PlayUserBuffer(buffer, bus, emitter, when);
  if (h.value == SND_HANDLE_INVALID_VALUE) return h;
  if (volume != 1.0f) sys->core->SetParamPlayback(h, snd::core::kParamPlaybackVolume, volume);
  if (pitch != 1.0f) sys->core->SetParamPlayback(h, snd::core::kParamPlaybackPitch, pitch);
  return h;
}
snd_playback_handle snd_play_input(snd_system* sys, const snd_input_desc* desc, snd_emitter_handle emitter,
                                   const snd_when* when) {
  return sys ? sys->core->PlayInput(desc, emitter, when) : snd_playback_handle{0};
}

// ---------------------------------------------------------------------------------------
// Queries (any thread)
// ---------------------------------------------------------------------------------------
snd_playback_state snd_get_playback_state(const snd_system* sys, snd_playback_handle playback) {
  return sys ? sys->core->GetPlaybackState(playback) : SND_PLAYBACK_STATE_NONE;
}
snd_result snd_get_position(const snd_system* sys, snd_playback_handle playback, snd_position* out) {
  if (sys == nullptr || out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->GetPosition(playback, out);
}
snd_result snd_get_transport_position(const snd_system* sys, snd_transport_handle transport, snd_position* out) {
  if (sys == nullptr || out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->GetTransportPosition(transport, out);
}
snd_result snd_get_meter(const snd_system* sys, snd_id bus, snd_meter* out) {
  if (sys == nullptr || out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->GetMeter(bus, out);
}
snd_result snd_get_stats(const snd_system* sys, snd_stats* out) {
  if (sys == nullptr || out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->GetStats(out);
}

// ---------------------------------------------------------------------------------------
// Effects at run time (stage 3, coverage: D-130, D-132). Variant commands of SetBusVolume /
// SetSwitch / SetParam: core checks the arguments (the live link's rule table does not know
// the variants yet; see the stage-3 report).
// ---------------------------------------------------------------------------------------
snd_result snd_set_effect_param(snd_system* sys, snd_id bus, uint32_t effect_index, uint32_t param_index, float value,
                                float fade_seconds) {
  return sys ? sys->core->SetEffectParam(bus, effect_index, param_index, value, fade_seconds) : SND_ERR_INVALID_ARGUMENT;
}
snd_result snd_set_event_voice_effect(snd_system* sys, snd_id event, uint32_t slot, snd_id effect_kind,
                                      const float* params, uint32_t param_count) {
  return sys ? sys->core->SetEventVoiceEffect(event, slot, effect_kind, params, param_count) : SND_ERR_INVALID_ARGUMENT;
}
snd_result snd_set_playback_effect_param(snd_system* sys, snd_playback_handle playback, uint32_t slot,
                                         uint32_t param_index, float value) {
  if (sys == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (playback.value == SND_HANDLE_INVALID_VALUE) return SND_ERR_INVALID_HANDLE;
  return sys->core->SetPlaybackEffectParam(playback, slot, param_index, value);
}

}  // extern "C"

// ---------------------------------------------------------------------------------------
// Stage 3 additions (session B, group 4: D-146, D-148)
// ---------------------------------------------------------------------------------------
extern "C" {
snd_playback_handle snd_play_fade(snd_system* sys, snd_id event, snd_emitter_handle emitter, const snd_when* when,
                                  float fade_in_seconds) {
  return sys ? sys->core->PlayFade(event, emitter, when, fade_in_seconds) : snd_playback_handle{0};
}
snd_result snd_get_spectrum(const snd_system* sys, snd_id bus, uint32_t channel, float* out_magnitudes,
                            uint32_t bin_count) {
  if (sys == nullptr || out_magnitudes == nullptr) return SND_ERR_INVALID_ARGUMENT;
  return sys->core->GetSpectrum(bus, channel, out_magnitudes, bin_count);
}
}  // extern "C"
