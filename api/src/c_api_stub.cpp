// Stage-0 stubs of the C API (snd/sonus.h). Every function exists so that the
// C++ wrapper and C users link; behaviour is "not implemented" except for the
// pure functions defined by the contract itself. The api owner replaces these
// in stage 2 (design.md 15.1) without changing any signature.
#include "snd/sonus.h"

namespace {
constexpr snd_result kNI = SND_ERR_NOT_IMPLEMENTED;
}

extern "C" {

uint32_t snd_contract_version(void) { return SND_CONTRACT_VERSION; }

snd_result snd_create(const snd_config* cfg, const snd_allocator*, const snd_file_system*,
                      snd_system** out) {
  if (out != nullptr) *out = nullptr;
  if (cfg == nullptr || out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (cfg->struct_size != sizeof(snd_config) ||
      (cfg->contract_version >> 16) != SND_CONTRACT_VERSION_MAJOR) {
    return SND_ERR_VERSION_MISMATCH;
  }
  return kNI;
}
void snd_destroy(snd_system*) {}
snd_result snd_update(snd_system* sys) { return sys ? kNI : SND_ERR_INVALID_ARGUMENT; }
snd_result snd_register_thread(snd_system*) { return kNI; }
snd_result snd_unregister_thread(snd_system*) { return kNI; }
snd_result snd_flush(snd_system*) { return kNI; }
snd_result snd_suspend(snd_system*) { return kNI; }
snd_result snd_resume(snd_system*) { return kNI; }
snd_result snd_set_callbacks(snd_system*, const snd_callbacks*) { return kNI; }

snd_bank_handle snd_load_bank_async(snd_system*, const char*) { return snd_bank_handle{0}; }
snd_result snd_unload_bank(snd_system*, snd_bank_handle, snd_unload_mode) { return kNI; }
snd_bank_state snd_get_bank_state(const snd_system*, snd_bank_handle) { return SND_BANK_STATE_UNLOADED; }
snd_result snd_set_language(snd_system*, snd_id) { return kNI; }
snd_result snd_declare_language(snd_system*, const char*) { return kNI; }
snd_result snd_get_bank_chunk(const snd_system*, snd_bank_handle, uint32_t, const void** data,
                              uint64_t* bytes) {
  if (data != nullptr) *data = nullptr;
  if (bytes != nullptr) *bytes = 0;
  return kNI;
}

snd_playback_handle snd_play(snd_system*, snd_id, snd_emitter_handle, const snd_when*) {
  return snd_playback_handle{0};
}
snd_result snd_play_one_shot(snd_system*, snd_id, snd_emitter_handle, const snd_when*) { return kNI; }
snd_result snd_play_at(snd_system*, snd_id, const snd_vec3*, const snd_when*) { return kNI; }
snd_result snd_stop(snd_system*, snd_playback_handle, float, const snd_when*) { return kNI; }
snd_result snd_pause(snd_system*, snd_playback_handle, float) { return kNI; }
snd_result snd_resume_playback(snd_system*, snd_playback_handle, float) { return kNI; }
snd_result snd_set_scheduled_stop(snd_system*, snd_playback_handle, const snd_when*, float) {
  return kNI;
}

// Pure function fixed by the contract (snd/types.h SND_TRANSPORT_PLAYBACK_BIT).
snd_transport_handle snd_get_playback_transport(snd_playback_handle playback) {
  if (playback.value == SND_HANDLE_INVALID_VALUE) return snd_transport_handle{0};
  const uint32_t slot = SND_HANDLE_SLOT(playback.value);
  if ((slot & SND_TRANSPORT_PLAYBACK_BIT) != 0u) return snd_transport_handle{0};
  return snd_transport_handle{
      SND_HANDLE_MAKE(slot | SND_TRANSPORT_PLAYBACK_BIT, SND_HANDLE_GENERATION(playback.value))};
}

snd_emitter_handle snd_create_emitter(snd_system*) { return snd_emitter_handle{0}; }
snd_result snd_destroy_emitter(snd_system*, snd_emitter_handle) { return kNI; }
snd_result snd_set_transform(snd_system*, snd_emitter_handle, const snd_vec3*, const snd_vec3*,
                             const snd_vec3*) {
  return kNI;
}
snd_result snd_set_listener(snd_system*, uint32_t, const snd_listener_desc*) { return kNI; }

snd_result snd_set_param_global(snd_system*, snd_id, float) { return kNI; }
snd_result snd_set_param_emitter(snd_system*, snd_emitter_handle, snd_id, float) { return kNI; }
snd_result snd_set_param_playback(snd_system*, snd_playback_handle, snd_id, float) { return kNI; }
snd_result snd_set_switch_global(snd_system*, snd_id, snd_id) { return kNI; }
snd_result snd_set_switch_emitter(snd_system*, snd_emitter_handle, snd_id, snd_id) { return kNI; }
snd_result snd_set_switch_playback(snd_system*, snd_playback_handle, snd_id, snd_id) { return kNI; }

snd_result snd_set_bus_volume(snd_system*, snd_id, float, float) { return kNI; }
snd_result snd_apply_snapshot(snd_system*, snd_id, float, float) { return kNI; }
snd_result snd_set_category_volume(snd_system*, snd_id, float, float) { return kNI; }
snd_result snd_pause_category(snd_system*, snd_id, int32_t) { return kNI; }
snd_result snd_mute_category(snd_system*, snd_id, int32_t) { return kNI; }

snd_result snd_set_music_state(snd_system*, snd_playback_handle, snd_id, const snd_when*) { return kNI; }
snd_result snd_prepare_music_state(snd_system*, snd_playback_handle, snd_id) { return kNI; }
snd_transport_handle snd_create_transport(snd_system*, const snd_transport_desc*) {
  return snd_transport_handle{0};
}
snd_result snd_destroy_transport(snd_system*, snd_transport_handle) { return kNI; }
snd_result snd_set_transport_state(snd_system*, snd_transport_handle, snd_transport_state,
                                   const snd_when*) {
  return kNI;
}
snd_result snd_set_tempo(snd_system*, snd_transport_handle, float, const snd_when*) { return kNI; }
snd_result snd_seek_transport(snd_system*, snd_transport_handle, const snd_musical_time*,
                              const snd_when*) {
  return kNI;
}

snd_playback_handle snd_play_synth(snd_system*, const snd_synth_desc*, snd_emitter_handle,
                                   const snd_when*) {
  return snd_playback_handle{0};
}
snd_user_buffer_handle snd_create_user_buffer(snd_system*, const snd_user_buffer_desc*) {
  return snd_user_buffer_handle{0};
}
snd_result snd_destroy_user_buffer(snd_system*, snd_user_buffer_handle) { return kNI; }
int32_t snd_get_user_buffer_released(const snd_system*, snd_user_buffer_handle) { return 0; }
uint32_t snd_write_user_buffer(snd_system*, snd_user_buffer_handle, const float* const*, uint32_t) {
  return 0;
}
snd_playback_handle snd_play_user_buffer(snd_system*, snd_user_buffer_handle, snd_id,
                                         snd_emitter_handle, const snd_when*) {
  return snd_playback_handle{0};
}
snd_playback_handle snd_play_input(snd_system*, const snd_input_desc*, snd_emitter_handle,
                                   const snd_when*) {
  return snd_playback_handle{0};
}

snd_playback_state snd_get_playback_state(const snd_system*, snd_playback_handle) {
  return SND_PLAYBACK_STATE_NONE;
}
snd_result snd_get_position(const snd_system*, snd_playback_handle, snd_position*) { return kNI; }
snd_result snd_get_transport_position(const snd_system*, snd_transport_handle, snd_position*) {
  return kNI;
}
snd_result snd_get_meter(const snd_system*, snd_id, snd_meter*) { return kNI; }
snd_result snd_get_stats(const snd_system*, snd_stats*) { return kNI; }

}  // extern "C"
