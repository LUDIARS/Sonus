#include <algorithm>
#include <cmath>
#include <limits>
#include "indep_src2_util.hpp"
#include "snd/test/test.hpp"

using namespace indep2;
namespace {
constexpr double tau = 6.283185307179586;
using Window = src::GrainWindow;
void SetLoop(Driver& d, Driver::Voice& v, unsigned first, unsigned end) {
  src::SourceControlOp op{};
  op.sourceSlot = v.slot;
  op.op = src::ControlOp::SetLoop;
  op.frame = first;
  op.frame2 = end;
  snd::rt::RtScope rt;
  d.Kind().Control(&op, 1);
}
}  // namespace

SND_TEST(IndepGranular, EveryBirthMatchesClosedCountIncludingRamps) {
  std::vector<float> data{1};
  auto a = Pcm(data, 48000);
  src::GranularParams p{&a, 937.25f, 0, 1, 0, Window::Rectangle};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 1));
  // A one-frame rectangle makes every birth observable, even when no other grains overlap.
  constexpr std::uint64_t Q = std::uint64_t{1} << 32;
  std::uint64_t total = Q - 1, observed = 0;
  for (unsigned block = 0; block < 510; ++block) {
    src::RenderItem it{};
    if (block % 3 == 1) it.grainDensityOffset = {1000, -100};
    if (block % 3 == 2) it.grainDensityOffset = {2000, NAN};
    auto y = Item(d, v, 997, it);
    for (unsigned i = 0; i < y.size(); ++i) {
      const double offset = block % 3 == 1 ? 1000 - 1100.0 * i / y.size() : 0;
      total += static_cast<std::uint64_t>(std::llround(Q * (937.25 + offset) / 48000));
      observed += y[i] == 1;
      SND_REQUIRE_EQ(observed, total / Q);
    }
  }
}

SND_TEST(IndepGranular, ComplementaryWindowsAndOldestOfEight) {
  std::vector<float> data(256, 0.75f);
  auto a = Pcm(data, 1024);
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice v;
  for (auto window : {Window::Hann, Window::Triangle, Window::Rectangle}) {
    const unsigned period = window == Window::Rectangle ? 32 : 16;
    src::GranularParams p{&a, 1024.f / period, 31.25f, 0, 0, window};
    SND_REQUIRE(d.Start(v, &p, 3));
    d.Render(v, 180);
    for (unsigned i = 0; i < 180; ++i) {
      double sum = 0;
      for (unsigned j = i % period; j < 32 && j <= i; j += period) {
        const double t = double(j) / 32;
        sum += window == Window::Rectangle ? 1
               : window == Window::Hann    ? (1 - std::cos(tau * t)) / 2
                                           : 1 - std::abs(2 * t - 1);
      }
      SND_CHECK_NEAR(v.out[2][i], 0.75 * sum, 1e-7);
    }
    d.Release(v);
  }
  for (unsigned i = 0; i < data.size(); ++i) data[i] = i / 256.f;
  src::GranularParams p{&a, 1024, 1000, 0, 0, Window::Rectangle};
  SND_REQUIRE(d.Start(v, &p, 1));
  d.Render(v, 128);
  for (unsigned i = 0; i < 128; ++i)
    SND_CHECK_EQ(v.out[0][i], std::min(i, 7u) / 512.f);  // mean age of the surviving births
}

