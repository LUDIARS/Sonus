// Independent re-check of the DriverCore / AudioDriver fixes (f9761be, 83d905f):
// re-read of the write index after a new ack, the ack carrying the last
// channel-change position, re-requesting after a flip, the fade-in position,
// droppedFrames / lostFrames, CancelService, and the shared clock (D-66, 2.2).
//
// Samples encode the renderer's CLOCK of the frame (frames rendered so far +
// every RenderInfo::lostFrames received so far) and the layout:
//   value = ((clock & 0xFFFF) + 1) << 7 | (channels - 1) << 3 | c
// so the device side can check both the layout and the time alignment.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/audio_driver.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/driver_core.hpp"
#include "snd/platform/timer_output_device.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {

constexpr std::uint32_t kMask = 0xFFFFu;

inline float Enc(std::uint64_t clock, std::uint32_t ch, std::uint32_t c) {
  return static_cast<float>(((static_cast<std::uint32_t>(clock & kMask) + 1u) << 7) | ((ch - 1u) << 3) | c);
}
inline bool Dec(float x, std::uint32_t* clock, std::uint32_t* ch, std::uint32_t* c) {
  if (!(x > 0.0f) || x >= 16777216.0f) return false;
  const auto v = static_cast<std::uint32_t>(x);
  if (static_cast<float>(v) != x) return false;
  *clock = (v >> 7) - 1u;
  *ch = ((v >> 3) & 7u) + 1u;
  *c = v & 7u;
  return true;
}

struct ClockRenderer {
  std::uint64_t clock = 0;  // music time of the next frame to render
  std::uint64_t lostSum = 0;
  std::uint64_t rendered = 0;
  std::uint32_t lastChannels = 0;
  std::atomic<int> cancelAfter{-1};  // > 0: call CancelService from inside the render function after n blocks
  DriverCore* core = nullptr;
  static void Render(void* u, float* out, std::uint32_t frames, std::uint32_t ch, const RenderInfo* info) {
    auto* r = static_cast<ClockRenderer*>(u);
    r->clock += info->lostFrames;  // 2.2: the shared clock advances by the silence the device played
    r->lostSum += info->lostFrames;
    for (std::uint32_t f = 0; f < frames; ++f) {
      for (std::uint32_t c = 0; c < ch; ++c) out[f * ch + c] = Enc(r->clock + f, ch, c);
    }
    r->clock += frames;
    r->rendered += frames;
    r->lastChannels = ch;
    const int n = r->cancelAfter.load();
    if (n > 0) {
      r->cancelAfter.store(n - 1);
      if (n == 1) r->core->CancelService();
    }
  }
};

DriverCoreDesc Desc(ClockRenderer& r, std::uint32_t block, std::uint32_t ahead, std::uint32_t maxPeriod = 1024) {
  DriverCoreDesc d;
  d.blockFrames = block;
  d.renderAheadBlocks = ahead;
  d.fadeFrames = 0;
  d.maxChannels = 8;
  d.maxPeriodFrames = maxPeriod;
  d.render = &ClockRenderer::Render;
  d.user = &r;
  return d;
}

// Device side: counts frames (the device clock) and checks every valid frame.
struct Device {
  std::uint64_t time = 0;  // frames handed out so far
  std::uint64_t valid = 0, silent = 0, badLayout = 0;
  std::int64_t lastOffset = 0;   // renderer clock - device time of the last valid frame
  std::int64_t maxAbsOffset = 0;
  bool any = false;
  void Feed(const float* out, std::uint32_t frames, std::uint32_t ch) {
    for (std::uint32_t f = 0; f < frames; ++f, ++time) {
      const float* fr = out + static_cast<std::size_t>(f) * ch;
      bool zero = true;
      for (std::uint32_t c = 0; c < ch; ++c) zero = zero && fr[c] == 0.0f;
      if (zero) {
        ++silent;
        continue;
      }
      std::uint32_t clk = 0, lch = 0, lc = 0, clk0 = 0;
      bool ok = true;
      for (std::uint32_t c = 0; c < ch; ++c) {
        if (!Dec(fr[c], &clk, &lch, &lc) || lch != ch || lc != c) ok = false;
        if (c == 0) clk0 = clk;
        if (clk != clk0) ok = false;
      }
      if (!ok) {
        ++badLayout;
        continue;
      }
      ++valid;
      // Offset modulo 2^16, as a signed value.
      const auto d = static_cast<std::int16_t>(static_cast<std::uint16_t>(clk0 - static_cast<std::uint32_t>(time & kMask)));
      lastOffset = d;
      if ((d < 0 ? -d : d) > maxAbsOffset) maxAbsOffset = d < 0 ? -d : d;
      any = true;
    }
  }
};

}  // namespace

