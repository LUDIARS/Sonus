// ALSA backend (CONTRACTS.md D-62..D-64) without a sound card: the PCMs are
// defined in a private configuration (OutputDeviceOptions::alsaConfig ->
// snd_pcm_open_lconf), so neither /usr/share/alsa nor ~/.asoundrc is used.
//   null     discards, never blocks (the device thread runs as fast as it can)
//   file     writes what it receives to a file, then passes it to null:
//            the test reads the file back and checks every sample.
//   linear   accepts integer formats only: forces the S32 conversion path.
// main() (alsa_test_main.cpp) exits with 77 (CTest SKIP) when the null PCM
// cannot be opened (libasound without the null / file plugins).
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/platform/audio_driver.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {

constexpr const char* kNullConfig = "pcm.sndnull { type null }";

struct TempFile {
  char path[64];
  TempFile() noexcept {
    std::snprintf(path, sizeof(path), "/tmp/snd-alsa-test-%d-XXXXXX", static_cast<int>(getpid()));
    const int fd = mkstemp(path);
    if (fd >= 0) close(fd);
  }
  ~TempFile() { std::remove(path); }
  std::vector<unsigned char> Read() const {
    std::vector<unsigned char> data;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return data;
    unsigned char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return data;
  }
};

struct Ramp {
  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint32_t> lastFrames{0};
  std::atomic<std::uint32_t> lastChannels{0};
  std::atomic<bool> ftz{true};
  std::atomic<bool> rt{true};
  std::atomic<int> misuse{0};  // > 0: call Stop()/Close() from the callback
  snd::IOutputDevice* device = nullptr;
  // Frame n, channel c -> (n + 1) / 2^20 * (c + 1): exact in float, < 1 for 2^19 frames.
  static float Value(std::uint64_t frame, std::uint32_t c) {
    return static_cast<float>((frame + 1u) % (1u << 19)) / 1048576.0f * static_cast<float>(c + 1u);
  }
  static void Callback(void* user, float* out, std::uint32_t frames, std::uint32_t channels) {
    auto* self = static_cast<Ramp*>(user);
    if (GetCpuFeatures().denormalControl && !FlushDenormalsEnabled()) self->ftz.store(false);
    if (snd::rt::kEnabled && !snd::rt::InRtScope()) self->rt.store(false);
    if (self->misuse.load() > 0) {
      self->misuse.fetch_sub(1);
      self->device->Stop();
      self->device->Close();
    }
    const std::uint64_t base = self->frames.load(std::memory_order_relaxed);
    for (std::uint32_t f = 0; f < frames; ++f) {
      for (std::uint32_t c = 0; c < channels; ++c) out[f * channels + c] = Value(base + f, c);
    }
    self->frames.store(base + frames, std::memory_order_relaxed);
    self->lastFrames.store(frames);
    self->lastChannels.store(channels);
    self->calls.fetch_add(1);
  }
};

bool WaitCalls(const Ramp& r, std::uint64_t n) {
  const std::uint64_t deadline = MonotonicNanoseconds() + 5000000000ull;
  while (r.calls.load() < n && MonotonicNanoseconds() < deadline) SleepFor(1000000);
  return r.calls.load() >= n;
}

OutputDeviceOptions Options(const char* name, const char* config) {
  OutputDeviceOptions o;
  o.deviceName = name;
  o.alsaConfig = config;
  o.realtimeThread = false;
  return o;
}

}  // namespace

// Used by main() to decide SKIP.
bool AlsaNullPcmUsable() {
  OutputDeviceOptions o = Options("sndnull", kNullConfig);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
  if (d == nullptr) return false;
  Ramp r;
  const bool ok = d->Open(snd::DeviceDesc{48000, 2, 256, 0}, &Ramp::Callback, &r);
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
  return ok;
}

SND_TEST(Alsa, BackendIsCompiledAndDefault) {
  SND_CHECK(OutputBackendAvailable(OutputBackend::Alsa));
  SND_CHECK_EQ(ResolveOutputBackend(OutputBackend::Default), OutputBackend::Alsa);
  SND_CHECK(!OutputBackendAvailable(OutputBackend::Wasapi));
  SND_CHECK(!OutputBackendAvailable(OutputBackend::CoreAudio));
  SND_CHECK(CreateOutputDevice(OutputBackend::Wasapi, snd::base::DefaultAllocator()) == nullptr);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Default, snd::base::DefaultAllocator());
  SND_REQUIRE(d != nullptr);
  SND_CHECK_EQ(d->Backend(), OutputBackend::Alsa);
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
}

