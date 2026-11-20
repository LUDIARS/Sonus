// Independent test (stage 3 group 2): snd_config grew by the four voice-effect fields at its
// end (D-132). config.h: "a shorter struct gets the defaults". The api layer (snd_create)
// owns the struct_size handling, so this test records what happens with a struct of the
// previous layout (ending after output_kind) and does not fail on it: the mismatch is
// material for a request to the api's owner (see the tester's report).
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "snd/sonus.h"
#include "snd/test/test.hpp"

SND_TEST(IndepG2Config, OutOfScope_ConfigOfThePreviousLayout) {
  // The previous layout ended after output_kind; its sizeof rounds up to the struct alignment.
  const std::size_t endOfOld = offsetof(snd_config, output_kind) + sizeof(snd_output_kind);
  const std::size_t oldSize = (endOfOld + alignof(snd_config) - 1u) / alignof(snd_config) * alignof(snd_config);
  std::printf("  previous sizeof = %llu, offsetof(max_voice_effects) = %llu, new sizeof = %llu\n", static_cast<unsigned long long>(oldSize),
              static_cast<unsigned long long>(offsetof(snd_config, max_voice_effects)), static_cast<unsigned long long>(sizeof(snd_config)));
  // The first new field sits inside the old struct's tail padding: a copy of `oldSize` bytes
  // carries the caller's padding into max_voice_effects instead of the default.
  if (offsetof(snd_config, max_voice_effects) < oldSize) {
    std::printf("  max_voice_effects lies in the previous layout's tail padding\n");
  }
  snd_config c;
  snd_config_init(&c);
  c.output_kind = SND_OUTPUT_MANUAL;
  c.struct_size = static_cast<std::uint32_t>(oldSize);
  snd_system* sys = nullptr;
  const snd_result r = snd_create(&c, nullptr, nullptr, &sys);
  std::printf("  snd_create with the previous layout's size: %d (config.h says: the defaults)\n", r);
  if (sys != nullptr) snd_destroy(sys);
  SND_CHECK(r == SND_OK || r == SND_ERR_VERSION_MISMATCH);
}
