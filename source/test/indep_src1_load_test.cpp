// Independent tests of the stage 7-D kinds, group 1: RT safety and load (design 14), and
// the late-chunk rule (D-43) at kind level.
//   - Each kind at its instance limit (SourceLimits::maxVoices): no allocation after Init
//     (counting allocator), every call in an RT scope, refused starts beyond the limit
//     report VOICE_LIMIT; the time of a block is printed.
//   - Noise: the cost of the block in which many pink voices come back from virtual (the
//     colour filters are rebuilt from 4096 samples, D-193) against a steady block, printed.
//   - D-43 detectors for ThreadSanitizer: a late chunk's RenderBatch on another thread while
//     the audio thread starts / releases / controls / advances other voices of the same kind
//     and runs ScheduleBackground.
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "indep_src1_util.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using synthtest::Driver;
namespace src = snd::source;
namespace bf = snd::bank_format;

namespace {

struct Counting final : snd::IAllocator {
  void* Allocate(std::size_t n, std::size_t al, snd::MemTag t) override {
    ++count;
    return snd::base::DefaultAllocator().Allocate(n, al, t);
  }
  void Free(void* p, snd::MemTag t) override {
    ++frees;
    snd::base::DefaultAllocator().Free(p, t);
  }
  std::uint64_t count = 0, frees = 0;
};

constexpr std::uint32_t kLimit = 256;

struct LoadResult {
  bool ok = false;
  bool noAlloc = false;
  double usPerBlock = 0;
  std::uint32_t refusedDetail = 0;
};

// All kLimit instances of a kind (8 channels for the synths), 100 blocks in batches of 64
// voices (as chunks), virtual advances, controls, releases and restarts in between.
template <class MakeParams>
LoadResult AtLimit(snd::SourceKind kind, MakeParams make, std::uint32_t channels) {
  LoadResult res;
  Counting alloc;
  Driver::Opt o;
  o.maxVoices = kLimit;
  o.allocator = &alloc;
  o.inputPorts = kLimit;
  o.inputPortFrames = 1024;
  o.inputPortChannels = channels;
  Driver d(kind, o);
  if (!d.Ok()) return res;
  std::vector<Driver::Voice> vs(kLimit);
  std::vector<std::vector<float>> feed(channels, std::vector<float>(256, 0.125f));
  std::vector<const float*> fp;
  for (auto& f : feed) fp.push_back(f.data());
  const std::uint64_t afterInit = alloc.count;
  for (std::uint32_t i = 0; i < kLimit; ++i) {
    const auto p = make(i);
    if (!d.Start(vs[i], &p, channels, i)) return res;
  }
  for (auto& v : vs) {
    for (auto& c : v.out) c.reserve(256u * 120u);
  }
  Driver::Voice extra;
  const auto p0 = make(0u);
  // Input: the 257th start needs a port that does not exist; use a busy one instead.
  if (d.Start(extra, &p0, channels, 999)) return res;
  res.refusedDetail = extra.endDetail;
  const std::uint64_t base = alloc.count;
  Driver::Mod m;
  m.pitch = snd::Ramp{0.9f, 1.3f};
  m.duty = snd::Ramp{0.0f, 0.2f};
  m.table = snd::Ramp{0.0f, 0.5f};
  for (int b = 0; b < 100; ++b) {
    if (kind == snd::SourceKind::Input) {
      for (std::uint32_t i = 0; i < kLimit; ++i) {
        snd::rt::RtScope rt;
        d.runtime.InputPortOf(i)->Write(fp.data(), channels, 256);
      }
    }
    for (std::uint32_t c = 0; c < kLimit; c += 64) {
      std::vector<Driver::Voice*> chunk;
      for (std::uint32_t i = c; i < c + 64; ++i) chunk.push_back(&vs[i]);
      d.Block(chunk, m);
    }
    if (b % 10 == 5) {
      d.Advance(vs[b], 256, 1.1f);
      d.Control(vs[b + 1], src::ControlOp::Seek, 4096);
      d.Control(vs[b + 2], src::ControlOp::Resume, 0);
      d.Release(vs[b + 3]);
      const auto p = make(b + 3u);
      if (!d.Start(vs[b + 3], &p, channels, 7)) return res;
      vs[b + 3].out.assign(channels, {});
      for (auto& c : vs[b + 3].out) c.reserve(256u * 120u);
    }
  }
  res.noAlloc = alloc.count == base && base == afterInit;
  res.usPerBlock = d.renderUs / 100.0;
  res.ok = true;
  return res;
}

}  // namespace

