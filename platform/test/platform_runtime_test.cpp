// platform: clock, threads, semaphore, FTZ/DAZ, CPU features, timer device
// (design.md 1.2 principle 1, 2.2, 2.3, 2.4, D-25).
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/platform/semaphore.hpp"
#include "snd/platform/thread.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

SND_TEST(Clock, MonotonicAndSleep) {
  std::uint64_t prev = MonotonicNanoseconds();
  {
    SND_RT_SCOPE();  // reading the clock is allowed in RT code
    for (int i = 0; i < 1000; ++i) {
      const std::uint64_t now = MonotonicNanoseconds();
      SND_CHECK(now >= prev);
      prev = now;
    }
  }
  const std::uint64_t t0 = MonotonicNanoseconds();
  SleepFor(2000000);  // 2 ms
  SND_CHECK(MonotonicNanoseconds() - t0 >= 2000000u);
  const std::uint64_t deadline = MonotonicNanoseconds() + 1000000;
  SleepUntil(deadline);
  SND_CHECK(MonotonicNanoseconds() >= deadline);
  SleepUntil(0);  // past deadline returns immediately
  SND_CHECK(MonotonicMicroseconds() > 0u);
}

SND_TEST(Clock, SleepIsReportedInRtScope) {
  snd::test::rt::ExpectViolations expect;
  snd::test::rt::Reset();
  {
    SND_RT_SCOPE();
    SleepFor(1000);
  }
  SND_CHECK(snd::test::rt::Count(snd::rt::Violation::Blocking) >= 1u);
  snd::test::rt::Reset();
}

namespace {
struct ThreadProbe {
  std::atomic<int> ran{0};
  std::atomic<bool> ftz{false};
  static void Fn(void* p) {
    auto* self = static_cast<ThreadProbe*>(p);
    ScopedFlushDenormals f;
    self->ftz = FlushDenormalsEnabled();
    self->ran.fetch_add(1);
  }
};
}  // namespace

SND_TEST(Thread, StartJoinAndPriorityPolicy) {
  ThreadProbe probe;
  {
    Thread t;
    ThreadDesc d;
    d.name = "snd-test-normal";
    SND_REQUIRE(t.Start(d, &ThreadProbe::Fn, &probe));
    SND_CHECK(t.Joinable());
    SND_CHECK_EQ(t.Priority(), PriorityResult::Applied);
    SND_CHECK(!t.Start(d, &ThreadProbe::Fn, &probe));  // already started
    t.Join();
    SND_CHECK(!t.Joinable());
  }
  SND_CHECK_EQ(probe.ran.load(), 1);
  if (GetCpuFeatures().denormalControl) SND_CHECK(probe.ftz.load());

  Thread rt;
  ThreadDesc d;
  d.name = "snd-test-rt-with-a-very-long-name";
  d.priority = ThreadPriority::Realtime;
  d.stackBytes = 256 * 1024;
  SND_REQUIRE(rt.Start(d, &ThreadProbe::Fn, &probe));
  const PriorityResult r = rt.Priority();
  // Without CAP_SYS_NICE the request falls back to normal priority; never "Failed".
  SND_CHECK(r == PriorityResult::Applied || r == PriorityResult::FellBack);
  snd::test::Note("realtime priority request: %s", PriorityResultName(r));
  rt.Join();  // destructor would also join
  SND_CHECK_EQ(probe.ran.load(), 2);
  SND_CHECK(HardwareConcurrency() >= 1u);
  SND_CHECK_EQ(SetCurrentThreadPriority(ThreadPriority::Normal), PriorityResult::Applied);
}

SND_TEST(Semaphore, BasicCounting) {
  Semaphore s(2);
  SND_RT_SCOPE();  // post / try-wait / wait on a pre-made semaphore are allowed (principle 1)
  SND_CHECK(s.TryWait());
  SND_CHECK(s.TryWait());
  SND_CHECK(!s.TryWait());
  s.Post(3);
  SND_CHECK_EQ(s.ValueApprox(), 3);
  s.Wait();
  SND_CHECK(s.WaitFor(0));
  SND_CHECK(s.TryWait());
  SND_CHECK(!s.TryWait());
}

SND_TEST(Semaphore, WaitForTimesOut) {
  Semaphore s;
  const std::uint64_t t0 = MonotonicNanoseconds();
  SND_CHECK(!s.WaitFor(3000000));  // 3 ms
  SND_CHECK(MonotonicNanoseconds() - t0 >= 3000000u);
  std::thread poster([&] {
    SleepFor(2000000);
    s.Post();
  });
  SND_CHECK(s.WaitFor(5000000000ull));
  poster.join();
}

