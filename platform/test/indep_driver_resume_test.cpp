// Independent tests of AudioDriver::Resume() (65e2863; design.md 2.6, D-63,
// D-66): resuming a device left SUSPENDED (iOS interruption without
// ShouldResume), calls before Start / while running / after Stop / from the
// render function, and Stats() read while the owner resumes (TSan).
//
// SuspendableDevice: a paced PlatformOutputDevice whose test can suspend it
// (callbacks stop, SUSPENDED posted). Start() on a suspended device resumes it
// and the device thread posts RESUMED before its next callback (D-63). At that
// first callback it records the driver's fill against the target.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/audio_driver.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {

class SuspendableDevice final : public PlatformOutputDevice {
 public:
  const AudioDriver* driver = nullptr;  // for the fill check at the first callback after a resume
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> silentFrames{0};
  std::atomic<std::int64_t> fillAtResume{-1};    // queued frames at the first callback after RESUMED
  std::atomic<std::int64_t> targetAtResume{-1};
  std::atomic<std::uint64_t> silentAfterResume{0};
  std::atomic<std::uint32_t> burst{0};  // > 0: that many callbacks flat out, then suspend
  std::atomic<std::uint32_t> starts{0};

  ~SuspendableDevice() override { Close(); }
  OutputBackend Backend() const noexcept override { return OutputBackend::SilentTimer; }
  bool Open(const snd::DeviceDesc& d, snd::DeviceCallback cb, void* user) override {
    if (open_ || cb == nullptr) return false;
    rate_ = d.sample_rate != 0u ? d.sample_rate : 48000u;
    ch_ = d.channels != 0u ? d.channels : 2u;
    period_ = d.period_frames != 0u ? d.period_frames : 256u;
    buf_.assign(static_cast<std::size_t>(period_) * ch_, 0.0f);
    cb_ = cb;
    user_ = user;
    open_ = true;
    return true;
  }
  bool Start() override {
    if (!open_) return false;
    starts.fetch_add(1);
    if (started_) {
      if (suspended_.exchange(false)) resumedPending_.store(true);
      return true;
    }
    running_.store(true);
    thread_ = std::thread([this] { Run(); });
    started_ = true;
    return true;
  }
  void Stop() override {
    if (!started_) return;
    running_.store(false);
    thread_.join();
    started_ = false;
  }
  void Close() override {
    Stop();
    open_ = false;
  }
  std::uint32_t PeriodFrames() const override { return period_; }
  std::uint32_t SampleRate() const override { return rate_; }
  std::uint32_t Channels() const override { return ch_; }
  double OutputLatencySeconds() const override { return 0.01; }
  bool Suspended() const { return suspended_.load(); }
  void Emit(snd::DeviceEventKind k, std::uint32_t ch, std::uint32_t rate, std::uint32_t period) {
    PostEvent(k, ch, rate, period);
  }
  void SuspendNow() {
    suspended_.store(true);
    PostEvent(snd::DeviceEventKind::Suspended, 0, 0, 0);
  }

 private:
  void Run() {
    std::uint64_t due = MonotonicNanoseconds();
    bool measure = false;
    while (running_.load()) {
      if (suspended_.load()) {
        SleepFor(1000000);
        due = MonotonicNanoseconds();
        continue;
      }
      if (resumedPending_.exchange(false)) {
        PostEvent(snd::DeviceEventKind::Resumed, ch_, rate_, period_);
        measure = true;
      }
      if (measure && driver != nullptr) {
        const DriverStats s = driver->Core().Stats();
        fillAtResume.store(s.queuedFrames);
        targetAtResume.store(s.targetFrames);
      }
      cb_(user_, buf_.data(), period_, ch_);
      std::uint64_t silent = 0;
      for (std::uint32_t f = 0; f < period_; ++f) {
        bool z = true;
        for (std::uint32_t c = 0; c < ch_; ++c) z = z && buf_[f * ch_ + c] == 0.0f;
        if (z) ++silent;
      }
      silentFrames.fetch_add(silent);
      if (measure) silentAfterResume.store(silent);
      measure = false;
      calls.fetch_add(1);
      const std::uint32_t b = burst.load();
      if (b > 0u) {
        if (b == 1u) SuspendNow();
        burst.store(b - 1u);
        continue;  // flat out
      }
      due += static_cast<std::uint64_t>(period_) * 1000000000ull / rate_;
      const std::uint64_t now = MonotonicNanoseconds();
      if (due > now) {
        SleepUntil(due);
      } else if (now - due > 20000000ull) {
        due = now;
      }
    }
  }

