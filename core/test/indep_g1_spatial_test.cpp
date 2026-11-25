// Properties 01..06 and 08, specified before inspecting the implementation.
#include "indep_g1_scene.hpp"

namespace {
using namespace indep_g1;

SND_TEST(IndepG1Spatial, DistanceRolloffAndSilenceBeyondMaximum) {
  auto b = Signal();
  b.events[0].maxDistance = 5;
  Scene s(b);
  s.Start();
  const double near = s.Level();
  SND_REQUIRE(near > 0.01);
  for (float distance : {0.0f, 1.0f, 2.0f, 4.0f, 6.0f}) {
    s.Position({0, 0, distance});
    s.h.Steps(8);
    const double expected = distance > 5 ? 0.0 : 1.0 / std::max(1.0f, distance);
    SND_CHECK_NEAR(s.Level() / near, expected, 0.002);
  }
}

SND_TEST(IndepG1Spatial, DistanceVolumeCurveInterpolatesDecibels) {
  auto b = Signal();
  b.events[0].curveVolume = Curve(b, bf::CurveTarget::Volume, 0, -12);
  Scene s(b);
  s.Position(kOrigin);
  s.Start();
  const double near = s.Level();
  SND_REQUIRE(near > 0.01);
  s.Position({0, 0, 2});
  s.h.Steps(8);
  SND_CHECK_NEAR(s.Level() / near, std::pow(10.0, -6.0 / 20.0), 0.003);
}

SND_TEST(IndepG1Spatial, OuterConeAttenuatesAccordingToDecibels) {
  auto b = Signal();
  b.events[0].coneInner = 40;
  b.events[0].coneOuter = 100;
  b.events[0].coneOuterDb = -12;
  Scene s(b);
  s.Position(kFront, {0, 0, -1});
  s.Start();
  const double inside = s.Level();
  SND_REQUIRE(inside > 0.01);
  s.Position(kFront, kFront);
  s.h.Steps(8);
  SND_CHECK_NEAR(s.Level() / inside, std::pow(10.0, -12.0 / 20.0), 0.003);
}

SND_TEST(IndepG1Spatial, SpreadMakesLateralEmitterCentered) {
  auto levels = [](float spread) {
    auto b = Signal();
    b.events[0].curveSpread = Curve(b, bf::CurveTarget::Spread, spread, spread);
    Scene s(b);
    s.Position({1, 0, 0});
    s.Start();
    return std::pair{s.Level(0), s.Level(1)};
  };
  const auto [left, right] = levels(0);
  SND_CHECK(right > 0.08 && left < 0.003);
  const auto [wideLeft, wideRight] = levels(1);
  SND_CHECK(wideLeft > 0.04);
  SND_CHECK_NEAR(wideLeft, wideRight, 0.001);
}

SND_TEST(IndepG1Spatial, BlendInterpolatesAttenuationLinearly) {
  auto level = [](float blend) {
    auto b = Signal();
    b.events[0].curveBlend = Curve(b, bf::CurveTarget::Blend3D, blend, blend);
    Scene s(b);
    s.Position({0, 0, 4});
    s.Start();
    return s.Level();
  };
  const double dry = level(0);
  SND_REQUIRE(dry > 0.04);
  SND_CHECK_NEAR(level(1) / dry, 0.25, 0.003);
  SND_CHECK_NEAR(level(0.5f) / dry, 0.625, 0.003);
}

SND_TEST(IndepG1Spatial, LowAndHighPassFollowDistanceInFrequencyDomain) {
  auto level = [](float hz, float distance, bool highpass) {
    auto b = Signal(false, hz);
    auto& event = b.events[0];
    event.curveVolume = Curve(b, bf::CurveTarget::Volume, 0, 0);
    if (highpass)
      event.curves = {Curve(b, bf::CurveTarget::Highpass, 20, 3000)};
    else
      event.curveLowpass = Curve(b, bf::CurveTarget::Lowpass, 20000, 600);
    Scene s(b);
    s.Position({0, 0, distance});
    s.Start();
    return s.Level();
  };
  for (bool hp : {false, true}) {
    const float cut = hp ? 150.0f : 6000.0f;
    const float pass = hp ? 6000.0f : 150.0f;
    const double near = level(cut, 0, hp);
    SND_REQUIRE(near > 0.03);
    SND_CHECK(level(cut, 4, hp) < near * 0.2);
    SND_CHECK(level(pass, 4, hp) > 0.03);
  }
}

std::vector<float> Moving(float emitterSpeed, float listenerSpeed, float factor, float blend = 1) {
  auto cfg = Config();
  cfg.doppler_factor = factor;
  auto b = Signal(false, 600, 48000);
  b.events[0].curveVolume = Curve(b, bf::CurveTarget::Volume, 0, 0);
  b.events[0].curveBlend = Curve(b, bf::CurveTarget::Blend3D, blend, blend);
  Scene s(b, cfg);
  auto listener = Listener();
  listener.velocity.z = listenerSpeed;
  SND_CHECK_EQ(s.h.sys->SetListener(0, &listener), SND_OK);
  s.Position({0, 0, 100}, kFront, {0, 0, emitterSpeed});
  s.Play();
  s.h.Steps(44);
  return s.h.all;
}

// Estimate period from positive-going zero crossings and fractional crossing time.
double Frequency(const std::vector<float>& signal) {
  double first = 0, last = 0;
  std::size_t count = 0;
  for (std::size_t i = 1025; i < signal.size() / 2; ++i) {
    const auto a = signal[2 * i - 2], b = signal[2 * i];
    if (a > 0 || b <= 0) continue;
    last = static_cast<double>(i - 1) - a / (b - a);
    if (!count) first = last;
    ++count;
  }
  return count > 1 ? 48000.0 * static_cast<double>(count - 1) / (last - first) : 0;
}

SND_TEST(IndepG1Spatial, DopplerDistinguishesEmitterAndListenerVelocity) {
  constexpr float speed = 343.0f * 0.25f;
  SND_CHECK_NEAR(Frequency(Moving(-speed, 0, 1)), 800, 3);
  SND_CHECK_NEAR(Frequency(Moving(speed, 0, 1)), 480, 3);
  SND_CHECK_NEAR(Frequency(Moving(0, speed, 1)), 750, 3);
  SND_CHECK_NEAR(Frequency(Moving(0, -speed, 1)), 450, 3);
  SND_CHECK_NEAR(Frequency(Moving(speed, speed, 1)), 600, 3);
  SND_CHECK_NEAR(Frequency(Moving(-speed, 0, 1, 0.5f)), std::sqrt(600.0 * 800), 3);
}

SND_TEST(IndepG1Spatial, ZeroVelocityAndDisabledDopplerKeepReferenceBits) {
  const auto base = Moving(0, 0, 0);
  for (const auto& wave : {Moving(0, 0, 1), Moving(-85.75f, 0, 0)}) {
    SND_REQUIRE_EQ(wave.size(), base.size());
    SND_CHECK_EQ(std::memcmp(wave.data(), base.data(), wave.size() * sizeof(float)), 0);
  }
}

SND_TEST(IndepG1Spatial, NearSonicDopplerIsFiniteAndBounded) {
  for (float speed : {-400.0f, -343.0f, -342.99f, 343.0f}) {
    const auto signal = Moving(speed, 0, 1);
    SND_CHECK(std::all_of(signal.begin(), signal.end(), [](float v) { return std::isfinite(v); }));
    const double hz = Frequency(signal);
    SND_CHECK(hz >= 600 * 0.25 - 3 && hz <= 600 * 4 + 3);
  }
}

SND_TEST(IndepG1Spatial, ListenerModesWithOneListenerAreBitIdentical) {
  std::vector<float> reference;
  for (auto mode : {SND_LISTENER_NEAREST, SND_LISTENER_WEIGHTED, SND_LISTENER_SPLIT}) {
    auto cfg = Config();
    cfg.listener_mode = mode;
    Scene s(Signal(false, 600), cfg);
    s.Position({1, 0, 1});
    s.Start();
    if (reference.empty()) reference = s.h.all;
    SND_REQUIRE_EQ(s.h.all.size(), reference.size());
    SND_CHECK_EQ(std::memcmp(s.h.all.data(), reference.data(), reference.size() * sizeof(float)), 0);
  }
}

SND_TEST(IndepG1Spatial, WeightedListenersSwapChannelsAndSplitIsSymmetric) {
  auto ears = [](snd_listener_mode mode, float first, float second) {
    auto cfg = Config();
    cfg.listener_mode = mode;
    Scene s(Signal(), cfg);
    auto l = Listener({-1, 0, 0}, first);
    auto r = Listener({1, 0, 0}, second);
    SND_CHECK_EQ(s.h.sys->SetListener(0, &l), SND_OK);
    SND_CHECK_EQ(s.h.sys->SetListener(1, &r), SND_OK);
    s.Position(kOrigin);
    s.Start();
    return std::pair{s.Level(0), s.Level(1)};
  };
  auto a = ears(SND_LISTENER_WEIGHTED, 1, 3);
  auto b = ears(SND_LISTENER_WEIGHTED, 3, 1);
  SND_CHECK(a.first > a.second && b.second > b.first);
  SND_CHECK_NEAR(a.first, b.second, 0.001);
  SND_CHECK_NEAR(a.second, b.first, 0.001);
  const auto split = ears(SND_LISTENER_SPLIT, 1, 1);
  SND_CHECK(split.first > 0.02);
  SND_CHECK_NEAR(split.first, split.second, 0.001);
  SND_CHECK(std::hypot(split.first, split.second) <= 0.101);
}

SND_TEST(IndepG1Spatial, BlendCutoffAtMidpointMatchesGeometricMean) {
  auto signal = [](float blend, float cutoff) {
    auto b = Signal(false, 6000);
    b.events[0].curveVolume = Curve(b, bf::CurveTarget::Volume, 0, 0);
    b.events[0].curveLowpass = Curve(b, bf::CurveTarget::Lowpass, cutoff, cutoff);
    b.events[0].curveBlend = Curve(b, bf::CurveTarget::Blend3D, blend, blend);
    Scene s(b);
    s.Start();
    s.h.Steps(16);
    return s.Level();
  };
  const double open = signal(0, 800);
  const double closed = signal(1, 800);
  const double middle = signal(0.5f, 800);
  SND_REQUIRE(open > 0.03);
  SND_CHECK(closed < middle && middle < open);
  SND_CHECK_NEAR(middle, signal(1, 4000), 0.0002);
}

SND_TEST(IndepG1Spatial, CollocatedListenersPreserveSingleListenerGain) {
  for (auto mode : {SND_LISTENER_NEAREST, SND_LISTENER_WEIGHTED, SND_LISTENER_SPLIT}) {
    auto cfg = Config();
    cfg.listener_mode = mode;
    Scene s(Signal(), cfg);
    s.Position({0.5f, 0, 2});
    s.Start();
    const double left = s.Level(0), right = s.Level(1);
    SND_REQUIRE(left > 0.01 && right > 0.01);
    auto listener = Listener();
    SND_CHECK_EQ(s.h.sys->SetListener(1, &listener), SND_OK);
    s.h.Steps(8);
    SND_CHECK_NEAR(s.Level(0), left, 1e-5);
    SND_CHECK_NEAR(s.Level(1), right, 1e-5);
  }
}

SND_TEST(IndepG1Spatial, DominantListenerSelectsDopplerRatio) {
  for (auto mode : {SND_LISTENER_WEIGHTED, SND_LISTENER_SPLIT}) {
    auto cfg = Config();
    cfg.listener_mode = mode;
    Scene s(Signal(false, 600, 48000), cfg);
    const snd_vec3 position{0, 0, 10};
    s.Position(position);
    auto near = Listener({0, 0, 9});
    auto far = Listener({0, 0, 1});
    near.velocity.z = 85.75f;
    far.velocity.z = -85.75f;
    SND_CHECK_EQ(s.h.sys->SetListener(0, &far), SND_OK);
    SND_CHECK_EQ(s.h.sys->SetListener(1, &near), SND_OK);
    s.Play();
    s.h.Steps(24);
    SND_CHECK_NEAR(Frequency(s.h.all), 750, 3);
    near.position.z = 0;
    far.position.z = 9;
    SND_CHECK_EQ(s.h.sys->SetListener(0, &far), SND_OK);
    SND_CHECK_EQ(s.h.sys->SetListener(1, &near), SND_OK);
    s.h.Clear();
    s.h.Steps(24);
    SND_CHECK_NEAR(Frequency(s.h.all), 450, 3);
  }
}

SND_TEST(IndepG1Spatial, LineRectangleAndBoxUseNearestPoint) {
  for (const auto kind : {SND_EMITTER_SHAPE_LINE, SND_EMITTER_SHAPE_RECTANGLE, SND_EMITTER_SHAPE_BOX}) {
    Scene s(Signal());
    const snd_vec3 ext{2, 2, 2};
    SND_CHECK_EQ(s.h.sys->SetEmitterShape(s.em, kind, &ext), SND_OK);
    s.Position({6, 0, 0});
    s.Start();
    SND_CHECK_NEAR(std::hypot(s.Level(0), s.Level(1)), 0.025, 0.001);
    s.Position({0.5f, 0, 0});
    s.h.Steps(8);
    SND_CHECK(s.Level(0) > 0.06);
    SND_CHECK_NEAR(s.Level(0), s.Level(1), 0.001);
  }
}

SND_TEST(IndepG1Spatial, RotatingLineChangesItsNearestPoint) {
  Scene s(Signal());
  const snd_vec3 ext{2, 0, 0};
  SND_CHECK_EQ(s.h.sys->SetEmitterShape(s.em, SND_EMITTER_SHAPE_LINE, &ext), SND_OK);
  s.Position({4, 0, 0});
  s.Start();
  const double along = std::hypot(s.Level(0), s.Level(1));
  s.Position({4, 0, 0}, {1, 0, 0});
  s.h.Steps(8);
  SND_CHECK_NEAR(along, 0.05, 0.001);
  SND_CHECK_NEAR(std::hypot(s.Level(0), s.Level(1)), 0.025, 0.001);
}
}  // namespace
