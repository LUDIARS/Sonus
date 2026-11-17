// DriverCore (design.md 2.2, 2.4, 2.6, CONTRACTS.md D-66): deterministic tests
// with the device callback and the audio thread called directly.
//
// The render function writes a frame counter (frame n, channel c ->
// (n + 1) * (c + 1), exact in float up to 2^24 / 8), so every output can be
// checked exactly: no gaps, no repeats, silence only where it is counted.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/driver_core.hpp"
#include "snd/platform/load_monitor.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {

struct CounterRenderer {
  std::uint64_t nextFrame = 0;  // frames rendered so far (per format epoch the counter continues)
  std::uint64_t lostSeen = 0;   // sum of RenderInfo::lostFrames
  std::uint64_t blocks = 0;
  std::uint32_t formatCalls = 0;
  std::uint32_t lastRate = 0;
  std::uint32_t lastChannels = 0;
  std::uint32_t maxBlocksPerService = 0;

  static void Render(void* user, float* out, std::uint32_t frames, std::uint32_t channels, const RenderInfo* info) {
    auto* self = static_cast<CounterRenderer*>(user);
    self->lostSeen += info->lostFrames;
    SND_CHECK_EQ(info->blockIndex, self->blocks);
    ++self->blocks;
    for (std::uint32_t f = 0; f < frames; ++f) {
      const float v = static_cast<float>(self->nextFrame + f + 1u);
      for (std::uint32_t c = 0; c < channels; ++c) out[f * channels + c] = v * static_cast<float>(c + 1u);
    }
    self->nextFrame += frames;
  }
  static void Format(void* user, std::uint32_t rate, std::uint32_t channels) {
    auto* self = static_cast<CounterRenderer*>(user);
    ++self->formatCalls;
    self->lastRate = rate;
    self->lastChannels = channels;
  }
};

DriverCoreDesc MakeDesc(CounterRenderer& r, std::uint32_t block, std::uint32_t ahead, std::uint32_t fade = 0) {
  DriverCoreDesc d;
  d.blockFrames = block;
  d.renderAheadBlocks = ahead;
  d.catchupBlocks = 2;
  d.fadeFrames = fade;
  d.render = &CounterRenderer::Render;
  d.formatChanged = &CounterRenderer::Format;
  d.user = &r;
  return d;
}

// Checks that `out` (frames x channels) continues the counter sequence:
// each frame is either the next counter value or silence. Returns silent frames.
struct SequenceChecker {
  std::uint64_t expect = 1;  // next counter value
  std::uint64_t silent = 0;
  std::uint64_t valid = 0;
  bool ok = true;
  void Feed(const float* out, std::uint32_t frames, std::uint32_t channels) {
    for (std::uint32_t f = 0; f < frames; ++f) {
      const float v0 = out[f * channels];
      if (v0 == 0.0f) {
        for (std::uint32_t c = 0; c < channels; ++c) ok = ok && out[f * channels + c] == 0.0f;
        ++silent;
        continue;
      }
      for (std::uint32_t c = 0; c < channels; ++c) {
        ok = ok && out[f * channels + c] == static_cast<float>(expect) * static_cast<float>(c + 1u);
      }
      ++expect;
      ++valid;
    }
  }
};

}  // namespace

SND_TEST(DriverCore, InitRejectsBadValuesAndReleasesMemory) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  CounterRenderer r;
  {
    DriverCore core;
    DriverCoreDesc d = MakeDesc(r, 256, 2);
    SND_CHECK(!core.Init(acc, d, 48000, 0, 256));
    SND_CHECK(!core.Init(acc, d, 48000, SND_MAX_CHANNELS + 1u, 256));
    SND_CHECK(!core.Init(acc, d, 0, 2, 256));
    DriverCoreDesc noRender = d;
    noRender.render = nullptr;
    SND_CHECK(!core.Init(acc, noRender, 48000, 2, 256));
    DriverCoreDesc zeroBlock = d;
    zeroBlock.blockFrames = 0;
    SND_CHECK(!core.Init(acc, zeroBlock, 48000, 2, 256));
    SND_CHECK_EQ(acc.TotalUsed(), 0u);
    SND_REQUIRE(core.Init(acc, d, 48000, 2, 256));
    SND_CHECK(core.Initialized());
    SND_CHECK(acc.TotalUsed() > 0u);
    // Ring holds at least two max periods plus the render-ahead in every channel count.
    SND_CHECK(core.CapacityFrames(SND_MAX_CHANNELS) >= 2u * (4096u + 5u * 256u));
  }
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

