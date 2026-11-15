/*
 * Physics-material audio (design.md 9), C API. FROZEN CONTRACT.
 *
 * The physics module is an upper layer built only on the public API. It runs
 * on the game thread that owns the snd_phys object; that thread must be a
 * registered thread of the system (it records snd_play / snd_set_param).
 */
#ifndef SND_PHYSICS_H
#define SND_PHYSICS_H

#include <stdint.h>

#include "snd/ext.h"
#include "snd/sonus.h"
#include "snd/types.h"

SND_EXTERN_C_BEGIN

typedef struct snd_phys snd_phys;

typedef uint32_t snd_contact_kind;
#define SND_CONTACT_IMPACT 0u
#define SND_CONTACT_SLIDE  1u
#define SND_CONTACT_ROLL   2u
#define SND_CONTACT_BREAK  3u

typedef uint32_t snd_motion; /* footstep motion */
#define SND_MOTION_WALK   0u
#define SND_MOTION_RUN    1u
#define SND_MOTION_SPRINT 2u
#define SND_MOTION_JUMP   3u
#define SND_MOTION_LAND   4u
#define SND_MOTION_SCUFF  5u

/* 9.2 Contact, as a fixed C layout (64 bytes). */
typedef struct snd_contact {
  uint64_t object_a, object_b;    /* game object ids (cooldown, tracking) */
  uint16_t material_a, material_b; /* sound material ids (index into the material table) */
  snd_contact_kind kind;
  float position[3];
  float normal[3];
  float impulse;        /* N*s */
  float relative_speed; /* m/s */
  float mass_a, mass_b; /* kg */
} snd_contact;
SND_STATIC_ASSERT(sizeof(snd_contact) == 64, "snd_contact must be 64 bytes");

typedef struct snd_surface_weight {
  uint16_t material;
  uint16_t reserved;
  float weight; /* >= 0; the two largest are played, weights mapped to volume (9.2) */
} snd_surface_weight;

typedef struct snd_phys_desc {
  snd_bank_handle bank;          /* bank holding the 'PHYS' chunk */
  snd_id table;                  /* material table id inside the chunk */
  uint32_t max_tracked_pairs;    /* sliding/rolling tracker capacity (9.5), default 64 */
  uint32_t max_cooldowns;        /* cooldown table capacity (9.4), default 256 */
  float impulse_threshold;       /* step 1 of 9.4 (N*s), default from the table */
  float grid_size;               /* step 2 (m), default 0.5 */
  float cooldown_seconds;        /* step 3, default 0.05 */
  float budget_per_second;       /* step 4 (仮 300) */
  float tracking_timeout_seconds;/* 9.5, default 0.2 */
  uint32_t reserved;
} snd_phys_desc;

/* Fill defaults (bank/table left 0). Thread: any. */
static inline void snd_phys_desc_init(snd_phys_desc* d) {
  if (d == 0) return;
  d->bank.value = 0u;
  d->table = SND_ID_NONE;
  d->max_tracked_pairs = 64u;
  d->max_cooldowns = 256u;
  d->impulse_threshold = 0.0f;
  d->grid_size = 0.5f;
  d->cooldown_seconds = 0.05f;
  d->budget_per_second = 300.0f;
  d->tracking_timeout_seconds = 0.2f;
  d->reserved = 0u;
}

/* Create on the calling (registered) thread; all memory comes from `allocator`
 * (NULL = system default) at this call. Errors: INVALID_ARGUMENT, NOT_READY (bank not loaded),
 * NOT_FOUND (table), OUT_OF_BUDGET. */
SND_API snd_result SND_CALL snd_phys_create(snd_system* sys, const snd_phys_desc* desc,
                                            const snd_allocator* allocator, snd_phys** out);
SND_API void SND_CALL snd_phys_destroy(snd_phys* phys);

/* One SoA culling pass over `contacts` (9.4) then records plays. Thread: the creating thread. */
SND_API snd_result SND_CALL snd_phys_submit_contacts(snd_phys* phys, const snd_contact* contacts,
                                                     uint32_t count, float dt_seconds);
SND_API snd_result SND_CALL snd_phys_play_footstep(snd_phys* phys, snd_emitter_handle foot,
                                                   uint16_t foot_material,
                                                   const snd_surface_weight* surfaces,
                                                   uint32_t surface_count, snd_motion motion);

SND_EXTERN_C_END

#endif /* SND_PHYSICS_H */
