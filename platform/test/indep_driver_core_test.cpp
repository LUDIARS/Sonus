// Independent tests of the device-independent driving parts (design.md 2.2,
// 2.4, 2.6, CONTRACTS.md D-66): OutputRing, LoadMonitor, DriverCore.
// Written from the specification and the public headers by the independent
// tester (properties: indep-platform-properties.md, sections A-C).
//
// The renderer encodes every sample so the output can be decoded exactly:
//   value = ((frame & 0xFFFF) + 1) << 7 | (channels - 1) << 3 | c
// (< 2^24, exact in float; never 0, so 0 is unambiguous silence). A callback
// in `ch` channels may only ever see samples whose channel field is `ch` and
// whose c field is the sample's position in the frame.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/driver_core.hpp"
#include "snd/platform/load_monitor.hpp"
#include "snd/platform/output_ring.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {

constexpr std::uint32_t kFrameMask = 0xFFFFu;

inline float Encode(std::uint64_t frame, std::uint32_t channels, std::uint32_t c) {
  const std::uint32_t v = ((static_cast<std::uint32_t>(frame & kFrameMask) + 1u) << 7) | ((channels - 1u) << 3) | c;
  return static_cast<float>(v);
}
struct Decoded {
  std::uint32_t frame;  // (frame & mask)
  std::uint32_t channels;
  std::uint32_t c;
};
inline bool Decode(float x, Decoded* d) {
  if (!(x > 0.0f) || x >= 16777216.0f) return false;
  const auto v = static_cast<std::uint32_t>(x);
  if (static_cast<float>(v) != x) return false;
  d->frame = (v >> 7) - 1u;
  d->channels = ((v >> 3) & 7u) + 1u;
  d->c = v & 7u;
  return true;
}

struct Recorder {
  std::uint64_t next = 0;  // next frame number to render
  std::uint64_t blocks = 0;
  std::uint64_t lostSum = 0;
  std::uint64_t lastLost = 0;
  std::uint32_t expectBlock = 0;  // required frames per render call
  std::uint32_t badFrames = 0;    // render calls with frames != expectBlock
  std::uint32_t badIndex = 0;     // blockIndex not consecutive
  std::uint32_t lastChannels = 0;
  std::uint32_t lastRate = 0;
  std::uint32_t lastLevel = 0;
  std::uint32_t formatCalls = 0;
  std::uint32_t formatRate = 0;
  std::uint32_t formatChannels = 0;
  std::uint32_t renderBeforeFormat = 0;  // render in a format that was not announced
  std::uint32_t announcedChannels = 0;
  std::uint32_t announcedRate = 0;
  std::uint32_t allocateInRender = 0;  // > 0: allocate in the render function (RT check sanity)

  static void Render(void* user, float* out, std::uint32_t frames, std::uint32_t channels, const RenderInfo* info) {
    auto* self = static_cast<Recorder*>(user);
    if (frames != self->expectBlock) ++self->badFrames;
    if (info->blockIndex != self->blocks) ++self->badIndex;
    if (channels != self->announcedChannels || info->sampleRate != self->announcedRate) ++self->renderBeforeFormat;
    if (self->allocateInRender > 0u) {
      --self->allocateInRender;
      volatile int* p = new int(3);
      delete p;
    }
    ++self->blocks;
    self->lostSum += info->lostFrames;
    self->lastLost = info->lostFrames;
    self->lastChannels = channels;
    self->lastRate = info->sampleRate;
    self->lastLevel = info->loadLevel;
    for (std::uint32_t f = 0; f < frames; ++f) {
      for (std::uint32_t c = 0; c < channels; ++c) out[f * channels + c] = Encode(self->next + f, channels, c);
    }
    self->next += frames;
  }
  static void Format(void* user, std::uint32_t rate, std::uint32_t channels) {
    auto* self = static_cast<Recorder*>(user);
    ++self->formatCalls;
    self->formatRate = rate;
    self->formatChannels = channels;
    self->announcedChannels = channels;
    self->announcedRate = rate;
  }
};

DriverCoreDesc Desc(Recorder& r, std::uint32_t block, std::uint32_t ahead, std::uint32_t fade, std::uint32_t maxCh,
                    std::uint32_t maxPeriod) {
  DriverCoreDesc d;
  d.blockFrames = block;
  d.renderAheadBlocks = ahead;
  d.catchupBlocks = 2;
  d.fadeFrames = fade;
  d.maxChannels = maxCh;
  d.maxPeriodFrames = maxPeriod;
  d.render = &Recorder::Render;
  d.formatChanged = &Recorder::Format;
  d.user = &r;
  r.expectBlock = block;
  return d;
}

