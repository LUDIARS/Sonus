#include <cstdio>
#include "indep_src2_util.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace indep2;
namespace {
struct Tracking final : snd::IAllocator {
  unsigned calls = 0;
  void* Allocate(std::size_t n, std::size_t align, snd::MemTag t) override {
    ++calls;
    return snd::base::DefaultAllocator().Allocate(n, align, t);
  }
  void Free(void* ptr, snd::MemTag t) override { snd::base::DefaultAllocator().Free(ptr, t); }
};
}  // namespace

SND_TEST(IndepSrc2, RuntimeRegistrationIsIndependent) {
  for (unsigned mask = 0; mask < 4; ++mask) {
    src::RuntimeDesc desc;
    desc.allocator = &snd::base::DefaultAllocator();
    desc.enableGranular = mask & 1;
    desc.enablePatch = mask & 2;
    src::Runtime rt;
    SND_REQUIRE(rt.Init(desc));
    const auto& kinds = rt.Kinds().kinds;
    SND_CHECK_EQ(kinds[static_cast<unsigned>(snd::SourceKind::Granular)] != nullptr,
                 desc.enableGranular);
    SND_CHECK_EQ(kinds[static_cast<unsigned>(snd::SourceKind::Patch)] != nullptr, desc.enablePatch);
  }
}

SND_TEST(IndepSrc2, FullStereoBatchSteadyStateAndReturn) {
  std::vector<float> pcm(8192);
  for (unsigned i = 0; i < pcm.size(); ++i)
    pcm[i] = static_cast<float>(std::cos(double(i) * 0.033));
  auto a = Pcm(pcm, 48000, 2);
  src::GranularParams g{&a, 0, 40, 0.25f, 200, src::GrainWindow::Hann, 0.1f};
  PatchNode osc{PatchOp::Oscillator};
  osc.arg[0] = 440;
  PatchNode lp{PatchOp::Lowpass, 0};
  lp.arg[0] = 1600;
  PatchNode env{PatchOp::Adsr};
  env.arg[2] = 0.75f;
  auto bytes = Program({osc, lp, env, {PatchOp::Multiply, 1, 2}, {PatchOp::Output, 3}});
  auto p = Patch(bytes);
  for (unsigned density : {0u, 20u, 200u}) {
    const bool patch = density == 0;
    const auto kind = patch ? snd::SourceKind::Patch : snd::SourceKind::Granular;
    g.density = static_cast<float>(density);
    const void* params = patch ? static_cast<const void*>(&p) : &g;
    Tracking tracking;
    Driver::Opt o;
    o.allocator = &tracking;
    o.maxVoices = 128;
    Driver d(kind, o);
    SND_REQUIRE(d.Ok());
    SND_CHECK_EQ(d.Kind().RendersAtOutputRate(), patch);
    const auto baseline = tracking.calls;
    Driver::Voice extra;
    SND_CHECK(!d.Start(extra, nullptr, 2));
    SND_CHECK_EQ(extra.endDetail, static_cast<unsigned>(SND_END_DETAIL_NONE));
    std::vector<Driver::Voice> voices(128);
    std::vector<Driver::Voice*> batch;
    for (auto& v : voices) {
      SND_REQUIRE(d.Start(v, params, 2, static_cast<unsigned>(batch.size())));
      batch.push_back(&v);
    }
    SND_CHECK(!d.Start(extra, params, 2));
    SND_CHECK_EQ(extra.endDetail, static_cast<unsigned>(SND_END_DETAIL_VOICE_LIMIT));
    std::uint8_t ready = 0;
    d.Kind().QueryReady(&voices[0].slot, 1, &ready);
    SND_CHECK_EQ(ready, 1);
    SND_CHECK_EQ(d.Kind().PrimeFrames(a, 999), 0u);
    for (unsigned i = 0; i < 10; ++i) d.Block(batch);
    const auto t = d.renderUs;
    for (unsigned i = 0; i < 16; ++i) d.Block(batch);
    const auto mean = (d.renderUs - t) / 16;
    const auto begin = std::chrono::steady_clock::now();
    for (auto& v : voices) d.Advance(v, 48000);
    const auto advance =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
    const auto before = d.renderUs;
    d.Block(batch);
    std::printf(
        "independent 128 stereo x 256 %s %u/s: mean %.1f us, advance %.1f us, first render %.1f "
        "us\n",
        patch ? "Patch" : "Granular", density, mean, advance, d.renderUs - before);
    for (auto& v : voices) {
      SND_CHECK_EQ(v.starvedBlocks, 0u);
      SND_CHECK_EQ(v.last.framesRendered, 256u);
      d.Release(v);
    }
    SND_CHECK_EQ(tracking.calls, baseline);
    SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  }
}
