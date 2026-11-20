// Stage 7-D kinds at their voice limit (design 14 "RT安全性", "負荷"): every call on
// the audio side inside SND_RT_SCOPE, no allocation after Init (counting allocator),
// and the time of one block (256 frames at 48 kHz = 5.33 ms) for 128 voices per kind,
// printed for the report.
#include <cstdio>
#include <vector>

#include "snd/format/bank_format.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using namespace synthtest;
namespace bf = snd::bank_format;

namespace {

struct Counting final : snd::IAllocator {
  void* Allocate(std::size_t n, std::size_t al, snd::MemTag t) override {
    ++count;
    return snd::base::DefaultAllocator().Allocate(n, al, t);
  }
  void Free(void* p, snd::MemTag t) override { snd::base::DefaultAllocator().Free(p, t); }
  std::uint64_t count = 0;
};

constexpr std::uint32_t kVoices = 128;

// Starts kVoices voices with `params(i)`, renders `blocks` blocks of all of them in one
// batch each (with virtual advances and controls in between), and returns the mean
// microseconds per block.
template <class Params>
double Load(snd::SourceKind kind, Params params, std::uint32_t channels, bool* noAlloc, int blocks = 200) {
  Counting alloc;
  Driver::Opt o;
  o.maxVoices = kVoices;
  o.allocator = &alloc;
  o.inputPorts = kVoices;
  o.inputPortFrames = 1024;
  o.inputPortChannels = channels;
  Driver d(kind, o);
  if (!d.Ok()) return -1.0;
  std::vector<Driver::Voice> vs(kVoices);
  std::vector<Driver::Voice*> ptrs;
  const std::uint64_t afterInit = alloc.count;
  std::vector<std::vector<float>> feed(channels, std::vector<float>(256, 0.1f));
  std::vector<const float*> feedPtr;
  for (auto& f : feed) feedPtr.push_back(f.data());
  for (std::uint32_t i = 0; i < kVoices; ++i) {
    const auto p = params(i);
    if (!d.Start(vs[i], &p, channels, i * 7u + 1u)) return -1.0;  // in an RT scope (driver)
    ptrs.push_back(&vs[i]);
  }
  // Warm the output vectors (the driver's own bookkeeping, not the kind's).
  for (auto* v : ptrs) {
    for (auto& c : v->out) c.reserve(std::size_t{256} * (blocks + 8));
  }
  Driver::Mod m;
  m.pitch = snd::Ramp{1.0f, 1.25f};
  m.duty = snd::Ramp{0.0f, 0.1f};
  for (int b = 0; b < blocks; ++b) {
    if (kind == snd::SourceKind::Input) {
      for (std::uint32_t i = 0; i < kVoices; ++i) d.runtime.InputPortOf(i)->Write(feedPtr.data(), channels, 256);
    }
    d.Block(ptrs, m);  // RenderBatch in an RT scope, timed (Driver::renderUs)
    if (b % 50 == 10) {
      d.Advance(vs[3], 256, 1.0f);
      d.Control(vs[5], snd::source::ControlOp::Seek, 1000);
      d.Control(vs[6], snd::source::ControlOp::Resume, 0);
    }
  }
  *noAlloc = alloc.count == afterInit;
  return d.renderUs / blocks;
}

}  // namespace

SND_TEST(SourceSynthLoad, VoiceLimitIsRtSafeAndAllocationFree) {
  bool ok = false;
  const double osc = Load(
      snd::SourceKind::Oscillator,
      [](std::uint32_t i) {
        src::OscillatorParams p{};
        p.waveform = i % 5u;
        p.frequencyHz = 100.0f + 37.0f * static_cast<float>(i);
        p.duty = 0.3f;
        return p;
      },
      2, &ok);
  SND_CHECK(osc >= 0.0);
  SND_CHECK_MSG(ok, "oscillator allocated after Init");
  const double noise = Load(
      snd::SourceKind::Noise, [](std::uint32_t i) { return src::NoiseParams{i % 3u, i}; }, 2, &ok);
  SND_CHECK(noise >= 0.0);
  SND_CHECK_MSG(ok, "noise allocated after Init");
  const double input = Load(
      snd::SourceKind::Input,
      [](std::uint32_t i) {
        src::InputParams p{};
        p.desc.device_index = i;
        p.desc.channels = 2;
        return p;
      },
      2, &ok);
  SND_CHECK(input >= 0.0);
  SND_CHECK_MSG(ok, "input allocated after Init");
  std::printf("block of 256 frames, %u stereo voices in one batch: oscillator %.1f us, noise %.1f us, input %.1f us "
              "(budget 5333 us)\n",
              kVoices, osc, noise, input);
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
}