// Checks the decoded output of the device callbacks.
struct Checker {
  bool started = false;
  std::uint64_t expect = 0;      // next frame number expected (full 64 bits)
  std::uint64_t valid = 0;
  std::uint64_t silent = 0;
  std::uint64_t badFormat = 0;   // sample in the wrong channel layout
  std::uint64_t badOrder = 0;    // frame not the next one (no switch allowed)
  std::uint64_t jumps = 0;       // forward jumps (allowed after a format switch)
  bool allowJump = false;        // set by the test around a format switch
  void Feed(const float* out, std::uint32_t frames, std::uint32_t channels) {
    for (std::uint32_t f = 0; f < frames; ++f) {
      const float* fr = out + static_cast<std::size_t>(f) * channels;
      bool allZero = true;
      for (std::uint32_t c = 0; c < channels; ++c) allZero = allZero && fr[c] == 0.0f;
      if (allZero) {
        ++silent;
        continue;
      }
      Decoded d0;
      bool ok = true;
      for (std::uint32_t c = 0; c < channels; ++c) {
        Decoded d;
        if (!Decode(fr[c], &d) || d.channels != channels || d.c != c) {
          ok = false;
          break;
        }
        if (c == 0) d0 = d;
        if (d.frame != d0.frame) ok = false;
      }
      if (!ok) {
        ++badFormat;
        continue;
      }
      if (!started) {
        started = true;
        expect = d0.frame;
      }
      const std::uint32_t want = static_cast<std::uint32_t>(expect & kFrameMask);
      if (d0.frame != want) {
        if (allowJump) {
          // Forward by less than half the mask range.
          const std::uint32_t ahead = (d0.frame - want) & kFrameMask;
          if (ahead < 0x8000u) {
            expect += ahead;
            ++jumps;
          } else {
            ++badOrder;
          }
        } else {
          ++badOrder;
        }
      }
      ++expect;
      ++valid;
    }
  }
};

std::uint32_t CeilDiv(std::uint32_t a, std::uint32_t b) { return (a + b - 1u) / b; }

}  // namespace

// ---------------------------------------------------------------------------
// A. OutputRing
// ---------------------------------------------------------------------------

SND_TEST(IndepOutputRing, BindValidatesAndDataSurvivesWrap) {
  std::vector<float> storage(16);
  OutputRing ring;
  SND_CHECK(!ring.Bind(nullptr, 16));
  SND_CHECK(!ring.Bind(storage.data(), 12));
  SND_CHECK(!ring.Bind(storage.data(), 0));
  SND_REQUIRE(ring.Bind(storage.data(), 16));
  SND_CHECK_EQ(ring.Capacity(), 16u);
  float in[16], out[16];
  float next = 1.0f, expect = 1.0f;
  bool ok = true;
  // Chunks of 1..11 samples, wrapping many times.
  for (int i = 0; i < 500; ++i) {
    const std::uint32_t n = 1u + static_cast<std::uint32_t>(i * 7 % 11);
    if (ring.Capacity() - ring.QueuedForWriter() >= n) {
      for (std::uint32_t k = 0; k < n; ++k) in[k] = next++;
      ring.Write(in, n);
    }
    const std::uint64_t avail = ring.AcquireWriteIndex() - ring.ReadIndex();
    const std::uint32_t m = static_cast<std::uint32_t>(avail < 5u ? avail : 5u);
    ring.Read(out, m);
    for (std::uint32_t k = 0; k < m; ++k) ok = ok && out[k] == expect++;
  }
  SND_CHECK(ok);
  SND_CHECK_EQ(ring.QueuedForWriter(), ring.WriteIndex() - ring.ReadIndex());
  // SkipTo drops exactly the samples below the index.
  const std::uint64_t w = ring.WriteIndex();
  for (std::uint32_t k = 0; k < 6; ++k) in[k] = 1000.0f + static_cast<float>(k);
  ring.SkipTo(w);
  ring.Write(in, 6);
  ring.SkipTo(w + 2);
  ring.Read(out, 4);
  SND_CHECK_EQ(out[0], 1002.0f);
  SND_CHECK_EQ(out[3], 1005.0f);
  SND_CHECK_EQ(ring.QueuedForWriter(), 0u);
}

// SPSC on two threads: no sample lost, duplicated or reordered (TSan target).
SND_TEST(IndepOutputRing, ConcurrentProducerConsumerKeepsOrder) {
  std::vector<float> storage(256);
  OutputRing ring;
  SND_REQUIRE(ring.Bind(storage.data(), 256));
  constexpr std::uint32_t kTotal = 400000;
  std::thread producer([&] {
    float buf[37];
    std::uint32_t next = 0;
    while (next < kTotal) {
      const std::uint32_t n = 1u + next % 37u;
      const std::uint32_t m = kTotal - next < n ? kTotal - next : n;
      if (ring.Capacity() - ring.QueuedForWriter() < m) {
        std::this_thread::yield();
        continue;
      }
      for (std::uint32_t k = 0; k < m; ++k) buf[k] = static_cast<float>((next + k) % 1000000u + 1u);
      ring.Write(buf, m);
      next += m;
    }
  });
  std::uint32_t got = 0;
  bool ok = true;
  float buf[64];
  while (got < kTotal) {
    const std::uint64_t avail = ring.AcquireWriteIndex() - ring.ReadIndex();
    if (avail == 0u) {
      std::this_thread::yield();
      continue;
    }
    const std::uint32_t m = static_cast<std::uint32_t>(avail < 64u ? avail : 64u);
    ring.Read(buf, m);
    for (std::uint32_t k = 0; k < m; ++k) ok = ok && buf[k] == static_cast<float>((got + k) % 1000000u + 1u);
    got += m;
  }
  producer.join();
  SND_CHECK(ok);
  SND_CHECK_EQ(got, kTotal);
}

// ---------------------------------------------------------------------------
// B. LoadMonitor
// ---------------------------------------------------------------------------