// 2.2: with underruns only, the renderer's clock at every frame the device
// plays equals the device time (lostFrames keeps them aligned).
SND_TEST(IndepDriverFix, ClockStaysAlignedThroughUnderruns) {
  ClockRenderer r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 64, 1), 48000, 2, 128));
  core.Service(true);
  std::vector<float> out(1024 * 2);
  Device dev;
  std::uint32_t lcg = 3u;
  for (int i = 0; i < 3000; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    const std::uint32_t n = 1u + (lcg >> 8) % 300u;
    core.OnDeviceCallback(out.data(), n, 2);
    dev.Feed(out.data(), n, 2);
    if ((lcg >> 20) % 4u != 0u) core.Service(false);
  }
  const DriverStats s = core.Stats();
  snd::test::Note("underruns %llu lost %llu, max |offset| %lld",
                  static_cast<unsigned long long>(s.underruns), static_cast<unsigned long long>(s.lostFrames),
                  static_cast<long long>(dev.maxAbsOffset));
  SND_CHECK(s.underruns > 0u);
  SND_CHECK_EQ(dev.badLayout, 0u);
  SND_CHECK_EQ(dev.maxAbsOffset, 0);
}

// The decision for spec gap 3: frames dropped at a channel switch are added
// to the next block's RenderInfo::lostFrames. They had already advanced the
// renderer's clock when they were rendered; the device never played them, so
// the renderer is already AHEAD by `dropped`. Adding them again as lost puts
// it ahead by 2 x dropped. Expected (property of 2.2): after the switch the
// renderer clock is not further from the device time than without the
// report, i.e. |offset| <= droppedFrames.
SND_TEST(IndepDriverFix, Defect_platform_DroppedFramesAdvanceTheClockTwice) {
  ClockRenderer r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 64, 2), 48000, 2, 128));
  core.Service(true);
  std::vector<float> out(256 * 8);
  Device dev;
  for (int i = 0; i < 20; ++i) {
    core.OnDeviceCallback(out.data(), 128, 2);
    dev.Feed(out.data(), 128, 2);
    core.Service(false);
  }
  SND_REQUIRE_EQ(dev.lastOffset, 0);
  // Route change to 6 channels with a full ring of stereo data.
  for (int i = 0; i < 20; ++i) {
    core.OnDeviceCallback(out.data(), 128, 6);
    dev.Feed(out.data(), 128, 6);
    core.Service(false);
  }
  const DriverStats s = core.Stats();
  snd::test::Note("dropped %llu lost %llu, renderer clock - device time after the switch: %lld",
                  static_cast<unsigned long long>(s.droppedFrames), static_cast<unsigned long long>(s.lostFrames),
                  static_cast<long long>(dev.lastOffset));
  SND_CHECK(s.droppedFrames > 0u);
  SND_CHECK_EQ(dev.badLayout, 0u);
  SND_CHECK(dev.lastOffset <= static_cast<std::int64_t>(s.droppedFrames));
}

// Accounting of the fix: sum of RenderInfo::lostFrames == lostFrames +
// droppedFrames (once everything pending has been reported); lostFrames ==
// silence the device played; droppedFrames == old-layout frames queued at
// the switch.
SND_TEST(IndepDriverFix, LostAndDroppedAddUp) {
  ClockRenderer r;
  DriverCore core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 50, 2), 48000, 2, 100));
  core.Service(true);
  std::vector<float> out(512 * 8);
  Device dev;
  const std::uint32_t seq[] = {2, 6, 1, 8, 2, 6, 6, 1};
  std::uint64_t droppedExpect = 0;
  std::uint32_t cur = 2;
  for (std::uint32_t ch : seq) {
    if (ch != cur) {
      // Everything queued in the old layout is dropped at this switch.
      droppedExpect += core.Stats().queuedFrames;
      cur = ch;
    }
    for (int k = 0; k < 5; ++k) {
      core.OnDeviceCallback(out.data(), 100, ch);
      dev.Feed(out.data(), 100, ch);
      core.Service(false);
    }
  }
  // Flush the pending report with one exact-fit callback and a render.
  const std::uint32_t q = core.Stats().queuedFrames;
  core.OnDeviceCallback(out.data(), q, cur);
  dev.Feed(out.data(), q, cur);
  core.Service(false);
  const DriverStats s = core.Stats();
  SND_CHECK_EQ(dev.badLayout, 0u);
  SND_CHECK_EQ(s.lostFrames, dev.silent);
  SND_CHECK_EQ(s.droppedFrames, droppedExpect);
  SND_CHECK_EQ(r.lostSum, s.lostFrames + s.droppedFrames);
  SND_CHECK_EQ(s.framesConsumed, dev.time);
}

