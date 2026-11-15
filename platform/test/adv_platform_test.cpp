// Independent tests of platform: semaphore, clock, threads, FTZ/DAZ and the
// timer-driven output device (design.md 1.2, 2.2, 2.3, 2.4, D-25).
//
// Timing tolerances: the suite must not fail spuriously on a loaded 4-core
// machine (other builds running at the same time, no realtime priority in a
// container). Lower bounds that follow from the API ("waits at least",
// "never before the deadline") are strict; upper bounds are generous (a
// descheduled thread can lose tens of milliseconds) and are documented at
// each check.
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/base/atomic.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/platform/semaphore.hpp"
#include "snd/platform/thread.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {
std::uint64_t NowNs() { return MonotonicNanoseconds(); }
constexpr std::uint64_t kMs = 1000000ull;
}  // namespace

// --- Semaphore ------------------------------------------------------------------------

SND_TEST(AdvSemaphore, PostBeforeWaitAndInitialCount) {
  Semaphore s(2);
  SND_CHECK_EQ(s.ValueApprox(), 2);
  s.Post(3);
  for (int i = 0; i < 5; ++i) SND_CHECK(s.TryWait());
  SND_CHECK(!s.TryWait());
  s.Post(0);  // no-op
  SND_CHECK(!s.TryWait());
  s.Post();
  const std::uint64_t t0 = NowNs();
  s.Wait();  // permit already there: must not block
  SND_CHECK(NowNs() - t0 < 500 * kMs);
  SND_CHECK(!s.WaitFor(0));  // zero timeout with no permit = TryWait
  s.Post();
  SND_CHECK(s.WaitFor(0));
  SND_CHECK_EQ(s.ValueApprox(), 0);
}

SND_TEST(AdvSemaphore, WaitForTimeoutNeverEarlyAndBoundedLate) {
  Semaphore s;
  for (std::uint64_t ms : {1ull, 5ull, 20ull}) {
    const std::uint64_t t0 = NowNs();
    const bool got = s.WaitFor(ms * kMs);
    const std::uint64_t dt = NowNs() - t0;
    SND_CHECK(!got);
    SND_CHECK_MSG(dt >= ms * kMs, "WaitFor(%llu ms) returned after %llu us (early)", static_cast<unsigned long long>(ms),
                  static_cast<unsigned long long>(dt / 1000u));
    // Upper bound 250 ms: a loaded machine may deschedule us, but a futex
    // timeout that is never re-armed (spurious early return loop) or a
    // seconds/nanoseconds mix-up would blow far past it.
    SND_CHECK_MSG(dt < ms * kMs + 250 * kMs, "WaitFor(%llu ms) returned after %llu us", static_cast<unsigned long long>(ms),
                  static_cast<unsigned long long>(dt / 1000u));
  }
  // A timeout above one second exercises tv_sec; a post after 30 ms ends it.
  std::thread poster([&] {
    SleepFor(30 * kMs);
    s.Post();
  });
  const std::uint64_t t0 = NowNs();
  SND_CHECK(s.WaitFor(3000 * kMs));
  SND_CHECK(NowNs() - t0 < 2000 * kMs);
  poster.join();
}

SND_TEST(AdvSemaphore, PostNWakesExactlyNOfManyWaiters) {
  Semaphore s;
  constexpr int kWaiters = 6;
  std::atomic<int> woke{0};
  std::atomic<int> started{0};
  std::vector<std::thread> ws;
  for (int i = 0; i < kWaiters; ++i) {
    ws.emplace_back([&] {
      started.fetch_add(1);
      if (s.WaitFor(5000 * kMs)) woke.fetch_add(1);
    });
  }
  while (started.load() < kWaiters) std::this_thread::yield();
  SleepFor(20 * kMs);  // let them block in the kernel
  s.Post(2);
  // Settles to exactly 2 (more would mean permits created from nothing).
  const std::uint64_t t0 = NowNs();
  while (woke.load() < 2 && NowNs() - t0 < 3000 * kMs) std::this_thread::yield();
  SleepFor(50 * kMs);
  SND_CHECK_EQ(woke.load(), 2);
  s.Post(kWaiters - 2);
  for (auto& t : ws) t.join();
  SND_CHECK_EQ(woke.load(), kWaiters);
  SND_CHECK_EQ(s.ValueApprox(), 0);
}