SND_TEST(IndepLoadMonitor, StreaksLevelsAndCounters) {
  LoadMonitorDesc d;
  d.downBlocks = 3;
  d.upBlocks = 5;
  d.maxLevel = 2;
  LoadMonitor m(d);
  // Two over, one within: no change (the streak is broken).
  m.Record(11, 10);
  m.Record(11, 10);
  m.Record(10, 10);
  SND_CHECK_EQ(m.Level(), 0u);
  // Within budget at level 0 stays at 0 however long.
  for (int i = 0; i < 20; ++i) m.Record(1, 10);
  SND_CHECK_EQ(m.Level(), 0u);
  SND_CHECK_EQ(m.LevelChanges(), 0u);
  // Six consecutive over: two levels (the streak counts again after a step).
  for (int i = 0; i < 6; ++i) m.Record(100, 10);
  SND_CHECK_EQ(m.Level(), 2u);
  for (int i = 0; i < 30; ++i) m.Record(100, 10);
  SND_CHECK_EQ(m.Level(), 2u);  // capped at maxLevel
  SND_CHECK_EQ(m.LevelChanges(), 2u);
  // Four within + one over: the up streak restarts.
  for (int i = 0; i < 4; ++i) m.Record(1, 10);
  m.Record(100, 10);
  for (int i = 0; i < 4; ++i) SND_CHECK_EQ(m.Record(1, 10), 2u);
  SND_CHECK_EQ(m.Record(1, 10), 1u);
  for (int i = 0; i < 4; ++i) SND_CHECK_EQ(m.Record(1, 10), 1u);
  SND_CHECK_EQ(m.Record(1, 10), 0u);
  SND_CHECK_EQ(m.OverBudgetBlocks(), 2u + 6u + 30u + 1u);
  SND_CHECK_EQ(m.LevelChanges(), 4u);
  m.Reset();
  SND_CHECK_EQ(m.Level(), 0u);
  SND_CHECK_EQ(m.OverBudgetBlocks(), 0u);
  // maxLevel 0: never leaves level 0.
  LoadMonitorDesc z;
  z.downBlocks = 1;
  z.maxLevel = 0;
  LoadMonitor mz(z);
  for (int i = 0; i < 10; ++i) SND_CHECK_EQ(mz.Record(100, 10), 0u);
  // Defaults are those of 2.4 (8 / 64 / 3 steps).
  LoadMonitor def;
  for (int i = 0; i < 7; ++i) def.Record(2, 1);
  SND_CHECK_EQ(def.Level(), 0u);
  def.Record(2, 1);
  SND_CHECK_EQ(def.Level(), 1u);
}

SND_TEST(IndepLoadMonitor, BudgetArithmetic) {
  // frames / rate x 1e9 x percent / 100, rounded to nearest.
  SND_CHECK_EQ(BlockBudgetNs(1, 3, 100.0), 333333333u);
  SND_CHECK_EQ(BlockBudgetNs(2, 3, 100.0), 666666667u);
  SND_CHECK_EQ(BlockBudgetNs(128, 48000, 60.0), 1600000u);
  SND_CHECK_EQ(BlockBudgetNs(256, 44100, 100.0), 5804989u);  // 5804988.66
  SND_CHECK_EQ(BlockBudgetNs(480, 48000, 50.0), 5000000u);
  SND_CHECK_EQ(BlockBudgetNs(0, 48000, 60.0), 0u);
  SND_CHECK_EQ(BlockBudgetNs(256, 0, 60.0), 0u);
  SND_CHECK_EQ(BlockBudgetNs(256, 48000, 0.0), 0u);
}

// ---------------------------------------------------------------------------
// C. DriverCore, deterministic
// ---------------------------------------------------------------------------

SND_TEST(IndepDriverCore, InitValidationAndReinit) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  Recorder r;
  DriverCore core;
  SND_CHECK(!core.Initialized());
  DriverCoreDesc d = Desc(r, 128, 2, 0, 8, 1024);
  SND_CHECK(!core.Init(acc, d, 48000, 0, 256));   // channels 0
  SND_CHECK(!core.Init(acc, d, 48000, 9, 256));   // > maxChannels
  SND_CHECK(!core.Init(acc, d, 0, 2, 256));       // rate 0
  DriverCoreDesc nb = d;
  nb.blockFrames = 0;
  SND_CHECK(!core.Init(acc, nb, 48000, 2, 256));
  DriverCoreDesc nr = d;
  nr.render = nullptr;
  SND_CHECK(!core.Init(acc, nr, 48000, 2, 256));
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
  SND_CHECK(!core.Initialized());
  // Not initialised: every entry point is harmless.
  float buf[16] = {};
  core.OnDeviceCallback(buf, 8, 2);
  SND_CHECK_EQ(core.Service(true), 0u);
  SND_REQUIRE(core.Init(acc, d, 48000, 8, 256));  // channels == maxChannels is valid
  SND_CHECK(core.Initialized());
  SND_CHECK(acc.TotalUsed() > 0u);
  // Ring big enough for the largest period plus render-ahead and catch-up.
  SND_CHECK(core.CapacityFrames(8) >= 1024u + (2u + 2u) * 128u);
  SND_CHECK_EQ(core.TargetFrames(), 256u + 2u * 128u);
  SND_CHECK_EQ(core.BlockFrames(), 128u);
  // Re-Init replaces the previous allocation (no leak), Release frees all.
  SND_REQUIRE(core.Init(acc, d, 44100, 1, 64));
  SND_CHECK_EQ(core.Stats().sampleRate, 44100u);
  SND_CHECK_EQ(core.Stats().channels, 1u);
  core.Release();
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
  SND_REQUIRE(core.Init(acc, d, 48000, 2, 0));  // period 0: something sane
  SND_CHECK(core.TargetFrames() >= 2u * 128u);
  core.Release();
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

