#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include "indep_src2_util.hpp"
#include "snd/test/test.hpp"

using namespace indep2;
namespace {
constexpr double tau = 6.2831853071795864769;
PatchNode Generator(PatchOp op, float x = 0) {
  PatchNode n{op};
  n.arg[0] = x;
  return n;
}
std::vector<float> Run(const std::vector<PatchNode>& graph, unsigned frames, unsigned split,
                       unsigned seed = 11, float pitch = 1) {
  auto bytes = Program(graph);
  const auto p = Patch(bytes);
  Driver d(snd::SourceKind::Patch, {});
  Driver::Voice v;
  if (bytes.empty() || !d.Start(v, &p, 1, seed)) return {};
  synthtest::BlockMod m;
  m.pitch = {pitch, pitch};
  d.Render(v, frames, m, split);
  return v.out[0];
}
}  // namespace

SND_TEST(IndepPatch, WireFormatUnalignedIsExactAndTransactional) {
  std::vector<std::uint8_t> wire(53, 0);
  auto* raw = wire.data() + 1;
  raw[0] = 'P';
  raw[1] = 'T';
  raw[2] = 'C';
  raw[3] = 'H';
  raw[4] = 1;
  raw[6] = 2;
  raw[8] = 1;
  raw[13] = 255;
  raw[14] = 255;
  raw[19] = 0xbf;  // binary32 -0.5 in little endian
  raw[32] = 10;
  raw[34] = 255;
  SND_REQUIRE(src::ValidatePatchProgram(raw, 52));
  std::vector<PatchNode> nodes{Generator(PatchOp::Constant, -0.5f), {PatchOp::Output, 0}};
  SND_CHECK(Program(nodes) == std::vector<std::uint8_t>(raw, raw + 52));
  for (unsigned cap = 0; cap < 52; ++cap) {
    const auto before = wire;
    SND_CHECK_EQ(src::BuildPatchProgram(nodes.data(), 2, raw, cap), std::size_t{0});
    SND_CHECK(wire == before);
  }
  nodes[0].arg[3] = 1;
  const auto before = wire;
  SND_CHECK_EQ(src::BuildPatchProgram(nodes.data(), 2, raw, 52), std::size_t{0});
  SND_CHECK(wire == before);
}

SND_TEST(IndepPatch, InvalidByteSlicesAreRejectedByValidatorAndStart) {
  auto valid = Program({Generator(PatchOp::Constant, 1),
                        Generator(PatchOp::WhiteNoise),
                        {PatchOp::Add, 0, 1},
                        {PatchOp::Output, 2}});
  Driver d(snd::SourceKind::Patch, {});
  Driver::Voice v;
  SND_REQUIRE(!valid.empty());
  auto rejected = [&](const std::vector<std::uint8_t>& blob) {
    auto p = Patch(blob);
    return !src::ValidatePatchProgram(blob.data(), blob.size()) && !d.Start(v, &p, 1) &&
           v.endDetail == SND_END_DETAIL_NONE;
  };
  for (unsigned n = 0; n < valid.size(); ++n)
    SND_REQUIRE(rejected(std::vector<std::uint8_t>(valid.begin(), valid.begin() + n)));
  auto extra = valid;
  extra.push_back(0);
  SND_REQUIRE(rejected(extra));
  for (auto [offset, value] : std::vector<std::pair<unsigned, unsigned>>{{0, 0},
                                                                         {4, 2},
                                                                         {6, 17},
                                                                         {8, 1},
                                                                         {10, 1},
                                                                         {12, 10},
                                                                         {32, 211},
                                                                         {53, 2},
                                                                         {54, 3},
                                                                         {53, 99},
                                                                         {73, 0},
                                                                         {75, 1},
                                                                         {55, 1},
                                                                         {16 + 3, 0x7f},
                                                                         {16 + 4, 1}}) {
    auto copy = valid;
    copy[offset] = static_cast<std::uint8_t>(value);
    SND_CHECK_MSG(rejected(copy), "bad field at %u", offset);
  }
  // Keep headers, sizes and most records valid so cases exercise deep validation.
  std::uint32_t state = 175;
  for (unsigned i = 0; i < 4096; ++i) {
    state = state * 1664525u + 1013904223u;
    auto blob = valid;
    switch (i % 4) {
      case 0:
        blob[53] = static_cast<std::uint8_t>(2 + (state % 254));
        break;
      case 1:
        blob[32] = static_cast<std::uint8_t>(11 + (state % 245));
        break;
      case 2: {  // infinity or NaN in a used arg
        const auto bits = 0x7f800000u | (state & 0x7fffffu);
        for (unsigned j = 0; j < 4; ++j) blob[16 + j] = static_cast<std::uint8_t>(bits >> (8 * j));
        break;
      }
      default:
        blob[36 + state % 16] = static_cast<std::uint8_t>(1 + (state % 127));
        break;
    }
    SND_REQUIRE(rejected(blob));
  }
  std::vector<PatchNode> full{Generator(PatchOp::Constant, 0.5f)};
  while (full.size() < 15) {
    const auto prev = static_cast<std::uint8_t>(full.size() - 1);
    full.push_back({PatchOp::Add, prev, prev});
  }
  full.push_back({PatchOp::Output, 14});
  auto blob = Program(full);
  SND_REQUIRE_EQ(blob.size(), std::size_t{332});
  SND_CHECK(src::ValidatePatchProgram(blob.data(), blob.size()));
  auto p = Patch(blob);
  SND_REQUIRE(d.Start(v, &p, 1));
  SND_CHECK(Item(d, v, 1) == std::vector<float>{1});
}