SND_TEST(AdvSemaphore, NoLostWakeupsManyProducersManyConsumers) {
  // Every Post must be matched by exactly one successful Wait. Each consumer
  // uses WaitFor with a long timeout as a hang guard; a lost wake-up would
  // show up as a timeout (counted) instead of hanging the test.
  Semaphore s;
  constexpr int kProducers = 3, kConsumers = 3;
  constexpr int kPerProducer = 20000;
  std::atomic<int> consumed{0}, timeouts{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  for (int c = 0; c < kConsumers; ++c) {
    ts.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        if (s.WaitFor(2000 * kMs)) consumed.fetch_add(1);
        else if (!stop.load()) timeouts.fetch_add(1);
      }
    });
  }
  std::vector<std::thread> ps;
  for (int p = 0; p < kProducers; ++p) {
    ps.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) {
        s.Post(1u + static_cast<std::uint32_t>((i + p) % 2));
        if (i % 64 == 0) std::this_thread::yield();
      }
    });
  }
  for (auto& t : ps) t.join();
  const int expected = kProducers * (kPerProducer / 2) * 3;  // posts alternate 1 and 2 permits
  const std::uint64_t t0 = NowNs();
  while (consumed.load() < expected && NowNs() - t0 < 20000 * kMs) std::this_thread::yield();
  stop = true;
  s.Post(kConsumers);
  for (auto& t : ts) t.join();
  // The final Post(kConsumers) only unblocks the consumers; it may add up to kConsumers.
  SND_CHECK_MSG(consumed.load() >= expected && consumed.load() <= expected + kConsumers, "consumed %d of %d permits",
                consumed.load(), expected);
  SND_CHECK_EQ(timeouts.load(), 0);
}

SND_TEST(AdvSemaphore, WaitAndPostAreRtSafe) {
  Semaphore s;
  snd::test::rt::Reset();
  {
    SND_RT_SCOPE();
    s.Post(2);
    s.Wait();
    SND_CHECK(s.TryWait());
    SND_CHECK(!s.WaitFor(100000));  // 100 us
  }
  SND_CHECK_EQ(snd::test::rt::Total(), 0u);
}

// --- Clock ------------------------------------------------------------------------------

SND_TEST(AdvClock, MonotonicAcrossThreadsAndSleepNeverEarly) {
  // A value published by one thread (release) is never ahead of a clock read
  // made by another thread after acquiring it.
  std::atomic<std::uint64_t> published{0};
  std::atomic<std::uint64_t> violations{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t) {
    ts.emplace_back([&] {
      std::uint64_t last = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const std::uint64_t seen = published.load(std::memory_order_acquire);
        const std::uint64_t now = MonotonicNanoseconds();
        if (now < seen || now < last) violations.fetch_add(1);
        last = now;
        snd::base::AtomicFetchMax(published, now, std::memory_order_release);
      }
    });
  }
  SleepFor(200 * kMs);
  stop = true;
  for (auto& t : ts) t.join();
  SND_CHECK_EQ(violations.load(), 0u);
  for (std::uint64_t ns : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{50000}, 2 * kMs}) {
    const std::uint64_t t0 = MonotonicNanoseconds();
    SleepFor(ns);
    SND_CHECK(MonotonicNanoseconds() - t0 >= ns);
  }
  const std::uint64_t past = MonotonicNanoseconds() - 10 * kMs;
  const std::uint64_t t0 = MonotonicNanoseconds();
  SleepUntil(past);  // deadline in the past: returns at once
  SND_CHECK(MonotonicNanoseconds() - t0 < 200 * kMs);
  SND_CHECK_EQ(MonotonicMicroseconds() / 1000u, MonotonicNanoseconds() / 1000000u);
}

