// Real output devices (CONTRACTS.md D-61..D-66). Plays sound: runs only with
// SND_DEVICE_TEST=1 (device_play_main.cpp), on the OS default backend
// (Windows WASAPI, macOS / iOS Core Audio, Linux ALSA "default").
//
//   SND_DEVICE_TEST=1                  open, 1 s sine on the device and through the AudioDriver,
//                                      enumeration, workgroup
//   SND_DEVICE_TEST_EXCLUSIVE=1        Windows: also WASAPI exclusive mode
//   SND_DEVICE_TEST_CHANGE=<seconds>   plays that long while you change the default device /
//                                      unplug headphones / (iOS) take a call; checks events
//   SND_DEVICE_TEST_INDEX=<k>          also plays on enumerated device k (device_index)
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/platform/audio_driver.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/platform/workgroup.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {

std::uint32_t EnvU32(const char* name, std::uint32_t fallback) {
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') return fallback;
  return static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
}

// 440 Hz at -12 dBFS on every channel; phase kept in double.
struct Sine {
  double phase = 0.0;
  std::atomic<std::uint32_t> rate{48000};
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint32_t> maxFrames{0};
  std::atomic<std::uint32_t> channels{0};
  std::atomic<bool> ftz{true};
  std::atomic<bool> rt{true};
  void Fill(float* out, std::uint32_t n, std::uint32_t ch) {
    const double step = 2.0 * 3.14159265358979323846 * 440.0 / static_cast<double>(rate.load());
    for (std::uint32_t f = 0; f < n; ++f) {
      const float v = static_cast<float>(0.25 * std::sin(phase));
      phase += step;
      if (phase > 6.283185307179586) phase -= 6.283185307179586;
      for (std::uint32_t c = 0; c < ch; ++c) out[f * ch + c] = v;
    }
  }
  static void Callback(void* user, float* out, std::uint32_t n, std::uint32_t ch) {
    auto* s = static_cast<Sine*>(user);
    if (GetCpuFeatures().denormalControl && !FlushDenormalsEnabled()) s->ftz.store(false);
    if (snd::rt::kEnabled && !snd::rt::InRtScope()) s->rt.store(false);
    s->Fill(out, n, ch);
    s->calls.fetch_add(1, std::memory_order_relaxed);
    s->frames.fetch_add(n, std::memory_order_relaxed);
    if (n > s->maxFrames.load(std::memory_order_relaxed)) s->maxFrames.store(n, std::memory_order_relaxed);
    s->channels.store(ch, std::memory_order_relaxed);
  }
  static void Render(void* user, float* out, std::uint32_t n, std::uint32_t ch, const RenderInfo*) {
    static_cast<Sine*>(user)->Fill(out, n, ch);
  }
  static void Format(void* user, std::uint32_t r, std::uint32_t) { static_cast<Sine*>(user)->rate.store(r); }
};

struct Events {
  std::atomic<std::uint32_t> counts[8] = {};
  static void Listener(void* user, const snd::DeviceEvent* e) {
    auto* self = static_cast<Events*>(user);
    if (e->kind < 8u) self->counts[e->kind].fetch_add(1);
    // Printing is not allowed in a real listener (post only); this is a manual test.
    std::printf("    event kind=%u channels=%u rate=%u period=%u\n", e->kind, e->channels, e->sample_rate,
                e->period_frames);
  }
};

void PlayOnDevice(const OutputDeviceOptions& opts, const snd::DeviceDesc& desc, const char* what) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Default, acc, &opts);
  SND_REQUIRE(d != nullptr);
  Sine sine;
  Events ev;
  d->SetEventListener(&Events::Listener, &ev);
  const bool opened = d->Open(desc, &Sine::Callback, &sine);
  SND_CHECK_MSG(opened, "%s: Open failed", what);
  if (!opened) {
    DestroyOutputDevice(d, acc);
    return;
  }
  sine.rate.store(d->SampleRate());
  snd::test::Note("%s on %s: %u Hz, %u ch, period %u, latency %.2f ms", what, OutputBackendName(d->Backend()),
                  d->SampleRate(), d->Channels(), d->PeriodFrames(), d->OutputLatencySeconds() * 1e3);
  SND_CHECK(d->SampleRate() >= 8000u && d->SampleRate() <= 384000u);
  SND_CHECK(d->Channels() >= 1u && d->Channels() <= SND_MAX_CHANNELS);
  SND_CHECK(d->PeriodFrames() > 0u);
  SND_CHECK(d->OutputLatencySeconds() > 0.0 && d->OutputLatencySeconds() < 0.5);
  const std::uint64_t t0 = MonotonicNanoseconds();
  SND_REQUIRE(d->Start());
  // Workgroup (Apple os_workgroup / Windows MMCSS): a thread can join it.
  SleepFor(200000000ull);
  std::uint32_t gen = 0;
  const Workgroup wg = d->CurrentWorkgroup(&gen);
  std::atomic<int> joined{-1};
  std::thread t([&] {
    WorkgroupMembership m;
    joined.store(static_cast<int>(m.Join(wg)));
    m.Leave();
  });
  t.join();
  snd::test::Note("workgroup kind %u valid %d join %s", static_cast<unsigned>(wg.kind), wg.Valid() ? 1 : 0,
                  WorkgroupJoinResultName(static_cast<WorkgroupJoinResult>(joined.load())));
