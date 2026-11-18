// Contract 1.1 (CONTRACTS.md D-43 .. D-52), public layer: the additions are
// present, keep the 1.0 layouts as prefixes and agree between C and C++.
// The public and internal layers in one translation unit: contract11_mixed_test.cpp (D-55).
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "snd/ext/codec.hpp"
#include "snd/sonus.hpp"
#include "snd/test/test.hpp"

SND_TEST(Contract11, VersionIsOnePointOne) {
  static_assert(SND_CONTRACT_VERSION_MAJOR == 1 && SND_CONTRACT_VERSION_MINOR >= 1);
  SND_CHECK_EQ(SND_CONTRACT_VERSION >> 16, 1u);
  SND_CHECK((SND_CONTRACT_VERSION & 0xFFFFu) >= 1u);
}

// D-44: new Config fields at the END of snd_config (1.0 layout is a prefix).
SND_TEST(Contract11, ConfigFieldsAppendedAfterThe10Layout) {
  static_assert(offsetof(snd_config, max_voice_channels) >= offsetof(snd_config, plugins) + sizeof(snd_plugin_config));
  static_assert(offsetof(snd_config, max_pitch_ratio) > offsetof(snd_config, max_voice_channels));
  snd_config c;
  snd_config_init(&c);
  SND_CHECK_EQ(c.max_voice_channels, 8u);
  SND_CHECK_EQ(c.max_pitch_ratio, 4.0f);
  snd::Config cpp;
  SND_CHECK_EQ(cpp.maxVoiceChannels, 8u);
  SND_CHECK_EQ(cpp.maxPitchRatio, 4.0f);
  cpp.maxVoiceChannels = 2;
  cpp.maxPitchRatio = 8.0f;
  snd_config out;
  snd::detail::ToC(cpp, &out);
  SND_CHECK_EQ(out.max_voice_channels, 2u);
  SND_CHECK_EQ(out.max_pitch_ratio, 8.0f);
  SND_CHECK(SND_MAX_PITCH_RATIO == 32.0f);
  SND_CHECK(SND_MAX_CHANNELS >= c.max_voice_channels);
}

// A1-4 / D-48: sample_rate appended to snd_effect_desc.
SND_TEST(Contract11, EffectDescHasSampleRateAtTheEnd) {
  static_assert(offsetof(snd_effect_desc, sample_rate) == offsetof(snd_effect_desc, blob_bytes) + 8);
  static_assert(sizeof(snd_effect_desc) == offsetof(snd_effect_desc, reserved) + 4);
  static_assert(std::is_same_v<snd::EffectDesc, snd_effect_desc>);
  snd::EffectDesc d{};
  SND_CHECK_EQ(d.sample_rate, 0u);
}

// D-50: result code and language API.
SND_TEST(Contract11, BankAlreadyLoadedAndDeclareLanguage) {
  static_assert(SND_ERR_BANK_ALREADY_LOADED == -18);
  static_assert(static_cast<int>(snd::Result::BankAlreadyLoaded) == SND_ERR_BANK_ALREADY_LOADED);
  static_assert(SND_MAX_LANGUAGE_NAME_BYTES == 32u && SND_MAX_LANGUAGES == 32u);
  using DeclareFn = snd_result (*)(snd_system*, const char*);
  static_assert(std::is_same_v<decltype(&snd_declare_language), DeclareFn>);  // unevaluated: no link
}

// D-52: CCodec is default constructible and can be (re)assigned.
namespace {
std::uint32_t SND_CALL Bytes(void*, const snd_codec_header*) { return 64; }
void SND_CALL InitFn(void*, void*, const snd_codec_header*) {}
std::uint32_t SND_CALL DecodeFn(void*, void*, const std::uint8_t*, std::uint32_t, float* const*, std::uint32_t) {
  return 7;
}
void SND_CALL SeekFn(void*, void*, std::uint64_t) {}
}  // namespace

SND_TEST(Contract11, CCodecDefaultConstructibleAndAssignable) {
  static_assert(std::is_nothrow_default_constructible_v<snd::CCodec>);
  snd::CCodec c;
  SND_CHECK_EQ(c.fourcc(), 0u);
  const snd_codec t{nullptr, SND_FOURCC('T', 'E', 'S', 'T'), 0u, &Bytes, &InitFn, &DecodeFn, &SeekFn};
  c.Assign(t);
  SND_CHECK_EQ(c.fourcc(), SND_FOURCC('T', 'E', 'S', 'T'));
  SND_CHECK_EQ(c.Table().codec, t.codec);
  snd::CodecHeader h{};
  SND_CHECK_EQ(c.StateBytes(h), 64u);
  SND_CHECK_EQ(c.Decode(nullptr, nullptr, 0, nullptr, 0), 7u);
}
