// Smoke test of the codec module: it links and identifies itself.
#include <cstring>

#include "snd/codec/codec_module.hpp"
#include "snd/test/test.hpp"

SND_TEST(Codec, ModuleLinks) { SND_CHECK(std::strcmp(snd::codec::ModuleName(), "codec") == 0); }