  std::uint32_t rate_ = 0, ch_ = 0, period_ = 0;
  std::vector<float> buf_;
  snd::DeviceCallback cb_ = nullptr;
  void* user_ = nullptr;
  bool open_ = false, started_ = false;
  std::atomic<bool> running_{false};
  std::atomic<bool> suspended_{false};
  std::atomic<bool> resumedPending_{false};
  std::thread thread_;
};

struct Renderer {
  std::atomic<std::uint32_t> sleepMs{0};
  std::atomic<std::uint64_t> blocks{0};
  std::atomic<int> resumeFromRender{0};
  AudioDriver* driver = nullptr;
  std::atomic<int> resumeResult{-1};
  static void Render(void* u, float* out, std::uint32_t frames, std::uint32_t ch, const RenderInfo*) {
    auto* r = static_cast<Renderer*>(u);
    if (r->sleepMs.load() > 0u) {
      snd::rt::SuspendScope sus;  // test-only: a slow render
      SleepFor(static_cast<std::uint64_t>(r->sleepMs.load()) * 1000000ull);
    }
    if (r->resumeFromRender.load() > 0) {
      r->resumeFromRender.fetch_sub(1);
      r->resumeResult.store(r->driver->Resume() ? 1 : 0);
    }
    for (std::uint32_t i = 0; i < frames * ch; ++i) out[i] = 0.25f;
    r->blocks.fetch_add(1);
  }
};

struct Events {
  std::atomic<std::uint32_t> suspended{0}, resumed{0};
  static void Fn(void* u, const snd::DeviceEvent* e) {
    auto* s = static_cast<Events*>(u);
    if (e->kind == SND_DEVICE_EVENT_SUSPENDED) s->suspended.fetch_add(1);
    if (e->kind == SND_DEVICE_EVENT_RESUMED) s->resumed.fetch_add(1);
  }
};

AudioDriverDesc Desc(Renderer& r, Events* ev, std::uint32_t block, std::uint32_t ahead) {
  AudioDriverDesc d;
  d.core.blockFrames = block;
  d.core.renderAheadBlocks = ahead;
  d.core.catchupBlocks = 1;
  d.core.fadeFrames = 0;
  d.core.render = &Renderer::Render;
  d.core.user = &r;
  d.realtimeThread = false;
  if (ev != nullptr) {
    d.listener = &Events::Fn;
    d.listenerUser = ev;
  }
  return d;
}

template <class F>
bool WaitUntil(F f, std::uint64_t ms = 20000) {
  const std::uint64_t deadline = MonotonicNanoseconds() + ms * 1000000ull;
  while (!f()) {
    if (MonotonicNanoseconds() > deadline) return false;
    SleepFor(500000);
  }
  return true;
}

}  // namespace