// Defect 2 fix under rapid flips: the device layout changes on almost every
// callback (random among 1, 2, 6, 8), the audio thread runs only sometimes.
// Whenever the layout then stays put, audio in that layout resumes within a
// few callback + wake rounds, and no frame is ever played in a wrong layout.
SND_TEST(IndepDriverFix, RapidFlipsAlwaysSettle) {
  const std::uint32_t layouts[] = {1, 2, 6, 8};
  std::uint32_t worst = 0;
  for (int trial = 0; trial < 300; ++trial) {
    ClockRenderer r;
    DriverCore core;
    SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 32, 1, 256), 48000, 2, 64));
    core.Service(true);
    std::vector<float> out(64 * 8);
    Device dev;
    std::uint32_t lcg = 1000u + static_cast<std::uint32_t>(trial);
    std::uint32_t ch = 2;
    for (int i = 0; i < 40; ++i) {
      lcg = lcg * 1664525u + 1013904223u;
      ch = layouts[(lcg >> 16) % 4u];
      core.OnDeviceCallback(out.data(), 64, ch);
      dev.Feed(out.data(), 64, ch);
      if (((lcg >> 8) & 3u) == 0u) core.Service(false);
      if (((lcg >> 10) & 7u) == 0u) core.OnDeviceChanged((lcg & 1u) != 0u ? 44100u : 48000u, 0);  // rate flips too
    }
    // Settle on `ch`: count rounds until a callback plays audio.
    std::uint32_t rounds = 0;
    for (; rounds < 20; ++rounds) {
      const std::uint64_t before = dev.valid;
      core.OnDeviceCallback(out.data(), 64, ch);
      dev.Feed(out.data(), 64, ch);
      if (dev.valid > before) break;
      core.Service(false);
    }
    if (rounds > worst) worst = rounds;
    if (rounds >= 20 || dev.badLayout != 0u) {
      snd::test::Note("trial %d: settled after %u rounds, bad %llu, device %u ring %u", trial, rounds,
                      static_cast<unsigned long long>(dev.badLayout), ch, core.Stats().channels);
    }
    SND_CHECK(rounds < 20);
    SND_CHECK_EQ(dev.badLayout, 0u);
  }
  snd::test::Note("worst: %u rounds", worst);
  SND_CHECK(worst <= 3u);
}

