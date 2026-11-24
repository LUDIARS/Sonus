// D-133: a voice whose plugin state does not fit plays dry and leaves the slot free.
#include <cstdint>

#include "indep_fixture.hpp"

namespace {

struct Plugin {
  std::uint32_t bytes;
  std::uint32_t inits = 0;
  std::uint32_t processes = 0;

  static std::uint32_t SND_CALL Bytes(void* user, const snd_effect_desc*) {
    return static_cast<Plugin*>(user)->bytes;
  }
  static void SND_CALL Init(void* user, void*, const snd_effect_desc*, std::uint32_t) {
    ++static_cast<Plugin*>(user)->inits;
  }
  static void SND_CALL Process(void* user, void*, const float* const* in, float* const* out,
                               std::uint32_t channels, std::uint32_t frames, const snd_param_block*) {
    ++static_cast<Plugin*>(user)->processes;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
      for (std::uint32_t f = 0; f < frames; ++f) out[ch][f] = 0.5f * in[ch][f];
    }
  }
  static std::int32_t SND_CALL TailDone(void*, const void*) { return 1; }

  snd_dsp_effect Table(snd::Id id) { return {this, id, 0, Bytes, Init, Process, TailDone}; }
};

void CheckBudget(bool includeFirstStage) {
  constexpr auto tooLargeId = snd::HashId("test.plugin.over_budget");
  constexpr auto fitsId = snd::HashId("test.plugin.fits_budget");
  constexpr auto dryEvent = snd::HashId("dry");
  constexpr auto wetEvent = snd::HashId("wet");
  Plugin tooLarge{129}, fits{128};
  const snd_dsp_effect plugins[] = {tooLarge.Table(tooLargeId), fits.Table(fitsId)};
  auto config = indep::Config();
  config.max_voice_effects = 1;
  config.voice_effect_bytes = fits.bytes;
  config.voice_effect_chain = 2;
  config.plugins.effects = plugins;
  config.plugins.effect_count = 2;
  indep::Fx fx(config);  // Serial rendering keeps all probe callbacks on the test thread.
  SND_REQUIRE_EQ(fx.created, SND_OK);
  indep::Bank bank;
  bank.Event("dry", 200.0f);
  bank.Event("wet", 400.0f);
  const auto loaded = fx.Load("plugin_budget", bank.Build("plugin_budget"));
  SND_REQUIRE_EQ(fx.sys->GetBankState(loaded), SND_BANK_STATE_LOADED);
  if (includeFirstStage) {
    SND_REQUIRE_EQ(fx.sys->SetEventVoiceEffect(dryEvent, 0, fitsId, nullptr, 0), SND_OK);
  }
  SND_REQUIRE_EQ(fx.sys->SetEventVoiceEffect(dryEvent, includeFirstStage ? 1u : 0u, tooLargeId, nullptr, 0), SND_OK);
  SND_REQUIRE_EQ(fx.sys->SetEventVoiceEffect(wetEvent, 0, fitsId, nullptr, 0), SND_OK);
  fx.Step();

  const auto rejected = fx.sys->Play(dryEvent, indep::NoEmitter(), nullptr);
  SND_REQUIRE(rejected.value != 0);
  fx.Steps(3);
  for (float sample : fx.out) SND_CHECK_NEAR(sample, 0.2f * indep::kCenter, 1e-6);
  SND_CHECK_EQ(tooLarge.inits, 0u);
  SND_CHECK_EQ(tooLarge.processes, 0u);
  SND_CHECK_EQ(fits.processes, 0u);
  SND_REQUIRE_EQ(fx.errors.size(), 1u);
  SND_CHECK_EQ(fx.errors.front().code, SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(fx.errors.front().detail, SND_END_DETAIL_VOICE_EFFECT_SLOTS);

  // Leave the dry voice playing while the second voice takes the only slot at exact fit.
  const auto accepted = fx.sys->Play(wetEvent, indep::NoEmitter(), nullptr);
  SND_REQUIRE(accepted.value != 0);
  fx.Steps(3);
  for (float sample : fx.out) SND_CHECK_NEAR(sample, (0.2f + 0.4f * 0.5f) * indep::kCenter, 1e-6);
  SND_CHECK(fits.processes > 0);
  SND_CHECK_EQ(fx.errors.size(), 1u);
}

}  // namespace

SND_TEST(IndepG2PluginBudget, OneByteOverBudgetLeavesSlotForExactFit) {
  CheckBudget(false);
}

SND_TEST(IndepG2PluginBudget, SecondStageOverBudgetLeavesEntireChainDry) {
  CheckBudget(true);
}