SND_TEST(IndepPatch, OscillatorGainAndFilterComplexResponse) {
  constexpr unsigned period = 64;
  std::vector<PatchNode> nodes{Generator(PatchOp::Oscillator, 375),
                               Generator(PatchOp::Constant, 0.375f),
                               {PatchOp::Multiply, 0, 1},
                               {PatchOp::Lowpass, 2},
                               {PatchOp::Output, 3}};
  nodes[3].arg[0] = 2100;
  auto y = Run(nodes, 8192, 71, 11, 2);
  SND_REQUIRE_EQ(y.size(), std::size_t{8192});
  const auto rot = std::exp(std::complex<double>{0, -tau / period});
  const double pole = std::exp(-tau * 2100 / 48000);
  const auto h = (1 - pole) / (1.0 - pole * rot);
  std::complex<double> response = 0;
  for (unsigned i = 4096; i < 8192; ++i)
    response += double(y[i]) * std::exp(std::complex<double>{0, -tau * i / period});
  const auto want = std::complex<double>(0, -0.375) * h;
  SND_CHECK(std::abs(response / 2048.0 - want) < 1e-7);
  nodes.erase(nodes.begin() + 3);
  nodes.back().a = 2;
  const auto plain = Run(nodes, 256, 93, 11, 2);
  for (unsigned i = 0; i < plain.size(); ++i)
    SND_CHECK_NEAR(plain[i], 0.375 * std::sin(tau * i / period), 1e-7);
}

SND_TEST(IndepPatch, ArithmeticSaturationAndBipolarClip) {
  for (float sign : {-1.f, 1.f}) {
    std::vector<PatchNode> n{Generator(PatchOp::Constant, sign * 12),
                             {PatchOp::Add, 0, 0},
                             Generator(PatchOp::Constant, 0.03125f),
                             {PatchOp::Multiply, 1, 2},
                             {PatchOp::Clip, 3},
                             {PatchOp::Output, 4}};
    for (float threshold : {0.f, 0.375f, 8.f}) {
      n[4].arg[0] = threshold;
      SND_CHECK(Run(n, 13, 4) == std::vector<float>(13, sign * std::min(0.5f, threshold)));
    }
    n[2].arg[0] = 12;
    SND_CHECK(Run(n, 9, 9) == std::vector<float>(9, sign));
  }
}

SND_TEST(IndepPatch, AdsrKnotsGateAndLastRelease) {
  auto env = Generator(PatchOp::Adsr);
  env.arg[0] = 45.f / 48000;
  env.arg[1] = 35.f / 48000;
  env.arg[2] = 0.3f;
  env.arg[3] = 47.f / 48000;
  auto bytes = Program({env, {PatchOp::Output, 0}});
  Driver d(snd::SourceKind::Patch, {});
  Driver::Voice v;
  for (unsigned gate : {0u, 17u, 64u, 93u}) {
    const auto p = Patch(bytes, gate);
    SND_REQUIRE(d.Start(v, &p, 1));
    const unsigned end = gate ? (gate + 47 + 31) / 32 * 32 : 256;
    d.Render(v, end - 1, {}, 13);
    SND_CHECK_EQ(v.last.ended, 0);
    d.Render(v, 16);
    SND_CHECK_EQ(v.last.ended, gate != 0);
    SND_CHECK_EQ(v.last.framesRendered, gate ? 1u : 16u);
    const auto held = [](unsigned i) -> double {
      if (i < 45) return i / 45.0;
      if (i < 80) return 1 + (0.3 - 1) * (i - 45) / 35.0;
      return 0.3;
    };
    const auto knot = [&](unsigned t) {
      return gate && t >= gate ? held(gate) * std::max(0.0, 1 - (t - gate) / 47.0) : held(t);
    };
    for (unsigned i = 0; i < end; ++i) {
      const unsigned t = i / 32 * 32;
      const double want = knot(t) + (knot(t + 32) - knot(t)) * (i - t) / 32;
      SND_CHECK_NEAR(v.out[0][i], want, 1.3e-7);
    }
    d.Release(v);
  }
  auto slow = env;
  slow.arg[3] = 99.f / 48000;
  bytes = Program({slow, env, {PatchOp::Multiply, 0, 1}, {PatchOp::Output, 2}});
  auto p = Patch(bytes, 17);
  SND_REQUIRE(d.Start(v, &p, 1));
  d.Advance(v, 127);
  SND_CHECK_EQ(v.last.ended, 0);
  d.Advance(v, 1);
  SND_CHECK_EQ(v.last.ended, 1);
}

