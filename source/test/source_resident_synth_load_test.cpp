#include <array>
#include <cstdio>
#include <thread>

#include "snd/source/patch_program.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using namespace synthtest;

namespace {
struct Counter final : snd::IAllocator {
  std::uint64_t allocations = 0;
  void* Allocate(std::size_t n, std::size_t align, snd::MemTag tag) override {
    ++allocations;
    return snd::base::DefaultAllocator().Allocate(n, align, tag);
  }
  void Free(void* ptr, snd::MemTag tag) override { snd::base::DefaultAllocator().Free(ptr, tag); }
};

void Measure(snd::SourceKind kind, const void* params, const char* name) {
  Counter counter;
  Driver::Opt options;
  options.maxVoices = 128;
  options.allocator = &counter;
  Driver driver(kind, options);
  SND_REQUIRE(driver.Ok());
  const auto initial = counter.allocations;
  Driver::Voice extra;
  SND_CHECK(!driver.Start(extra, nullptr, 2));
  SND_CHECK_EQ(extra.endDetail, static_cast<unsigned>(SND_END_DETAIL_NONE));
  std::vector<Driver::Voice> voices(128);
  std::vector<Driver::Voice*> batch;
  for (auto& voice : voices) {
    SND_REQUIRE(driver.Start(voice, params, 2, static_cast<unsigned>(batch.size())));
    batch.push_back(&voice);
  }
  SND_CHECK(!driver.Start(extra, params, 2));
  SND_CHECK_EQ(extra.endDetail, static_cast<unsigned>(SND_END_DETAIL_VOICE_LIMIT));
  for (unsigned i = 0; i < 4; ++i) driver.Block(batch);
  const double before = driver.renderUs;
  for (unsigned i = 0; i < 24; ++i) driver.Block(batch);
  const double mean = (driver.renderUs-before)/24;
  for (auto& v : voices) driver.Advance(v, 48000);
  const double start = driver.renderUs;
  driver.Block(batch);
  std::printf("%s 128 stereo voices, 256 frames: mean %.1f us, simultaneous return %.1f us\n",
              name, mean, driver.renderUs-start);
  SND_CHECK_EQ(counter.allocations, initial);
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
}
}  // namespace

SND_TEST(SourceResidentSynth, BoundedLoadAndReturn) {
  std::array<float, 8192> pcm{};
  for (unsigned i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<float>(std::sin(i/16.0));
  src::SourceAsset asset{};
  asset.codec.codec = SND_CODEC_PCM_F32;
  asset.codec.channels = 2;
  asset.codec.sample_rate = 48000;
  asset.codec.total_frames = pcm.size()/2;
  asset.data = reinterpret_cast<const std::uint8_t*>(pcm.data());
  asset.dataBytes = sizeof(pcm);
  src::GranularParams g{&asset, 20, 40, 0.3f, 100};
  Measure(snd::SourceKind::Granular, &g, "Granular 20/s");
  g.density = 200;
  Measure(snd::SourceKind::Granular, &g, "Granular 200/s");
  src::PatchNode n[5]{};
  n[0] = {src::PatchOp::Oscillator};
  n[0].arg[0] = 440;
  n[1] = {src::PatchOp::Lowpass, 0};
  n[1].arg[0] = 1200;
  n[2] = {src::PatchOp::Adsr};
  n[2].arg[2] = 0.5f;
  n[3] = {src::PatchOp::Multiply, 1, 2};
  n[4] = {src::PatchOp::Output, 3};
  std::uint8_t program[src::PatchProgramBytes(5)]{};
  SND_REQUIRE_EQ(src::BuildPatchProgram(n, 5, program, sizeof(program)), sizeof(program));
  src::PatchParams p{program, sizeof(program), 0};
  Measure(snd::SourceKind::Patch, &p, "Patch Osc-LP-ADSR");
}

SND_TEST(SourceResidentSynth, HeldVoiceRendersWhileOtherSlotsAreReused) {
  std::array<std::uint8_t, src::PatchProgramBytes(2)> bytes{};
  src::PatchNode n[2]{};
  n[0].arg[0] = 0.5f;
  n[1] = {src::PatchOp::Output, 0};
  SND_REQUIRE_EQ(src::BuildPatchProgram(n, 2, bytes.data(), bytes.size()), bytes.size());
  src::PatchParams patch{bytes.data(), static_cast<unsigned>(bytes.size()), 0};
  const float dc = 1;
  src::SourceAsset a{};
  a.codec.codec = SND_CODEC_PCM_F32;
  a.codec.channels = 1;
  a.codec.sample_rate = 48000;
  a.codec.total_frames = 1;
  a.data = reinterpret_cast<const std::uint8_t*>(&dc);
  a.dataBytes = sizeof(dc);
  src::GranularParams granular{&a, 48000, 0, 0, 0};
  granular.window = src::GrainWindow::Rectangle;
  for (auto kind : {snd::SourceKind::Granular, snd::SourceKind::Patch}) {
    const void* params = kind == snd::SourceKind::Granular ? static_cast<const void*>(&granular) : &patch;
    Driver d(kind, {});
    Driver::Voice held;
    SND_REQUIRE(d.Start(held, params, 1));
    std::atomic<bool> run{false};
    std::thread worker([&] {
      while (!run.load(std::memory_order_acquire)) std::this_thread::yield();
      d.Render(held, 65536);
    });
    run.store(true, std::memory_order_release);
    bool ok = true;
    for (unsigned i = 0; i < 500; ++i) {
      Driver::Voice other;
      if (!d.Start(other, params, 1)) { ok = false; break; }
      d.Control(other, src::ControlOp::Seek, i);
      d.Kind().ScheduleBackground({});
      d.Release(other);
    }
    worker.join();
    SND_CHECK(ok);
    SND_CHECK_EQ(held.out[0].size(), std::size_t{65536});
    SND_CHECK_EQ(held.out[0].back(), kind == snd::SourceKind::Granular ? 1.f : 0.5f);
  }
}
