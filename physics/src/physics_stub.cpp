// Stage-0 stubs of the physics C API (snd/physics.h). Implemented in stage 4
// on top of the public API only (design.md 9, 12.1).
#include "snd/physics.h"

extern "C" {

snd_result snd_phys_create(snd_system*, const snd_phys_desc* desc, const snd_allocator*,
                           snd_phys** out) {
  if (out != nullptr) *out = nullptr;
  if (desc == nullptr || out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  return SND_ERR_NOT_IMPLEMENTED;
}
void snd_phys_destroy(snd_phys*) {}
snd_result snd_phys_submit_contacts(snd_phys*, const snd_contact*, uint32_t, float) {
  return SND_ERR_NOT_IMPLEMENTED;
}
snd_result snd_phys_play_footstep(snd_phys*, snd_emitter_handle, uint16_t, const snd_surface_weight*,
                                  uint32_t, snd_motion) {
  return SND_ERR_NOT_IMPLEMENTED;
}

}  // extern "C"