SND_TEST(Alsa, NegotiatesRequestedAndDefaultFormat) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  OutputDeviceOptions o = Options("sndnull", kNullConfig);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, acc, &o);
  SND_REQUIRE(d != nullptr);
  Ramp r;
  SND_REQUIRE(d->Open(snd::DeviceDesc{0, 0, 0, 0}, &Ramp::Callback, &r));
  SND_CHECK_EQ(d->SampleRate(), 48000u);
  SND_CHECK_EQ(d->Channels(), 2u);
  SND_CHECK(d->PeriodFrames() > 0u && d->PeriodFrames() <= 1024u);
  SND_CHECK(d->OutputLatencySeconds() > 0.0 && d->OutputLatencySeconds() < 0.5);
  SND_CHECK(!d->Open(snd::DeviceDesc{}, &Ramp::Callback, &r));  // already open
  d->Close();
  SND_REQUIRE(d->Open(snd::DeviceDesc{44100, 6, 512, 0}, &Ramp::Callback, &r));
  SND_CHECK_EQ(d->SampleRate(), 44100u);
  SND_CHECK_EQ(d->Channels(), 6u);
  snd::test::Note("44100/6/512 -> period %u, latency %.2f ms", d->PeriodFrames(), d->OutputLatencySeconds() * 1e3);
  d->Close();
  SND_CHECK(!d->Open(snd::DeviceDesc{48000, SND_MAX_CHANNELS + 1u, 256, 0}, &Ramp::Callback, &r));
  SND_CHECK(!d->Open(snd::DeviceDesc{}, nullptr, &r));
  DestroyOutputDevice(d, acc);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(Alsa, UnknownPcmFailsCleanly) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  {
    OutputDeviceOptions o = Options("does-not-exist", kNullConfig);
    PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, acc, &o);
    SND_REQUIRE(d != nullptr);
    Ramp r;
    SND_CHECK(!d->Open(snd::DeviceDesc{}, &Ramp::Callback, &r));
    SND_CHECK(!d->Start());
    d->Stop();
    d->Close();
    DestroyOutputDevice(d, acc);
  }
  {
    OutputDeviceOptions o = Options("sndnull", "this is { not valid");
    PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, acc, &o);
    SND_REQUIRE(d != nullptr);
    Ramp r;
    SND_CHECK(!d->Open(snd::DeviceDesc{}, &Ramp::Callback, &r));
    DestroyOutputDevice(d, acc);
  }
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

// The file plugin records exactly what the device thread wrote: the silent
// prefill (one buffer) followed by every frame the callback produced, in order.
SND_TEST(Alsa, FilePluginReceivesExactlyTheCallbackFrames) {
  TempFile tmp;
  char config[512];
  std::snprintf(config, sizeof(config),
                "pcm.sndnull { type null } "
                "pcm.sndfile { type file slave.pcm \"sndnull\" file \"%s\" format \"raw\" }",
                tmp.path);
  OutputDeviceOptions o = Options("sndfile", config);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
  SND_REQUIRE(d != nullptr);
  Ramp r;
  r.device = d;
  SND_REQUIRE(d->Open(snd::DeviceDesc{48000, 2, 256, 0}, &Ramp::Callback, &r));
  const std::uint32_t period = d->PeriodFrames();
  SND_REQUIRE(d->Start());
  SND_REQUIRE(WaitCalls(r, 40));
  d->Stop();
  const std::uint64_t calls = r.calls.load();
  SND_CHECK(r.ftz.load());
  SND_CHECK(r.rt.load());
  SND_CHECK_EQ(r.lastFrames.load(), period);
  SND_CHECK_EQ(r.lastChannels.load(), 2u);
  const DeviceStats st = d->Stats();
  SND_CHECK_EQ(st.callbacks, calls);
  SND_CHECK_EQ(st.framesDelivered, calls * period);
  SND_CHECK_EQ(st.xruns, 0u);
  OutputClockSample clock;
  SND_CHECK(d->OutputClock(&clock));
  SND_CHECK_EQ(clock.sampleRate, 48000u);
  SND_CHECK(clock.framesPlayed <= clock.framesWritten);
  d->Close();
  DestroyOutputDevice(d, snd::base::DefaultAllocator());

  const std::vector<unsigned char> bytes = tmp.Read();
  SND_REQUIRE(bytes.size() % 8u == 0u);
  const std::size_t frames = bytes.size() / 8u;
  std::vector<float> data(frames * 2u);
  std::memcpy(data.data(), bytes.data(), bytes.size());
  // Leading silence (prefill), then the ramp from frame 0.
  std::size_t lead = 0;
  while (lead < frames && data[lead * 2] == 0.0f) ++lead;
  snd::test::Note("file: %zu frames, %zu silent prefill, %llu callbacks of %u", frames, lead,
                  static_cast<unsigned long long>(calls), period);
  SND_CHECK(lead > 0u && lead % period == 0u);
  SND_CHECK_EQ(frames - lead, static_cast<std::size_t>(calls) * period);
  bool exact = true;
  for (std::size_t f = lead; f < frames; ++f) {
    exact = exact && data[f * 2] == Ramp::Value(f - lead, 0) && data[f * 2 + 1] == Ramp::Value(f - lead, 1);
  }
  SND_CHECK(exact);
}

