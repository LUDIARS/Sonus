/*
 * Runtime IDs (design.md 10.1).
 *
 * FROZEN CONTRACT. The hash below MUST stay bit-identical between the
 * runtime, buildtool (bank ID tables, generated game headers) and the C++
 * literal "..."_id (snd/id.hpp). Test vectors: contract/test/hash_test.cpp.
 *
 * Algorithm: 32-bit FNV-1a over the exact bytes of the name (UTF-8, no case
 * folding, no path normalisation, no terminating NUL).
 *   h = 2166136261u; for each byte b: h = (h ^ b) * 16777619u (mod 2^32)
 * The value 0 is reserved for "no id" (SND_ID_NONE). If the hash of a name is
 * 0 the id is 1 instead. Collisions (including with remapped 0) are detected
 * project-wide by buildtool and are build errors (10.1).
 */
#ifndef SND_ID_H
#define SND_ID_H

#include <stddef.h>
#include <stdint.h>

#include "snd/version.h"

SND_EXTERN_C_BEGIN

typedef uint32_t snd_id;

#define SND_ID_NONE ((snd_id)0u)
/* 1.2 (D-104): ids the runtime reserves for itself (the distance curve input 0xFFFFFFFE,
 * the per-playback volume 0xFFFFFFFD and pitch 0xFFFFFFFC). No name may hash to them
 * (buildtool reports an error); snd_set_param_* refuse them (SND_ERR_INVALID_ARGUMENT). */
#define SND_ID_RESERVED_FIRST ((snd_id)0xFFFFFFFCu)
#define SND_ID_RESERVED_LAST ((snd_id)0xFFFFFFFEu)
#define SND_FNV1A32_OFFSET 2166136261u
#define SND_FNV1A32_PRIME 16777619u

/* Hash `len` bytes. Never returns SND_ID_NONE. Thread: any. */
static inline snd_id snd_hash_id_n(const char* name, size_t len) {
  uint32_t h = SND_FNV1A32_OFFSET;
  size_t i;
  for (i = 0; i < len; ++i) {
    h ^= (uint32_t)(unsigned char)name[i];
    h *= SND_FNV1A32_PRIME;
  }
  return h == 0u ? 1u : h;
}

/* Hash a NUL-terminated name. NULL hashes like "". Thread: any. */
static inline snd_id snd_hash_id(const char* name) {
  size_t len = 0;
  if (name != NULL) {
    while (name[len] != '\0') ++len;
  }
  return snd_hash_id_n(name, len);
}

SND_EXTERN_C_END

#endif /* SND_ID_H */
