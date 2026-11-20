/* Independent test (P10): the physics C header compiles as C11 and its init helpers and
 * layouts agree with what C++ sees (indep_api_test.cpp compares the numbers). */
#include <stddef.h>
#include <string.h>

#include "snd/physics/physics.h"

int snd_physics_indep_c11_check(unsigned* sizes, unsigned count);

int snd_physics_indep_c11_check(unsigned* sizes, unsigned count) {
  snd_physics_config c;
  snd_physics_table_desc t;
  snd_physics_stats s;
  snd_physics_record r;
  snd_physics_material_entry e;
  snd_physics_curves cv;
  snd_physics_observer obs = NULL;
  if (count < 8u) return -1;
  snd_physics_config_init(&c);
  snd_physics_table_desc_init(&t);
  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&e, 0, sizeof(e));
  memset(&cv, 0, sizeof(cv));
  sizes[0] = (unsigned)sizeof(snd_physics_config);
  sizes[1] = (unsigned)sizeof(snd_physics_table_desc);
  sizes[2] = (unsigned)sizeof(snd_physics_stats);
  sizes[3] = (unsigned)sizeof(snd_physics_record);
  sizes[4] = (unsigned)sizeof(snd_physics_material_entry);
  sizes[5] = (unsigned)sizeof(snd_physics_curves);
  sizes[6] = (unsigned)offsetof(snd_physics_config, params);
  sizes[7] = (unsigned)offsetof(snd_physics_record, params);
  if (c.struct_size != sizeof(c) || c.version != SND_PHYSICS_CONFIG_VERSION) return 1;
  if (c.budget_per_second != 300.0f || c.grid_size != 0.5f || c.max_held_emitters != 64u) return 2;
  if (c.params.impulse != snd_hash_id(SND_PHYSICS_PARAM_IMPULSE_NAME)) return 3;
  if (t.struct_size != sizeof(t) || t.material_count != 0u || t.footstep_default != SND_ID_NONE) return 4;
  if (obs != NULL) return 5;
  /* The functions are callable from C (NULL arguments only: no system here). */
  if (snd_physics_create(NULL, &c, NULL, NULL) != SND_ERR_INVALID_ARGUMENT) return 6;
  if (snd_physics_material_count(NULL) != 0u) return 7;
  snd_physics_destroy(NULL);
  snd_phys_destroy(NULL);
  return 0;
}