// A PCM that only takes integer formats: the device negotiates S32 and
// converts on the device thread; the slave stores S16 (ALSA keeps the top 16 bits).
SND_TEST(Alsa, IntegerOnlyDeviceGetsConvertedSamples) {
  TempFile tmp;
  char config[640];
  std::snprintf(config, sizeof(config),
                "pcm.sndnull { type null } "
                "pcm.sndfile { type file slave.pcm \"sndnull\" file \"%s\" format \"raw\" } "
                "pcm.sndint { type linear slave { pcm \"sndfile\" format S16_LE } }",
                tmp.path);
  OutputDeviceOptions o = Options("sndint", config);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
  SND_REQUIRE(d != nullptr);
  Ramp r;
  SND_REQUIRE(d->Open(snd::DeviceDesc{48000, 1, 128, 0}, &Ramp::Callback, &r));
  SND_REQUIRE(d->Start());
  SND_REQUIRE(WaitCalls(r, 20));
  d->Stop();
  const std::uint64_t calls = r.calls.load();
  const std::uint32_t period = d->PeriodFrames();
  d->Close();
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
  const std::vector<unsigned char> bytes = tmp.Read();
  const std::size_t frames = bytes.size() / 2u;
  std::vector<std::int16_t> s16(frames);
  std::memcpy(s16.data(), bytes.data(), frames * 2u);
  std::size_t lead = 0;
  while (lead < frames && s16[lead] == 0) ++lead;
  // The ramp starts at 1/2^20, which is 0 in 16 bits: find the start by count instead.
  const std::size_t start = frames - static_cast<std::size_t>(calls) * period;
  SND_REQUIRE(start <= lead);
  int worst = 0;
  for (std::size_t f = start; f < frames; ++f) {
    const double ideal = static_cast<double>(Ramp::Value(f - start, 0)) * 32767.0;
    const int diff = static_cast<int>(std::lround(std::fabs(static_cast<double>(s16[f]) - ideal)));
    if (diff > worst) worst = diff;
  }
  snd::test::Note("int16 via S32: worst error %d LSB over %zu frames", worst, frames - start);
  SND_CHECK(worst <= 1);
}

SND_TEST(Alsa, StopAndCloseFromCallbackAreIgnored) {
  OutputDeviceOptions o = Options("sndnull", kNullConfig);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
  SND_REQUIRE(d != nullptr);
  Ramp r;
  r.device = d;
  r.misuse.store(3);
  SND_REQUIRE(d->Open(snd::DeviceDesc{48000, 2, 256, 0}, &Ramp::Callback, &r));
  SND_REQUIRE(d->Start());
  SND_REQUIRE(WaitCalls(r, 10));
  SND_CHECK_EQ(d->Stats().misuseFromCallback, 6u);  // 3 x (Stop + Close)
  d->Stop();
  d->Close();
  // Restart after Stop works (prepare again).
  SND_REQUIRE(d->Open(snd::DeviceDesc{48000, 2, 256, 0}, &Ramp::Callback, &r));
  SND_REQUIRE(d->Start());
  const std::uint64_t before = r.calls.load();
  SND_REQUIRE(WaitCalls(r, before + 5));
  d->Stop();
  SND_REQUIRE(d->Start());
  SND_REQUIRE(WaitCalls(r, r.calls.load() + 5));
  DestroyOutputDevice(d, snd::base::DefaultAllocator());  // closes
}