// Calls outside the running state and from the render function.
SND_TEST(IndepDriverResume, RefusedWhenNotRunningOrFromRender) {
  SuspendableDevice dev;
  Renderer r;
  AudioDriver driver;
  r.driver = &driver;
  SND_CHECK(!driver.Resume());  // before Start
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 256, 0}, Desc(r, nullptr, 128, 2),
                           snd::base::DefaultAllocator()));
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= 5u; }));
  // While running and not suspended: harmless (the device just stays started).
  SND_CHECK(driver.Resume());
  const std::uint64_t c = dev.calls.load();
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= c + 10u; }));
  // From the render function: refused and counted (would re-enter the device).
  const std::uint64_t misuse = driver.MisuseCount();
  r.resumeFromRender.store(1);
  SND_REQUIRE(WaitUntil([&] { return r.resumeResult.load() >= 0; }));
  SND_CHECK_EQ(r.resumeResult.load(), 0);
  SND_CHECK_EQ(driver.MisuseCount(), misuse + 1u);
  driver.Stop();
  SND_CHECK(!driver.Resume());  // after Stop
  SND_CHECK_EQ(dev.starts.load(), 2u);  // Start + the one Resume while running
}

// SUSPENDED -> Resume -> RESUMED; the device plays again, no silence.
SND_TEST(IndepDriverResume, ResumesASuspendedDevice) {
  SuspendableDevice dev;
  Renderer r;
  Events ev;
  AudioDriver driver;
  dev.driver = &driver;
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 256, 0}, Desc(r, &ev, 128, 2),
                           snd::base::DefaultAllocator()));
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= 10u; }));
  dev.SuspendNow();
  SND_REQUIRE(WaitUntil([&] { return ev.suspended.load() == 1u; }));
  SleepFor(30000000ull);
  const std::uint64_t c = dev.calls.load();
  SleepFor(20000000ull);
  SND_CHECK_EQ(dev.calls.load(), c);  // really suspended
  SND_CHECK(driver.Resume());
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= c + 10u; }));
  SND_CHECK_EQ(ev.resumed.load(), 1u);
  SND_CHECK_EQ(dev.silentAfterResume.load(), 0u);
  SND_CHECK(dev.fillAtResume.load() >= dev.targetAtResume.load());
  driver.Stop();
}

// 2.6 "レジューム時は先行レンダーを満たしてから再開する": the ring is left below
// its target when the device suspends (a burst drained it and one wake may
// render only ceil(period / block) + catchup blocks); Resume must fill to the
// target BEFORE the device plays again, as Start() does. Resume() posts the
// prefill and calls device->Start() at once, so with a slow renderer the
// device restarts on a partly filled ring.
SND_TEST(IndepDriverResume, ResumeRestartsDeviceAfterPrefill) {
  SuspendableDevice dev;
  Renderer r;
  Events ev;
  AudioDriver driver;
  dev.driver = &driver;
  // block 64, period 256, ahead 12: target 256 + 768 = 1024; one wake renders
  // at most 4 + 1 = 5 blocks (320 frames); the burst leaves at most two wakes.
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 256, 0}, Desc(r, &ev, 64, 12),
                           snd::base::DefaultAllocator()));
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= 10u; }));
  r.sleepMs.store(5);  // 5 ms per block from now on
  dev.burst.store(12);  // 12 callbacks flat out, then SUSPENDED
  SND_REQUIRE(WaitUntil([&] { return dev.Suspended(); }));
  SleepFor(300000000ull);  // the audio thread finishes its pending wake
  const DriverStats before = driver.Core().Stats();
  snd::test::Note("suspended with %u of %u queued", before.queuedFrames, before.targetFrames);
  SND_REQUIRE(before.queuedFrames < before.targetFrames);  // precondition of the scenario
  SND_CHECK(driver.Resume());
  SND_REQUIRE(WaitUntil([&] { return dev.fillAtResume.load() >= 0; }));
  snd::test::Note("first callback after the resume: %lld of %lld queued",
                  static_cast<long long>(dev.fillAtResume.load()), static_cast<long long>(dev.targetAtResume.load()));
  SND_CHECK(dev.fillAtResume.load() >= dev.targetAtResume.load());
  r.sleepMs.store(0);
  driver.Stop();
}