// The heart of 2.2: any block size against any device period. The renderer is
// always called with exactly blockFrames, consecutive block indices, the
// device's format; the device sees one gapless, ordered stream; one wake never
// renders more than ceil(period/block) + catchup blocks; after a wake the fill
// is at least the target and less than target + one block.
SND_TEST(IndepDriverCore, BlockPeriodMatrixIsSeamless) {
  const std::uint32_t blocks[] = {1, 3, 64, 100, 128, 256, 257, 1000};
  const std::uint32_t periods[] = {1, 7, 100, 127, 128, 255, 256, 509, 1024, 4096};
  const std::uint32_t aheads[] = {0, 1, 2};
  const std::uint32_t chans[] = {1, 2, 8};
  std::uint32_t failures = 0, combos = 0;
  for (std::uint32_t block : blocks) {
    for (std::uint32_t period : periods) {
      for (std::uint32_t ahead : aheads) {
        for (std::uint32_t ch : chans) {
          if ((block * 7u + period + ahead * 3u + ch) % 3u != 0u && ch != 2u) continue;  // thin the matrix
          ++combos;
          Recorder r;
          DriverCore core;
          if (!core.Init(snd::base::DefaultAllocator(), Desc(r, block, ahead, 0, ch, 1), 48000, ch, period)) {
            ++failures;
            snd::test::Note("Init failed block %u period %u ahead %u ch %u", block, period, ahead, ch);
            continue;
          }
          const std::uint32_t target = period + ahead * block;
          bool ok = core.TargetFrames() == target;
          core.Service(true);
          ok = ok && core.Stats().queuedFrames >= target && core.Stats().queuedFrames < target + block;
          std::vector<float> out(static_cast<std::size_t>(period) * ch);
          Checker check;
          const std::uint64_t ringSamples = core.CapacityFrames(ch) * ch;
          const std::uint64_t iterations = 3u * ringSamples / (static_cast<std::uint64_t>(period) * ch) + 20u;
          const std::uint32_t limit = CeilDiv(period, block) + 2u;
          std::uint32_t maxPerWake = 0;
          for (std::uint64_t i = 0; i < iterations; ++i) {
            core.OnDeviceCallback(out.data(), period, ch);
            check.Feed(out.data(), period, ch);
            const std::uint32_t n = core.Service(false);
            if (n > maxPerWake) maxPerWake = n;
            const DriverStats s = core.Stats();
            if (s.queuedFrames < target || s.queuedFrames >= target + block) ok = false;
          }
          const DriverStats s = core.Stats();
          ok = ok && check.badFormat == 0u && check.badOrder == 0u && check.silent == 0u;
          ok = ok && check.valid == iterations * period && s.underruns == 0u && s.lostFrames == 0u;
          ok = ok && r.badFrames == 0u && r.badIndex == 0u && r.renderBeforeFormat == 0u;
          ok = ok && maxPerWake <= limit && s.blocksRendered == r.blocks;
          ok = ok && s.framesConsumed == iterations * period && s.callbacks == iterations;
          ok = ok && r.formatCalls == 1u && r.formatChannels == ch && r.formatRate == 48000u;
          if (!ok) {
            ++failures;
            snd::test::Note(
                "block %u period %u ahead %u ch %u: valid %llu silent %llu badFmt %llu badOrd %llu under %llu "
                "badFrames %u badIdx %u perWake %u/%u target %u queued %u",
                block, period, ahead, ch, static_cast<unsigned long long>(check.valid),
                static_cast<unsigned long long>(check.silent), static_cast<unsigned long long>(check.badFormat),
                static_cast<unsigned long long>(check.badOrder), static_cast<unsigned long long>(s.underruns),
                r.badFrames, r.badIndex, maxPerWake, limit, core.TargetFrames(), s.queuedFrames);
          }
        }
      }
    }
  }
  snd::test::Note("%u combinations", combos);
  SND_CHECK_EQ(failures, 0u);
}

// Callback lengths that vary on every call, all within PeriodFrames (D-62:
// WASAPI padding, Core Audio slices). No underrun, gapless, block-exact renders.
SND_TEST(IndepDriverCore, VaryingCallbackLengthsWithinPeriod) {
  const std::uint32_t cases[][2] = {{256, 480}, {128, 441}, {480, 128}, {64, 1}, {97, 1021}};
  for (const auto& cs : cases) {
    const std::uint32_t block = cs[0], period = cs[1];
    Recorder r;
    DriverCore core;
    SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, block, 1, 0, 2, 1), 48000, 2, period));
    core.Service(true);
    std::vector<float> out(static_cast<std::size_t>(period) * 2u);
    Checker check;
    std::uint32_t lcg = 12345u;
    std::uint64_t total = 0;
    for (int i = 0; i < 4000; ++i) {
      lcg = lcg * 1664525u + 1013904223u;
      const std::uint32_t n = 1u + (lcg >> 8) % period;
      core.OnDeviceCallback(out.data(), n, 2);
      check.Feed(out.data(), n, 2);
      total += n;
      SND_CHECK(core.Service(false) <= CeilDiv(period, block) + 2u);
    }
    const DriverStats s = core.Stats();
    SND_CHECK_EQ(s.underruns, 0u);
    SND_CHECK_EQ(check.silent, 0u);
    SND_CHECK_EQ(check.badOrder, 0u);
    SND_CHECK_EQ(check.badFormat, 0u);
    SND_CHECK_EQ(check.valid, total);
    SND_CHECK_EQ(r.badFrames, 0u);
    SND_CHECK_EQ(s.framesConsumed, total);
  }
}

