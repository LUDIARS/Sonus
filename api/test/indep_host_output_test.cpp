// Independent tests: the built-in output devices (CONTRACTS.md D-102) and the 1.2 host
// fields of snd_config (config.h SND_CONFIG_TOP_1_2_HOST).
// Properties (from D-102, config.h, sonus.h, sonus.hpp; before reading the implementation):
//  H1 output_kind outside 0..2 -> INVALID_ARGUMENT; a game device with SILENT / MANUAL ->
//     INVALID_ARGUMENT; a game device with PLATFORM is fine.
//  H2 snd_advance_output: sys NULL -> INVALID_ARGUMENT; another thread -> WRONG_THREAD; a
//     system without the manual device -> UNSUPPORTED; out non-NULL with out_frames below
//     periods x block_size -> INVALID_ARGUMENT (exactly enough is fine); out NULL and
//     periods 0 are fine.
//  H3 the frames delivered, concatenated, ARE the common clock: frame i of all deliveries is
//     common-clock sample i (checked with a ramp synth that writes f(clock)); slow advancing
//     never underruns.
//  H4 output_channels 1 -> the out buffer is mono, 0 -> 2 channels; one period = block_size.
//  H5 the wait before a period is bounded (about 2 s): if the audio thread is stuck the call
//     returns after about 2 s, the period is an underrun, the clock catches up; afterwards
//     advancing is fast again (the bound does not apply to every later period).
//  H6 SILENT advances in real time.
//  S1 struct_size: the 1.2 layout before the host fields (a caller built against the earlier
//     1.2 header) is accepted and gets file_system_root NULL / output_kind PLATFORM (config.h:
//     "a config whose struct_size ends before them ... gets the defaults").
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "config_check.hpp"
#include "indep_rig.hpp"

using apitest::kCenter;
using apitest::SmallConfig;

namespace {

constexpr std::uint32_t kBlock = 256;

float RampAt(std::uint64_t c) { return 0.1f + 0.0008f * static_cast<float>(c % 1000u); }

struct Gate {
  std::atomic<int> hold{0};
  std::atomic<int> inside{0};
};

void SND_CALL RampSynth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                        const snd_render_context* ctx) {
  auto* g = static_cast<Gate*>(user);
  if (g != nullptr && g->hold.load(std::memory_order_acquire) != 0) {
    g->inside.store(1, std::memory_order_release);
    while (g->hold.load(std::memory_order_acquire) != 0) {
    }
    g->inside.store(0, std::memory_order_release);
  }
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = RampAt(ctx->clock_sample + f);
  }
}

// A system on a built-in device (no game device), the in-memory file system.
struct Host {
  explicit Host(snd_output_kind kind, std::uint32_t channels = 2) {
    snd_config c = SmallConfig();
    c.output_kind = kind;
    c.output_channels = channels;
    fsTable = snd::ToC(fs);
    created = snd_create(&c, nullptr, &fsTable, &sys);
  }
  ~Host() { snd_destroy(sys); }
  snd::mock::MemoryFileSystem fs;
  snd_file_system fsTable{};
  snd_system* sys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
};

snd_playback_handle PlayRamp(snd_system* sys, Gate* g) {
  snd_synth_desc d{};
  d.callback = &RampSynth;
  d.user = g;
  d.channels = 1;
  d.mode = SND_SYNTH_LOW_LATENCY;
  d.volume = 1.0f;
  return snd_play_synth(sys, &d, snd_emitter_handle{0}, nullptr);
}

}  // namespace