// --- Threads ------------------------------------------------------------------------------

namespace {
void Increment(void* p) { static_cast<std::atomic<int>*>(p)->fetch_add(1); }
}  // namespace

SND_TEST(AdvThread, StartJoinRestartAndMisuse) {
  std::atomic<int> n{0};
  Thread t;
  SND_CHECK(!t.Joinable());
  t.Join();  // not started: no-op
  ThreadDesc d;
  d.name = "a-very-long-thread-name-over-15-bytes";
  d.stackBytes = 256u << 10;
  SND_REQUIRE(t.Start(d, &Increment, &n));
  SND_CHECK(t.Joinable());
  SND_CHECK(!t.Start(d, &Increment, &n));  // second start while running fails
  t.Join();
  t.Join();  // twice is a no-op
  SND_CHECK(!t.Joinable());
  SND_CHECK_EQ(n.load(), 1);
  for (int i = 0; i < 50; ++i) {  // restart the same object many times
    SND_REQUIRE(t.Start(ThreadDesc{}, &Increment, &n));
    t.Join();
  }
  SND_CHECK_EQ(n.load(), 51);
  SND_CHECK(!t.Start(ThreadDesc{}, nullptr, &n));
  ThreadDesc nameless;
  nameless.name = nullptr;
  SND_REQUIRE(t.Start(nameless, &Increment, &n));
  t.Join();
  {
    Thread scoped;  // destructor joins
    SND_REQUIRE(scoped.Start(ThreadDesc{}, &Increment, &n));
  }
  SND_CHECK_EQ(n.load(), 53);
}

SND_TEST(AdvThread, RealtimeRequestIsAppliedOrReportedAsFallback) {
  std::atomic<int> n{0};
  for (int prio : {-5, 1, 80, 1000}) {  // out-of-range priorities are clamped, never fatal
    Thread t;
    ThreadDesc d;
    d.priority = ThreadPriority::Realtime;
    d.realtimePriority = prio;
    SND_REQUIRE(t.Start(d, &Increment, &n));
    const PriorityResult r = t.Priority();
    SND_CHECK_MSG(r == PriorityResult::Applied || r == PriorityResult::FellBack, "prio %d -> %s", prio,
                  PriorityResultName(r));
    t.Join();
  }
  SND_CHECK_EQ(n.load(), 4);
  Thread normal;
  SND_REQUIRE(normal.Start(ThreadDesc{}, &Increment, &n));
  SND_CHECK(normal.Priority() == PriorityResult::Applied);
  normal.Join();
  SND_CHECK(SetCurrentThreadPriority(ThreadPriority::Normal) == PriorityResult::Applied);
  SND_CHECK(HardwareConcurrency() >= 1u);
}

// --- FTZ / DAZ ------------------------------------------------------------------------------

namespace {
// volatile everywhere: the compiler must not fold the denormal arithmetic.
float MulTiny() {
  volatile float a = 1e-30f, b = 1e-10f;  // product 1e-40 is subnormal
  volatile float r = a * b;
  return r;
}
float AddDenormalInput() {
  volatile float d = 1e-40f;  // subnormal input
  volatile float z = 0.0f;
  volatile float r = d + z;   // DAZ treats d as zero
  return r;
}
}  // namespace

