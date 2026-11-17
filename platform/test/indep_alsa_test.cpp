// Independent tests of the ALSA backend (CONTRACTS.md D-61..D-64) and of the
// float -> integer conversion it shares with WASAPI (D-62), written from the
// specification and the public headers (properties: section F).
//
// No sound card: the PCMs come from a private configuration
// (OutputDeviceOptions::alsaConfig -> snd_pcm_open_lconf). main() at the end
// exits with 77 (CTest SKIP) when libasound cannot open the private null PCM.
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/audio_driver.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {

constexpr const char* kNull = "pcm.inull { type null }";

struct Tmp {
  char path[80];
  Tmp() noexcept {
    std::snprintf(path, sizeof(path), "/tmp/snd-indep-alsa-%d-XXXXXX", static_cast<int>(getpid()));
    const int fd = mkstemp(path);
    if (fd >= 0) close(fd);
  }
  ~Tmp() { std::remove(path); }
  std::vector<unsigned char> Read() const {
    std::vector<unsigned char> v;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return v;
    unsigned char b[8192];
    std::size_t n;
    while ((n = std::fread(b, 1, sizeof(b), f)) > 0) v.insert(v.end(), b, b + n);
    std::fclose(f);
    return v;
  }
};

std::size_t StackSize() {
  pthread_attr_t a;
  if (pthread_getattr_np(pthread_self(), &a) != 0) return 0;
  std::size_t s = 0;
  pthread_attr_getstacksize(&a, &s);
  pthread_attr_destroy(&a);
  return s;
}

// Callback: frame n channel c -> ((n % 2^19) + 1) / 2^20 * (c + 1) (exact in float),
// or a fixed table of special values (conversion test).
struct Source {
  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint64_t> calls{0};
  std::atomic<bool> badLength{false};
  std::atomic<bool> badChannels{false};
  std::atomic<bool> notRt{false};
  std::atomic<bool> noFtz{false};
  std::atomic<std::size_t> stack{0};
  std::uint32_t period = 0;
  std::uint32_t channels = 0;
  const float* table = nullptr;  // non-null: repeat these values (mono)
  std::uint32_t tableSize = 0;
  static float Value(std::uint64_t frame, std::uint32_t c) {
    return static_cast<float>((frame % (1u << 19)) + 1u) / 1048576.0f * static_cast<float>(c + 1u);
  }
  static void Cb(void* u, float* out, std::uint32_t n, std::uint32_t ch) {
    auto* s = static_cast<Source*>(u);
    if (n == 0u || n > s->period) s->badLength.store(true);
    if (ch != s->channels) s->badChannels.store(true);
    if (snd::rt::kEnabled && !snd::rt::InRtScope()) s->notRt.store(true);
    if (GetCpuFeatures().denormalControl && !FlushDenormalsEnabled()) s->noFtz.store(true);
    if (s->stack.load() == 0u) {
      snd::rt::SuspendScope sus;
      s->stack.store(StackSize());
    }
    const std::uint64_t base = s->frames.load(std::memory_order_relaxed);
    for (std::uint32_t f = 0; f < n; ++f) {
      for (std::uint32_t c = 0; c < ch; ++c) {
        out[f * ch + c] = s->table != nullptr ? s->table[(base + f) % s->tableSize] : Value(base + f, c);
      }
    }
    s->frames.store(base + n, std::memory_order_relaxed);
    s->calls.fetch_add(1);
  }
};

bool WaitCalls(const Source& s, std::uint64_t n) {
  const std::uint64_t deadline = MonotonicNanoseconds() + 5000000000ull;
  while (s.calls.load() < n && MonotonicNanoseconds() < deadline) SleepFor(500000);
  return s.calls.load() >= n;
}

OutputDeviceOptions Opts(const char* name, const char* config, std::uint32_t periods = 0) {
  OutputDeviceOptions o;
  o.deviceName = name;
  o.alsaConfig = config;
  o.realtimeThread = false;
  o.bufferPeriods = periods;
  return o;
}

std::uint32_t BufferFramesOf(const PlatformOutputDevice& d) {
  // D-62: ALSA latency = buffer length.
  return static_cast<std::uint32_t>(std::lround(d.OutputLatencySeconds() * d.SampleRate()));
}

void FileConfig(char* out, std::size_t n, const char* path) {
  std::snprintf(out, n,
                "pcm.inull { type null } "
                "pcm.ifile { type file slave.pcm \"inull\" file \"%s\" format \"raw\" }",
                path);
}

}  // namespace