// Every combination of block, device period and render-ahead: with the
// callback and Service alternating, there is never an underrun, the output
// is exactly the rendered sequence, the fill after Service is in
// [target, target + block) and Service never exceeds the per-wake limit.
SND_TEST(DriverCore, BlockAndPeriodCombinationsAreSeamless) {
  const std::uint32_t blocks[] = {64, 128, 256, 480};
  const std::uint32_t periods[] = {1, 63, 64, 100, 256, 441, 512, 1024};
  const std::uint32_t aheads[] = {0, 1, 2, 4};
  std::vector<float> out;
  for (std::uint32_t block : blocks) {
    for (std::uint32_t period : periods) {
      for (std::uint32_t ahead : aheads) {
        CounterRenderer r;
        DriverCore core;
        SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), MakeDesc(r, block, ahead), 48000, 2, period));
        const std::uint32_t target = period + ahead * block;
        SND_CHECK_EQ(core.TargetFrames(), target);
        core.Service(true);
        SND_CHECK_EQ(r.formatCalls, 1u);
        SND_CHECK_EQ(r.lastRate, 48000u);
        SND_CHECK_EQ(r.lastChannels, 2u);
        DriverStats s = core.Stats();
        SND_CHECK(s.queuedFrames >= target && s.queuedFrames < target + block);
        SequenceChecker check;
        out.assign(static_cast<std::size_t>(period) * 2u, -1.0f);
        const std::uint32_t perWake = (period + block - 1u) / block + 2u;
        const int rounds = static_cast<int>(20000u / period) + 8;
        for (int i = 0; i < rounds; ++i) {
          core.OnDeviceCallback(out.data(), period, 2);
          check.Feed(out.data(), period, 2);
          const std::uint32_t n = core.Service(false);
          SND_CHECK(n <= perWake);
          s = core.Stats();
          SND_CHECK(s.queuedFrames >= target && s.queuedFrames < target + block);
        }
        s = core.Stats();
        SND_CHECK_MSG(check.ok, "block %u period %u ahead %u", block, period, ahead);
        SND_CHECK_EQ(check.silent, 0u);
        SND_CHECK_EQ(s.underruns, 0u);
        SND_CHECK_EQ(s.lostFrames, 0u);
        SND_CHECK_EQ(check.valid, static_cast<std::uint64_t>(rounds) * period);
        SND_CHECK_EQ(s.framesConsumed, static_cast<std::uint64_t>(rounds) * period);
        // Everything rendered is either consumed or still queued.
        SND_CHECK_EQ(r.nextFrame, s.framesConsumed + s.queuedFrames);
        SND_CHECK_EQ(r.lostSeen, 0u);
      }
    }
  }
}

// Callbacks of varying length (WASAPI padding, Core Audio slices): once the
// largest length has been seen, the target covers it and nothing underruns.
SND_TEST(DriverCore, VariableCallbackLengths) {
  CounterRenderer r;
  DriverCore core;
  const std::uint32_t maxFrames = 700;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), MakeDesc(r, 256, 1), 48000, 2, maxFrames));
  core.Service(true);
  std::vector<float> out(maxFrames * 2u);
  SequenceChecker check;
  std::uint32_t lcg = 12345u;
  std::uint64_t total = 0;
  for (int i = 0; i < 5000; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    const std::uint32_t frames = 1u + (lcg >> 8) % maxFrames;
    core.OnDeviceCallback(out.data(), frames, 2);
    check.Feed(out.data(), frames, 2);
    total += frames;
    core.Service(false);
  }
  const DriverStats s = core.Stats();
  SND_CHECK(check.ok);
  SND_CHECK_EQ(s.underruns, 0u);
  SND_CHECK_EQ(check.valid, total);
  SND_CHECK_EQ(s.callbacks, 5000u);
}