SND_TEST(Semaphore, PingPongUnderLoad) {
  Semaphore ping, pong;
  constexpr int kRounds = 20000;
  std::uint64_t shared = 0;  // plain data handed over by the semaphore (TSan checks the ordering)
  std::thread other([&] {
    SND_RT_SCOPE();
    for (int i = 0; i < kRounds; ++i) {
      ping.Wait();
      ++shared;
      pong.Post();
    }
  });
  for (int i = 0; i < kRounds; ++i) {
    ++shared;
    ping.Post();
    pong.Wait();
  }
  other.join();
  SND_CHECK_EQ(shared, 2u * kRounds);
}

SND_TEST(Semaphore, PostManyWakesAllWaiters) {
  Semaphore s;
  std::atomic<int> woke{0};
  std::vector<std::thread> waiters;
  for (int i = 0; i < 6; ++i) {
    waiters.emplace_back([&] {
      for (int k = 0; k < 500; ++k) s.Wait();
      woke.fetch_add(1);
    });
  }
  for (int k = 0; k < 500; ++k) s.Post(6);
  for (auto& t : waiters) t.join();
  SND_CHECK_EQ(woke.load(), 6);
  SND_CHECK_EQ(s.ValueApprox(), 0);
}

SND_TEST(Cpu, FeaturesAndFlushDenormals) {
  const CpuFeatures& f = GetCpuFeatures();
  SND_CHECK(f.logicalCores >= 1u);
#if defined(__x86_64__)
  SND_CHECK(f.sse2);
  if (f.avx2) SND_CHECK(f.avx);
  if (f.fma) SND_CHECK(f.avx);
#elif defined(__aarch64__)
  SND_CHECK(f.neon);
#endif
  snd::test::Note("cpu: sse4.1=%d avx=%d avx2=%d fma=%d avx512f=%d neon=%d cores=%u", f.sse41, f.avx, f.avx2,
                  f.fma, f.avx512f, f.neon, f.logicalCores);
  if (!f.denormalControl) return;

  const FpControl before = SetFlushDenormals(false);
  volatile float tiny = 1e-39f;  // denormal
  volatile float scale = 0.5f;
  float r = tiny * scale;
  SND_CHECK(r != 0.0f);  // gradual underflow without FTZ
  {
    SND_RT_SCOPE();
    ScopedFlushDenormals ftz;
    SND_CHECK(FlushDenormalsEnabled());
    r = tiny * scale;
    SND_CHECK_EQ(r, 0.0f);
  }
  SND_CHECK(!FlushDenormalsEnabled());
  RestoreFpControl(before);
}

namespace {
struct DeviceProbe {
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint32_t> frames{0};
  std::atomic<std::uint32_t> channels{0};
  std::atomic<bool> ftz{true};
  std::atomic<bool> rtViolation{false};
  static void Callback(void* user, float* out, std::uint32_t frames, std::uint32_t channels) {
    auto* p = static_cast<DeviceProbe*>(user);
    SND_RT_SCOPE();  // the device callback must obey principle 1
    for (std::uint32_t i = 0; i < frames * channels; ++i) out[i] = 0.25f;  // discarded
    p->frames = frames;
    p->channels = channels;
    if (GetCpuFeatures().denormalControl && !FlushDenormalsEnabled()) p->ftz = false;
    p->calls.fetch_add(1, std::memory_order_relaxed);
  }
};
}  // namespace