bool AlsaIndepUsable() {
  OutputDeviceOptions o = Opts("inull", kNull);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
  if (d == nullptr) return false;
  Source s;
  const bool ok = d->Open(snd::DeviceDesc{48000, 2, 256, 0}, &Source::Cb, &s);
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
  return ok;
}

// D-63: start = one buffer of silence, then the callback frames, exact in
// float, frames 1..PeriodFrames() in the device layout, FTZ/DAZ, RT scope,
// D-47 stack. Also with bufferPeriods 5 and an odd format (44.1 kHz, 3 ch, period 100).
SND_TEST(IndepAlsa, PrefillIsOneBufferThenExactData) {
  struct Case {
    std::uint32_t rate, ch, period, periods;
  } cases[] = {{48000, 2, 256, 0}, {44100, 3, 100, 5}, {96000, 1, 333, 2}};
  for (const Case& cs : cases) {
    Tmp tmp;
    char cfg[512];
    FileConfig(cfg, sizeof(cfg), tmp.path);
    OutputDeviceOptions o = Opts("ifile", cfg, cs.periods);
    PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
    SND_REQUIRE(d != nullptr);
    Source s;
    SND_REQUIRE(d->Open(snd::DeviceDesc{cs.rate, cs.ch, cs.period, 0}, &Source::Cb, &s));
    s.period = d->PeriodFrames();
    s.channels = d->Channels();
    const std::uint32_t ch = s.channels;
    const std::uint32_t buffer = BufferFramesOf(*d);
    snd::test::Note("asked %u/%u/%u x%u -> rate %u ch %u period %u buffer %u", cs.rate, cs.ch, cs.period, cs.periods,
                    d->SampleRate(), ch, s.period, buffer);
    SND_CHECK_EQ(d->SampleRate(), cs.rate);
    SND_CHECK_EQ(ch, cs.ch);
    if (cs.periods != 0u) SND_CHECK_EQ(buffer, s.period * cs.periods);
    SND_REQUIRE(d->Start());
    SND_REQUIRE(WaitCalls(s, 30));
    d->Stop();
    OutputClockSample clk;
    SND_REQUIRE(d->OutputClock(&clk));
    const DeviceStats st = d->Stats();
    d->Close();
    DestroyOutputDevice(d, snd::base::DefaultAllocator());
    SND_CHECK(!s.badLength.load());
    SND_CHECK(!s.badChannels.load());
    SND_CHECK(!s.notRt.load());
    SND_CHECK(!s.noFtz.load());
    SND_CHECK(s.stack.load() >= kRtThreadMinStackBytes);
    const std::vector<unsigned char> bytes = tmp.Read();
    SND_REQUIRE(bytes.size() % (4u * ch) == 0u);
    const std::size_t frames = bytes.size() / (4u * ch);
    std::vector<float> data(frames * ch);
    std::memcpy(data.data(), bytes.data(), bytes.size());
    std::size_t lead = 0;
    while (lead < frames && data[lead * ch] == 0.0f) ++lead;
    const std::uint64_t delivered = s.frames.load();
    SND_CHECK_EQ(lead, static_cast<std::size_t>(buffer / s.period * s.period));  // "one buffer" in whole periods
    SND_CHECK_EQ(frames - lead, static_cast<std::size_t>(delivered));           // nothing lost at Stop()
    SND_CHECK_EQ(st.framesDelivered, delivered);
    SND_CHECK_EQ(clk.framesWritten, static_cast<std::uint64_t>(frames));       // D-62: OS frames since Open
    SND_CHECK(clk.framesPlayed <= clk.framesWritten);
    bool exact = true;
    for (std::size_t f = lead; f < frames; ++f) {
      for (std::uint32_t c = 0; c < ch; ++c) exact = exact && data[f * ch + c] == Source::Value(f - lead, c);
    }
    SND_CHECK(exact);
  }
}

