// Independent tests of AudioDriver (design.md 2.2, 2.4, 2.6, CONTRACTS.md
// D-47, D-61, D-65, D-66) and of the silent timer device's shared behaviour
// (D-61, D-62), written from the specification and the public headers
// (properties: indep-platform-properties.md, sections D, E, G, H).
//
// ScriptDevice is a scripted PlatformOutputDevice with its own device thread:
// it can vary the callback length, switch the channel count (posting CHANGED
// first, D-62), run paced (real time) or flat out, post events from a
// separate notification thread, and records what the driver gave it.
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/audio_driver.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/platform/workgroup.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

constexpr std::uint32_t kFrameMask = 0xFFFFu;
inline float Encode(std::uint64_t frame, std::uint32_t channels, std::uint32_t c) {
  const std::uint32_t v = ((static_cast<std::uint32_t>(frame & kFrameMask) + 1u) << 7) | ((channels - 1u) << 3) | c;
  return static_cast<float>(v);
}
inline bool DecodeLayout(float x, std::uint32_t* channels, std::uint32_t* c) {
  if (!(x > 0.0f) || x >= 16777216.0f) return false;
  const auto v = static_cast<std::uint32_t>(x);
  if (static_cast<float>(v) != x) return false;
  *channels = ((v >> 3) & 7u) + 1u;
  *c = v & 7u;
  return true;
}

std::size_t CurrentStackSize() {
  pthread_attr_t a;
  if (pthread_getattr_np(pthread_self(), &a) != 0) return 0;
  std::size_t size = 0;
  pthread_attr_getstacksize(&a, &size);
  pthread_attr_destroy(&a);
  return size;
}

class ScriptDevice final : public PlatformOutputDevice {
 public:
  // --- script (set before Open/Start) ---
  std::uint32_t defRate = 48000, defChannels = 2, defPeriod = 256;
  bool paced = true;        // sleep the callback's duration (real time)
  bool vary = false;        // callback lengths 1..period
  bool failOpen = false;
  bool failStart = false;
  std::atomic<std::uint32_t> switchTo{0};  // test -> device thread: change the channel count

  // --- observations ---
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> framesOut{0};
  std::atomic<std::uint64_t> silentFramesOut{0};
  std::atomic<std::uint64_t> mixedLayoutFrames{0};  // frame whose samples are not in the callback layout
  std::atomic<std::uint64_t> firstCallSilentFrames{~0ull};
  std::atomic<std::uint64_t> callsWhileClosed{0};
  std::atomic<std::uint32_t> opens{0}, closes{0};
  std::atomic<std::thread::id> deviceThread{};

  explicit ScriptDevice() noexcept = default;
  ~ScriptDevice() override { Close(); }

  OutputBackend Backend() const noexcept override { return OutputBackend::SilentTimer; }
  bool Open(const snd::DeviceDesc& d, snd::DeviceCallback cb, void* user) override {
    if (open_ || cb == nullptr || failOpen) return false;
    rate_ = d.sample_rate != 0u ? d.sample_rate : defRate;
    channels_.store(d.channels != 0u ? d.channels : defChannels);
    period_ = d.period_frames != 0u ? d.period_frames : defPeriod;
    cb_ = cb;
    user_ = user;
    buffer_.assign(static_cast<std::size_t>(period_) * 8u, 0.0f);
    open_ = true;
    opens.fetch_add(1);
    return true;
  }
  bool Start() override {
    if (!open_ || failStart) return false;
    if (started_) return true;
    running_.store(true);
    thread_ = std::thread([this] { Run(); });
    started_ = true;
    return true;
  }
  void Stop() override {
    if (std::this_thread::get_id() == deviceThread.load()) {
      statMisuse_.fetch_add(1);
      return;
    }
    if (!started_) return;
    running_.store(false);
    thread_.join();
    started_ = false;
  }
  void Close() override {
    if (std::this_thread::get_id() == deviceThread.load()) {
      statMisuse_.fetch_add(1);
      return;
    }
    Stop();
    if (open_) closes.fetch_add(1);
    open_ = false;
    cb_ = nullptr;
  }
  std::uint32_t PeriodFrames() const override { return period_; }
  std::uint32_t SampleRate() const override { return rate_; }
  std::uint32_t Channels() const override { return channels_.load(); }
  double OutputLatencySeconds() const override { return 0.01; }
  bool IsOpen() const { return open_; }