// Underrun: the callback outputs what is there, the rest is silence, it is
// counted, and the next block reports exactly the lost frames (2.2: the clock
// advances by them). Then the catch-up is limited per wake.
SND_TEST(DriverCore, UnderrunIsCountedReportedAndCaughtUp) {
  CounterRenderer r;
  DriverCore core;
  const std::uint32_t period = 256, block = 128;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), MakeDesc(r, block, 1), 48000, 2, period));
  core.Service(true);
  const std::uint32_t queued = core.Stats().queuedFrames;  // 384 = period + 1 block
  SND_CHECK_EQ(queued, 384u);
  std::vector<float> out(period * 2u);
  SequenceChecker check;
  // Three callbacks without rendering: 256 + 128 available, then nothing.
  core.OnDeviceCallback(out.data(), period, 2);
  check.Feed(out.data(), period, 2);
  core.OnDeviceCallback(out.data(), period, 2);  // 128 valid + 128 silent
  check.Feed(out.data(), period, 2);
  core.OnDeviceCallback(out.data(), period, 2);  // all silent
  check.Feed(out.data(), period, 2);
  DriverStats s = core.Stats();
  SND_CHECK(check.ok);
  SND_CHECK_EQ(check.valid, 384u);
  SND_CHECK_EQ(check.silent, 384u);
  SND_CHECK_EQ(s.underruns, 2u);
  SND_CHECK_EQ(s.lostFrames, 384u);
  // Catch-up: at most ceil(256/128) + 2 = 4 blocks per wake, then the target.
  const std::uint32_t n = core.Service(false);
  SND_CHECK_EQ(n, 3u);  // target 384 = 3 blocks from empty, below the limit of 4
  SND_CHECK_EQ(r.lostSeen, 384u);  // reported once, in the first block after the gap
  SND_CHECK_EQ(core.Stats().queuedFrames, 384u);
  // A much longer gap: the limit applies.
  for (int i = 0; i < 4; ++i) core.OnDeviceCallback(out.data(), period, 2);
  CounterRenderer& rr = r;
  const std::uint64_t lostBefore = rr.lostSeen;
  const std::uint32_t n2 = core.Service(false);
  SND_CHECK_EQ(n2, 3u);
  SND_CHECK_EQ(rr.lostSeen - lostBefore, 4u * 256u - 384u);
}

SND_TEST(DriverCore, UnderrunFadesOutAndFadesIn) {
  CounterRenderer r;
  DriverCore core;
  const std::uint32_t fade = 16;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), MakeDesc(r, 64, 0, fade), 48000, 1, 64));
  core.Service(true);  // 64 frames queued (target = period)
  std::vector<float> out(128);
  core.OnDeviceCallback(out.data(), 100, 1);  // 64 valid, 36 silent
  // Frames 0..47 untouched, 48..63 faded linearly to 0 at the last valid frame.
  for (std::uint32_t i = 0; i < 48; ++i) SND_CHECK_EQ(out[i], static_cast<float>(i + 1u));
  for (std::uint32_t i = 0; i < fade; ++i) {
    const float g = static_cast<float>(fade - 1u - i) / static_cast<float>(fade);
    SND_CHECK_EQ(out[48 + i], static_cast<float>(49u + i) * g);
  }
  SND_CHECK_EQ(out[63], 0.0f);
  for (std::uint32_t i = 64; i < 100; ++i) SND_CHECK_EQ(out[i], 0.0f);
  core.Service(false);
  core.OnDeviceCallback(out.data(), 32, 1);  // next data fades in over 16 frames
  for (std::uint32_t i = 0; i < fade; ++i) {
    const float g = static_cast<float>(i + 1u) / static_cast<float>(fade + 1u);
    SND_CHECK_EQ(out[i], static_cast<float>(65u + i) * g);
  }
  for (std::uint32_t i = fade; i < 32; ++i) SND_CHECK_EQ(out[i], static_cast<float>(65u + i));
}