SND_TEST(AdvCpu, FlushDenormalsReallyFlushesRestoresAndIsPerThread) {
  if (!GetCpuFeatures().denormalControl) {
    snd::test::Note("no denormal control on this CPU: skipped");
    return;
  }
  const FpControl original = SetFlushDenormals(false);
  SND_CHECK(MulTiny() != 0.0f);           // gradual underflow by default
  SND_CHECK(AddDenormalInput() != 0.0f);
  const FpControl prev = SetFlushDenormals(true);
  SND_CHECK(FlushDenormalsEnabled());
  SND_CHECK_EQ(MulTiny(), 0.0f);          // FTZ: subnormal result -> 0
#if defined(__x86_64__) || defined(__i386__)
  SND_CHECK_EQ(AddDenormalInput(), 0.0f);  // DAZ: subnormal input -> 0
#endif
  // Another thread started now has its own control word; enabling there does
  // not leak back, disabling there does not disable here.
  std::atomic<int> otherFlushed{-1};
  std::thread other([&] {
    SetFlushDenormals(false);
    otherFlushed.store(MulTiny() == 0.0f ? 1 : 0);
  });
  other.join();
  SND_CHECK_EQ(otherFlushed.load(), 0);
  SND_CHECK(FlushDenormalsEnabled());       // unaffected by the other thread
  SND_CHECK_EQ(MulTiny(), 0.0f);
  RestoreFpControl(prev);
  SND_CHECK(!FlushDenormalsEnabled());
  SND_CHECK(MulTiny() != 0.0f);
  {
    ScopedFlushDenormals scope;
    SND_CHECK_EQ(MulTiny(), 0.0f);
    {
      ScopedFlushDenormals nested;  // nesting restores to "on", not "off"
    }
    SND_CHECK(FlushDenormalsEnabled());
  }
  SND_CHECK(!FlushDenormalsEnabled());
  // Other MXCSR bits (rounding mode) survive Set/Restore.
  RestoreFpControl(original);
}

// --- Timer output device ------------------------------------------------------------------------

namespace {
struct CbStats {
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> lastNs{0};
  std::atomic<std::uint64_t> sumIntervalNs{0};
  std::atomic<std::uint64_t> maxIntervalNs{0};
  std::atomic<std::uint32_t> frames{0};
  std::atomic<std::uint32_t> channels{0};
  std::atomic<int> inCallback{0};
  std::atomic<int> concurrent{0};
  std::atomic<int> sleepMs{0};
  std::atomic<std::uint64_t> afterStop{0};
  std::atomic<bool> stopped{false};
  std::atomic<int> ftz{-1};
};

void SND_CALL Callback(void* user, float* interleaved, std::uint32_t frames, std::uint32_t channels) {
  auto* s = static_cast<CbStats*>(user);
  if (s->inCallback.fetch_add(1) != 0) s->concurrent.fetch_add(1);
  if (s->stopped.load()) s->afterStop.fetch_add(1);
  const std::uint64_t now = MonotonicNanoseconds();
  const std::uint64_t last = s->lastNs.exchange(now);
  if (last != 0u) {
    s->sumIntervalNs.fetch_add(now - last);
    snd::base::AtomicFetchMax(s->maxIntervalNs, now - last);
  }
  s->frames.store(frames);
  s->channels.store(channels);
  s->ftz.store(FlushDenormalsEnabled() ? 1 : 0);
  // The device owns the buffer: writing all of it must be safe (ASan).
  for (std::uint32_t i = 0; i < frames * channels; ++i) interleaved[i] = 0.25f;
  const int ms = s->sleepMs.load();
  if (ms > 0) SleepFor(static_cast<std::uint64_t>(ms) * kMs);
  s->calls.fetch_add(1);
  s->inCallback.fetch_sub(1);
}

TimerDeviceConfig NonRtConfig() {
  TimerDeviceConfig c;
  c.realtimeThread = false;  // keep a runaway loop from starving the machine
  return c;
}
}  // namespace