SND_TEST(IndepSrc1Load, EveryKindAtItsLimitIsRtSafeAndAllocationFree) {
  const LoadResult osc = AtLimit(
      snd::SourceKind::Oscillator,
      [](std::uint32_t i) {
        src::OscillatorParams p{};
        p.waveform = i % 5u;
        p.frequencyHz = 100.0f + 37.0f * static_cast<float>(i);
        p.duty = 0.3f;
        return p;
      },
      8);
  const LoadResult noise = AtLimit(
      snd::SourceKind::Noise, [](std::uint32_t i) { return src::NoiseParams{i % 3u, i}; }, 8);
  const LoadResult input = AtLimit(
      snd::SourceKind::Input,
      [](std::uint32_t i) {
        src::InputParams p{};
        p.desc.device_index = i % kLimit;
        p.desc.channels = 2;
        return p;
      },
      2);
  for (const LoadResult* r : {&osc, &noise, &input}) {
    SND_CHECK(r->ok);
    SND_CHECK(r->noAlloc);
    SND_CHECK_EQ(r->refusedDetail, std::uint32_t{SND_END_DETAIL_VOICE_LIMIT});
  }
  std::printf("%u voices, 256 frames per block (budget 5333 us): oscillator 8 ch %.0f us, noise 8 ch %.0f us, "
              "input 2 ch %.0f us\n",
              kLimit, osc.usPerBlock, noise.usPerBlock, input.usPerBlock);
}

// D-193: pink voices returning from virtual rebuild their filters in their first block.
// Printed: the steady block and the returning block (128 stereo pink voices); the returning
// block must stay within 50x of a steady one (a budget-sized spike is reported).
SND_TEST(IndepSrc1Load, NoiseRebuildAfterVirtual) {
  Driver::Opt o;
  o.maxVoices = 128;
  Driver d(snd::SourceKind::Noise, o);
  SND_REQUIRE(d.Ok());
  std::vector<Driver::Voice> vs(128);
  std::vector<Driver::Voice*> ptrs;
  for (std::uint32_t i = 0; i < 128; ++i) {
    src::NoiseParams p{1u, i};
    SND_REQUIRE(d.Start(vs[i], &p, 2, i));
    ptrs.push_back(&vs[i]);
  }
  for (int b = 0; b < 40; ++b) d.Block(ptrs);  // past the 4096-sample warm-up window
  d.renderUs = 0;
  for (int b = 0; b < 20; ++b) d.Block(ptrs);
  const double steady = d.renderUs / 20.0;
  for (auto& v : vs) d.Advance(v, 256, 1.0f);
  d.renderUs = 0;
  d.Block(ptrs);
  const double back = d.renderUs;
  std::printf("128 stereo pink voices: steady block %.0f us, block after a virtual advance %.0f us (x%.1f)\n", steady,
              back, back / steady);
  SND_CHECK(back < 50.0 * steady + 1000.0);
}

namespace {

// A late chunk (RenderBatch of voice `late` on another thread) against the audio thread
// working on the other voices of the same kind.
template <class MakeParams>
void LateChunk(snd::SourceKind kind, MakeParams make, std::uint32_t channels) {
  Driver::Opt o;
  o.maxVoices = 8;
  o.inputPorts = 8;
  Driver d(kind, o);
  SND_REQUIRE(d.Ok());
  std::vector<Driver::Voice> vs(6);
  for (std::uint32_t i = 0; i < 6; ++i) {
    const auto p = make(i);
    SND_REQUIRE(d.Start(vs[i], &p, channels, i));
  }
  src::InputPort* port = kind == snd::SourceKind::Input ? d.runtime.InputPortOf(0) : nullptr;
  std::vector<float> feed(512, 0.5f);
  const float* fp[2] = {feed.data(), feed.data()};
  for (int round = 0; round < 50; ++round) {
    if (port != nullptr) port->Write(fp, channels, 512);
    std::thread late([&] { d.Block({&vs[0]}, Driver::Mod{}, 256); });  // the late chunk of voice 0
    // Meanwhile the audio thread: RunEventLogic on other voices, Render's virtual pass,
    // Publish (ScheduleBackground).
    d.Control(vs[1], src::ControlOp::Seek, 1000);
    d.Control(vs[2], src::ControlOp::Resume, 0);
    d.Advance(vs[3], 256, 1.0f);
    d.Release(vs[4]);
    const auto p = make(4u);
    SND_REQUIRE(d.Start(vs[4], &p, channels, 4));
    {
      snd::rt::RtScope rt;
      src::BlockInfo bi{static_cast<snd::BlockIndex>(round), 0, 256, 48000};
      d.Kind().ScheduleBackground(bi);
    }
    (void)d.runtime.Stats();
    late.join();
    d.Block({&vs[1], &vs[2], &vs[5]}, Driver::Mod{}, 256);
  }
  SND_CHECK_EQ(vs[0].out[0].size(), std::size_t{50 * 256});
  SND_CHECK(isrc::AllFinite(vs[0].out[0]));
}

}  // namespace

SND_TEST(IndepSrc1Load, LateChunkAgainstTheAudioThread) {
  LateChunk(
      snd::SourceKind::Oscillator,
      [](std::uint32_t i) {
        src::OscillatorParams p{};
        p.waveform = i % 5u;
        p.frequencyHz = 440.0f;
        p.duty = 0.4f;
        return p;
      },
      2);
  LateChunk(snd::SourceKind::Noise, [](std::uint32_t i) { return src::NoiseParams{i % 3u, i}; }, 2);
  LateChunk(
      snd::SourceKind::Input,
      [](std::uint32_t i) {
        src::InputParams p{};
        p.desc.device_index = i;
        p.desc.channels = 2;
        return p;
      },
      2);
}
