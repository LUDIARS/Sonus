// Independent tests of dsp against contract 1.1:
//  - D-48 for EVERY standard effect kind: desc.sample_rate 0 or equal to the
//    rate argument works (same size), any other value fails EffectStateBytes /
//    EffectInit / EffectInstanceBytes; the size depends on the rate argument only.
//  - D-44: SND_MAX_PITCH_RATIO == kResamplerMaxRatio; the sinc state for the
//    Config defaults (8 ch, ratio 4) is far above the removed 64-byte column and
//    grows with channels and maxRatio; ratios above maxRatio are clamped (the
//    state is never overrun: ASan).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "snd/config.h"
#include "snd/dsp/effects.hpp"
#include "snd/dsp/resampler.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

SND_TEST(IndepDsp11, EveryStandardEffectFollowsTheDescRateRule) {
  alignas(64) static unsigned char mem[8u << 20];
  for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(EffectKind::Count); ++k) {
    const EffectInfo& info = GetEffectInfo(static_cast<EffectKind>(k));
    SND_CHECK(FindEffect(info.typeId) == &info);
    SND_CHECK_EQ(info.typeId, snd::HashId(info.name));
    for (std::uint32_t rate : {22050u, 44100u, 48000u, 96000u}) {
      snd::EffectDesc d{};
      d.effect_type = info.typeId;
      d.channels = 2;
      d.max_frames = 256;
      const std::uint32_t unset = EffectStateBytes(d, rate);
      SND_CHECK_MSG(unset > 0u && unset % 64u == 0u, "%s at %u: %u bytes", info.name, rate, unset);
      d.sample_rate = rate;
      SND_CHECK_EQ(EffectStateBytes(d, rate), unset);
      SND_CHECK_EQ(EffectInstanceBytes(d, rate, nullptr, 0), unset);
      SND_CHECK(unset <= sizeof(mem));
      SND_CHECK_MSG(EffectInit(mem, unset, d, rate), "%s init at %u", info.name, rate);
      for (std::uint32_t other : {rate - 1u, rate + 1u, 1u}) {
        d.sample_rate = other;
        SND_CHECK_MSG(EffectStateBytes(d, rate) == 0u, "%s: desc %u, argument %u accepted", info.name, other, rate);
        SND_CHECK_EQ(EffectInstanceBytes(d, rate, nullptr, 0), 0u);
        SND_CHECK_MSG(!EffectInit(mem, sizeof(mem), d, rate), "%s: init with desc %u, argument %u", info.name, other,
                      rate);
      }
    }
  }
}

SND_TEST(IndepDsp11, PitchRatioLimitAndResamplerPoolSlot) {
  SND_CHECK(SND_MAX_PITCH_RATIO == kResamplerMaxRatio);
  snd_config c;
  snd_config_init(&c);
  const std::size_t slot = ResamplerStateBytes(ResamplerQuality::Sinc, c.max_voice_channels, c.max_pitch_ratio);
  SND_CHECK_MSG(slot > 64u, "sinc state %zu", slot);  // why the 64-byte column had to go (A1-2)
  SND_CHECK(!ResamplerFitsInline(ResamplerQuality::Sinc, 1, 1.0f));
  for (std::uint32_t ch = 1; ch < 8; ++ch) {
    SND_CHECK(ResamplerStateBytes(ResamplerQuality::Sinc, ch, 4.0f) <= ResamplerStateBytes(ResamplerQuality::Sinc, ch + 1, 4.0f));
  }
  for (float r : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f}) {
    SND_CHECK(ResamplerStateBytes(ResamplerQuality::Sinc, 8, r) <= ResamplerStateBytes(ResamplerQuality::Sinc, 8, r * 2.0f));
  }
  // The pool slot (sized for Sinc) also holds the light qualities of the load LOD (2.4).
  SND_CHECK(ResamplerStateBytes(ResamplerQuality::Cubic, 8, 4.0f) <= slot);
  SND_CHECK(ResamplerStateBytes(ResamplerQuality::Linear, 8, 4.0f) <= slot);
  // A ratio above maxRatio is clamped: exactly-sized state and pad, no overrun.
  for (std::uint32_t ch : {1u, 8u}) {
    const float maxRatio = 4.0f;
    std::vector<unsigned char> stateMem(ResamplerStateBytes(ResamplerQuality::Sinc, ch, maxRatio) + 64);
    void* st = stateMem.data();
    while (reinterpret_cast<std::uintptr_t>(st) % 64 != 0) st = static_cast<unsigned char*>(st) + 1;
    ResamplerInit(st, ResamplerQuality::Sinc, ch, maxRatio);
    const std::uint32_t out = 256;
    const std::uint32_t pad = ResamplerPadFrames(ResamplerQuality::Sinc, maxRatio);
    const std::uint32_t need = ResamplerInputFrames(st, out, 10.0f, 10.0f);  // asks for 10, gets 4
    SND_CHECK_MSG(need <= static_cast<std::uint32_t>(std::ceil(maxRatio * out)) + 64u, "input frames %u", need);
    std::vector<std::vector<float>> in(ch, std::vector<float>(pad + need + 16));
    std::vector<std::vector<float>> o(ch, std::vector<float>(out));
    std::vector<float*> ip(ch), op(ch);
    for (std::uint32_t k = 0; k < ch; ++k) {
      for (std::size_t i = 0; i < in[k].size(); ++i) in[k][i] = std::sin(0.01f * static_cast<float>(i));
      ip[k] = in[k].data();
      op[k] = o[k].data();
    }
    const std::uint32_t used = ResamplerProcess(st, ip.data(), need, op.data(), out, 10.0f, 10.0f);
    SND_CHECK_EQ(used, need);
    const std::uint32_t at4 = ResamplerInputFrames(st, out, 4.0f, 4.0f);
    SND_CHECK_EQ(ResamplerInputFrames(st, out, 10.0f, 10.0f), at4);
  }
}