  // Posts from the calling thread (an "OS notification thread").
  void Emit(snd::DeviceEventKind k, std::uint32_t ch, std::uint32_t rate, std::uint32_t period) {
    PostEvent(k, ch, rate, period);
  }

 private:
  void Run() {
    deviceThread.store(std::this_thread::get_id());
    std::uint32_t lcg = 5u;
    std::uint64_t due = MonotonicNanoseconds();
    while (running_.load()) {
      const std::uint32_t sw = switchTo.exchange(0);
      if (sw != 0u) {
        // D-62: CHANGED with the new values before the first callback in the new format.
        PostEvent(snd::DeviceEventKind::Changed, sw, rate_, period_);
        channels_.store(sw);
      }
      const std::uint32_t ch = channels_.load();
      lcg = lcg * 1664525u + 1013904223u;
      const std::uint32_t n = vary ? 1u + (lcg >> 8) % period_ : period_;
      if (!open_) callsWhileClosed.fetch_add(1);
      {
        SND_RT_SCOPE();
        ScopedFlushDenormals ftz;
        cb_(user_, buffer_.data(), n, ch);
      }
      std::uint64_t silent = 0, mixed = 0;
      for (std::uint32_t f = 0; f < n; ++f) {
        bool zero = true;
        bool layoutOk = true;
        for (std::uint32_t c = 0; c < ch; ++c) {
          const float x = buffer_[f * ch + c];
          if (x != 0.0f) zero = false;
          std::uint32_t lc = 0, cc = 0;
          if (x != 0.0f && (!DecodeLayout(x, &lc, &cc) || lc != ch || cc != c)) layoutOk = false;
        }
        if (zero) ++silent;
        if (!zero && !layoutOk) ++mixed;
      }
      if (calls.load() == 0u) firstCallSilentFrames.store(silent);
      silentFramesOut.fetch_add(silent);
      mixedLayoutFrames.fetch_add(mixed);
      framesOut.fetch_add(n);
      calls.fetch_add(1);
      if (paced) {
        due += static_cast<std::uint64_t>(n) * 1000000000ull / rate_;
        const std::uint64_t now = MonotonicNanoseconds();
        if (due > now) {
          SleepUntil(due);
        } else if (now - due > 20000000ull) {
          due = now;  // resync after a long stall (sanitizers)
        }
      } else {
        std::this_thread::yield();
      }
    }
    deviceThread.store(std::thread::id{});
  }

  std::uint32_t rate_ = 0;
  std::atomic<std::uint32_t> channels_{0};
  std::uint32_t period_ = 0;
  snd::DeviceCallback cb_ = nullptr;
  void* user_ = nullptr;
  std::vector<float> buffer_;
  bool open_ = false;
  bool started_ = false;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

struct Renderer {
  std::atomic<std::uint64_t> next{0};
  std::atomic<std::uint64_t> renders{0};
  std::atomic<std::uint64_t> lost{0};
  std::atomic<std::uint32_t> formatCalls{0};
  std::atomic<std::uint32_t> formatChannels{0};
  std::atomic<std::uint32_t> wrongBlock{0};
  std::atomic<std::uint32_t> renderBeforeFormat{0};
  std::atomic<bool> notRt{false};
  std::atomic<bool> noFtz{false};
  std::atomic<std::size_t> stack{0};
  std::atomic<std::thread::id> renderThread{};
  std::atomic<std::uint32_t> sleepMsPerBlock{0};
  std::atomic<int> stopFromRender{0};
  std::uint32_t block = 0;
  AudioDriver* driver = nullptr;