// Stop / Start / Stop on one Open: each start prefills one buffer of silence
// again, the callback stream continues, nothing the callback produced is
// dropped at Stop() (D-61: "Stop returns after the last callback"; the frames
// of that callback still reach the device).
SND_TEST(IndepAlsa, RestartKeepsEveryCallbackFrame) {
  Tmp tmp;
  char cfg[512];
  FileConfig(cfg, sizeof(cfg), tmp.path);
  OutputDeviceOptions o = Opts("ifile", cfg);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
  SND_REQUIRE(d != nullptr);
  Source s;
  SND_REQUIRE(d->Open(snd::DeviceDesc{48000, 2, 128, 0}, &Source::Cb, &s));
  s.period = d->PeriodFrames();
  s.channels = 2;
  const std::uint32_t lead = BufferFramesOf(*d) / s.period * s.period;
  std::uint64_t seg[3];
  for (int k = 0; k < 3; ++k) {
    const std::uint64_t before = s.frames.load();
    SND_REQUIRE(d->Start());
    SND_REQUIRE(WaitCalls(s, s.calls.load() + 10u + static_cast<std::uint64_t>(k) * 7u));
    d->Stop();
    seg[k] = s.frames.load() - before;
  }
  d->Close();
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
  const std::vector<unsigned char> bytes = tmp.Read();
  const std::size_t frames = bytes.size() / 8u;
  std::vector<float> data(frames * 2u);
  std::memcpy(data.data(), bytes.data(), frames * 8u);
  snd::test::Note("file %zu frames; segments %llu %llu %llu, lead %u", frames, static_cast<unsigned long long>(seg[0]),
                  static_cast<unsigned long long>(seg[1]), static_cast<unsigned long long>(seg[2]), lead);
  SND_CHECK_EQ(frames, static_cast<std::size_t>(3u * lead + seg[0] + seg[1] + seg[2]));
  // Walk: silence(lead), segment, silence(lead), segment, ...
  std::size_t at = 0;
  std::uint64_t value = 0;
  bool ok = true;
  for (int k = 0; k < 3 && ok; ++k) {
    for (std::uint32_t i = 0; i < lead && at < frames; ++i, ++at) ok = ok && data[at * 2] == 0.0f;
    for (std::uint64_t i = 0; i < seg[k] && at < frames; ++i, ++at, ++value) {
      ok = ok && data[at * 2] == Source::Value(value, 0) && data[at * 2 + 1] == Source::Value(value, 1);
    }
  }
  SND_CHECK(ok);
}

// Integer-only PCM: the backend picks S32 (float refused) and converts on the
// device thread: clipping, rounding and NaN -> 0 are visible bit-exactly in
// the file (linear plugin S32 -> S32 is a copy).
SND_TEST(IndepAlsa, S32PathIsBitExact) {
  Tmp tmp;
  char cfg[700];
  std::snprintf(cfg, sizeof(cfg),
                "pcm.inull { type null } "
                "pcm.ifile { type file slave.pcm \"inull\" file \"%s\" format \"raw\" } "
                "pcm.iint { type linear slave { pcm \"ifile\" format S32_LE } }",
                tmp.path);
  OutputDeviceOptions o = Opts("iint", cfg);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
  SND_REQUIRE(d != nullptr);
  static const float table[] = {0.5f,  -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, std::numeric_limits<float>::quiet_NaN(),
                                0.25f, 1e-12f, -0.125f, 0.999999f};
  static const std::int32_t expect[] = {1073741824, -1073741824, 2147483647, -2147483647, 2147483647,
                                        -2147483647, 0, 536870912, 0, -268435456,
                                        static_cast<std::int32_t>(std::llround(0.999999f * 2147483647.0))};
  constexpr std::uint32_t kN = sizeof(table) / sizeof(table[0]);
  Source s;
  s.table = table;
  s.tableSize = kN;
  SND_REQUIRE(d->Open(snd::DeviceDesc{48000, 1, 64, 0}, &Source::Cb, &s));
  s.period = d->PeriodFrames();
  s.channels = 1;
  SND_REQUIRE(d->Start());
  SND_REQUIRE(WaitCalls(s, 10));
  d->Stop();
  const std::uint64_t delivered = s.frames.load();
  d->Close();
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
  const std::vector<unsigned char> bytes = tmp.Read();
  const std::size_t frames = bytes.size() / 4u;
  std::vector<std::int32_t> v(frames);
  std::memcpy(v.data(), bytes.data(), frames * 4u);
  SND_REQUIRE(frames >= delivered);
  const std::size_t start = frames - static_cast<std::size_t>(delivered);
  bool ok = true;
  for (std::size_t i = 0; i < start; ++i) ok = ok && v[i] == 0;
  for (std::size_t i = start; i < frames; ++i) {
    if (v[i] != expect[(i - start) % kN]) {
      if (ok) snd::test::Note("sample %zu: %d expected %d", i - start, v[i], expect[(i - start) % kN]);
      ok = false;
    }
  }
  SND_CHECK(ok);
}

