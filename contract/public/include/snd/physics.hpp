// Physics-material audio (design.md 9), C++ wrapper. FROZEN CONTRACT.
#ifndef SND_PHYSICS_HPP
#define SND_PHYSICS_HPP

#include <cstdint>

#include "snd/physics.h"
#include "snd/sonus.hpp"

namespace snd::phys {

using PhysicsAudio = snd_phys;
using Contact = snd_contact;
using SurfaceWeight = snd_surface_weight;
using PhysicsDesc = snd_phys_desc;

enum class ContactKind : std::uint32_t {
  Impact = SND_CONTACT_IMPACT,
  Slide = SND_CONTACT_SLIDE,
  Roll = SND_CONTACT_ROLL,
  Break = SND_CONTACT_BREAK,
};

enum class Motion : std::uint32_t {
  Walk = SND_MOTION_WALK,
  Run = SND_MOTION_RUN,
  Sprint = SND_MOTION_SPRINT,
  Jump = SND_MOTION_JUMP,
  Land = SND_MOTION_LAND,
  Scuff = SND_MOTION_SCUFF,
};

inline PhysicsDesc DefaultDesc(BankHandle bank, Id table) noexcept {
  PhysicsDesc d;
  snd_phys_desc_init(&d);
  d.bank = bank.ToC();
  d.table = table;
  return d;
}

inline Result Create(System* sys, const PhysicsDesc& desc, IAllocator* allocator,
                     PhysicsAudio** out) noexcept {
  snd_allocator a{};
  if (allocator != nullptr) a = ToC(*allocator);
  return static_cast<Result>(snd_phys_create(sys, &desc, allocator ? &a : nullptr, out));
}
inline void Destroy(PhysicsAudio* p) noexcept { snd_phys_destroy(p); }

inline Result SubmitContacts(PhysicsAudio* p, const Contact* contacts, std::uint32_t count,
                             float dtSeconds) noexcept {
  return static_cast<Result>(snd_phys_submit_contacts(p, contacts, count, dtSeconds));
}
inline Result PlayFootstep(PhysicsAudio* p, EmitterHandle foot, std::uint16_t footMaterial,
                           const SurfaceWeight* surfaces, std::uint32_t surfaceCount,
                           Motion motion) noexcept {
  return static_cast<Result>(snd_phys_play_footstep(p, foot.ToC(), footMaterial, surfaces,
                                                    surfaceCount,
                                                    static_cast<snd_motion>(motion)));
}

}  // namespace snd::phys

#endif  // SND_PHYSICS_HPP