#if defined(_WIN32) || defined(__APPLE__)
  SND_CHECK(wg.Valid());
  SND_CHECK_EQ(joined.load(), static_cast<int>(WorkgroupJoinResult::Joined));
#endif
  SleepFor(800000000ull);
  d->Stop();
  const double elapsed = static_cast<double>(MonotonicNanoseconds() - t0) / 1e9;
  const std::uint64_t callsAtStop = sine.calls.load();
  SleepFor(50000000ull);
  SND_CHECK_EQ(sine.calls.load(), callsAtStop);  // nothing after Stop()
  const DeviceStats st = d->Stats();
  OutputClockSample clock;
  const bool haveClock = d->OutputClock(&clock);
  const double expected = elapsed * d->SampleRate();
  snd::test::Note("%llu callbacks, %llu frames (expected ~%.0f), max %u per callback, xruns %llu",
                  static_cast<unsigned long long>(st.callbacks), static_cast<unsigned long long>(st.framesDelivered),
                  expected, sine.maxFrames.load(), static_cast<unsigned long long>(st.xruns));
  SND_CHECK(static_cast<double>(st.framesDelivered) > expected * 0.7);
  SND_CHECK(static_cast<double>(st.framesDelivered) < expected * 1.3 + 2.0 * d->PeriodFrames() + 8192.0);
  SND_CHECK(sine.maxFrames.load() <= d->PeriodFrames());  // D-61: 1..PeriodFrames()
  SND_CHECK_EQ(sine.channels.load(), d->Channels());
  SND_CHECK(sine.ftz.load());
  SND_CHECK(sine.rt.load());
  SND_CHECK(haveClock);
  if (haveClock) {
    SND_CHECK(clock.framesPlayed <= clock.framesWritten);
    SND_CHECK(clock.framesWritten > 0u);
  }
  d->Close();
  DestroyOutputDevice(d, acc);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

}  // namespace

SND_TEST(DevicePlay, DefaultDeviceOneSecondSine) {
  OutputDeviceOptions o;
  PlayOnDevice(o, snd::DeviceDesc{48000, 2, 0, 0}, "default 48k stereo");
  PlayOnDevice(o, snd::DeviceDesc{0, 0, 0, 0}, "default device format");
  PlayOnDevice(o, snd::DeviceDesc{44100, 2, 128, 0}, "44.1k stereo, period 128");
}

SND_TEST(DevicePlay, ExclusiveModeWhenAsked) {
#if defined(_WIN32)
  if (EnvU32("SND_DEVICE_TEST_EXCLUSIVE", 0) == 0u) return;
  OutputDeviceOptions o;
  o.exclusive = true;
  PlayOnDevice(o, snd::DeviceDesc{48000, 2, 0, 0}, "exclusive");
#endif
}

SND_TEST(DevicePlay, EnumerateAndPlayIndex) {
  std::vector<OutputDeviceInfo> list(32);
  const std::uint32_t n = EnumerateOutputDevices(OutputBackend::Default, list.data(), 32);
  SND_CHECK(n >= 1u);
  std::uint32_t defaults = 0;
  for (std::uint32_t i = 0; i < n && i < 32u; ++i) {
    snd::test::Note("device %u: %s | %s%s", i + 1u, list[i].name, list[i].id, list[i].isDefault ? " [default]" : "");
    if (list[i].isDefault) ++defaults;
  }
  SND_CHECK(defaults <= 1u);
  const std::uint32_t k = EnvU32("SND_DEVICE_TEST_INDEX", 0);
  if (k != 0u) {
    OutputDeviceOptions o;
    PlayOnDevice(o, snd::DeviceDesc{48000, 2, 0, k}, "device_index");
    // The same device by id (deviceName).
    if (k <= n && k <= 32u) {
      o.deviceName = list[k - 1u].id;
      PlayOnDevice(o, snd::DeviceDesc{48000, 2, 0, 0}, "deviceName");
    }
  }
}