SND_TEST(IndepPatch, FixedKnotLfoRandomAndWhiteNoise) {
  constexpr unsigned frames = 4096;
  const std::vector<PatchNode> graph{Generator(PatchOp::Lfo, 187.5f), {PatchOp::Output, 0}};
  const auto lfo = Run(graph, frames, 29);
  SND_REQUIRE_EQ(lfo.size(), std::size_t{frames});
  for (unsigned i = 0; i < frames; ++i) {
    const auto t = i / 32 * 32;
    const double a = std::sin(tau * t / 256), b = std::sin(tau * (t + 32) / 256);
    SND_CHECK_NEAR(lfo[i], a + (b - a) * (i - t) / 32, 1.2e-7);
  }
  for (auto type : {PatchOp::RandomHold, PatchOp::WhiteNoise}) {
    auto nodes = graph;
    nodes[0] = Generator(type, type == PatchOp::RandomHold ? 187.5f : 0);
    const auto y = Run(nodes, frames, 211);
    SND_REQUIRE_EQ(y.size(), std::size_t{frames});
    SND_CHECK(y == Run(nodes, frames, 17));
    SND_CHECK(y != Run(nodes, frames, 211, 12));
    for (unsigned i = 0; i < frames - 32; ++i) {
      SND_CHECK(y[i] >= -1 && y[i] <= 1);
      if (type == PatchOp::RandomHold) {
        const auto t = i / 32 * 32;
        if (t % 256 != 224)
          SND_CHECK_EQ(y[i], y[t]);
        else
          SND_CHECK_NEAR(y[i], y[t] + (double(y[t + 32]) - y[t]) * (i - t) / 32, 1.3e-7);
      }
    }
  }
}

SND_TEST(IndepPatch, ScratchShortageSeekAndFilterVirtualResidual) {
  auto bytes = Program({Generator(PatchOp::Oscillator, 375), {PatchOp::Output, 0}});
  auto p = Patch(bytes);
  Driver d(snd::SourceKind::Patch, {});
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 1));
  bytes.clear();
  bytes.shrink_to_fit();  // source owns a copy after Start
  src::RenderItem it{};
  it.pitch = {0.5f, 2};
  SND_CHECK(Item(d, v, 7, it, 7) == std::vector<float>(7, 0));  // 2 nodes, one byte short
  SND_CHECK_EQ(v.last.starved, 1);
  it.pitch = {1, 1};
  for (auto op : {src::ControlOp::SetLoop, src::ControlOp::Pause, src::ControlOp::Resume,
                  src::ControlOp::Virtualize, src::ControlOp::Devirtualize})
    d.Control(v, op);
  auto y = Item(d, v, 67, it, 8);
  for (unsigned i = 0; i < y.size(); ++i)
    SND_CHECK_NEAR(y[i], std::sin(tau * (14 + i) / 128), 1e-7);
  d.Control(v, src::ControlOp::Seek, 13);
  SND_CHECK_NEAR(Item(d, v, 1, it)[0], std::sin(tau * 13 / 128), 1e-7);
  d.Release(v);
  auto lp = PatchNode{PatchOp::Lowpass, 0};
  lp.arg[0] = 375;
  bytes = Program({Generator(PatchOp::Constant, 0.75f), lp, {PatchOp::Output, 1}});
  p = Patch(bytes);
  Driver::Voice ref;
  SND_REQUIRE(d.Start(v, &p, 1));
  SND_REQUIRE(d.Start(ref, &p, 1));
  d.Render(v, 128);
  d.Render(ref, 192);
  d.Advance(v, 64);
  const float old = ref.out[0].back();
  const auto after = Item(d, v, 128);
  const auto continued = Item(d, ref, 128);
  for (unsigned i = 0; i < after.size(); ++i)
    SND_CHECK_NEAR(continued[i] - after[i], old * std::exp(-tau * 375 * (i + 1) / 48000), 1.3e-7);
}