SND_TEST(IndepGranular, ExactSizePcmEndpointsLoopsAndLatchedModulation) {
  std::vector<float> data{NAN, -2, 0.5f, 2};
  auto a = Pcm(data, 1000);
  src::GranularParams p{&a, 125, 8, 0, 0, Window::Rectangle, 0, -1200};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  SND_CHECK_EQ(v.reserved, 1000u);
  SND_CHECK(!d.Kind().RendersAtOutputRate());
  a = {};  // descriptor copied, PCM is still owned by the bank
  d.Render(v, 8);
  const std::vector<float> expected{0, -0.5f, -1, -0.25f, 0.5f, 0.75f, 1, 1};
  SND_CHECK(v.out[0] == expected && v.out[1] == expected);
  SetLoop(d, v, 1, 3);
  src::RenderItem it{};
  it.grainPositionOffset = {1, 0};
  it.grainLengthMsOffset = {-4, 4};
  // Birth at the start latches position 1, duration 4; interpolation crosses loop end.
  SND_CHECK(Item(d, v, 8, it) == std::vector<float>({0.5f, -0.25f, -1, -0.25f, 0, 0, 0, 0}));
  SetLoop(d, v, 0, 0);
  it.grainPositionOffset = {1, 1};
  it.grainLengthMsOffset = {0, 0};
  SND_CHECK(Item(d, v, 8, it) == std::vector<float>({1, 1, 0, 0, 0, 0, 0, 0}));
}

SND_TEST(IndepGranular, PitchSpectrumAndSeededJitterExtent) {
  std::vector<float> data(8192);
  auto a = Pcm(data, 8192);
  for (unsigned i = 0; i < data.size(); ++i) data[i] = static_cast<float>(std::sin(tau * i / 64));
  src::GranularParams p{&a, 1, 500, 0, 0, Window::Rectangle, 0, 1200};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 1));
  d.Render(v, 2048);
  auto power = synthtest::Power(v.out[0], 0, 2048);
  SND_CHECK_EQ(std::max_element(power.begin(), power.end()) - power.begin(), 64);
  d.Release(v);
  for (unsigned i = 0; i < data.size(); ++i) data[i] = i / 8192.f;
  p = {&a, 0, 1, 0.5f, 0, Window::Rectangle, 0.25f, 0};
  SND_REQUIRE(d.Start(v, &p, 1, 12345));
  double low = 2, high = 0, left = 1, right = 0;
  for (unsigned i = 0; i < 2048; ++i) {
    src::RenderItem birth{};
    birth.grainDensityOffset = {8192, 8192};
    birth.grainPitchJitterCentsOffset = {1200, 1200};
    const float origin = Item(d, v, 1, birth)[0];
    const float next = Item(d, v, 7)[0];
    const double stride = (next - origin) * 8192;
    // Two rounded float reads at |x|<1 give at most 2^-23 * 8192 error.
    SND_REQUIRE(stride >= 0.5 - 0.001 && stride <= 2.0 + 0.001);
    SND_REQUIRE(origin >= 0.25 * 8191 / 8192 - 1e-7 && origin <= 0.75 * 8191 / 8192 + 1e-7);
    low = std::min(low, stride);
    high = std::max(high, stride);
    left = std::min(left, double(origin));
    right = std::max(right, double(origin));
  }
  SND_CHECK(low < 0.52 && high > 1.95 && left < 0.27 && right > 0.73);
}

SND_TEST(IndepGranular, SeedSlotsSeekAndFractionalVirtualTime) {
  std::vector<float> data(257);
  for (unsigned i = 0; i < data.size(); ++i) data[i] = static_cast<float>(std::sin(tau * i / 89));
  auto a = Pcm(data, 48000);
  src::GranularParams p{&a, 9000, 80, 0.4f, 700, Window::Triangle, 0.4f};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice first, same, other;
  SND_REQUIRE(d.Start(first, &p, 1, 17));
  SND_REQUIRE(d.Start(same, &p, 1, 17));
  SND_REQUIRE(d.Start(other, &p, 1, 18));
  d.Render(first, 1509, {}, 59);
  for (unsigned i = 0; i < 16; ++i)
    d.Block(i % 2 ? std::vector<Driver::Voice*>{&other, &same}
                  : std::vector<Driver::Voice*>{&same, &other},
            {}, 61);
  SND_CHECK(std::equal(same.out[0].begin(), same.out[0].end(), first.out[0].begin()));
  SND_CHECK(same.out[0] != other.out[0]);
  d.Control(same, src::ControlOp::Seek, 3);
  for (auto op : {src::ControlOp::Pause, src::ControlOp::Resume, src::ControlOp::Virtualize})
    d.Control(same, op);
  d.Advance(same, 999, 1.5f);
  d.Advance(same, 1, 1.5f);
  SND_CHECK_EQ(same.ph.frame, std::uint64_t{1503});
  d.Control(same, src::ControlOp::Devirtualize);
  SND_CHECK(Item(d, same, 6) == std::vector<float>(first.out[0].end() - 6, first.out[0].end()));
  d.Release(same);
  SND_REQUIRE(d.Start(same, &p, 1, 17));
  SND_CHECK(Item(d, same, 111) ==
            std::vector<float>(first.out[0].begin(), first.out[0].begin() + 111));
}