// Route change to 6 channels (2.6): the callback with the new count outputs
// silence and asks; Service switches (FormatFn), the reader drops the old
// stereo data and the 6-channel stream starts where the switch was made.
SND_TEST(DriverCore, ChannelChangeHandshake) {
  CounterRenderer r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), MakeDesc(r, 128, 1), 48000, 2, 256));
  core.Service(true);
  std::vector<float> out(256 * 8);
  core.OnDeviceCallback(out.data(), 256, 2);
  core.Service(false);
  const std::uint64_t renderedStereo = r.nextFrame;
  core.OnDeviceCallback(out.data(), 256, 6);  // new format: silence, request
  for (std::uint32_t i = 0; i < 256u * 6u; ++i) SND_CHECK_EQ(out[i], 0.0f);
  SND_CHECK_EQ(r.formatCalls, 1u);
  core.Service(false);
  SND_CHECK_EQ(r.formatCalls, 2u);
  SND_CHECK_EQ(r.lastChannels, 6u);
  SND_CHECK_EQ(r.lastRate, 48000u);
  SND_CHECK_EQ(core.Stats().channels, 6u);
  SND_CHECK_EQ(core.Stats().formatChanges, 1u);
  // The 6-channel data starts at the counter value after the stereo data.
  core.OnDeviceCallback(out.data(), 256, 6);
  // First frames fade in (the switch is a discontinuity); check the tail exactly.
  for (std::uint32_t f = 64; f < 256; ++f) {
    for (std::uint32_t c = 0; c < 6; ++c) {
      SND_CHECK_EQ(out[f * 6 + c], static_cast<float>(renderedStereo + f + 1u) * static_cast<float>(c + 1u));
    }
  }
  const DriverStats s = core.Stats();
  SND_CHECK_EQ(s.lostFrames, 256u);
  // Back to stereo.
  core.Service(false);
  core.OnDeviceCallback(out.data(), 256, 2);
  core.Service(false);
  SND_CHECK_EQ(r.lastChannels, 2u);
  // A count the ring cannot hold is never requested: silence, counted.
  core.OnDeviceCallback(out.data(), 16, SND_MAX_CHANNELS + 1u);
  core.Service(false);
  SND_CHECK_EQ(r.lastChannels, 2u);
}

// Rate change (CHANGED event): FormatFn gets the new rate, no audio is dropped.
SND_TEST(DriverCore, RateAndPeriodChangeFromEvent) {
  CounterRenderer r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), MakeDesc(r, 128, 1, 0), 48000, 2, 256));
  core.Service(true);
  std::vector<float> out(2048 * 2);
  SequenceChecker check;
  core.OnDeviceCallback(out.data(), 256, 2);
  check.Feed(out.data(), 256, 2);
  core.OnDeviceChanged(44100, 1024);
  SND_CHECK_EQ(core.TargetFrames(), 1024u + 128u);
  core.Service(false);
  SND_CHECK_EQ(r.lastRate, 44100u);
  SND_CHECK_EQ(r.formatCalls, 2u);
  SND_CHECK_EQ(core.Stats().sampleRate, 44100u);
  for (int i = 0; i < 8; ++i) {
    core.Service(false);
    core.OnDeviceCallback(out.data(), 1024, 2);
    check.Feed(out.data(), 1024, 2);
  }
  SND_CHECK(check.ok);
  SND_CHECK_EQ(check.silent, 0u);
  // An event without values changes nothing.
  core.OnDeviceChanged(0, 0);
  core.Service(false);
  SND_CHECK_EQ(r.formatCalls, 2u);
}

namespace {
struct SlowRenderer {
  std::uint64_t spinNs = 0;
  static void Render(void* user, float* out, std::uint32_t frames, std::uint32_t channels, const RenderInfo*) {
    auto* self = static_cast<SlowRenderer*>(user);
    const std::uint64_t until = MonotonicNanoseconds() + self->spinNs;
    while (MonotonicNanoseconds() < until) {
    }
    std::memset(out, 0, sizeof(float) * frames * channels);
  }
};
}  // namespace