  static void Render(void* u, float* out, std::uint32_t frames, std::uint32_t channels, const RenderInfo* info) {
    auto* s = static_cast<Renderer*>(u);
    if (frames != s->block) s->wrongBlock.fetch_add(1);
    if (channels != s->formatChannels.load()) s->renderBeforeFormat.fetch_add(1);
    if (snd::rt::kEnabled && !snd::rt::InRtScope()) s->notRt.store(true);
    if (GetCpuFeatures().denormalControl && !FlushDenormalsEnabled()) s->noFtz.store(true);
    if (s->stack.load() == 0u) {
      snd::rt::SuspendScope suspend;  // pthread_getattr_np reads /proc
      s->stack.store(CurrentStackSize());
    }
    s->renderThread.store(std::this_thread::get_id());
    if (s->stopFromRender.load() > 0) {
      s->stopFromRender.fetch_sub(1);
      s->driver->Stop();
    }
    if (s->sleepMsPerBlock.load() > 0u) {
      snd::rt::SuspendScope suspend;
      SleepFor(static_cast<std::uint64_t>(s->sleepMsPerBlock.load()) * 1000000ull);
    }
    s->lost.fetch_add(info->lostFrames);
    const std::uint64_t base = s->next.load();
    for (std::uint32_t f = 0; f < frames; ++f) {
      for (std::uint32_t c = 0; c < channels; ++c) out[f * channels + c] = Encode(base + f, channels, c);
    }
    s->next.store(base + frames);
    s->renders.fetch_add(1);
  }
  static void Format(void* u, std::uint32_t, std::uint32_t channels) {
    auto* s = static_cast<Renderer*>(u);
    s->formatCalls.fetch_add(1);
    s->formatChannels.store(channels);
  }
};

struct Events {
  std::atomic<std::uint32_t> total{0}, changed{0}, suspended{0}, resumed{0}, lost{0};
  static void Fn(void* u, const snd::DeviceEvent* e) {
    auto* s = static_cast<Events*>(u);
    s->total.fetch_add(1);
    if (e->kind == SND_DEVICE_EVENT_CHANGED) s->changed.fetch_add(1);
    if (e->kind == SND_DEVICE_EVENT_SUSPENDED) s->suspended.fetch_add(1);
    if (e->kind == SND_DEVICE_EVENT_RESUMED) s->resumed.fetch_add(1);
    if (e->kind == SND_DEVICE_EVENT_LOST) s->lost.fetch_add(1);
  }
};

AudioDriverDesc MakeDesc(Renderer& r, std::uint32_t block, Events* ev = nullptr) {
  AudioDriverDesc d;
  d.core.blockFrames = block;
  d.core.renderAheadBlocks = 2;
  d.core.fadeFrames = 0;
  d.core.render = &Renderer::Render;
  d.core.formatChanged = &Renderer::Format;
  d.core.user = &r;
  d.realtimeThread = false;
  if (ev != nullptr) {
    d.listener = &Events::Fn;
    d.listenerUser = ev;
  }
  r.block = block;
  return d;
}

template <class F>
bool WaitUntil(F f, std::uint64_t ms = 5000) {
  const std::uint64_t deadline = MonotonicNanoseconds() + ms * 1000000ull;
  while (!f()) {
    if (MonotonicNanoseconds() > deadline) return false;
    SleepFor(200000);
  }
  return true;
}

}  // namespace

// D-66 Start: format announced before the first block, the ring prefilled
// before the device starts (the very first callback is not silent), the
// render function on its own thread (not the device thread, not the owner)
// with FTZ/DAZ, inside SND_RT_SCOPE and with the D-47 stack.
SND_TEST(IndepAudioDriver, StartPrefillsAndRendersOnTheAudioThread) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  ScriptDevice dev;
  dev.defPeriod = 480;
  Renderer r;
  AudioDriver driver;
  r.driver = &driver;
  AudioDriverDesc d = MakeDesc(r, 128);
  d.workgroupSource = &dev;
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 0, 0}, d, acc));
  SND_CHECK(driver.Running());
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= 50u; }));
  driver.Stop();
  SND_CHECK(!driver.Running());
  SND_CHECK_EQ(dev.firstCallSilentFrames.load(), 0u);
  SND_CHECK_EQ(r.formatCalls.load(), 1u);
  SND_CHECK_EQ(r.formatChannels.load(), 2u);
  SND_CHECK_EQ(r.wrongBlock.load(), 0u);
  SND_CHECK_EQ(r.renderBeforeFormat.load(), 0u);
  SND_CHECK(!r.notRt.load());
  SND_CHECK(!r.noFtz.load());
  SND_CHECK(r.stack.load() >= kRtThreadMinStackBytes);
  snd::test::Note("audio thread stack %zu bytes", r.stack.load());
  SND_CHECK(r.renderThread.load() != std::this_thread::get_id());
  SND_CHECK_EQ(dev.mixedLayoutFrames.load(), 0u);
  const AudioDriverStats s = driver.Stats();
  SND_CHECK_EQ(s.workgroup, WorkgroupJoinResult::NoWorkgroup);  // Linux (D-65)
  SND_CHECK(s.wakeups > 0u);
  SND_CHECK_EQ(s.core.callbacks, dev.calls.load());
  SND_CHECK_EQ(s.core.framesConsumed, dev.framesOut.load());
  SND_CHECK_EQ(s.core.lostFrames, dev.silentFramesOut.load());  // fade off: silence == lost exactly
  if (!kSanitized) SND_CHECK_EQ(s.core.underruns, 0u);
  SND_CHECK_EQ(dev.closes.load(), 1u);
  SND_CHECK(!dev.IsOpen());
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