// D-64: device_index k >= 1 is entry k-1 of the enumeration (system
// configuration); 0 is "default"; out of range fails; a name that does not
// exist fails and the device is NOT silently replaced (D-61).
SND_TEST(IndepAlsa, DeviceIndexFollowsEnumeration) {
  const std::uint32_t n = EnumerateOutputDevices(OutputBackend::Alsa, nullptr, 0);
  std::vector<OutputDeviceInfo> list(n + 2u);
  // Capacity smaller than the count: only `capacity` entries written, count returned.
  OutputDeviceInfo canary[2];
  std::memset(static_cast<void*>(canary), 0x5A, sizeof(canary));
  if (n >= 1u) {
    SND_CHECK_EQ(EnumerateOutputDevices(OutputBackend::Alsa, canary, 1), n);
    SND_CHECK(canary[0].id[0] != '\0' && std::memchr(canary[0].id, '\0', sizeof(canary[0].id)) != nullptr);
    SND_CHECK_EQ(static_cast<unsigned char>(canary[1].id[0]), 0x5Au);
  }
  SND_CHECK_EQ(EnumerateOutputDevices(OutputBackend::Alsa, list.data(), n + 2u), n);
  std::int32_t nullAt = -1;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (std::strcmp(list[i].id, "null") == 0) nullAt = static_cast<std::int32_t>(i);
  }
  snd::test::Note("%u ALSA devices, 'null' at %d", n, nullAt);
  Source s;
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator());
  SND_REQUIRE(d != nullptr);
  SND_CHECK(!d->Open(snd::DeviceDesc{48000, 2, 256, n + 1u}, &Source::Cb, &s));
  SND_CHECK(!d->Open(snd::DeviceDesc{48000, 2, 256, 0xFFFFFFFFu}, &Source::Cb, &s));
  if (nullAt >= 0) {
    SND_CHECK(d->Open(snd::DeviceDesc{48000, 2, 256, static_cast<std::uint32_t>(nullAt) + 1u}, &Source::Cb, &s));
    s.period = d->PeriodFrames();
    s.channels = d->Channels();
    SND_CHECK(d->Start());
    SND_CHECK(WaitCalls(s, 5));
    d->Stop();
    d->Close();
  }
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
  // Unknown name, private config: fails, still ALSA.
  OutputDeviceOptions o = Opts("no-such-pcm-xyz", kNull);
  PlatformOutputDevice* u = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
  SND_REQUIRE(u != nullptr);
  SND_CHECK(!u->Open(snd::DeviceDesc{}, &Source::Cb, &s));
  SND_CHECK_EQ(u->Backend(), OutputBackend::Alsa);
  SND_CHECK(!u->Start());
  DestroyOutputDevice(u, snd::base::DefaultAllocator());
}

// Lifecycle under concurrency (TSan / Release x20): Open / Start / Stop /
// Close cycles while another thread replaces the listener and reads the
// queries, stats and clock (D-25: queries from any thread after Open).
SND_TEST(IndepAlsa, LifecycleCyclesWithConcurrentReaders) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  OutputDeviceOptions o = Opts("inull", kNull);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, acc, &o);
  SND_REQUIRE(d != nullptr);
  Source s;
  std::atomic<bool> opened{false};
  std::atomic<bool> stop{false};
  struct Ev {
    static void Fn(void*, const snd::DeviceEvent*) {}
  };
  std::thread reader([&] {
    while (!stop.load()) {
      d->SetEventListener((MonotonicNanoseconds() & 1u) != 0u ? &Ev::Fn : nullptr, nullptr);
      if (opened.load()) {
        volatile std::uint32_t x = d->PeriodFrames() + d->SampleRate() + d->Channels();
        (void)x;
        volatile double l = d->OutputLatencySeconds();
        (void)l;
      }
      OutputClockSample c;
      (void)d->OutputClock(&c);
      const DeviceStats st = d->Stats();
      (void)st;
      std::this_thread::yield();
    }
  });
  for (int i = 0; i < 25; ++i) {
    const std::uint32_t ch = 1u + static_cast<std::uint32_t>(i % 4);
    SND_REQUIRE(d->Open(snd::DeviceDesc{48000, ch, 64u + static_cast<std::uint32_t>(i % 3) * 100u, 0}, &Source::Cb, &s));
    s.period = d->PeriodFrames();
    s.channels = ch;
    opened.store(true);
    for (int k = 0; k < 2; ++k) {
      SND_REQUIRE(d->Start());
      SND_REQUIRE(WaitCalls(s, s.calls.load() + 3u));
      d->Stop();
    }
    opened.store(false);
    d->Close();
  }
  stop.store(true);
  reader.join();
  SND_CHECK(!s.badLength.load());
  SND_CHECK(!s.badChannels.load());
  DestroyOutputDevice(d, acc);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