// A callback longer than the announced period raises the effective period
// (D-66: the largest callback seen), so the target covers it from then on.
SND_TEST(IndepDriverCore, LongerCallbackRaisesTarget) {
  Recorder r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 64, 1, 0, 2, 4096), 48000, 2, 64));
  SND_CHECK_EQ(core.TargetFrames(), 128u);
  core.Service(true);
  std::vector<float> out(600 * 2);
  core.OnDeviceCallback(out.data(), 500, 2);  // more than is queued: an underrun is allowed once
  SND_CHECK_EQ(core.TargetFrames(), 500u + 64u);
  const std::uint64_t underBefore = core.Stats().underruns;
  // The next wake may only render ceil(500/64) + 2 blocks.
  SND_CHECK(core.Service(false) <= CeilDiv(500, 64) + 2u);
  for (int i = 0; i < 4; ++i) core.Service(false);
  for (int i = 0; i < 200; ++i) {
    core.OnDeviceCallback(out.data(), 500, 2);
    core.Service(false);
  }
  SND_CHECK_EQ(core.Stats().underruns, underBefore);
  // A CHANGED with a smaller period does not lower the target below what the callbacks need.
  core.OnDeviceChanged(0, 32);
  SND_CHECK_EQ(core.TargetFrames(), 500u + 64u);
}

// Prefill fills to the target in one call even from empty and with a target
// larger than the per-wake limit; a full ring renders nothing; a normal wake
// after a long stall is bounded by ceil(period / block) + catchupBlocks.
SND_TEST(IndepDriverCore, PrefillReachesTargetInOneCall) {
  Recorder r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 100, 4, 0, 2, 1), 48000, 2, 1000));
  const std::uint32_t target = 1000u + 400u;
  SND_CHECK_EQ(core.TargetFrames(), target);
  SND_CHECK_EQ(core.Service(true), 14u);
  SND_CHECK_EQ(core.Stats().queuedFrames, 1400u);
  SND_CHECK_EQ(core.Service(true), 0u);
  SND_CHECK_EQ(core.Service(false), 0u);
  std::vector<float> out(1000 * 2);
  for (int i = 0; i < 3; ++i) core.OnDeviceCallback(out.data(), 1000, 2);  // drained + 1600 lost
  SND_CHECK_EQ(core.Stats().queuedFrames, 0u);
  SND_CHECK_EQ(core.Service(false), 10u + 2u);  // catch-up limit, below the target
  SND_CHECK_EQ(core.Stats().queuedFrames, 1200u);
  SND_CHECK_EQ(core.Service(false), 2u);        // the rest on the next wake
  for (int i = 0; i < 3; ++i) core.OnDeviceCallback(out.data(), 1000, 2);
  SND_CHECK_EQ(core.Service(true), 14u);        // prefill (RESUMED) is not limited
}

// Underrun: what is queued is played, its last min(fade, n) frames ramp down
// monotonically to (near) zero, the rest is exact silence, the next data ramps
// up monotonically from (near) zero and reaches full scale after `fade`
// frames. The silence is reported once, exactly, as lostFrames of the next block.
SND_TEST(IndepDriverCore, UnderrunFadeShapesAndLostFrames) {
  const std::uint32_t fades[] = {1, 4, 64};
  for (std::uint32_t fade : fades) {
    const std::uint32_t avails[] = {1, fade > 1u ? fade - 1u : 1u, fade, fade + 5u, 3u * fade};
    for (std::uint32_t avail : avails) {
      Recorder r;
      DriverCore core;
      // block 1 so the fill can be set exactly; ahead 0, period = avail.
      SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 1, 0, fade, 1, 512), 48000, 1, avail));
      core.Service(true);
      SND_REQUIRE_EQ(core.Stats().queuedFrames, avail);
      const std::uint32_t extra = 37u + fade;  // silent frames (also makes the period cover the fade-in callback)
      const std::uint32_t frames = avail + extra;
      std::vector<float> out(frames);
      core.OnDeviceCallback(out.data(), frames, 1);
      const std::uint32_t len = fade < avail ? fade : avail;
      bool ok = true;
      // Untouched part.
      for (std::uint32_t i = 0; i + len < avail; ++i) ok = ok && out[i] == Encode(i, 1, 0);
      // Fade-out: gains non-increasing, < 1, the last one <= 1/len.
      float prevGain = 1.0f;
      for (std::uint32_t i = avail - len; i < avail; ++i) {
        const float g = out[i] / Encode(i, 1, 0);
        ok = ok && g <= prevGain + 1e-6f && g < 1.0f && g >= 0.0f;
        prevGain = g;
      }
      ok = ok && prevGain <= 1.0f / static_cast<float>(len) + 1e-6f;
      for (std::uint32_t i = avail; i < frames; ++i) ok = ok && out[i] == 0.0f;
      DriverStats s = core.Stats();
      ok = ok && s.underruns == 1u && s.lostFrames == extra;
      // Next block carries the lost frames once.
      core.Service(false);
      ok = ok && r.lostSum == extra;
      core.Service(true);
      // Fade-in over the first `fade` frames of the next callback.
      std::vector<float> in(fade + 3u);
      core.OnDeviceCallback(in.data(), fade + 3u, 1);
      prevGain = 0.0f;
      for (std::uint32_t i = 0; i < fade + 3u; ++i) {
        const float g = in[i] / Encode(avail + i, 1, 0);
        if (i < fade) {
          ok = ok && g >= prevGain - 1e-6f && g <= 1.0f;
          if (i == 0) ok = ok && g < 1.0f;
        } else {
          ok = ok && g == 1.0f;
        }
        prevGain = g;
      }
      ok = ok && r.lostSum == extra && core.Stats().underruns == 1u;
      if (!ok) snd::test::Note("fade %u avail %u failed", fade, avail);
      SND_CHECK(ok);
    }
  }
}