// Real-time paced device with callback lengths varying on every call (D-62):
// gapless and block-exact; every silent frame is accounted as lost.
SND_TEST(IndepAudioDriver, PacedVaryingLengthsAreGapless) {
  ScriptDevice dev;
  dev.defPeriod = 441;
  dev.vary = true;
  Renderer r;
  AudioDriver driver;
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{44100, 2, 0, 0}, MakeDesc(r, 256), snd::base::DefaultAllocator()));
  SleepFor(400000000ull);
  driver.Stop();
  const AudioDriverStats s = driver.Stats();
  snd::test::Note("calls %llu frames %llu underruns %llu lost %llu interval min/avg/max %llu/%llu/%llu us",
                  static_cast<unsigned long long>(s.core.callbacks), static_cast<unsigned long long>(s.core.framesConsumed),
                  static_cast<unsigned long long>(s.core.underruns), static_cast<unsigned long long>(s.core.lostFrames),
                  static_cast<unsigned long long>(s.core.intervalMinNs / 1000u),
                  static_cast<unsigned long long>(s.core.intervalAvgNs / 1000u),
                  static_cast<unsigned long long>(s.core.intervalMaxNs / 1000u));
  SND_CHECK(s.core.callbacks > 50u);
  SND_CHECK_EQ(r.wrongBlock.load(), 0u);
  SND_CHECK_EQ(s.core.lostFrames, dev.silentFramesOut.load());
  SND_CHECK_EQ(dev.mixedLayoutFrames.load(), 0u);
  if (!kSanitized) SND_CHECK_EQ(s.core.underruns, 0u);
  // Every lost frame is reported to the renderer (minus what is still pending).
  SND_CHECK(r.lost.load() <= s.core.lostFrames);
}

// D-66 Stop: after it returns there are no more callbacks and no more renders;
// the device is closed and the listener detached (an event posted afterwards
// does not reach the driver). Stop twice and the destructor are harmless.
SND_TEST(IndepAudioDriver, StopIsFinal) {
  ScriptDevice dev;
  dev.paced = false;
  Renderer r;
  Events ev;
  {
    AudioDriver driver;
    SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 128, 0}, MakeDesc(r, 64, &ev),
                             snd::base::DefaultAllocator()));
    SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= 200u; }));
    driver.Stop();
    const std::uint64_t calls = dev.calls.load();
    const std::uint64_t renders = r.renders.load();
    const std::uint64_t events = driver.Stats().events;
    dev.Emit(snd::DeviceEventKind::Changed, 2, 48000, 128);
    SleepFor(20000000ull);
    SND_CHECK_EQ(dev.calls.load(), calls);
    SND_CHECK_EQ(r.renders.load(), renders);
    SND_CHECK_EQ(driver.Stats().events, events);
    SND_CHECK_EQ(ev.total.load(), 0u);
    SND_CHECK(!dev.IsOpen());
    driver.Stop();
    SND_CHECK_EQ(dev.closes.load(), 1u);
    // Start again on the same objects.
    SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 128, 0}, MakeDesc(r, 64, &ev),
                             snd::base::DefaultAllocator()));
    SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= calls + 50u; }));
  }  // destructor stops
  SND_CHECK(!dev.IsOpen());
  SND_CHECK_EQ(dev.closes.load(), 2u);
  SND_CHECK_EQ(dev.callsWhileClosed.load(), 0u);
}