// AudioDriver over ALSA with odd sizes (block 100 against period 333): the
// file holds one gapless stream; silence only where counted (fade off).
SND_TEST(IndepAlsa, AudioDriverOddSizesAccounting) {
  Tmp tmp;
  char cfg[512];
  FileConfig(cfg, sizeof(cfg), tmp.path);
  OutputDeviceOptions o = Opts("ifile", cfg);
  PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
  SND_REQUIRE(d != nullptr);
  struct R {
    std::uint64_t next = 0;
    std::uint32_t bad = 0;
    static void Render(void* u, float* out, std::uint32_t frames, std::uint32_t ch, const RenderInfo*) {
      auto* r = static_cast<R*>(u);
      if (frames != 100u) ++r->bad;
      for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < ch; ++c) out[f * ch + c] = static_cast<float>(((r->next + f) % (1u << 22)) + 1u) * static_cast<float>(c + 1u) / 8388608.0f;
      }
      r->next += frames;
    }
  } rr;
  AudioDriver drv;
  AudioDriverDesc ad;
  ad.core.blockFrames = 100;
  ad.core.renderAheadBlocks = 1;
  ad.core.fadeFrames = 0;
  ad.core.render = &R::Render;
  ad.core.user = &rr;
  ad.realtimeThread = false;
  SND_REQUIRE(drv.Start(*d, snd::DeviceDesc{48000, 2, 333, 0}, ad, snd::base::DefaultAllocator()));
  const std::uint64_t deadline = MonotonicNanoseconds() + 3000000000ull;
  while (drv.Stats().core.callbacks < 400u && MonotonicNanoseconds() < deadline) SleepFor(500000);
  drv.Stop();
  const AudioDriverStats s = drv.Stats();
  DestroyOutputDevice(d, snd::base::DefaultAllocator());
  const std::vector<unsigned char> bytes = tmp.Read();
  const std::size_t frames = bytes.size() / 8u;
  std::vector<float> data(frames * 2u);
  std::memcpy(data.data(), bytes.data(), frames * 8u);
  std::uint64_t expect = 0, valid = 0, silent = 0;
  bool ok = true;
  for (std::size_t f = 0; f < frames; ++f) {
    if (data[f * 2] == 0.0f && data[f * 2 + 1] == 0.0f) {
      ++silent;
      continue;
    }
    const float e = static_cast<float>((expect % (1u << 22)) + 1u) / 8388608.0f;
    ok = ok && data[f * 2] == e && data[f * 2 + 1] == e * 2.0f;
    ++expect;
    ++valid;
  }
  SND_CHECK(ok);
  SND_CHECK_EQ(rr.bad, 0u);
  SND_CHECK_EQ(valid + s.core.lostFrames, s.core.framesConsumed);
  SND_CHECK(valid > 0u);
}

// D-63 loss and re-open. A file plugin writing into a pipe whose reader exits
// after N bytes makes snd_pcm_writei fail with -EIO (a vanished device): the
// backend posts LOST, closes, and with reopenOnLoss opens the same name again
// every reopenIntervalMs; success posts CHANGED and the callbacks resume.
// Without reopenOnLoss: one LOST, no more callbacks. Stop() is never held up.
namespace {
struct EvLog {
  std::atomic<std::uint32_t> lost{0}, changed{0}, other{0};
  std::atomic<std::uint32_t> changedWithoutLost{0};
  std::atomic<std::uint32_t> lastChannels{0}, lastRate{0}, lastPeriod{0};
  static void Fn(void* u, const snd::DeviceEvent* e) {
    auto* s = static_cast<EvLog*>(u);
    if (e->kind == SND_DEVICE_EVENT_LOST) {
      s->lost.fetch_add(1);
    } else if (e->kind == SND_DEVICE_EVENT_CHANGED) {
      if (s->changed.load() >= s->lost.load()) s->changedWithoutLost.fetch_add(1);
      s->changed.fetch_add(1);
      s->lastChannels.store(e->channels);
      s->lastRate.store(e->sample_rate);
      s->lastPeriod.store(e->period_frames);
    } else {
      s->other.fetch_add(1);
    }
  }
};
}  // namespace