SND_TEST(Alsa, EnumerationIsConsistent) {
  const std::uint32_t n = EnumerateOutputDevices(OutputBackend::Alsa, nullptr, 0);
  std::vector<OutputDeviceInfo> list(n + 1u);
  const std::uint32_t m = EnumerateOutputDevices(OutputBackend::Alsa, list.data(), n + 1u);
  SND_CHECK_EQ(m, n);
  for (std::uint32_t i = 0; i < m; ++i) {
    SND_CHECK(list[i].id[0] != '\0');
    SND_CHECK(std::strchr(list[i].name, '\n') == nullptr);
    snd::test::Note("ALSA device %u: %s (%s)%s", i + 1u, list[i].id, list[i].name, list[i].isDefault ? " [default]" : "");
  }
  // device_index beyond the list fails at Open (D-64).
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator());
  SND_REQUIRE(d != nullptr);
  Ramp r;
  SND_CHECK(!d->Open(snd::DeviceDesc{48000, 2, 256, n + 1u}, &Ramp::Callback, &r));
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
}

// The AudioDriver over ALSA. The null PCM consumes as fast as the device
// thread writes, far faster than real time, so the audio thread cannot keep
// up: this is an overload / underrun stress test. Every frame that reached
// the file is either the next rendered frame or counted silence.
SND_TEST(Alsa, AudioDriverOverFilePluginUnderStress) {
  TempFile tmp;
  char config[512];
  std::snprintf(config, sizeof(config),
                "pcm.sndnull { type null } "
                "pcm.sndfile { type file slave.pcm \"sndnull\" file \"%s\" format \"raw\" }",
                tmp.path);
  OutputDeviceOptions o = Options("sndfile", config);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
  SND_REQUIRE(d != nullptr);
  struct Counter {
    // Frame n -> (n + 1) / 2^23 (exact, no wrap for 8 M frames).
    static float Value(std::uint64_t frame, std::uint32_t c) {
      return static_cast<float>(frame + 1u) / 8388608.0f * static_cast<float>(c + 1u);
    }
    std::uint64_t next = 0;
    static void Render(void* u, float* out, std::uint32_t frames, std::uint32_t channels, const RenderInfo*) {
      auto* c = static_cast<Counter*>(u);
      for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) out[f * channels + ch] = Value(c->next + f, ch);
      }
      c->next += frames;
    }
  } counter;
  AudioDriver driver;
  AudioDriverDesc ad;
  ad.core.blockFrames = 256;
  ad.core.fadeFrames = 0;
  ad.core.render = &Counter::Render;
  ad.core.user = &counter;
  ad.realtimeThread = false;
  ad.workgroupSource = d;
  SND_REQUIRE(driver.Start(*d, snd::DeviceDesc{48000, 2, 256, 0}, ad, snd::base::DefaultAllocator()));
  const std::uint64_t deadline = MonotonicNanoseconds() + 5000000000ull;
  while (driver.Stats().core.callbacks < 1000u && MonotonicNanoseconds() < deadline) SleepFor(200000);
  driver.Stop();
  const AudioDriverStats s = driver.Stats();
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
  const std::vector<unsigned char> bytes = tmp.Read();
  const std::size_t frames = bytes.size() / 8u;
  std::vector<float> data(frames * 2u);
  std::memcpy(data.data(), bytes.data(), frames * 8u);
  std::uint64_t expect = 0, silent = 0, valid = 0;
  bool ok = true;
  for (std::size_t f = 0; f < frames; ++f) {
    if (data[f * 2] == 0.0f && data[f * 2 + 1] == 0.0f) {
      ++silent;
      continue;
    }
    ok = ok && data[f * 2] == Counter::Value(expect, 0) && data[f * 2 + 1] == Counter::Value(expect, 1);
    ++expect;
    ++valid;
  }
  snd::test::Note("frames %zu valid %llu silent %llu underruns %llu callbacks %llu", frames,
                  static_cast<unsigned long long>(valid), static_cast<unsigned long long>(silent),
                  static_cast<unsigned long long>(s.core.underruns), static_cast<unsigned long long>(s.core.callbacks));
  SND_CHECK(ok);
  SND_CHECK(valid > 0u);
  SND_CHECK(valid < (1u << 23));
  // Silence in the file = device prefill + every lost frame the driver counted.
  SND_CHECK_EQ(valid + s.core.lostFrames, s.core.framesConsumed);
  SND_CHECK(silent >= s.core.lostFrames);
  SND_CHECK_EQ(s.workgroup, WorkgroupJoinResult::NoWorkgroup);
}
