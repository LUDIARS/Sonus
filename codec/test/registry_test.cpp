// Codec registry tests (D-31) and the Opus placeholder (A0-3).
#include <cstdint>

#include "snd/codec/codec.hpp"
#include "snd/codec/registry.hpp"
#include "snd/ext/codec.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using snd::codec::CodecRegistry;
using snd::codec::LookupFailure;
using snd::codec::RegistryStatus;

namespace {

class FakeCodec final : public snd::ICodec {
 public:
  std::uint32_t StateBytes(const snd::CodecHeader&) const override { return 77; }
  void Init(void*, const snd::CodecHeader&) override { ++inits; }
  std::uint32_t Decode(void*, const std::uint8_t*, std::uint32_t, float* const*, std::uint32_t) override {
    return 5;
  }
  void Seek(void*, std::uint64_t) override {}
  int inits = 0;
};

int gCalls = 0;
std::uint32_t SND_CALL CStateBytes(void* user, const snd_codec_header*) {
  ++gCalls;
  return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(user));
}
void SND_CALL CInit(void*, void*, const snd_codec_header*) { ++gCalls; }
std::uint32_t SND_CALL CDecode(void*, void*, const std::uint8_t*, std::uint32_t, float* const*, std::uint32_t) {
  ++gCalls;
  return 9;
}
void SND_CALL CSeek(void*, void*, std::uint64_t) { ++gCalls; }

snd_codec CTable(std::uint32_t fourcc, std::uintptr_t user) {
  return snd_codec{reinterpret_cast<void*>(user), fourcc, 0u, &CStateBytes, &CInit, &CDecode, &CSeek};
}

}  // namespace

SND_TEST(Registry, BuiltinsAndOpus) {
  CodecRegistry reg;
  LookupFailure why = LookupFailure::NotRegistered;
  SND_CHECK(reg.Find(SND_CODEC_PCM_F32, &why) == snd::codec::BuiltinCodec(SND_CODEC_PCM_F32));
  SND_CHECK(why == LookupFailure::None);
  SND_CHECK(reg.Find(SND_CODEC_PCM_S16) != nullptr);
  SND_CHECK(reg.Find(SND_CODEC_ADPCM) != nullptr);
  SND_CHECK(reg.Find(SND_CODEC_OPUS, &why) == nullptr);
  SND_CHECK(why == LookupFailure::OpusUnavailable);
  SND_CHECK(reg.Find(snd::Fourcc('N', 'O', 'P', 'E'), &why) == nullptr);
  SND_CHECK(why == LookupFailure::NotRegistered);
  SND_CHECK(snd::codec::LookupFailureName(LookupFailure::OpusUnavailable)[0] != '?');
  SND_CHECK(snd::codec::ValidateHeader(snd::CodecHeader{SND_CODEC_OPUS, 2, 48000, 960, 1000, 312, 3840, 0, 0, nullptr}) ==
            snd::codec::HeaderError::CodecUnavailable);
  SND_CHECK(CodecRegistry::FindForSource(&reg, SND_CODEC_ADPCM) == snd::codec::BuiltinCodec(SND_CODEC_ADPCM));
  SND_CHECK(CodecRegistry::FindForSource(nullptr, SND_CODEC_ADPCM) == snd::codec::BuiltinCodec(SND_CODEC_ADPCM));
}

SND_TEST(Registry, UserCodecOverridesBuiltinAndAddsOpus) {
  FakeCodec pcm, opus;
  const snd_codec tables[] = {snd::ToC(pcm, SND_CODEC_PCM_S16), snd::ToC(opus, SND_CODEC_OPUS),
                              CTable(snd::Fourcc('C', 'U', 'S', 'T'), 1234)};
  CodecRegistry reg;
  SND_REQUIRE(reg.Init(tables, 3) == RegistryStatus::Ok);
  SND_CHECK_EQ(reg.UserCodecCount(), 3u);
  // C++ plugins passed through snd::ToC are unwrapped (no double indirection).
  SND_CHECK(reg.Find(SND_CODEC_PCM_S16) == &pcm);
  SND_CHECK(reg.Find(SND_CODEC_OPUS) == &opus);
  SND_CHECK(reg.IsUserCodec(SND_CODEC_PCM_S16));
  SND_CHECK(!reg.IsUserCodec(SND_CODEC_PCM_F32));
  SND_CHECK(reg.Find(SND_CODEC_PCM_F32) == snd::codec::BuiltinCodec(SND_CODEC_PCM_F32));
  // A plain C table goes through an adapter.
  snd::ICodec* c = reg.Find(snd::Fourcc('C', 'U', 'S', 'T'));
  SND_REQUIRE(c != nullptr);
  gCalls = 0;
  snd::CodecHeader h{};
  SND_CHECK_EQ(c->StateBytes(h), 1234u);
  c->Init(nullptr, h);
  SND_CHECK_EQ(c->Decode(nullptr, nullptr, 0, nullptr, 0), 9u);
  c->Seek(nullptr, 0);
  SND_CHECK_EQ(gCalls, 4);
  // Lookups are RT-safe.
  {
    SND_RT_SCOPE();
    SND_CHECK(reg.Find(SND_CODEC_PCM_S16) == &pcm);
    SND_CHECK(CodecRegistry::FindForSource(&reg, SND_CODEC_ADPCM) != nullptr);
    LookupFailure why;
    SND_CHECK(reg.Find(snd::Fourcc('A', 'B', 'C', 'D'), &why) == nullptr);
  }
}

SND_TEST(Registry, RejectsBadConfigurations) {
  CodecRegistry reg;
  snd_codec dup[] = {CTable(snd::Fourcc('A', 'A', 'A', 'A'), 1), CTable(snd::Fourcc('A', 'A', 'A', 'A'), 2)};
  SND_CHECK(reg.Init(dup, 2) == RegistryStatus::DuplicateFourcc);
  SND_CHECK_EQ(reg.UserCodecCount(), 0u);
  snd_codec zero[] = {CTable(0u, 1)};
  SND_CHECK(reg.Init(zero, 1) == RegistryStatus::InvalidEntry);
  snd_codec nullfn[] = {CTable(snd::Fourcc('B', 'B', 'B', 'B'), 1)};
  nullfn[0].decode = nullptr;
  SND_CHECK(reg.Init(nullfn, 1) == RegistryStatus::InvalidEntry);
  SND_CHECK(reg.Init(nullptr, 1) == RegistryStatus::InvalidEntry);
  snd_codec many[CodecRegistry::kMaxUserCodecs + 1];
  for (std::uint32_t i = 0; i <= CodecRegistry::kMaxUserCodecs; ++i) many[i] = CTable(1000u + i, 1);
  SND_CHECK(reg.Init(many, CodecRegistry::kMaxUserCodecs + 1) == RegistryStatus::TooManyCodecs);
  SND_CHECK(reg.Init(many, CodecRegistry::kMaxUserCodecs) == RegistryStatus::Ok);
  SND_CHECK(reg.Find(1000u + CodecRegistry::kMaxUserCodecs - 1) != nullptr);
  // After a failed Init only the built-ins remain.
  SND_CHECK(reg.Init(dup, 2) == RegistryStatus::DuplicateFourcc);
  SND_CHECK(reg.Find(1000u) == nullptr);
  SND_CHECK(reg.Find(SND_CODEC_ADPCM) != nullptr);
  SND_CHECK(reg.Init(nullptr, 0) == RegistryStatus::Ok);
}