// DEFECT candidate: the fade-in after an underrun is cut to the length of the
// first callback that has data. With short callbacks (Core Audio slices,
// WASAPI padding: D-62 allows any 1..PeriodFrames) the ramp jumps to full
// scale after `frames` instead of `fadeFrames`: with 1-frame callbacks the
// gain goes 0 -> 0.5 -> 1.0. Expected: the ramp continues over the following
// callbacks until fadeFrames frames have been faded (gain still < 1 at frame
// fadeFrames / 2).
SND_TEST(IndepDriverCore, Defect_platform_FadeInTruncatedToCallbackLength) {
  Recorder r;
  DriverCore core;
  const std::uint32_t fade = 64;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 64, 1, fade, 1, 1), 48000, 1, 64));
  std::vector<float> out(64);
  core.OnDeviceCallback(out.data(), 64, 1);  // empty ring: underrun, fade-in pending
  core.Service(true);
  float gains[fade];
  for (std::uint32_t i = 0; i < fade; ++i) {
    float x = 0.0f;
    core.OnDeviceCallback(&x, 1, 1);
    core.Service(false);
    gains[i] = x / Encode(i, 1, 0);
  }
  snd::test::Note("fade-in gains with 1-frame callbacks: %.3f %.3f %.3f ... [%u] %.3f", gains[0], gains[1], gains[2],
                  fade / 2u, gains[fade / 2u]);
  SND_CHECK(gains[1] < 1.0f);
  SND_CHECK(gains[fade / 2u] < 1.0f);
  bool monotone = true;
  for (std::uint32_t i = 1; i < fade; ++i) monotone = monotone && gains[i] >= gains[i - 1];
  SND_CHECK(monotone);
}

// Several underruns between two blocks accumulate into one lostFrames report;
// the sum of all reports equals Stats().lostFrames.
SND_TEST(IndepDriverCore, LostFramesAccumulateAndSumUp) {
  Recorder r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 64, 1, 0, 2, 1), 48000, 2, 128));
  std::vector<float> out(512 * 2);
  // Before any Service: everything silent.
  core.OnDeviceCallback(out.data(), 100, 2);
  core.OnDeviceCallback(out.data(), 28, 2);
  for (std::uint32_t i = 0; i < 128u * 2u; ++i) SND_CHECK_EQ(out[i], 0.0f);
  core.Service(true);
  SND_CHECK_EQ(r.lostSum, 128u);
  SND_CHECK_EQ(r.lastLost, 0u);  // only the first block carries it
  std::uint32_t lcg = 7u;
  for (int i = 0; i < 3000; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    const std::uint32_t n = 1u + (lcg >> 9) % 400u;
    core.OnDeviceCallback(out.data(), n, 2);
    if ((lcg >> 20) % 3u != 0u) core.Service(false);  // sometimes skip a wake
  }
  core.Service(false);
  // Consume exactly what is queued (no new loss), then render: the pending
  // report (if any) is delivered with that block.
  const std::uint32_t q = core.Stats().queuedFrames;
  std::vector<float> rest(static_cast<std::size_t>(q) * 2u + 2u);
  if (q > 0u) core.OnDeviceCallback(rest.data(), q, 2);
  SND_CHECK(core.Service(false) > 0u);
  const DriverStats s = core.Stats();
  SND_CHECK(s.underruns > 0u);
  SND_CHECK_EQ(r.lostSum, s.lostFrames);
}

// Channel change (2.6, D-66): the switching callback is silent and counted,
// FormatFn comes before the first block in the new layout, no frame in the
// old layout ever reaches a callback in the new layout, and back again.
SND_TEST(IndepDriverCore, ChannelSwitchNeverLeaksOldLayout) {
  Recorder r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 100, 2, 0, 8, 1), 48000, 2, 256));
  core.Service(true);
  std::vector<float> out(256 * 8);
  Checker check;
  const std::uint32_t seq[] = {2, 6, 6, 1, 8, 2, 2, 1};
  std::uint32_t prev = 2;
  std::uint32_t switches = 0;
  for (std::uint32_t ch : seq) {
    for (int k = 0; k < 6; ++k) {
      if (ch != prev) check.allowJump = true;  // until the new layout plays
      const std::uint64_t before = check.valid;
      core.OnDeviceCallback(out.data(), 256, ch);
      check.Feed(out.data(), 256, ch);
      if (check.valid > before) check.allowJump = false;
      if (ch != prev && k == 0) {
        for (std::uint32_t i = 0; i < 256u * ch; ++i) SND_CHECK_EQ(out[i], 0.0f);  // switching callback
        ++switches;
      }
      core.Service(false);
      if (k == 0) prev = ch;
    }
    SND_CHECK_EQ(r.lastChannels, ch);
    SND_CHECK_EQ(core.Stats().channels, ch);
  }
  SND_CHECK_EQ(check.badFormat, 0u);
  SND_CHECK_EQ(check.badOrder, 0u);
  SND_CHECK_EQ(r.renderBeforeFormat, 0u);
  SND_CHECK_EQ(r.formatCalls, 1u + switches);
  SND_CHECK_EQ(core.Stats().formatChanges, switches);
  SND_CHECK_EQ(r.badFrames, 0u);
}