// Start / Stop many times with a flat-out device (TSan / Release x20 target).
SND_TEST(IndepAudioDriver, StartStopCycles) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  ScriptDevice dev;
  dev.paced = false;
  dev.vary = true;
  Renderer r;
  Events ev;
  AudioDriver driver;
  for (int i = 0; i < 30; ++i) {
    const std::uint32_t block = 32u + static_cast<std::uint32_t>(i % 4) * 96u;
    SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 1u + static_cast<std::uint32_t>(i % 3), 100, 0},
                             MakeDesc(r, block, &ev), acc));
    const std::uint64_t c0 = dev.calls.load();
    SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= c0 + 20u; }));
    driver.Stop();
    SND_CHECK_EQ(acc.TotalUsed(), 0u);
  }
  SND_CHECK_EQ(r.wrongBlock.load(), 0u);
  SND_CHECK_EQ(dev.mixedLayoutFrames.load(), 0u);
  SND_CHECK_EQ(dev.opens.load(), 30u);
  SND_CHECK_EQ(dev.closes.load(), 30u);
}

// D-66: Stop() from the render function is ignored and counted; the driver
// keeps running. (From the device callback the driver's own thunk is the
// callback, so no user code runs there; see AudioDriver's existing test.)
SND_TEST(IndepAudioDriver, StopFromRenderIsCountedAndIgnored) {
  ScriptDevice dev;
  dev.paced = false;
  Renderer r;
  AudioDriver driver;
  r.driver = &driver;
  r.stopFromRender.store(3);
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 128, 0}, MakeDesc(r, 64), snd::base::DefaultAllocator()));
  SND_REQUIRE(WaitUntil([&] { return r.stopFromRender.load() == 0; }));
  const std::uint64_t c = dev.calls.load();
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() > c + 50u; }));  // still running
  SND_CHECK(driver.Running());
  SND_CHECK_EQ(driver.MisuseCount(), 3u);
  SND_CHECK_EQ(driver.Stats().misuse, 3u);
  driver.Stop();
  SND_CHECK(!driver.Running());
}

// Channel change (2.6, D-62, D-66) on a running driver: the device posts
// CHANGED and then calls back in the new layout; the renderer is told
// (FormatFn) and renders the new layout; no frame of the old layout is ever
// played in the new one. Several switches, flat out and paced.
SND_TEST(IndepAudioDriver, LiveChannelSwitches) {
  for (int pacedMode = 0; pacedMode < 2; ++pacedMode) {
    ScriptDevice dev;
    dev.paced = pacedMode != 0;
    dev.defPeriod = 256;
    Renderer r;
    Events ev;
    AudioDriver driver;
    SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 0, 0}, MakeDesc(r, 100, &ev),
                             snd::base::DefaultAllocator()));
    const std::uint32_t seq[] = {6, 1, 8, 2, 6, 2};
    for (std::uint32_t ch : seq) {
      dev.switchTo.store(ch);
      SND_REQUIRE(WaitUntil([&] { return dev.switchTo.load() == 0u; }));
      SND_REQUIRE(WaitUntil([&] { return r.formatChannels.load() == ch; }));
      const std::uint64_t silent = dev.silentFramesOut.load();
      const std::uint64_t frames = dev.framesOut.load();
      // The new layout plays (not only silence) shortly after.
      SND_CHECK(WaitUntil([&] { return dev.framesOut.load() - frames > (dev.silentFramesOut.load() - silent) + 512u; }));
    }
    driver.Stop();
    const AudioDriverStats s = driver.Stats();
    SND_CHECK_EQ(dev.mixedLayoutFrames.load(), 0u);
    SND_CHECK_EQ(r.renderBeforeFormat.load(), 0u);
    SND_CHECK_EQ(r.formatCalls.load(), 1u + 6u);
    SND_CHECK_EQ(ev.changed.load(), 6u);
    SND_CHECK_EQ(s.core.lostFrames, dev.silentFramesOut.load());
    SND_CHECK_EQ(s.core.formatChanges, 6u);
  }
}

