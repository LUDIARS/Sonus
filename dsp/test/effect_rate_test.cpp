// Contract 1.1 D-48: snd_effect_desc.sample_rate and the rate arguments of dsp.
#include <cstdint>

#include "snd/dsp/effects.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {
std::uint32_t gSeenRate = 0;
std::uint32_t SND_CALL PlugBytes(void*, const snd_effect_desc* d) {
  gSeenRate = d->sample_rate;
  return d->sample_rate / 100u;  // depends on the rate
}
void SND_CALL PlugInit(void*, void*, const snd_effect_desc*, std::uint32_t) {}
void SND_CALL PlugProcess(void*, void*, const float* const*, float* const*, std::uint32_t, std::uint32_t,
                          const snd_param_block*) {}
std::int32_t SND_CALL PlugTail(void*, const void*) { return 1; }
}  // namespace

SND_TEST(EffectRate, UnsetOrEqualRateIsAccepted) {
  snd::EffectDesc d{};
  d.effect_type = kEffectEcho;
  d.channels = 2;
  d.max_frames = 256;
  const std::uint32_t a = EffectStateBytes(d, 48000);
  SND_CHECK(a > 0u);
  d.sample_rate = 48000;
  SND_CHECK_EQ(EffectStateBytes(d, 48000), a);
  // A different rate in the desc than the argument is a caller error.
  d.sample_rate = 44100;
  SND_CHECK_EQ(EffectStateBytes(d, 48000), 0u);
  alignas(64) static unsigned char mem[1u << 20];
  SND_CHECK(!EffectInit(mem, sizeof(mem), d, 48000));
  d.sample_rate = 0;
  SND_CHECK(EffectInit(mem, sizeof(mem), d, 48000));
  // Rate-dependent size: a higher rate needs more delay memory.
  SND_CHECK(EffectStateBytes(d, 96000) > a);
}

SND_TEST(EffectRate, PluginsSeeTheRateInTheDesc) {
  const std::uint32_t id = snd::HashId("test.fx.rate");
  const snd_dsp_effect table{nullptr, id, 0u, &PlugBytes, &PlugInit, &PlugProcess, &PlugTail};
  snd::EffectDesc d{};
  d.effect_type = id;
  d.channels = 1;
  d.max_frames = 128;
  gSeenRate = 0;
  SND_CHECK_EQ(EffectInstanceBytes(d, 48000, &table, 1), 512u);  // 480 rounded up to 64
  SND_CHECK_EQ(gSeenRate, 48000u);
  const snd::bank_format::DspPoolNeed need = ComputeDspPoolNeed(&d, 1, 96000, &table, 1);
  SND_CHECK_EQ(gSeenRate, 96000u);
  SND_CHECK_EQ(need.instanceBytes, 960u);
  // PluginStateBytes passes the desc as is.
  d.sample_rate = 22050;
  SND_CHECK_EQ(PluginStateBytes(table, d), 256u);
  SND_CHECK_EQ(gSeenRate, 22050u);
  SND_CHECK_EQ(EffectInstanceBytes(d, 48000, &table, 1), 0u);  // desc and argument disagree
}