// DEFECT candidate: the device's layout flips 2 -> 1 -> 2 before the audio
// thread services the first request. The reader then acknowledges a mono
// stream while the device is back to stereo, but it does not ask again
// (requestedChannels_ is already 2), so every following callback is silent
// forever. Expected: after a few wakes the device plays stereo audio again.
SND_TEST(IndepDriverCore, Defect_platform_ChannelFlapBeforeServiceLeavesPermanentSilence) {
  Recorder r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 128, 2, 0, 8, 1), 48000, 2, 256));
  core.Service(true);
  std::vector<float> out(256 * 2);
  core.OnDeviceCallback(out.data(), 256, 1);  // route change to mono: silence, request
  core.OnDeviceCallback(out.data(), 256, 2);  // back to stereo before the audio thread ran
  core.Service(false);                        // applies the (stale) mono request
  Checker check;
  check.allowJump = true;
  for (int i = 0; i < 50; ++i) {
    core.OnDeviceCallback(out.data(), 256, 2);
    check.Feed(out.data(), 256, 2);
    core.Service(false);
  }
  snd::test::Note("after the flap: valid %llu silent %llu, ring channels %u",
                  static_cast<unsigned long long>(check.valid), static_cast<unsigned long long>(check.silent),
                  core.Stats().channels);
  SND_CHECK_EQ(core.Stats().channels, 2u);
  SND_CHECK(check.valid >= 40u * 256u);
}

// Rate change (CHANGED): FormatFn with the new rate, later blocks report it,
// no audio dropped, budget follows the rate. Same rate again: no FormatFn.
SND_TEST(IndepDriverCore, RateChangeKeepsAudioAndUpdatesBudget) {
  Recorder r;
  DriverCore core;
  DriverCoreDesc d = Desc(r, 128, 1, 0, 2, 1);
  d.loadTargetPercent = 50.0;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), d, 48000, 2, 256));
  core.Service(true);
  SND_CHECK_EQ(core.Stats().budgetNs, BlockBudgetNs(128, 48000, 50.0));
  std::vector<float> out(256 * 2);
  Checker check;
  for (int i = 0; i < 10; ++i) {
    core.OnDeviceCallback(out.data(), 256, 2);
    check.Feed(out.data(), 256, 2);
    if (i == 4) core.OnDeviceChanged(96000, 0);
    if (i == 7) core.OnDeviceChanged(96000, 0);  // same again
    core.Service(false);
  }
  SND_CHECK_EQ(check.silent, 0u);
  SND_CHECK_EQ(check.badOrder, 0u);
  SND_CHECK_EQ(r.formatCalls, 2u);
  SND_CHECK_EQ(r.formatRate, 96000u);
  SND_CHECK_EQ(r.lastRate, 96000u);
  SND_CHECK_EQ(r.renderBeforeFormat, 0u);
  SND_CHECK_EQ(core.Stats().sampleRate, 96000u);
  SND_CHECK_EQ(core.Stats().formatChanges, 1u);
  SND_CHECK_EQ(core.Stats().budgetNs, BlockBudgetNs(128, 96000, 50.0));
}

// Statistics: callback interval min <= avg <= max (with real gaps), render
// times, counters that must add up.
SND_TEST(IndepDriverCore, StatisticsAreConsistent) {
  Recorder r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 64, 1, 0, 2, 1), 48000, 2, 64));
  core.Service(true);
  std::vector<float> out(64 * 2);
  for (int i = 0; i < 20; ++i) {
    core.OnDeviceCallback(out.data(), 64, 2);
    core.Service(false);
    SleepFor(500000);  // 0.5 ms
  }
  const DriverStats s = core.Stats();
  SND_CHECK_EQ(s.callbacks, 20u);
  SND_CHECK_EQ(s.framesConsumed, 20u * 64u);
  SND_CHECK(s.intervalMinNs >= 400000u);
  SND_CHECK(s.intervalMinNs <= s.intervalAvgNs);
  SND_CHECK(s.intervalAvgNs <= s.intervalMaxNs);
  SND_CHECK(s.renderLastNs <= s.renderMaxNs);
  SND_CHECK(s.renderAvgNs <= s.renderMaxNs);
  SND_CHECK_EQ(s.blocksRendered, r.blocks);
  SND_CHECK_EQ(s.targetFrames, core.TargetFrames());
  SND_CHECK_EQ(s.loadLevel, core.LoadLevel());
  SND_CHECK(s.queuedFrames >= s.targetFrames);
}