// D-66 Start failures undo everything: the device does not open / start, or
// the first prefill takes longer than prefillTimeoutMs. Then a normal start works.
SND_TEST(IndepAudioDriver, StartFailuresLeaveNothingBehind) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  ScriptDevice dev;
  Renderer r;
  Events ev;
  AudioDriver driver;
  dev.failOpen = true;
  SND_CHECK(!driver.Start(dev, snd::DeviceDesc{48000, 2, 128, 0}, MakeDesc(r, 64, &ev), acc));
  dev.failOpen = false;
  dev.failStart = true;
  SND_CHECK(!driver.Start(dev, snd::DeviceDesc{48000, 2, 128, 0}, MakeDesc(r, 64, &ev), acc));
  SND_CHECK(!dev.IsOpen());
  dev.failStart = false;
  // Prefill timeout: 3 blocks of 150 ms against a 30 ms limit.
  r.sleepMsPerBlock.store(150);
  AudioDriverDesc slow = MakeDesc(r, 64, &ev);
  slow.prefillTimeoutMs = 30;
  const std::uint64_t t0 = MonotonicNanoseconds();
  SND_CHECK(!driver.Start(dev, snd::DeviceDesc{48000, 2, 128, 0}, slow, acc));
  snd::test::Note("prefill timeout: Start returned after %llu ms",
                  static_cast<unsigned long long>((MonotonicNanoseconds() - t0) / 1000000u));
  SND_CHECK(!driver.Running());
  SND_CHECK(!dev.IsOpen());
  SND_CHECK_EQ(dev.calls.load(), 0u);  // the device was never started
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
  dev.Emit(snd::DeviceEventKind::Changed, 2, 48000, 128);
  SND_CHECK_EQ(ev.total.load(), 0u);  // listener detached
  r.sleepMsPerBlock.store(0);
  SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 128, 0}, MakeDesc(r, 64, &ev), acc));
  SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= 10u; }));
  driver.Stop();
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

// Events from a separate notification thread while the driver starts and
// stops repeatedly (D-61: any thread, short lock; D-66: forwarded). TSan target.
SND_TEST(IndepAudioDriver, EventsFromAnotherThreadDuringStartStop) {
  ScriptDevice dev;
  dev.paced = false;
  Renderer r;
  Events ev;
  AudioDriver driver;
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> posted{0};
  std::thread notifier([&] {
    std::uint32_t i = 0;
    while (!stop.load()) {
      const auto k = (i % 3u == 0u) ? snd::DeviceEventKind::Changed
                                    : ((i % 3u == 1u) ? snd::DeviceEventKind::Suspended : snd::DeviceEventKind::Resumed);
      dev.Emit(k, 2, (i & 4u) != 0u ? 44100u : 48000u, 128u + (i % 2u) * 64u);
      posted.fetch_add(1);
      ++i;
      std::this_thread::yield();
    }
  });
  for (int i = 0; i < 20; ++i) {
    SND_REQUIRE(driver.Start(dev, snd::DeviceDesc{48000, 2, 128, 0}, MakeDesc(r, 64, &ev),
                             snd::base::DefaultAllocator()));
    const std::uint64_t c0 = dev.calls.load();
    SND_REQUIRE(WaitUntil([&] { return dev.calls.load() >= c0 + 30u; }));
    driver.Stop();
  }
  stop.store(true);
  notifier.join();
  SND_CHECK(ev.total.load() > 0u);
  SND_CHECK(ev.total.load() <= posted.load());
  SND_CHECK_EQ(r.wrongBlock.load(), 0u);
  SND_CHECK_EQ(dev.mixedLayoutFrames.load(), 0u);
}