SND_TEST(IndepGranular, AssetValidationPcm16AndFiniteDefaults) {
  std::vector<float> data{0.5f};
  auto a = Pcm(data, 48000);
  src::GranularParams p{&a, 48000, 0, 0, 0, Window::Rectangle};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice v;
  SND_CHECK(!d.Start(v, nullptr, 1));
  for (unsigned k = 0; k < 10; ++k) {
    auto bad = a;
    switch (k) {
      case 0:
        bad.form = src::LoadForm::Stream;
        break;
      case 1:
        bad.codec.total_frames = 0;
        break;
      case 2:
        bad.dataBytes = 3;
        break;
      case 3:
        bad.codec.channels = 0;
        break;
      case 4:
        bad.codec.channels = 9;
        break;
      case 5:
        bad.codec.sample_rate = 384001;
        break;
      case 6:
        bad.codec.codec = 0;
        break;
      case 7:
        bad.form = src::LoadForm::CompressedResident;
        break;
      case 8:
        bad.data = nullptr;
        break;
      default:
        bad.codec.total_frames = std::uint64_t{1} << 32;
        break;
    }
    p.asset = &bad;
    SND_CHECK(!d.Start(v, &p, 1));
    SND_CHECK_EQ(v.endDetail, static_cast<unsigned>(SND_END_DETAIL_NONE));
  }
  p.asset = nullptr;
  SND_CHECK(!d.Start(v, &p, 1));
  const std::uint8_t pcm16[]{0xfe, 0xff, 0x00, 0x40, 0x00, 0x80};
  a.codec.codec = SND_CODEC_PCM_S16;
  a.codec.channels = 3;
  a.data = pcm16;
  a.dataBytes = sizeof(pcm16);
  p = {&a, 48000, NAN, INFINITY, -INFINITY, Window::Rectangle, NAN, NAN};
  SND_REQUIRE(d.Start(v, &p, 4));
  d.Render(v, 4);
  for (unsigned c = 0; c < 4; ++c) {
    const float want = c == 0 ? -2.f / 32768 : c == 1 ? 0.5f : -1.f;
    SND_CHECK(v.out[c] == std::vector<float>(4, want));
  }
  for (float density : {0.f, -1.f, float(INFINITY), float(NAN)}) {
    d.Release(v);
    p.density = density;
    SND_REQUIRE(d.Start(v, &p, 1));
    SND_CHECK(Item(d, v, 12) == std::vector<float>(12, 0));
  }
}

SND_TEST(IndepGranular, SeekPublishesAnIntegerPlayhead) {
  const std::vector<float> pcm{0.5f};
  auto a = Pcm(pcm, 48000);
  src::GranularParams p{&a, 48000, 0, 0, 0, src::GrainWindow::Rectangle};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice v, fresh;
  SND_REQUIRE(d.Start(v, &p, 1));
  d.Advance(v, 1, 0.5f);
  SND_REQUIRE_EQ(v.ph.fraction, 0x80000000u);
  d.Control(v, src::ControlOp::Seek, 17);
  SND_REQUIRE(d.Start(fresh, &p, 1, 0, 17));
  SND_CHECK(Item(d, v, 1) == Item(d, fresh, 1));
  SND_CHECK_EQ(v.ph.frame, std::uint64_t{18});
  SND_CHECK_EQ(v.ph.fraction, fresh.ph.fraction);
}