SND_TEST(TimerDevice, CallsBackInRealTimeAndStops) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  {
    TimerOutputDevice dev(acc);
    DeviceProbe probe;
    snd::DeviceDesc desc{48000, 2, 240, 0};  // 5 ms periods
    SND_REQUIRE(dev.Open(desc, &DeviceProbe::Callback, &probe));
    SND_CHECK(!dev.Open(desc, &DeviceProbe::Callback, &probe));  // already open
    SND_CHECK_EQ(dev.SampleRate(), 48000u);
    SND_CHECK_EQ(dev.Channels(), 2u);
    SND_CHECK_EQ(dev.PeriodFrames(), 240u);
    SND_CHECK_NEAR(dev.OutputLatencySeconds(), 0.005, 1e-9);
    SND_CHECK_EQ(probe.calls.load(), 0u);  // nothing before Start
    SND_REQUIRE(dev.Start());
    SND_CHECK(dev.Start());  // idempotent
    const std::uint64_t t0 = MonotonicNanoseconds();
    SleepFor(200000000);  // 200 ms ~ 40 periods
    dev.Stop();
    const std::uint64_t elapsedMs = (MonotonicNanoseconds() - t0) / 1000000u;
    const std::uint64_t calls = probe.calls.load();
    snd::test::Note("timer device: %llu callbacks in %llu ms, late=%llu, priority=%s",
                    static_cast<unsigned long long>(calls), static_cast<unsigned long long>(elapsedMs),
                    static_cast<unsigned long long>(dev.LateWakeups()),
                    PriorityResultName(dev.ThreadPriorityResult()));
    // Real time, generous bounds for loaded CI machines / sanitizers.
    SND_CHECK(calls >= 10u);
    SND_CHECK(calls <= elapsedMs / 5u + 3u);
    SND_CHECK_EQ(dev.Callbacks(), calls);
    SND_CHECK_EQ(dev.FramesDelivered(), calls * 240u);
    SND_CHECK_EQ(probe.frames.load(), 240u);
    SND_CHECK_EQ(probe.channels.load(), 2u);
    SND_CHECK(probe.ftz.load());
    SleepFor(20000000);
    SND_CHECK_EQ(probe.calls.load(), calls);  // Stop returned after the last callback
    // Restart works.
    SND_REQUIRE(dev.Start());
    SleepFor(30000000);
    dev.Stop();
    SND_CHECK(probe.calls.load() > calls);
    dev.Close();
    SND_CHECK(!dev.IsOpen());
    SND_CHECK(!dev.Start());
  }
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(TimerDevice, DefaultsRejectsAndFactoryThroughCTable) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  DeviceProbe probe;
  {
    TimerOutputDevice bad(acc);
    snd::DeviceDesc tooMany{0, SND_MAX_CHANNELS + 1u, 0, 0};
    SND_CHECK(!bad.Open(tooMany, &DeviceProbe::Callback, &probe));
    SND_CHECK(!bad.Open(snd::DeviceDesc{}, nullptr, &probe));
  }
  snd::IOutputDevice* dev = CreateOutputDevice(OutputBackend::Default, acc);
  SND_REQUIRE(dev != nullptr);
  snd_output_device c = snd::ToC(*dev);  // as passed through Config.output.device
  snd_device_desc desc{0, 0, 0, 0};
  SND_REQUIRE_EQ(c.open(c.user, &desc, &DeviceProbe::Callback, &probe), 1);
  SND_CHECK_EQ(c.sample_rate(c.user), 48000u);
  SND_CHECK_EQ(c.channels(c.user), 2u);
  SND_CHECK_EQ(c.period_frames(c.user), 256u);
  c.set_event_listener(c.user, nullptr, nullptr);
  SND_REQUIRE_EQ(c.start(c.user), 1);
  const std::uint64_t deadline = MonotonicNanoseconds() + 2000000000ull;
  while (probe.calls.load() < 3u && MonotonicNanoseconds() < deadline) SleepFor(1000000);
  c.stop(c.user);
  c.close(c.user);
  SND_CHECK(probe.calls.load() >= 3u);
  DestroyOutputDevice(dev, acc);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

namespace {
struct SelfStopper {
  TimerOutputDevice* dev = nullptr;
  std::atomic<std::uint64_t> calls{0};
  static void Callback(void* user, float*, std::uint32_t, std::uint32_t) {
    auto* s = static_cast<SelfStopper*>(user);
    if (s->calls.fetch_add(1) == 2u) {
      s->dev->Stop();   // forbidden: ignored, counted
      s->dev->Close();  // forbidden: ignored, counted
    }
  }
};
}  // namespace

SND_TEST(TimerDevice, StopAndCloseFromCallbackAreIgnoredAndCounted) {
  TimerDeviceConfig cfg;
  cfg.realtimeThread = false;
  TimerOutputDevice dev(snd::base::DefaultAllocator(), cfg);
  SelfStopper s;
  s.dev = &dev;
  SND_REQUIRE(dev.Open(snd::DeviceDesc{48000, 2, 96, 0}, &SelfStopper::Callback, &s));
  SND_REQUIRE(dev.Start());
  const std::uint64_t deadline = MonotonicNanoseconds() + 5000000000ull;
  while (s.calls.load() < 10u && MonotonicNanoseconds() < deadline) SleepFor(1000000);
  SND_CHECK(s.calls.load() >= 10u);  // kept running after the ignored Stop/Close
  SND_CHECK_EQ(dev.MisuseFromCallback(), 2u);
  SND_CHECK(dev.IsOpen());
  dev.Close();  // from the owner thread: works
  SND_CHECK(!dev.IsOpen());
  SND_CHECK(!dev.IsStarted());
}