// Load LOD through DriverCore: RenderInfo::loadLevel follows the monitor.
SND_TEST(IndepDriverCore, LoadLevelReachesRenderer) {
  struct Spin {
    std::uint64_t ns = 0;
    std::uint32_t lastLevel = 0;
    std::uint32_t maxLevel = 0;
    static void Render(void* u, float* out, std::uint32_t frames, std::uint32_t channels, const RenderInfo* info) {
      auto* s = static_cast<Spin*>(u);
      s->lastLevel = info->loadLevel;
      if (info->loadLevel > s->maxLevel) s->maxLevel = info->loadLevel;
      const std::uint64_t until = MonotonicNanoseconds() + s->ns;
      while (MonotonicNanoseconds() < until) {
      }
      std::memset(out, 0, sizeof(float) * frames * channels);
    }
  } spin;
  DriverCoreDesc d;
  d.blockFrames = 48;  // 1 ms; budget 0.6 ms
  d.renderAheadBlocks = 0;
  d.lod.downBlocks = 2;
  d.lod.upBlocks = 3;
  d.render = &Spin::Render;
  d.user = &spin;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), d, 48000, 1, 48));
  std::vector<float> out(48);
  spin.ns = 1500000;
  for (int i = 0; i < 12; ++i) {
    core.Service(false);
    core.OnDeviceCallback(out.data(), 48, 1);
  }
  SND_CHECK_EQ(core.LoadLevel(), 3u);  // 2 blocks per step, capped at 3
  SND_CHECK(spin.maxLevel >= 2u);      // the renderer saw the raised levels
  spin.ns = 0;
  for (int i = 0; i < 9; ++i) {
    core.Service(false);
    core.OnDeviceCallback(out.data(), 48, 1);
  }
  SND_CHECK_EQ(core.LoadLevel(), 0u);
  SND_CHECK_EQ(core.Stats().loadLevel, 0u);
}

// RT safety (D-66): OnDeviceCallback, Service and the stats are free of
// allocation / locks / IO. The runner fails the test on any unexpected
// violation inside SND_RT_SCOPE. A sanity check proves detection works.
SND_TEST(IndepDriverCore, EntryPointsAreRtSafe) {
  Recorder r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 64, 2, 32, 8, 1), 48000, 2, 100));
  std::vector<float> out(512 * 8);
  {
    SND_RT_SCOPE();
    core.Service(true);
    for (int i = 0; i < 200; ++i) {
      const std::uint32_t ch = (i / 40) % 2 == 0 ? 2u : 6u;  // includes channel switches
      core.OnDeviceCallback(out.data(), 1u + static_cast<std::uint32_t>(i * 37 % 300), ch);
      if (i % 50 == 0) core.OnDeviceChanged(i % 100 == 0 ? 44100u : 48000u, 200u);
      if (i % 3 != 0) core.Service(false);
      const DriverStats s = core.Stats();
      (void)s;
    }
  }
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  if (snd::rt::kEnabled && snd::test::rt::NewHooksActive()) {
    snd::test::rt::ExpectViolations expect;
    r.allocateInRender = 1;
    SND_RT_SCOPE();
    float x[16];
    core.OnDeviceCallback(x, 1, 6);
    for (int i = 0; i < 20 && r.allocateInRender > 0u; ++i) {
      core.OnDeviceCallback(out.data(), 300, 6);
      core.Service(false);
    }
    SND_CHECK(snd::test::rt::Total() > 0u);  // the harness does see a violation
  }
}

// ---------------------------------------------------------------------------
// C18. DriverCore on real threads (TSan / Release x20)
// ---------------------------------------------------------------------------

// Reader with varying lengths and channel switches (each switch only after
// the previous one has been seen to complete, so the flap defect above is not
// exercised here), writer looping Service, a third thread posting CHANGED
// events and reading the statistics. Every non-silent output frame must be in
// the layout of the callback that played it, and in order within a layout.
SND_TEST(IndepDriverCore, ThreadedReaderWriterEventsAndSwitches) {
  Recorder r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 64, 2, 0, 8, 512), 48000, 2, 128));
  core.Service(true);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      if (core.Service(false) == 0u) std::this_thread::yield();
    }
  });
  std::thread events([&] {
    std::uint32_t i = 0;
    while (!stop.load(std::memory_order_acquire)) {
      core.OnDeviceChanged((i & 1u) != 0u ? 44100u : 48000u, 64u + (i % 4u) * 32u);
      const DriverStats s = core.Stats();
      (void)s;
      ++i;
      std::this_thread::yield();
    }
  });
  std::vector<float> out(512 * 8);
  Checker check;
  std::uint64_t frames = 0;
  std::uint32_t ch = 2;
  std::uint32_t switches = 0;
  bool switching = false;
  std::uint32_t lcg = 99u;
  for (int i = 0; i < 30000; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    const std::uint32_t n = 1u + (lcg >> 8) % 128u;
    check.allowJump = switching;
    const std::uint64_t validBefore = check.valid;
    core.OnDeviceCallback(out.data(), n, ch);
    check.Feed(out.data(), n, ch);
    frames += n;
    if (switching && check.valid > validBefore) switching = false;  // new layout is playing
    if (!switching && i % 1500 == 1499) {
      ch = ch == 2u ? 6u : (ch == 6u ? 1u : 2u);
      switching = true;
      ++switches;
    }
  }
  stop.store(true, std::memory_order_release);
  writer.join();
  events.join();
  const DriverStats s = core.Stats();
  snd::test::Note("frames %llu valid %llu silent %llu switches %u underruns %llu",
                  static_cast<unsigned long long>(frames), static_cast<unsigned long long>(check.valid),
                  static_cast<unsigned long long>(check.silent), switches,
                  static_cast<unsigned long long>(s.underruns));
  SND_CHECK_EQ(check.badFormat, 0u);
  SND_CHECK_EQ(check.badOrder, 0u);
  SND_CHECK_EQ(check.valid + check.silent, frames);
  SND_CHECK_EQ(check.silent, s.lostFrames);
  SND_CHECK_EQ(s.framesConsumed, frames);
  SND_CHECK_EQ(r.badFrames, 0u);
  // The reader is not paced (much faster than real time): underruns are
  // expected; what matters is that every layout switch completed.
  SND_CHECK(check.valid > 0u);
  SND_CHECK(switches >= 5u);
}