// The silent timer device (D-61): callback with FTZ/DAZ in SND_RT_SCOPE on a
// thread with the D-47 stack, frames == period, Stop() after the last
// callback, Stop()/Close() from the callback ignored and counted, the clock
// sample (D-62) false before the first callback, then monotonic.
namespace {
struct TimerProbe {
  snd::IOutputDevice* dev = nullptr;
  std::atomic<std::uint64_t> calls{0};
  std::atomic<bool> inCallback{false};
  std::atomic<bool> badFrames{false};
  std::atomic<bool> notRt{false};
  std::atomic<bool> noFtz{false};
  std::atomic<std::size_t> stack{0};
  std::atomic<int> misuse{0};
  std::uint32_t period = 0;
  static void Cb(void* u, float* out, std::uint32_t frames, std::uint32_t channels) {
    auto* p = static_cast<TimerProbe*>(u);
    p->inCallback.store(true);
    if (frames != p->period) p->badFrames.store(true);
    if (snd::rt::kEnabled && !snd::rt::InRtScope()) p->notRt.store(true);
    if (GetCpuFeatures().denormalControl && !FlushDenormalsEnabled()) p->noFtz.store(true);
    if (p->stack.load() == 0u) {
      snd::rt::SuspendScope s;
      p->stack.store(CurrentStackSize());
    }
    if (p->misuse.load() > 0) {
      p->misuse.fetch_sub(1);
      p->dev->Stop();
      p->dev->Close();
    }
    std::memset(out, 0, sizeof(float) * frames * channels);
    {
      snd::rt::SuspendScope s;  // test-only: stretch the callback so Stop() has to wait for it
      SleepFor(200000);
    }
    p->calls.fetch_add(1);
    p->inCallback.store(false);
  }
};
}  // namespace

SND_TEST(IndepTimerDevice, SharedBackendBehaviour) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  OutputDeviceOptions o;
  o.realtimeThread = false;
  PlatformOutputDevice* dev = CreateOutputDevice(OutputBackend::SilentTimer, acc, &o);
  SND_REQUIRE(dev != nullptr);
  SND_CHECK_EQ(dev->Backend(), OutputBackend::SilentTimer);
  TimerProbe p;
  p.dev = dev;
  SND_CHECK(!dev->Open(snd::DeviceDesc{48000, SND_MAX_CHANNELS + 1u, 128, 0}, &TimerProbe::Cb, &p));
  SND_CHECK(!dev->Open(snd::DeviceDesc{48000, 2, 128, 0}, nullptr, &p));
  SND_REQUIRE(dev->Open(snd::DeviceDesc{48000, 2, 128, 0}, &TimerProbe::Cb, &p));
  SND_CHECK(!dev->Open(snd::DeviceDesc{48000, 2, 128, 0}, &TimerProbe::Cb, &p));  // already open
  p.period = dev->PeriodFrames();
  SND_CHECK_EQ(p.period, 128u);
  OutputClockSample clk;
  SND_CHECK(!dev->OutputClock(&clk));
  p.misuse.store(2);
  SND_REQUIRE(dev->Start());
  SND_REQUIRE(WaitUntil([&] { return p.calls.load() >= 20u; }));
  SND_CHECK_EQ(dev->Stats().misuseFromCallback, 4u);
  OutputClockSample a, b;
  SND_REQUIRE(dev->OutputClock(&a));
  SleepFor(5000000);
  SND_REQUIRE(dev->OutputClock(&b));
  SND_CHECK(b.framesWritten >= a.framesWritten);
  SND_CHECK(b.timestampNs >= a.timestampNs);
  SND_CHECK(a.framesPlayed <= a.framesWritten);
  SND_CHECK_EQ(a.sampleRate, 48000u);
  // Listener replaced while running (no events from the timer, but no race either).
  Events ev;
  for (int i = 0; i < 100; ++i) dev->SetEventListener((i & 1) != 0 ? &Events::Fn : nullptr, &ev);
  dev->Stop();
  SND_CHECK(!p.inCallback.load());  // returned after the last callback finished
  const std::uint64_t calls = p.calls.load();
  SleepFor(10000000);
  SND_CHECK_EQ(p.calls.load(), calls);
  SND_CHECK(!p.badFrames.load());
  SND_CHECK(!p.notRt.load());
  SND_CHECK(!p.noFtz.load());
  SND_CHECK(p.stack.load() >= kRtThreadMinStackBytes);
  const DeviceStats st = dev->Stats();
  SND_CHECK_EQ(st.callbacks, calls);
  SND_CHECK_EQ(st.framesDelivered, calls * 128u);
  // Start / Stop / Close / Open cycles.
  for (int i = 0; i < 20; ++i) {
    SND_REQUIRE(dev->Start());
    SleepFor(1000000);
    dev->Stop();
    SND_CHECK(!p.inCallback.load());
    if (i % 4 == 3) {
      dev->Close();
      SND_REQUIRE(dev->Open(snd::DeviceDesc{44100, 1, 64, 0}, &TimerProbe::Cb, &p));
      p.period = 64;
    }
  }
  DestroyOutputDevice(dev, acc);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