SND_TEST(AdvTimerDevice, OperationsBeforeOpenAndRequestedFormat) {
  TimerOutputDevice dev(snd::base::DefaultAllocator(), NonRtConfig());
  SND_CHECK(!dev.Start());  // not open
  dev.Stop();               // no-ops
  dev.Close();
  SND_CHECK_EQ(dev.PeriodFrames(), 0u);
  SND_CHECK_EQ(dev.SampleRate(), 0u);
  SND_CHECK_EQ(dev.Channels(), 0u);
  CbStats st;
  snd::DeviceDesc desc{44100, 6, 333, 0};
  SND_REQUIRE(dev.Open(desc, &Callback, &st));
  SND_CHECK(!dev.Open(desc, &Callback, &st));  // already open
  SND_CHECK_EQ(dev.SampleRate(), 44100u);
  SND_CHECK_EQ(dev.Channels(), 6u);
  SND_CHECK_EQ(dev.PeriodFrames(), 333u);
  SND_CHECK_NEAR(dev.OutputLatencySeconds(), 333.0 / 44100.0, 1e-12);
  SND_CHECK_EQ(st.calls.load(), 0u);  // no callback before Start
  SND_REQUIRE(dev.Start());
  SND_CHECK(dev.Start());  // second start: already running, true, no second thread
  const std::uint64_t t0 = NowNs();
  while (st.calls.load() < 3u && NowNs() - t0 < 2000 * kMs) SleepFor(kMs);
  dev.Stop();
  SND_CHECK(st.calls.load() >= 3u);
  SND_CHECK_EQ(st.frames.load(), 333u);
  SND_CHECK_EQ(st.channels.load(), 6u);
  SND_CHECK_EQ(st.concurrent.load(), 0);
  SND_CHECK_EQ(st.ftz.load(), 1);  // 2.4: device thread runs with FTZ/DAZ
  SND_CHECK_EQ(dev.FramesDelivered(), dev.Callbacks() * 333u);
  dev.Close();
  SND_CHECK(!dev.IsOpen());
  SND_CHECK(!dev.Start());
  // Reopen with defaults from the config (desc zeros).
  TimerDeviceConfig cfg = NonRtConfig();
  cfg.sampleRate = 22050;
  cfg.channels = 1;
  cfg.periodFrames = 64;
  cfg.latencySeconds = 0.125;
  TimerOutputDevice dev2(snd::base::DefaultAllocator(), cfg);
  SND_REQUIRE(dev2.Open(snd::DeviceDesc{0, 0, 0, 0}, &Callback, &st));
  SND_CHECK_EQ(dev2.SampleRate(), 22050u);
  SND_CHECK_EQ(dev2.Channels(), 1u);
  SND_CHECK_EQ(dev2.PeriodFrames(), 64u);
  SND_CHECK_NEAR(dev2.OutputLatencySeconds(), 0.125, 1e-12);
  // Bad arguments.
  TimerOutputDevice dev3(snd::base::DefaultAllocator(), NonRtConfig());
  SND_CHECK(!dev3.Open(snd::DeviceDesc{48000, SND_MAX_CHANNELS + 1u, 256, 0}, &Callback, &st));
  SND_CHECK(!dev3.Open(snd::DeviceDesc{48000, 2, 256, 0}, nullptr, &st));
  SND_CHECK(dev3.Open(snd::DeviceDesc{48000, SND_MAX_CHANNELS, 256, 0}, &Callback, &st));
}