// Stats() / LoadLevel() from a monitoring thread while the owner suspends and
// resumes repeatedly (TSan target).
SND_TEST(IndepDriverResume, StatsWhileResumingIsRaceFree) {
  SuspendableDevice dev;
  Renderer r;
  Events ev;
  AudioDriver driver;
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 128, 0}, Desc(r, &ev, 64, 2),
                           snd::base::DefaultAllocator()));
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> polls{0};
  std::thread monitor([&] {
    while (!stop.load()) {
      const AudioDriverStats s = driver.Stats();
      volatile std::uint64_t x = s.core.blocksRendered + s.events + driver.LoadLevel();
      (void)x;
      polls.fetch_add(1);
    }
  });
  for (int i = 0; i < 50; ++i) {
    dev.SuspendNow();
    SleepFor(2000000);
    SND_CHECK(driver.Resume());
    const std::uint64_t c = dev.calls.load();
    SND_REQUIRE(WaitUntil([&] { return dev.calls.load() > c; }));
  }
  stop.store(true);
  monitor.join();
  driver.Stop();
  SND_CHECK(polls.load() > 0u);
  SND_CHECK_EQ(ev.suspended.load(), 50u);
  SND_CHECK_EQ(ev.resumed.load(), 50u);
}

namespace {
// Fails the test run (instead of hanging ctest) if `done` is not set in time.
class Watchdog {
 public:
  Watchdog(const char* what, std::uint64_t ms) : what_(what), ms_(ms), t_([this] { Loop(); }) {}
  ~Watchdog() {
    done.store(true);
    t_.join();
  }
  std::atomic<bool> done{false};

 private:
  void Loop() {
    const std::uint64_t deadline = MonotonicNanoseconds() + ms_ * 1000000ull;
    while (!done.load()) {
      if (MonotonicNanoseconds() > deadline) {
        std::printf("WATCHDOG: %s did not return within %llu ms (hang)\n", what_, static_cast<unsigned long long>(ms_));
        std::fflush(stdout);
        std::_Exit(3);
      }
      SleepFor(5000000);
    }
  }
  const char* what_;
  std::uint64_t ms_;
  std::thread t_;
};
}  // namespace

// Resume times out (slow renderer), the device stays suspended and nothing
// more is rendered; a second Resume with a fast renderer succeeds.
SND_TEST(IndepDriverResume, RetryAfterTimeoutSucceeds) {
  SuspendableDevice dev;
  Renderer r;
  Events ev;
  AudioDriver driver;
  dev.driver = &driver;
  AudioDriverDesc d = Desc(r, &ev, 64, 12);  // target 1024: 16 blocks
  d.prefillTimeoutMs = 150;  // also bounds Start's own (fast) prefill: retried on a loaded machine
  bool started = false;
  for (int attempt = 0; attempt < 10 && !started; ++attempt) {
    started = driver.Start(dev, snd::DeviceDesc{48000, 2, 256, 0}, d, snd::base::DefaultAllocator());
  }
  SND_REQUIRE(started);
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= 10u; }));
  r.sleepMs.store(30);  // 16 blocks x 30 ms >> 150 ms
  dev.burst.store(12);
  SND_REQUIRE(WaitUntil([&] { return dev.Suspended(); }));
  SleepFor(400000000ull);
  const std::uint64_t t0 = MonotonicNanoseconds();
  SND_CHECK(!driver.Resume());
  const std::uint64_t ms = (MonotonicNanoseconds() - t0) / 1000000u;
  SND_CHECK(ms >= 150u);
  SND_CHECK(ms < 150u + 500u);  // timeout + at most the block in progress (30 ms) + slack for load
  SND_CHECK(dev.Suspended());  // the device was not restarted
  const std::uint64_t blocks = r.blocks.load();
  SleepFor(100000000ull);
  SND_CHECK_EQ(r.blocks.load(), blocks);  // nothing rendered after the timeout
  r.sleepMs.store(0);
  SND_CHECK(driver.Resume());
  SND_REQUIRE(WaitUntil([&] { return dev.fillAtResume.load() >= 0; }));
  SND_CHECK(dev.fillAtResume.load() >= dev.targetAtResume.load());
  SND_CHECK(!dev.Suspended());
  const std::uint64_t c = dev.calls.load();
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() > c + 10u; }));
  SND_CHECK_EQ(ev.resumed.load(), 1u);
  driver.Stop();
}