// D-61 / D-65 / factory on this OS.
SND_TEST(IndepPlatformDevice, FactoryWorkgroupAndEnumeration) {
  SND_CHECK(OutputBackendAvailable(OutputBackend::Default));
  SND_CHECK(OutputBackendAvailable(OutputBackend::SilentTimer));
  SND_CHECK(!OutputBackendAvailable(OutputBackend::Wasapi));
  SND_CHECK(!OutputBackendAvailable(OutputBackend::CoreAudio));
  const OutputBackend def = ResolveOutputBackend(OutputBackend::Default);
  SND_CHECK(def == OutputBackend::Alsa || def == OutputBackend::SilentTimer);
  SND_CHECK_EQ(def == OutputBackend::Alsa, OutputBackendAvailable(OutputBackend::Alsa));
  SND_CHECK_EQ(ResolveOutputBackend(OutputBackend::Wasapi), OutputBackend::Wasapi);  // others unchanged
  SND_CHECK(CreateOutputDevice(OutputBackend::Wasapi, snd::base::DefaultAllocator()) == nullptr);
  SND_CHECK(CreateOutputDevice(OutputBackend::CoreAudio, snd::base::DefaultAllocator()) == nullptr);
  SND_CHECK_EQ(EnumerateOutputDevices(OutputBackend::Wasapi, nullptr, 0), 0u);
  SND_CHECK_EQ(EnumerateOutputDevices(OutputBackend::CoreAudio, nullptr, 0), 0u);
  OutputDeviceInfo one;
  SND_CHECK_EQ(EnumerateOutputDevices(OutputBackend::SilentTimer, &one, 1), 1u);
  SND_CHECK(std::strcmp(one.id, "silent") == 0);
  SND_CHECK_EQ(EnumerateOutputDevices(OutputBackend::SilentTimer, nullptr, 5), 1u);  // null out, capacity ignored
  for (OutputBackend b : {OutputBackend::Default, OutputBackend::SilentTimer, OutputBackend::Alsa,
                          OutputBackend::Wasapi, OutputBackend::CoreAudio}) {
    SND_CHECK(OutputBackendName(b) != nullptr);
  }
  // Workgroups (D-65): nothing to join on Linux.
  Workgroup none;
  SND_CHECK(!none.Valid());
  Workgroup apple{WorkgroupKind::AppleOsWorkgroup, nullptr, 0};
  SND_CHECK(!apple.Valid());
  int dummy = 0;
  apple.native = &dummy;
  SND_CHECK(apple.Valid());
  Workgroup win{WorkgroupKind::WindowsMmcss, nullptr, 0};
  SND_CHECK(!win.Valid());
  win.taskIndex = 3;
  SND_CHECK(win.Valid());
  WorkgroupMembership m;
  SND_CHECK_EQ(m.Join(none), WorkgroupJoinResult::NoWorkgroup);
  SND_CHECK(!m.Joined());
  m.Leave();
  for (auto r : {WorkgroupJoinResult::Joined, WorkgroupJoinResult::NoWorkgroup, WorkgroupJoinResult::Unsupported,
                 WorkgroupJoinResult::Failed}) {
    SND_CHECK(WorkgroupJoinResultName(r) != nullptr);
  }
  ScriptDevice dev;
  std::uint32_t gen = 77;
  const Workgroup g = dev.CurrentWorkgroup(&gen);
  SND_CHECK(!g.Valid());
  SND_CHECK_EQ(gen, dev.WorkgroupGeneration());
}
