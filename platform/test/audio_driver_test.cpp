// AudioDriver (design.md 2.2, 2.4, 2.6, CONTRACTS.md D-66) over the null
// device mock (pumped by the test) and the silent timer device (real time).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/mocks/null_output_device.hpp"
#include "snd/platform/audio_driver.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {

struct Renderer {
  std::atomic<std::uint64_t> nextFrame{0};
  std::atomic<std::uint64_t> lost{0};
  std::atomic<std::uint32_t> formatCalls{0};
  std::atomic<std::uint32_t> lastChannels{0};
  std::atomic<std::uint32_t> lastRate{0};
  std::atomic<bool> inRtScope{true};
  std::atomic<bool> ftz{true};
  std::atomic<int> stopFromRender{0};  // > 0: call Stop() from the render function this many times
  AudioDriver* driver = nullptr;

  static void Render(void* user, float* out, std::uint32_t frames, std::uint32_t channels, const RenderInfo* info) {
    auto* self = static_cast<Renderer*>(user);
    self->lost.fetch_add(info->lostFrames, std::memory_order_relaxed);
    if (snd::rt::kEnabled && !snd::rt::InRtScope()) self->inRtScope.store(false);
    if (GetCpuFeatures().denormalControl && !FlushDenormalsEnabled()) self->ftz.store(false);
    if (self->stopFromRender.load() > 0) {
      self->stopFromRender.fetch_sub(1);
      self->driver->Stop();  // forbidden: ignored and counted
    }
    const std::uint64_t base = self->nextFrame.load(std::memory_order_relaxed);
    for (std::uint32_t f = 0; f < frames; ++f) {
      const float v = static_cast<float>(base + f + 1u);
      for (std::uint32_t c = 0; c < channels; ++c) out[f * channels + c] = v * static_cast<float>(c + 1u);
    }
    self->nextFrame.store(base + frames, std::memory_order_relaxed);
  }
  static void Format(void* user, std::uint32_t rate, std::uint32_t channels) {
    auto* self = static_cast<Renderer*>(user);
    self->formatCalls.fetch_add(1);
    self->lastChannels.store(channels);
    self->lastRate.store(rate);
  }
};

struct EventLog {
  std::atomic<std::uint32_t> changed{0};
  std::atomic<std::uint32_t> suspended{0};
  std::atomic<std::uint32_t> resumed{0};
  static void Listener(void* user, const snd::DeviceEvent* e) {
    auto* self = static_cast<EventLog*>(user);
    if (e->kind == SND_DEVICE_EVENT_CHANGED) self->changed.fetch_add(1);
    if (e->kind == SND_DEVICE_EVENT_SUSPENDED) self->suspended.fetch_add(1);
    if (e->kind == SND_DEVICE_EVENT_RESUMED) self->resumed.fetch_add(1);
  }
};

AudioDriverDesc MakeDesc(Renderer& r, std::uint32_t block, std::uint32_t ahead) {
  AudioDriverDesc d;
  d.core.blockFrames = block;
  d.core.renderAheadBlocks = ahead;
  d.core.fadeFrames = 0;  // exact checks
  d.core.render = &Renderer::Render;
  d.core.formatChanged = &Renderer::Format;
  d.core.user = &r;
  d.realtimeThread = false;
  return d;
}

// True when the 1-minute load average is below the core count (Linux), or
// when it cannot be read (other OSes). Timing checks run only then.
bool LoadBelowCoreCount() {
  FILE* f = std::fopen("/proc/loadavg", "r");
  if (f == nullptr) return true;
  double load = 0.0;
  const int n = std::fscanf(f, "%lf", &load);
  std::fclose(f);
  return n != 1 || load < static_cast<double>(HardwareConcurrency());
}

// Waits (bounded) until the audio thread has refilled the ring to its target.
bool WaitFilled(const AudioDriver& d) {
  const std::uint64_t deadline = MonotonicNanoseconds() + 2000000000ull;
  while (MonotonicNanoseconds() < deadline) {
    const DriverStats s = d.Core().Stats();
    if (s.queuedFrames >= s.targetFrames) return true;
    SleepFor(100000);
  }
  return false;
}

}  // namespace

