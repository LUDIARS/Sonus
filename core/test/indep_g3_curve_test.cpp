#include "harness.hpp"
#include "indep_g2_fixture.hpp"

namespace {
using namespace coretest;
using snd::HashId;

std::vector<float> RenderCurves(float hz, unsigned target, unsigned compose, std::initializer_list<float> values) {
  Harness h;
  TestBank bank = BasicBank();
  bank.assets.push_back({HashId("Tone"), 1, 48000, Sine(40000, hz, 48000, 0.4f), 0, 0});
  TEvent tone = SimpleEvent("ToneEvent", 0);
  for (float value : values) {
    tone.curves.push_back(static_cast<unsigned>(bank.curves.size()));
    bank.curves.push_back({HashId("Control"), target, compose, {{0, value}, {1, value}}});
  }
  bank.events.push_back(tone);
  h.Load("curves", bank.Build(HashId("Curves")));
  h.sys->Play(HashId("ToneEvent"), {}, nullptr);
  h.Steps(12);
  h.Clear();
  h.Steps(16);
  return h.all;
}

SND_TEST(IndepG3Curve, CentsAddToAnOctaveInEitherOrder) {
  auto up = RenderCurves(750, 1, 1, {450, 750});
  auto reversed = RenderCurves(750, 1, 1, {750, 450});
  SND_REQUIRE(up.size() == 8192 && up == reversed);
  SND_CHECK(g2::ToneOf(up, 2, 0, 0, 4096, 1500) > 0.25);
  SND_CHECK(g2::ToneOf(up, 2, 0, 0, 4096, 750) < 0.001);
}

SND_TEST(IndepG3Curve, LowpassUsesMinimumAndHighpassUsesMaximum) {
  for (const bool highpass : {false, true}) {
    const unsigned target = highpass ? 3 : 2;
    const float hz = highpass ? 250.f : 8000.f;
    const float loose = highpass ? 30.f : 15000.f;
    const float strict = highpass ? 2000.f : 650.f;
    const auto reference = RenderCurves(hz, target, target, {strict});
    const auto broad = RenderCurves(hz, target, target, {loose});
    const auto both = RenderCurves(hz, target, target, {loose, strict});
    const auto swapped = RenderCurves(hz, target, target, {strict, loose});
    SND_REQUIRE(reference.size() == 8192 && broad.size() == 8192);
    SND_CHECK(reference == both && reference == swapped);
    SND_CHECK(g2::ToneOf(reference, 2, 0, 0, 4096, hz) < 0.3 * g2::ToneOf(broad, 2, 0, 0, 4096, hz));
  }
}
} // namespace