SND_TEST(IndepGranular, LongVirtualDurationIsPartitionIndependent) {
  const std::vector<float> pcm{0.5f};
  auto a = Pcm(pcm, 48000);
  src::GranularParams p{&a, 48000, 0, 0, 0, src::GrainWindow::Rectangle};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice once, split;
  SND_REQUIRE(d.Start(once, &p, 1));
  SND_REQUIRE(d.Start(split, &p, 1));
  constexpr float ratio = 1.f + 0x1p-23f;
  constexpr std::uint32_t n = UINT32_MAX;
  const std::uint64_t frame = std::uint64_t{n} + (n >> 23);
  const std::uint32_t fraction = (n & 0x7fffff) << 9;
  d.Advance(once, n, ratio);
  // All the pieces have products representable in double. Their exact sum is n * ratio.
  for (unsigned i = 0; i < 1023; ++i) d.Advance(split, 1u << 22, ratio);
  d.Advance(split, (1u << 22) - 1, ratio);
  SND_REQUIRE_EQ(split.ph.frame, frame);
  SND_REQUIRE_EQ(split.ph.fraction, fraction);
  SND_CHECK_EQ(once.ph.frame, frame);
  SND_CHECK_EQ(once.ph.fraction, fraction);
}

SND_TEST(IndepGranular, AllBinary32FractionalPlacesCarryThroughAFullFrame) {
  const std::vector<float> pcm{0.5f};
  auto a = Pcm(pcm, 48000);
  src::GranularParams p{&a, 48000, 0, 0, 0, src::GrainWindow::Rectangle};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice v, other;
  SND_REQUIRE(d.Start(v, &p, 1));
  SND_REQUIRE(d.Start(other, &p, 1));
  // 2^-1 + ... + 2^-149 is exactly one least subnormal short of one. All 149
  // places matter, even when a large integer advance occurs between additions.
  for (int k = 1; k <= 149; ++k) {
    d.Advance(v, 1, std::ldexp(1.f, -k));
    d.Advance(other, 1, std::ldexp(1.f, k - 150));
  }
  d.Advance(v, UINT32_MAX, 64.f);  // clamps to 32 without losing the fraction
  d.Advance(other, UINT32_MAX, 32.f);
  const auto whole = std::uint64_t{UINT32_MAX} * 32;
  SND_REQUIRE_EQ(v.ph.frame, whole);
  SND_REQUIRE_EQ(v.ph.fraction, UINT32_MAX);
  SND_CHECK_EQ(other.ph.fraction, v.ph.fraction);
  for (auto* voice : {&v, &other}) {
    for (float invalid : {0.f, -4.f, float(NAN), float(INFINITY)}) d.Advance(*voice, 128, invalid);
    d.Advance(*voice, 1, std::numeric_limits<float>::denorm_min());
    SND_CHECK_EQ(voice->ph.frame, whole + 1);
    SND_CHECK_EQ(voice->ph.fraction, 0u);
    // Both nonempty virtual and empty render calls publish the post-seek fraction.
    d.Advance(*voice, 1, 0.25f);
    d.Control(*voice, src::ControlOp::Seek, 31);
    d.Advance(*voice, 1, 0.5f);
    SND_CHECK_EQ(voice->ph.frame, std::uint64_t{31});
    SND_CHECK_EQ(voice->ph.fraction, 0x80000000u);
    d.Control(*voice, src::ControlOp::Seek, 12);
    Item(d, *voice, 0);
    SND_CHECK_EQ(voice->ph.frame, std::uint64_t{12});
    SND_CHECK_EQ(voice->ph.fraction, 0u);
  }
}