SND_TEST(AudioDriver, PumpedNullDeviceIsSeamless) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  snd::mock::NullOutputDevice dev({.sampleRate = 48000, .channels = 2, .periodFrames = 300});
  Renderer r;
  AudioDriver driver;
  r.driver = &driver;
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{0, 0, 0, 0}, MakeDesc(r, 128, 2), acc));
  SND_CHECK(driver.Running());
  SND_CHECK(dev.IsStarted());
  // Prefilled before the device started: 300 + 2 x 128 = 556 -> 5 blocks (640).
  SND_CHECK_EQ(r.nextFrame.load(), 640u);
  SND_CHECK_EQ(r.formatCalls.load(), 1u);
  SND_CHECK_EQ(r.lastChannels.load(), 2u);
  SND_CHECK_EQ(r.lastRate.load(), 48000u);
  for (int i = 0; i < 200; ++i) {
    dev.Pump(1);
    SND_REQUIRE(WaitFilled(driver));
  }
  const std::vector<float>& cap = dev.Captured();
  SND_REQUIRE_EQ(cap.size(), 200u * 300u * 2u);
  bool exact = true;
  for (std::size_t f = 0; f < 200u * 300u; ++f) {
    exact = exact && cap[f * 2] == static_cast<float>(f + 1u) && cap[f * 2 + 1] == static_cast<float>(2u * (f + 1u));
  }
  SND_CHECK(exact);
  const AudioDriverStats s = driver.Stats();
  SND_CHECK_EQ(s.core.underruns, 0u);
  SND_CHECK_EQ(s.core.callbacks, 200u);
  SND_CHECK(s.wakeups >= 1u);
  SND_CHECK(r.inRtScope.load());  // render runs inside SND_RT_SCOPE
  SND_CHECK(r.ftz.load());
  driver.Stop();
  SND_CHECK(!driver.Running());
  SND_CHECK(!dev.IsOpen());
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
  driver.Stop();  // idempotent
}

SND_TEST(AudioDriver, EventsAreForwardedAndChangeTheFormat) {
  snd::mock::NullOutputDevice dev({.sampleRate = 48000, .channels = 2, .periodFrames = 256});
  Renderer r;
  EventLog log;
  AudioDriver driver;
  AudioDriverDesc d = MakeDesc(r, 256, 1);
  d.listener = &EventLog::Listener;
  d.listenerUser = &log;
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{0, 0, 0, 0}, d, snd::base::DefaultAllocator()));
  dev.Pump(4);
  SND_REQUIRE(WaitFilled(driver));
  // Headphones -> 5.1 at 44.1 kHz, period 512.
  dev.EmitEvent(snd::DeviceEvent{SND_DEVICE_EVENT_CHANGED, 6, 44100, 512});
  SND_CHECK_EQ(log.changed.load(), 1u);
  dev.Pump(1);  // 6 channels: silence, switch requested
  const std::uint64_t deadline = MonotonicNanoseconds() + 2000000000ull;
  while (r.lastChannels.load() != 6u && MonotonicNanoseconds() < deadline) SleepFor(100000);
  SND_CHECK_EQ(r.lastChannels.load(), 6u);
  SND_CHECK_EQ(r.lastRate.load(), 44100u);
  SND_REQUIRE(WaitFilled(driver));
  dev.ClearCapture();
  dev.Pump(1);
  SND_REQUIRE_EQ(dev.Captured().size(), 512u * 6u);
  // 6-channel frames: channel c = counter x (c + 1).
  const std::vector<float>& cap = dev.Captured();
  bool shape = cap[0] != 0.0f;
  for (std::uint32_t f = 0; f < 512; ++f) {
    for (std::uint32_t c = 0; c < 6; ++c) shape = shape && cap[f * 6 + c] == cap[f * 6] * static_cast<float>(c + 1u);
    if (f > 0) shape = shape && cap[f * 6] == cap[(f - 1) * 6] + 1.0f;
  }
  SND_CHECK(shape);
  SND_CHECK_EQ(driver.Core().Stats().targetFrames, 512u + 256u);
  // Suspend / resume are forwarded; resume triggers a prefill wake.
  dev.EmitEvent(snd::DeviceEvent{SND_DEVICE_EVENT_SUSPENDED, 0, 0, 0});
  dev.EmitEvent(snd::DeviceEvent{SND_DEVICE_EVENT_RESUMED, 6, 44100, 512});
  SND_CHECK_EQ(log.suspended.load(), 1u);
  SND_CHECK_EQ(log.resumed.load(), 1u);
  SND_CHECK(WaitFilled(driver));
  SND_CHECK(driver.Stats().events >= 3u);
  driver.Stop();
}

SND_TEST(AudioDriver, StopFromRenderOrCallbackIsIgnored) {
  snd::mock::NullOutputDevice dev({.sampleRate = 48000, .channels = 2, .periodFrames = 256});
  Renderer r;
  AudioDriver driver;
  r.driver = &driver;
  r.stopFromRender.store(2);
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{0, 0, 0, 0}, MakeDesc(r, 64, 1), snd::base::DefaultAllocator()));
  dev.Pump(2);
  SND_REQUIRE(WaitFilled(driver));
  SND_CHECK_EQ(driver.MisuseCount(), 2u);
  SND_CHECK(driver.Running());
  driver.Stop();
  SND_CHECK(!driver.Running());
}