SND_TEST(DevicePlay, AudioDriverOneSecond) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Default, acc);
  SND_REQUIRE(d != nullptr);
  Sine sine;
  AudioDriver driver;
  AudioDriverDesc ad;
  ad.core.blockFrames = 256;
  ad.core.render = &Sine::Render;
  ad.core.formatChanged = &Sine::Format;
  ad.core.user = &sine;
  ad.workgroupSource = d;
  SND_REQUIRE(driver.Start(*d, snd::DeviceDesc{48000, 2, 0, 0}, ad, acc));
  SleepFor(1000000000ull);
  driver.Stop();
  const AudioDriverStats s = driver.Stats();
  snd::test::Note("driver: %llu callbacks, interval avg %.3f / min %.3f / max %.3f ms, render avg %.1f us max %.1f us, "
                  "underruns %llu (lost %llu frames), priority %s, workgroup %s",
                  static_cast<unsigned long long>(s.core.callbacks), static_cast<double>(s.core.intervalAvgNs) / 1e6,
                  static_cast<double>(s.core.intervalMinNs) / 1e6, static_cast<double>(s.core.intervalMaxNs) / 1e6,
                  static_cast<double>(s.core.renderAvgNs) / 1e3, static_cast<double>(s.core.renderMaxNs) / 1e3,
                  static_cast<unsigned long long>(s.core.underruns), static_cast<unsigned long long>(s.core.lostFrames),
                  PriorityResultName(s.priority), WorkgroupJoinResultName(s.workgroup));
  SND_CHECK(s.core.callbacks > 10u);
  SND_CHECK(s.core.underruns <= 2u);  // the start may glitch once; steady state must not
#if defined(_WIN32) || defined(__APPLE__)
  SND_CHECK_EQ(s.workgroup, WorkgroupJoinResult::Joined);
#endif
  DestroyOutputDevice(d, acc);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

// Manual: change the default output / unplug headphones / (iOS) take a call
// while this runs. The device must keep calling back after each event.
SND_TEST(DevicePlay, DefaultDeviceChangeWhilePlaying) {
  const std::uint32_t seconds = EnvU32("SND_DEVICE_TEST_CHANGE", 0);
  if (seconds == 0u) return;
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Default, snd::base::DefaultAllocator());
  SND_REQUIRE(d != nullptr);
  Sine sine;
  Events ev;
  AudioDriver driver;
  AudioDriverDesc ad;
  ad.core.render = &Sine::Render;
  ad.core.formatChanged = &Sine::Format;
  ad.core.user = &sine;
  ad.listener = &Events::Listener;
  ad.listenerUser = &ev;
  ad.workgroupSource = d;
  SND_REQUIRE(driver.Start(*d, snd::DeviceDesc{0, 0, 0, 0}, ad, snd::base::DefaultAllocator()));
  std::printf("    playing for %u s: change the default output device / unplug headphones now\n", seconds);
  std::uint64_t lastCallbacks = 0;
  std::uint32_t stalledSeconds = 0;
  for (std::uint32_t i = 0; i < seconds; ++i) {
    SleepFor(1000000000ull);
    const AudioDriverStats s = driver.Stats();
    std::printf("    t=%us callbacks=%llu underruns=%llu rate=%u ch=%u\n", i + 1u,
                static_cast<unsigned long long>(s.core.callbacks), static_cast<unsigned long long>(s.core.underruns),
                s.core.sampleRate, s.core.channels);
    if (s.core.callbacks == lastCallbacks) ++stalledSeconds;
    lastCallbacks = s.core.callbacks;
  }
  driver.Stop();
  const std::uint32_t changes = ev.counts[SND_DEVICE_EVENT_CHANGED].load() + ev.counts[SND_DEVICE_EVENT_LOST].load();
  snd::test::Note("events: changed %u lost %u suspended %u resumed %u, reopens %llu, stalled %u s",
                  ev.counts[SND_DEVICE_EVENT_CHANGED].load(), ev.counts[SND_DEVICE_EVENT_LOST].load(),
                  ev.counts[SND_DEVICE_EVENT_SUSPENDED].load(), ev.counts[SND_DEVICE_EVENT_RESUMED].load(),
                  static_cast<unsigned long long>(d->Stats().reopens), stalledSeconds);
  SND_CHECK(changes >= 1u);       // you changed something
  SND_CHECK(stalledSeconds <= 2u);  // and the stream came back
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
}