// Defect 1 fix under the heaviest interleaving: the reader flips the layout on
// every callback once audio plays, the writer spins, a third thread flips the
// rate and period continuously (rate-only acks between channel acks). No
// frame in a wrong layout, exact accounting. TSan / Release x20 target.
SND_TEST(IndepDriverFix, ThreadedFlipsRatesAndChannels) {
  for (int round = 0; round < 3; ++round) {
    ClockRenderer r;
    DriverCore core;
    SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 32, 2, 512), 48000, 2, 64));
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
        core.OnDeviceChanged((i & 1u) != 0u ? 44100u : 96000u, 32u + (i % 3u) * 32u);
        ++i;
      }
    });
    std::vector<float> out(128 * 8);
    Device dev;
    const std::uint32_t layouts[] = {2, 6, 1, 8};
    std::uint32_t li = 0, ch = 2, switches = 0;
    std::uint32_t lcg = 77u + static_cast<std::uint32_t>(round);
    for (int i = 0; i < 40000; ++i) {
      lcg = lcg * 1664525u + 1013904223u;
      const std::uint32_t n = 1u + (lcg >> 8) % 128u;
      const std::uint64_t before = dev.valid;
      core.OnDeviceCallback(out.data(), n, ch);
      dev.Feed(out.data(), n, ch);
      if (dev.valid > before) {
        ch = layouts[++li % 4u];
        ++switches;
      }
      if ((i & 3) == 0) std::this_thread::yield();
    }
    // Liveness: with the layout held still, audio resumes (bounded wait, the
    // machine may be loaded; the reader above is not paced).
    bool resumed = false;
    for (int k = 0; k < 2000 && !resumed; ++k) {
      const std::uint64_t before = dev.valid;
      core.OnDeviceCallback(out.data(), 64, ch);
      dev.Feed(out.data(), 64, ch);
      resumed = dev.valid > before;
      if (!resumed) SleepFor(1000000);
    }
    stop.store(true, std::memory_order_release);
    writer.join();
    events.join();
    const DriverStats s = core.Stats();
    SND_CHECK(resumed);
    snd::test::Note("round %d: switches %u valid %llu silent %llu dropped %llu", round, switches,
                    static_cast<unsigned long long>(dev.valid), static_cast<unsigned long long>(dev.silent),
                    static_cast<unsigned long long>(s.droppedFrames));
    SND_CHECK_EQ(dev.badLayout, 0u);
    SND_CHECK_EQ(dev.valid + dev.silent, dev.time);
    SND_CHECK_EQ(dev.silent, s.lostFrames);
    SND_CHECK_EQ(s.framesConsumed, dev.time);
  }
}

// CancelService (spec gap 2): the block being rendered completes, the running
// Service returns before the next block, every later Service renders
// nothing, Init clears it.
SND_TEST(IndepDriverFix, CancelServiceBoundaries) {
  ClockRenderer r;
  DriverCore core;
  r.core = &core;
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 16, 4), 48000, 1, 64));
  r.cancelAfter.store(3);  // cancel from inside the 3rd render call
  SND_CHECK_EQ(core.Service(true), 3u);  // the 3rd block completes, no 4th
  SND_CHECK_EQ(core.Stats().queuedFrames, 48u);
  SND_CHECK_EQ(core.Service(true), 0u);
  SND_CHECK_EQ(core.Service(false), 0u);
  std::vector<float> out(64);
  core.OnDeviceCallback(out.data(), 48, 1);  // the reader keeps working on what is there
  SND_CHECK_EQ(core.Stats().underruns, 0u);
  core.CancelService();  // twice is harmless
  SND_REQUIRE(core.Init(snd::base::DefaultAllocator(), Desc(r, 16, 4), 48000, 1, 64));
  SND_CHECK(core.Service(true) > 0u);
}

// AudioDriver prefill timeout (decision for spec gap 2): Start fails after
// the timeout plus at most the block in progress, not after the whole prefill.
SND_TEST(IndepDriverFix, PrefillTimeoutWaitsOnlyForTheBlockInProgress) {
  struct Slow {
    std::atomic<std::uint32_t> blocks{0};
    static void Render(void* u, float* out, std::uint32_t frames, std::uint32_t ch, const RenderInfo*) {
      static_cast<Slow*>(u)->blocks.fetch_add(1);
      snd::rt::SuspendScope sus;  // test-only: a very slow render
      SleepFor(150000000ull);     // 150 ms per block
      std::memset(out, 0, sizeof(float) * frames * ch);
    }
  } slow;
  TimerDeviceConfig tc;
  tc.realtimeThread = false;
  TimerOutputDevice dev(snd::base::DefaultAllocator(), tc);
  AudioDriver driver;
  AudioDriverDesc d;
  d.core.blockFrames = 64;
  d.core.renderAheadBlocks = 4;  // prefill = 64 + 256 = 5 blocks = 750 ms
  d.core.render = &Slow::Render;
  d.core.user = &slow;
  d.realtimeThread = false;
  d.prefillTimeoutMs = 30;
  const std::uint64_t t0 = MonotonicNanoseconds();
  SND_CHECK(!driver.Start(dev, snd::DeviceDesc{48000, 2, 64, 0}, d, snd::base::DefaultAllocator()));
  const std::uint64_t ms = (MonotonicNanoseconds() - t0) / 1000000u;
  snd::test::Note("Start failed after %llu ms, %u blocks rendered", static_cast<unsigned long long>(ms),
                  slow.blocks.load());
  SND_CHECK(slow.blocks.load() <= 1u);
  SND_CHECK(ms < 400u);
  SND_CHECK(!driver.Running());
}