SND_TEST(IndepAlsa, LostDeviceIsReportedAndReopened) {
  signal(SIGPIPE, SIG_IGN);  // the pipe reader exits on purpose
  const char* cfg =
      "pcm.inull { type null } "
      "pcm.ipipe { type file slave.pcm \"inull\" file \"|head -c 60000 >/dev/null\" format \"raw\" }";
  for (int reopen = 1; reopen >= 0; --reopen) {
    OutputDeviceOptions o = Opts("ipipe", cfg);
    o.reopenOnLoss = reopen != 0;
    o.reopenIntervalMs = 30;
    PlatformOutputDevice* d = CreateOutputDevice(OutputBackend::Alsa, snd::base::DefaultAllocator(), &o);
    SND_REQUIRE(d != nullptr);
    EvLog ev;
    d->SetEventListener(&EvLog::Fn, &ev);
    Source s;
    SND_REQUIRE(d->Open(snd::DeviceDesc{48000, 2, 256, 0}, &Source::Cb, &s));
    s.period = d->PeriodFrames();
    s.channels = 2;
    SND_REQUIRE(d->Start());
    const std::uint64_t deadline = MonotonicNanoseconds() + 5000000000ull;
    const std::uint32_t wantLost = reopen != 0 ? 3u : 1u;
    while (ev.lost.load() < wantLost && MonotonicNanoseconds() < deadline) SleepFor(1000000);
    if (reopen != 0) {
      while (ev.changed.load() < wantLost && MonotonicNanoseconds() < deadline) SleepFor(1000000);
    } else {
      SleepFor(150000000ull);
    }
    const std::uint64_t callsAtCheck = s.calls.load();
    SleepFor(60000000ull);
    const std::uint64_t t0 = MonotonicNanoseconds();
    d->Stop();
    const std::uint64_t stopMs = (MonotonicNanoseconds() - t0) / 1000000u;
    const DeviceStats st = d->Stats();
    snd::test::Note("reopen=%d: lost %u changed %u reopens %llu losses %llu calls %llu, Stop took %llu ms", reopen,
                    ev.lost.load(), ev.changed.load(), static_cast<unsigned long long>(st.reopens),
                    static_cast<unsigned long long>(st.losses), static_cast<unsigned long long>(s.calls.load()),
                    static_cast<unsigned long long>(stopMs));
    SND_CHECK(ev.lost.load() >= wantLost);
    SND_CHECK_EQ(st.losses, ev.lost.load());
    SND_CHECK_EQ(ev.changedWithoutLost.load(), 0u);
    SND_CHECK(stopMs < 300u);
    if (reopen != 0) {
      SND_CHECK(ev.changed.load() >= wantLost);
      SND_CHECK_EQ(st.reopens, ev.changed.load());
      SND_CHECK_EQ(ev.lastChannels.load(), 2u);
      SND_CHECK_EQ(ev.lastRate.load(), 48000u);
      SND_CHECK_EQ(ev.lastPeriod.load(), s.period);
    } else {
      SND_CHECK_EQ(ev.lost.load(), 1u);
      SND_CHECK_EQ(ev.changed.load(), 0u);
      SND_CHECK_EQ(st.reopens, 0u);
      SND_CHECK_EQ(s.calls.load(), callsAtCheck);  // no callbacks after the loss
    }
    SND_CHECK(!s.badLength.load());
    d->Close();
    DestroyOutputDevice(d, snd::base::DefaultAllocator());
  }
}

int main(int argc, char** argv) {
  if (!AlsaIndepUsable()) {
    std::printf("SKIP: libasound cannot open a private 'null' PCM; ALSA tests not run\n");
    return 77;
  }
  return snd::test::RunAll(argc, argv);
}
