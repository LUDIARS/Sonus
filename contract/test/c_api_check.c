/* Compiled as C11: the public C headers work from C and agree with C++. */
#include <stddef.h>
#include <string.h>

#include "snd/physics.h"
#include "snd/sonus.h"

int snd_contract_c_check(void);

int snd_contract_c_check(void) {
  snd_config cfg;
  snd_when w;
  snd_phys_desc pd;
  snd_playback_handle h;
  snd_config_init(&cfg);
  if (cfg.struct_size != sizeof(snd_config)) return 1;
  if (cfg.sample_rate != 48000u || cfg.block_size != 256u) return 2;
  if (cfg.memory.stream_pool_bytes != (8ull << 20)) return 3;
  if (cfg.commands.arenas_per_thread != 3u) return 4;
  if (cfg.jobs.external != NULL || cfg.output.device != NULL) return 5;
  if (snd_hash_id("foobar") != 0xBF9CF968u) return 6;
  memset(&w, 0, sizeof(w));
  if (w.kind != SND_WHEN_IMMEDIATE || sizeof(w) != 32) return 7;
  h.value = SND_HANDLE_MAKE(3u, 4u);
  if (SND_HANDLE_SLOT(h.value) != 3u || SND_HANDLE_GENERATION(h.value) != 4u) return 8;
  snd_phys_desc_init(&pd);
  if (pd.budget_per_second != 300.0f) return 9;
  if (SND_FOURCC('O', 'P', 'U', 'S') != SND_CODEC_OPUS) return 10;
  if (offsetof(snd_contact, impulse) != 48) return 11;
  return 0;
}