SND_TEST(AudioDriver, StartFailuresUndoEverything) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  Renderer r;
  {
    snd::mock::NullOutputDevice failing({.failOpen = true});
    AudioDriver driver;
    SND_CHECK(!driver.Start(failing, snd::DeviceDesc{}, MakeDesc(r, 256, 2), acc));
    SND_CHECK(!driver.Running());
  }
  {
    snd::mock::NullOutputDevice dev;
    AudioDriver driver;
    AudioDriverDesc d = MakeDesc(r, 256, 2);
    d.core.render = nullptr;  // DriverCore::Init fails
    SND_CHECK(!driver.Start(dev, snd::DeviceDesc{}, d, acc));
    SND_CHECK(!dev.IsOpen());
  }
  {
    snd::mock::NullOutputDevice dev;
    AudioDriver driver;
    SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{}, MakeDesc(r, 256, 2), acc));
    SND_CHECK(!driver.Start(dev, snd::DeviceDesc{}, MakeDesc(r, 256, 2), acc));  // already running
    driver.Stop();
  }
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

namespace {
void* volatile gAllocSink = nullptr;
struct AllocatingRenderer {
  static void Render(void*, float* out, std::uint32_t frames, std::uint32_t channels, const RenderInfo*) {
    int* p = new int(3);  // forbidden on the audio thread: reported by the RT checks
    gAllocSink = p;       // escapes: the optimiser may not elide the allocation
    delete p;
    std::memset(out, 0, sizeof(float) * frames * channels);
  }
};
}  // namespace

SND_TEST(AudioDriver, RenderRunsInRtScope) {
  if (!snd::rt::kEnabled) return;
  snd::mock::NullOutputDevice dev;
  AudioDriver driver;
  AudioDriverDesc d;
  d.core.render = &AllocatingRenderer::Render;
  d.realtimeThread = false;
  snd::test::rt::ExpectViolations expect;
  snd::test::rt::Reset();
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{}, d, snd::base::DefaultAllocator()));
  driver.Stop();
  SND_CHECK(snd::test::rt::Count(snd::rt::Violation::Alloc) >= 1u);
  snd::test::rt::Reset();
}

// Real time over the silent timer device (2.2): the audio thread keeps up.
SND_TEST(AudioDriver, TimerDeviceRealTime) {
  TimerDeviceConfig tc;
  tc.realtimeThread = false;
  TimerOutputDevice dev(snd::base::DefaultAllocator(), tc);
  Renderer r;
  AudioDriver driver;
  AudioDriverDesc d = MakeDesc(r, 256, 2);
  d.workgroupSource = &dev;
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 480, 0}, d, snd::base::DefaultAllocator()));
  // At least 10 callbacks (~100 ms), bounded wait: a loaded machine may delay the timer thread.
  const std::uint64_t deadline = MonotonicNanoseconds() + 20000000000ull;
  while (driver.Stats().core.callbacks < 10u && MonotonicNanoseconds() < deadline) SleepFor(5000000);
  SleepFor(200000000ull);
  driver.Stop();
  const AudioDriverStats s = driver.Stats();  // counters stay readable after Stop
  snd::test::Note("callbacks=%llu underruns=%llu blocks=%llu interval avg=%.3f ms max=%.3f ms render avg=%.1f us",
                  static_cast<unsigned long long>(s.core.callbacks), static_cast<unsigned long long>(s.core.underruns),
                  static_cast<unsigned long long>(s.core.blocksRendered), static_cast<double>(s.core.intervalAvgNs) / 1e6,
                  static_cast<double>(s.core.intervalMaxNs) / 1e6, static_cast<double>(s.core.renderAvgNs) / 1e3);
  SND_CHECK(s.core.callbacks >= 10u);
  // Timing is only checked when the machine is not overloaded (the timer
  // thread has no realtime priority here); otherwise it is only recorded.
  if (LoadBelowCoreCount()) {
    SND_CHECK(s.core.intervalAvgNs > 5000000u && s.core.intervalAvgNs < 20000000u);
  } else {
    snd::test::Note("load average >= cores: interval average not checked");
  }
  // Everything rendered was consumed or is still queued; lost frames were reported to the renderer.
  SND_CHECK_EQ(r.nextFrame.load(), s.core.framesConsumed - s.core.lostFrames + s.core.queuedFrames);
  SND_CHECK(r.lost.load() <= s.core.lostFrames);
  SND_CHECK_EQ(s.workgroup, WorkgroupJoinResult::NoWorkgroup);  // the timer device has none
  SND_CHECK_EQ(s.misuse, 0u);
}
