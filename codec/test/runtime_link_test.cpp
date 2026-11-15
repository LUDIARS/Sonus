// Uses only the runtime API (decoders, registry, decoder pool). The CTest
// codec_runtime_has_no_encoder checks with nm that this executable does not
// contain the build-time encoder (snd/codec/encode.hpp).
#include <cstdint>

#include "snd/codec/codec.hpp"
#include "snd/codec/decoder_pool.hpp"
#include "snd/codec/registry.hpp"
#include "snd/test/test.hpp"

SND_TEST(CodecRuntime, DecodesHandMadePcm16) {
  snd::codec::CodecRegistry reg;
  snd::ICodec* c = reg.Find(SND_CODEC_PCM_S16);
  SND_REQUIRE(c != nullptr);
  snd::CodecHeader h{SND_CODEC_PCM_S16, 1, 48000, 1, 3, 0, 0, 2, 0, nullptr};
  const std::uint8_t data[] = {0x00, 0x40, 0x00, 0xC0, 0xFF, 0x7F};  // 0.5, -0.5, 32767/32768
  alignas(64) static std::uint8_t poolMem[1 << 16];
  snd::codec::DecoderPool pool;
  SND_REQUIRE(pool.Init(poolMem, sizeof(poolMem), 1, c->StateBytes(h)));
  snd::codec::DecoderSlot slot;
  SND_REQUIRE(pool.Acquire(c->StateBytes(h), &slot) == snd::codec::DecoderPoolStatus::Ok);
  c->Init(slot.state, h);
  float out[4] = {};
  float* dst[1] = {out};
  SND_CHECK_EQ(c->Decode(slot.state, data, 5, dst, 4), 2u);
  SND_CHECK_EQ(c->Decode(slot.state, data + 5, 1, dst + 0, 4), 1u);
  SND_CHECK_EQ(out[0], 32767.0f / 32768.0f);
  SND_CHECK(pool.Release(slot.index));
}