// H1
SND_TEST(IndepHostOutput, OutputKindRules) {
  snd::mock::NullOutputDevice dev;
  snd_output_device dt = snd::ToC(dev);
  snd::mock::MemoryFileSystem fs;
  snd_file_system ft = snd::ToC(fs);
  auto create = [&](snd_output_kind kind, bool gameDevice) {
    snd_config c = SmallConfig();
    c.output_kind = kind;
    if (gameDevice) c.output.device = &dt;
    snd_system* sys = reinterpret_cast<snd_system*>(&c);
    const snd_result r = snd_create(&c, nullptr, &ft, &sys);
    if (r != SND_OK) SND_CHECK(sys == nullptr);
    if (r != SND_OK) sys = nullptr;
    snd_destroy(sys);
    return r;
  };
  for (const snd_output_kind k : {3u, 4u, 0x80000000u, 0xFFFFFFFFu}) {
    SND_CHECK_MSG(create(k, false) == SND_ERR_INVALID_ARGUMENT, "kind %u", k);
    SND_CHECK_MSG(create(k, true) == SND_ERR_INVALID_ARGUMENT, "kind %u with a device", k);
  }
  SND_CHECK_EQ(create(SND_OUTPUT_SILENT, true), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(create(SND_OUTPUT_MANUAL, true), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(create(SND_OUTPUT_PLATFORM, true), SND_OK);
  SND_CHECK_EQ(create(SND_OUTPUT_SILENT, false), SND_OK);
  SND_CHECK_EQ(create(SND_OUTPUT_MANUAL, false), SND_OK);
  // The C++ defaults.
  const snd::Config cpp;
  SND_CHECK(cpp.outputKind == snd::OutputKind::Platform);
  SND_CHECK(cpp.fileSystemRoot == nullptr);
  snd_config ci;
  snd_config_init(&ci);
  SND_CHECK_EQ(ci.output_kind, SND_OUTPUT_PLATFORM);
  SND_CHECK(ci.file_system_root == nullptr);
}

// H2
SND_TEST(IndepHostOutput, AdvanceEntryRules) {
  SND_CHECK_EQ(snd_advance_output(nullptr, 1, nullptr, 0), SND_ERR_INVALID_ARGUMENT);
  {
    Host h(SND_OUTPUT_MANUAL);
    SND_REQUIRE_EQ(h.created, SND_OK);
    std::vector<float> out(3u * kBlock * 2u);
    SND_CHECK_EQ(snd_advance_output(h.sys, 0, nullptr, 0), SND_OK);
    SND_CHECK_EQ(snd_advance_output(h.sys, 0, out.data(), 0), SND_OK);
    SND_CHECK_EQ(snd_advance_output(h.sys, 2, nullptr, 0), SND_OK);
    SND_CHECK_EQ(snd_advance_output(h.sys, 3, out.data(), 3u * kBlock - 1u), SND_ERR_INVALID_ARGUMENT);
    SND_CHECK_EQ(snd_advance_output(h.sys, 3, out.data(), 3u * kBlock), SND_OK);
    snd_result other = SND_OK;
    std::thread t([&] { other = snd_advance_output(h.sys, 1, nullptr, 0); });
    t.join();
    SND_CHECK_EQ(other, SND_ERR_WRONG_THREAD);
    // Registered is not enough: owner only.
    std::thread t2([&] {
      snd_register_thread(h.sys);
      other = snd_advance_output(h.sys, 1, nullptr, 0);
      snd_unregister_thread(h.sys);
    });
    t2.join();
    SND_CHECK_EQ(other, SND_ERR_WRONG_THREAD);
  }
  {
    Host h(SND_OUTPUT_SILENT);
    SND_REQUIRE_EQ(h.created, SND_OK);
    SND_CHECK_EQ(snd_advance_output(h.sys, 1, nullptr, 0), SND_ERR_UNSUPPORTED);
  }
  {
    indep::Rig r;  // the game's device
    SND_REQUIRE_EQ(r.created, SND_OK);
    SND_CHECK_EQ(snd_advance_output(r.sys, 1, nullptr, 0), SND_ERR_UNSUPPORTED);
  }
}

// H3 + H4: frame i of the deliveries is common-clock sample i, for stereo and mono outputs,
// advanced slowly (sleeps between calls) and in bigger steps.
SND_TEST(IndepHostOutput, DeliveredFramesAreTheCommonClock) {
  for (const std::uint32_t ch : {1u, 2u, 0u}) {
    Host h(SND_OUTPUT_MANUAL, ch);
    SND_REQUIRE_EQ(h.created, SND_OK);
    const std::uint32_t outCh = ch == 0u ? 2u : ch;
    SND_REQUIRE(PlayRamp(h.sys, nullptr).value != 0u);
    snd_update(h.sys);
    std::vector<float> all;
    std::vector<float> buf(8u * kBlock * outCh);
    std::uint32_t total = 0;
    for (int i = 0; i < 30; ++i) {
      const std::uint32_t n = (i % 3 == 0) ? 1u : (i % 3 == 1 ? 3u : 8u);
      SND_REQUIRE_EQ(snd_advance_output(h.sys, n, buf.data(), 8u * kBlock), SND_OK);
      all.insert(all.end(), buf.begin(), buf.begin() + std::size_t{n} * kBlock * outCh);
      total += n;
      snd_update(h.sys);
      if (i % 5 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(15));  // slow host
    }
    const snd::compose::Parts parts = snd::compose::PartsOf(h.sys);
    SND_REQUIRE(parts.driver != nullptr);
    const snd::platform::DriverStats st = parts.driver->Stats().core;
    SND_CHECK_EQ(st.underruns, 0u);
    SND_CHECK_EQ(st.framesConsumed, std::uint64_t{total} * kBlock);
    SND_CHECK_EQ(st.channels, outCh);
    // The gain from the voice to the output channel (centre pan on stereo, none on mono):
    // measured once, then every heard frame i must be gain x RampAt(i).
    std::uint64_t first = 0;
    const std::uint64_t frames = all.size() / outCh;
    while (first < frames && all[first * outCh] == 0.0f) ++first;
    SND_REQUIRE(first < 8u * kBlock);
    const float gain = all[first * outCh] / RampAt(first);
    SND_CHECK(gain > 0.5f && gain <= 1.0f);
    std::uint64_t wrong = 0, heard = 0;
    for (std::uint64_t f = first; f < frames; ++f) {
      const float x = all[f * outCh];
      if (x == 0.0f) continue;
      ++heard;
      if (std::fabs(x - gain * RampAt(f)) > 2e-6f) ++wrong;
      if (outCh == 2u) wrong += all[f * 2u + 1u] != x ? 1u : 0u;  // the same in both channels
    }
    SND_CHECK_MSG(wrong == 0u, "channels %u: %llu frames off the clock", ch, static_cast<unsigned long long>(wrong));
    SND_CHECK_EQ(heard, frames - first);  // no gap once it started
  }
}

// H5: the audio thread is held inside a render; the wait gives up after about 2 s, and the
// following periods are not slowed down by it.
SND_TEST(IndepHostOutput, Defect_BoundedWaitThenBackToNormal) {
  Host h(SND_OUTPUT_MANUAL);
  SND_REQUIRE_EQ(h.created, SND_OK);
  Gate g;
  SND_REQUIRE(PlayRamp(h.sys, &g).value != 0u);
  snd_update(h.sys);
  SND_REQUIRE_EQ(snd_advance_output(h.sys, 4, nullptr, 0), SND_OK);
  g.hold.store(1, std::memory_order_release);
  // The next period wakes the audio thread, which enters the callback and stays.
  auto t0 = std::chrono::steady_clock::now();
  while (g.inside.load(std::memory_order_acquire) == 0 &&
         std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) {
    SND_REQUIRE_EQ(snd_advance_output(h.sys, 1, nullptr, 0), SND_OK);
  }
  SND_REQUIRE(g.inside.load() != 0);
  // Now the ring cannot be refilled: each period waits for the bound.
  t0 = std::chrono::steady_clock::now();
  SND_REQUIRE_EQ(snd_advance_output(h.sys, 4, nullptr, 0), SND_OK);
  const double held = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  snd::test::Note("4 periods with the audio thread held: %.2f s", held);
  SND_CHECK(held >= 1.5);       // it did wait (at least one bound)
  SND_CHECK(held <= 4 * 2.5);   // bounded per period
  g.hold.store(0, std::memory_order_release);
  // Released: normal speed again (D-102: "過ぎたらそのまま進めてアンダーランとして時計が追い付く").
  // Let the audio thread catch up first (by quantity: until a period returns quickly or 60 s).
  t0 = std::chrono::steady_clock::now();
  SND_REQUIRE_EQ(snd_advance_output(h.sys, 5, nullptr, 0), SND_OK);
  const double after = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  snd::test::Note("5 periods after the release: %.2f s", after);
  SND_CHECK_MSG(after < 1.5, "5 periods after the release took %.2f s (each period waits for the bound)", after);
  const snd::platform::DriverStats st = snd::compose::PartsOf(h.sys).driver->Stats().core;
  SND_CHECK(st.lostFrames > 0u);  // the held periods were underruns
}

// H6: SILENT runs in real time: over ~0.6 s of wall time the clock moves by about as much.
SND_TEST(IndepHostOutput, SilentRunsInRealTime) {
  Host h(SND_OUTPUT_SILENT);
  SND_REQUIRE_EQ(h.created, SND_OK);
  snd_stats a{}, b{};
  for (int i = 0; i < 200 && snd_get_stats(h.sys, &a) != SND_OK; ++i) {
    snd_update(h.sys);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  snd_update(h.sys);
  SND_REQUIRE_EQ(snd_get_stats(h.sys, &a), SND_OK);
  const auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  snd_update(h.sys);
  SND_REQUIRE_EQ(snd_get_stats(h.sys, &b), SND_OK);
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double moved = static_cast<double>(b.clock_sample - a.clock_sample) / 48000.0;
  snd::test::Note("wall %.3f s, clock %.3f s", wall, moved);
  // The device's timer drives it; the render-ahead (a few blocks) and the snapshot timing
  // allow some slack, a factor of 2 none.
  SND_CHECK(moved > wall * 0.7 - 0.05);
  SND_CHECK(moved < wall * 1.3 + 0.05);
}

// S1: the 1.2 layout before the host fields.
SND_TEST(IndepHostOutput, Defect_EarlierOneTwoStructSizeGetsTheHostDefaults) {
  const std::size_t size12 = offsetof(snd_config, file_system_root);
  SND_REQUIRE(size12 > snd::compose::kConfigSize11 && size12 < sizeof(snd_config));
  alignas(8) unsigned char buf[sizeof(snd_config) + 32];
  std::memset(buf, 0xFF, sizeof(buf));  // garbage where the host fields would be
  snd_config c = SmallConfig();
  std::memcpy(buf, &c, size12);
  auto* p = reinterpret_cast<snd_config*>(buf);
  p->struct_size = static_cast<std::uint32_t>(size12);
  snd_config norm;
  const snd_result r = snd::compose::NormalizeConfig(p, &norm);
  SND_CHECK_MSG(r == SND_OK, "struct_size %llu (1.2 before D-102/D-103) -> %d", static_cast<unsigned long long>(size12), r);
  if (r == SND_OK) {
    SND_CHECK(norm.file_system_root == nullptr);
    SND_CHECK_EQ(norm.output_kind, SND_OUTPUT_PLATFORM);
    SND_CHECK_EQ(norm.max_buses, 16u);  // the caller's 1.2 value
  }
  // The 1.0 and 1.1 layouts get the host defaults too.
  for (const std::size_t size : {snd::compose::kConfigSize10, snd::compose::kConfigSize11}) {
    std::memset(buf, 0xFF, sizeof(buf));
    std::memcpy(buf, &c, size);
    p->struct_size = static_cast<std::uint32_t>(size);
    SND_REQUIRE_EQ(snd::compose::NormalizeConfig(p, &norm), SND_OK);
    SND_CHECK(norm.file_system_root == nullptr);
    SND_CHECK_EQ(norm.output_kind, SND_OUTPUT_PLATFORM);
  }
}