SND_TEST(AdvTimerDevice, PeriodAverageAndNoBursts) {
  // 48 kHz / 480 frames = 10 ms. Over ~1.2 s the device must deliver about
  // elapsed/period callbacks: never more than elapsed/period + 2 (absolute
  // deadlines: no catch-up bursts) and, on a loaded machine without RT
  // priority, at least 60 % of them (a missed period is resynchronised, not
  // replayed). The mean interval is checked against 10 ms within 20 %, the
  // worst single interval only loosely (< 250 ms).
  TimerOutputDevice dev(snd::base::DefaultAllocator(), NonRtConfig());
  CbStats st;
  SND_REQUIRE(dev.Open(snd::DeviceDesc{48000, 2, 480, 0}, &Callback, &st));
  const std::uint64_t t0 = NowNs();
  SND_REQUIRE(dev.Start());
  SleepFor(1200 * kMs);
  dev.Stop();
  const std::uint64_t elapsed = NowNs() - t0;
  const std::uint64_t calls = st.calls.load();
  const double expected = static_cast<double>(elapsed) / 10e6;
  snd::test::Note("calls=%llu expected~%.1f late=%llu mean=%.3f ms max=%.3f ms", static_cast<unsigned long long>(calls),
                  expected, static_cast<unsigned long long>(dev.LateWakeups()),
                  calls > 1 ? static_cast<double>(st.sumIntervalNs.load()) / static_cast<double>(calls - 1) / 1e6 : 0.0,
                  static_cast<double>(st.maxIntervalNs.load()) / 1e6);
  SND_CHECK(static_cast<double>(calls) <= expected + 2.0);
  SND_CHECK(static_cast<double>(calls) >= expected * 0.6);
  if (calls > 10u) {
    const double mean = static_cast<double>(st.sumIntervalNs.load()) / static_cast<double>(calls - 1u);
    SND_CHECK_MSG(mean > 8e6 && mean < 12e6 * (dev.LateWakeups() > 0 ? 1.5 : 1.0), "mean interval %.3f ms", mean / 1e6);
  }
  SND_CHECK(st.maxIntervalNs.load() < 250 * kMs);
}

SND_TEST(AdvTimerDevice, StopWaitsForRunningCallbackAndNoneAfter) {
  TimerOutputDevice dev(snd::base::DefaultAllocator(), NonRtConfig());
  CbStats st;
  st.sleepMs.store(40);  // long callbacks: Stop is likely to arrive mid-callback
  SND_REQUIRE(dev.Open(snd::DeviceDesc{48000, 2, 48, 0}, &Callback, &st));
  for (int round = 0; round < 5; ++round) {
    SND_REQUIRE(dev.Start());
    while (st.inCallback.load() == 0) std::this_thread::yield();  // a callback is running now
    dev.Stop();
    SND_CHECK_EQ(st.inCallback.load(), 0);  // returned after the last callback finished
    st.stopped.store(true);
    const std::uint64_t calls = st.calls.load();
    SleepFor(60 * kMs);
    SND_CHECK_EQ(st.calls.load(), calls);  // nothing after Stop
    SND_CHECK_EQ(st.afterStop.load(), 0u);
    st.stopped.store(false);
  }
  SND_CHECK_EQ(st.concurrent.load(), 0);
}

SND_TEST(AdvTimerDevice, RapidStartStopCyclesAndDestroyWhileRunning) {
  CbStats st;
  {
    TimerOutputDevice dev(snd::base::DefaultAllocator(), NonRtConfig());
    SND_REQUIRE(dev.Open(snd::DeviceDesc{96000, 2, 32, 0}, &Callback, &st));
    for (int i = 0; i < 200; ++i) {
      SND_REQUIRE(dev.Start());
      if (i % 3 == 0) SleepFor(200000);
      dev.Stop();
      SND_CHECK(!dev.IsStarted());
    }
    SND_REQUIRE(dev.Start());
    SleepFor(5 * kMs);
    // Destructor while running: must stop, join and free (ASan/TSan).
  }
  SND_CHECK_EQ(st.concurrent.load(), 0);
  SND_CHECK_EQ(st.inCallback.load(), 0);
  // Close while running.
  TimerOutputDevice dev(snd::base::DefaultAllocator(), NonRtConfig());
  SND_REQUIRE(dev.Open(snd::DeviceDesc{48000, 2, 64, 0}, &Callback, &st));
  SND_REQUIRE(dev.Start());
  SleepFor(5 * kMs);
  dev.Close();
  SND_CHECK(!dev.IsStarted());
  SND_CHECK(!dev.IsOpen());
  const std::uint64_t calls = st.calls.load();
  SleepFor(30 * kMs);
  SND_CHECK_EQ(st.calls.load(), calls);
}