// 2.4: blocks over budget raise the level after `downBlocks`, blocks within
// budget lower it after `upBlocks`. Exact with the monitor alone.
SND_TEST(LoadMonitor, HysteresisIsExact) {
  LoadMonitorDesc d;
  d.downBlocks = 8;
  d.upBlocks = 64;
  d.maxLevel = 3;
  LoadMonitor m(d);
  for (int i = 0; i < 7; ++i) SND_CHECK_EQ(m.Record(200, 100), 0u);
  SND_CHECK_EQ(m.Record(200, 100), 1u);  // 8th consecutive
  for (int i = 0; i < 7; ++i) m.Record(200, 100);
  m.Record(50, 100);  // breaks the streak
  for (int i = 0; i < 7; ++i) SND_CHECK_EQ(m.Record(200, 100), 1u);
  SND_CHECK_EQ(m.Record(200, 100), 2u);
  for (int i = 0; i < 100; ++i) m.Record(200, 100);
  SND_CHECK_EQ(m.Level(), 3u);  // capped
  for (int i = 0; i < 63; ++i) SND_CHECK_EQ(m.Record(100, 100), 3u);  // equal = within budget
  SND_CHECK_EQ(m.Record(100, 100), 2u);
  SND_CHECK_EQ(m.OverBudgetBlocks(), 7u + 1u + 7u + 7u + 1u + 100u);
  SND_CHECK_EQ(BlockBudgetNs(256, 48000, 60.0), 3200000u);  // 5.333 ms x 60 %
  SND_CHECK_EQ(BlockBudgetNs(256, 0, 60.0), 0u);
}

SND_TEST(DriverCore, OverloadRaisesLoadLevel) {
  SlowRenderer slow;
  DriverCore core;
  DriverCoreDesc d;
  d.blockFrames = 48;  // 1 ms at 48 kHz, budget 0.6 ms
  d.renderAheadBlocks = 1;
  d.lod.downBlocks = 4;
  d.lod.upBlocks = 4;
  d.render = &SlowRenderer::Render;
  d.user = &slow;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), d, 48000, 1, 48));
  std::vector<float> out(48);
  slow.spinNs = 2000000;  // 2 ms per block: over budget
  for (int i = 0; i < 8 && core.LoadLevel() == 0u; ++i) {
    core.Service(false);
    core.OnDeviceCallback(out.data(), 48, 1);
  }
  SND_CHECK(core.LoadLevel() >= 1u);
  SND_CHECK(core.Stats().overBudgetBlocks >= 4u);
  SND_CHECK(core.Stats().renderMaxNs >= 2000000u);
  slow.spinNs = 0;
  for (int i = 0; i < 64 && core.LoadLevel() != 0u; ++i) {
    core.Service(false);
    core.OnDeviceCallback(out.data(), 48, 1);
  }
  SND_CHECK_EQ(core.LoadLevel(), 0u);
}

// The reader and the writer on two threads (TSan target). With fade off,
// every output frame is either the next counter value or counted silence.
SND_TEST(DriverCore, ConcurrentReaderAndWriter) {
  CounterRenderer r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), MakeDesc(r, 64, 2, 0), 48000, 2, 100));
  core.Service(true);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      if (core.Service(false) == 0u) std::this_thread::yield();
    }
  });
  std::vector<float> out(100 * 2);
  SequenceChecker check;
  std::uint64_t frames = 0;
  for (int i = 0; i < 20000; ++i) {
    const std::uint32_t n = 1u + static_cast<std::uint32_t>(i % 100);
    core.OnDeviceCallback(out.data(), n, 2);
    check.Feed(out.data(), n, 2);
    frames += n;
  }
  stop.store(true, std::memory_order_release);
  writer.join();
  const DriverStats s = core.Stats();
  SND_CHECK(check.ok);
  SND_CHECK_EQ(check.valid + check.silent, frames);
  SND_CHECK_EQ(check.silent, s.lostFrames);
  SND_CHECK_EQ(s.framesConsumed, frames);
}