// CHANGED and LOST from an OS notification thread while Resume waits for the
// prefill: forwarded, no deadlock, Resume still completes.
SND_TEST(IndepDriverResume, EventsWhileResumeWaits) {
  SuspendableDevice dev;
  Renderer r;
  Events ev;
  AudioDriver driver;
  dev.driver = &driver;
  AudioDriverDesc d = Desc(r, &ev, 64, 12);
  d.prefillTimeoutMs = 5000;
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 256, 0}, d, snd::base::DefaultAllocator()));
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= 10u; }));
  r.sleepMs.store(4);
  dev.burst.store(12);
  SND_REQUIRE(WaitUntil([&] { return dev.Suspended(); }));
  SleepFor(200000000ull);
  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> posted{0};
  std::thread notifier([&] {
    std::uint32_t i = 0;
    while (!stop.load()) {
      if ((i & 1u) == 0u) {
        dev.Emit(snd::DeviceEventKind::Changed, 2, (i & 2u) != 0u ? 44100u : 48000u, 256u);
      } else {
        dev.Emit(snd::DeviceEventKind::Lost, 0, 0, 0);
      }
      posted.fetch_add(1);
      ++i;
      SleepFor(1000000);
    }
  });
  bool ok = false;
  {
    Watchdog wd("Resume with concurrent events", 20000);
    ok = driver.Resume();
    wd.done.store(true);
  }
  stop.store(true);
  notifier.join();
  SND_CHECK(ok);
  SND_CHECK(posted.load() > 0u);
  SND_CHECK(driver.Stats().events >= posted.load());
  r.sleepMs.store(0);
  const std::uint64_t c = dev.calls.load();
  SND_CHECK(WaitUntil([&] { return dev.calls.load() > c + 10u; }));
  driver.Stop();
}

// Outside the contract (Resume and Stop are owner-thread calls): Stop() from
// another thread while Resume() waits for a slow prefill. It does not hang
// (watchdog), but TSan reports data races (AudioDriver::device_ read in
// Resume vs cleared in Stop; the device's Start after its Stop/Close), so it
// is a specification question (which thread may call snd_resume /
// snd_shutdown), not run by default. SND_INDEP_CONTRACT_EDGE=1 runs it.
SND_TEST(IndepDriverResume, StopFromAnotherThreadWhileResumeWaits) {
  if (std::getenv("SND_INDEP_CONTRACT_EDGE") == nullptr) {
    snd::test::Note("skipped (outside the owner-thread contract); set SND_INDEP_CONTRACT_EDGE=1");
    return;
  }
  for (int round = 0; round < 3; ++round) {
    SuspendableDevice dev;
    Renderer r;
    AudioDriver driver;
    AudioDriverDesc d = Desc(r, nullptr, 64, 12);
    d.prefillTimeoutMs = 300;
    SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 256, 0}, d, snd::base::DefaultAllocator()));
    SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= 10u; }));
    r.sleepMs.store(20);
    dev.burst.store(12);
    SND_REQUIRE(WaitUntil([&] { return dev.Suspended(); }));
    SleepFor(300000000ull);
    Watchdog wd("Resume racing Stop", 20000);
    std::thread resumer([&] { (void)driver.Resume(); });
    SleepFor(static_cast<std::uint64_t>(10 + round * 100) * 1000000ull);  // before / around / after the timeout
    driver.Stop();
    resumer.join();
    wd.done.store(true);
    SND_CHECK(!driver.Running());
  }
}
