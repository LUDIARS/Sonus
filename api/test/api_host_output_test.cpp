// The built-in output devices of the C API (CONTRACTS.md D-102): Config.output_kind
// with output.device NULL. MANUAL runs only inside snd_advance_output (the common
// clock does not follow real time, every advanced period is rendered, the output is
// what core rendered); SILENT runs in real time on its own thread (the clock advances
// at the sample rate, never faster); the configuration rules of snd_create.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "api_rig.hpp"
#include "snd/sonus.hpp"

using namespace apitest;

namespace {

// A system on a built-in device of `kind` (the game's in-memory file system).
struct HostSystem {
  explicit HostSystem(snd_output_kind kind, snd_config cfg = SmallConfig()) {
    fsTable = snd::ToC(fs);
    cfg.output.device = nullptr;
    cfg.output_kind = kind;
    created = snd_create(&cfg, nullptr, &fsTable, &sys);
  }
  ~HostSystem() { snd_destroy(sys); }
  HostSystem(const HostSystem&) = delete;
  HostSystem& operator=(const HostSystem&) = delete;

  std::uint64_t Clock() {
    snd_update(sys);
    snd_stats st{};
    snd_get_stats(sys, &st);
    return st.clock_sample;
  }

  snd::mock::MemoryFileSystem fs;
  snd_file_system fsTable{};
  snd_system* sys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
};

constexpr std::uint32_t kBlock = 256;

}  // namespace

SND_TEST(ApiHostOutput, ManualDeviceDeliversEveryRenderedFrameInOrder) {
  // A looping static buffer of DC 0.5 (mono, centre pan on the stereo master). Its memory
  // outlives the system.
  std::vector<float> dc(1024, 0.5f);
  HostSystem h(SND_OUTPUT_MANUAL);
  SND_REQUIRE_EQ(h.created, SND_OK);
  const snd::compose::Parts parts = snd::compose::PartsOf(h.sys);
  SND_REQUIRE(parts.driver != nullptr);
  float* ch[1] = {dc.data()};
  snd_user_buffer_desc d{ch, 1, 1024, 0, SND_USER_BUFFER_STATIC, 1, 0};
  const snd_user_buffer_handle b = snd_create_user_buffer(h.sys, &d);
  SND_REQUIRE(b.value != 0u);
  const snd_playback_handle p = snd_play_user_buffer(h.sys, b, 0u, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  snd_update(h.sys);
  constexpr std::uint32_t kPeriods = 40;
  std::vector<float> out(std::size_t{kPeriods} * kBlock * 2u, -7.0f);
  for (std::uint32_t i = 0; i < kPeriods; ++i) {
    SND_REQUIRE_EQ(snd_advance_output(h.sys, 1, out.data() + std::size_t{i} * kBlock * 2u, kBlock), SND_OK);
    snd_update(h.sys);
  }
  // Every frame came from a rendered block: no underrun, exactly kPeriods periods consumed.
  const snd::platform::DriverStats st = parts.driver->Stats().core;
  SND_CHECK_EQ(st.framesConsumed, std::uint64_t{kPeriods} * kBlock);
  SND_CHECK_EQ(st.underruns, 0u);
  SND_CHECK_EQ(st.lostFrames, 0u);
  // Frame 0 is common-clock sample 0 (core's first block is silent before the play is
  // applied); once the voice plays, every frame is 0.5 at the centre pan, both channels.
  SND_CHECK_EQ(out[0], 0.0f);
  std::uint64_t first = 0;
  while (first < std::uint64_t{kPeriods} * kBlock && std::fabs(out[first * 2u]) < 1e-6f) ++first;
  SND_REQUIRE(first < 8u * kBlock);  // within the render-ahead + the start
  std::uint64_t bad = 0;
  for (std::uint64_t f = first + 64u; f < std::uint64_t{kPeriods} * kBlock; ++f) {  // past the resampler's start
    bad += std::fabs(out[f * 2u] - 0.5f * kCenter) > 1e-4f || std::fabs(out[f * 2u + 1u] - 0.5f * kCenter) > 1e-4f;
  }
  SND_CHECK_EQ(bad, 0u);
  // The common clock follows the advanced periods, not real time: once the audio thread has
  // refilled the render-ahead after the last period, it is the frames consumed plus at most
  // the render-ahead (target) and a block, and it stays there.
  auto refilled = [&] {
    for (int i = 0; i < 20000; ++i) {
      const snd::platform::DriverStats s = parts.driver->Stats().core;
      if (s.blocksRendered * kBlock >= s.framesConsumed + s.targetFrames) return true;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
  };
  SND_REQUIRE(refilled());
  const std::uint64_t c0 = h.Clock();
  SND_CHECK(c0 >= std::uint64_t{kPeriods} * kBlock);
  SND_CHECK(c0 <= std::uint64_t{kPeriods} * kBlock + st.targetFrames + kBlock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // ~19 blocks of real time
  SND_CHECK_EQ(h.Clock(), c0);
  // Advancing again moves it by exactly the periods (the render-ahead is refilled each time).
  SND_CHECK_EQ(snd_advance_output(h.sys, 10, nullptr, 0), SND_OK);
  SND_REQUIRE(refilled());
  SND_CHECK_EQ(h.Clock() - c0, 10u * kBlock);
  SND_CHECK_EQ(snd_destroy_user_buffer(h.sys, b), SND_OK);
}

SND_TEST(ApiHostOutput, ManualDeviceEntryRules) {
  HostSystem h(SND_OUTPUT_MANUAL);
  SND_REQUIRE_EQ(h.created, SND_OK);
  std::vector<float> out(2u * kBlock * 2u);
  SND_CHECK_EQ(snd_advance_output(nullptr, 1, nullptr, 0), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_advance_output(h.sys, 3, out.data(), 2u * kBlock), SND_ERR_INVALID_ARGUMENT);  // too small
  SND_CHECK_EQ(snd_advance_output(h.sys, 2, out.data(), 2u * kBlock), SND_OK);
  SND_CHECK_EQ(snd_advance_output(h.sys, 0, nullptr, 0), SND_OK);
  snd_result other = SND_OK;
  std::thread t([&] { other = snd_advance_output(h.sys, 1, nullptr, 0); });
  t.join();
  SND_CHECK_EQ(other, SND_ERR_WRONG_THREAD);
  // A system on another device has no manual device.
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_CHECK_EQ(snd_advance_output(r.sys, 1, nullptr, 0), SND_ERR_UNSUPPORTED);
  // C++ wrapper.
  SND_CHECK(snd::AdvanceOutput(h.sys, 1) == snd::Result::Ok);
  SND_CHECK(snd::AdvanceOutput(r.sys, 1) == snd::Result::Unsupported);
}

SND_TEST(ApiHostOutput, SilentDeviceRunsInRealTime) {
  HostSystem h(SND_OUTPUT_SILENT);
  SND_REQUIRE_EQ(h.created, SND_OK);
  SND_CHECK_EQ(snd_advance_output(h.sys, 1, nullptr, 0), SND_ERR_UNSUPPORTED);
  const auto t0 = std::chrono::steady_clock::now();
  const std::uint64_t c0 = h.Clock();
  // The clock reaches 0.25 s of samples (TSan may be slow: up to 20 s).
  std::uint64_t c = c0;
  while (c < c0 + 12000u && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    c = h.Clock();
  }
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  SND_CHECK(c >= c0 + 12000u);
  // Never ahead of real time by more than the render-ahead (period + 2 blocks) and a block.
  const snd::compose::Parts parts = snd::compose::PartsOf(h.sys);
  SND_REQUIRE(parts.driver != nullptr);
  const std::uint64_t ahead = parts.driver->Stats().core.targetFrames + 2u * kBlock;
  SND_CHECK_MSG(static_cast<double>(c - c0) <= elapsed * 48000.0 + static_cast<double>(ahead),
                "clock %llu in %.3f s", static_cast<unsigned long long>(c - c0), elapsed);
  // The device is the silent timer at the internal rate with one block per period.
  SND_CHECK_EQ(parts.driver->Stats().core.sampleRate, 48000u);
  SND_CHECK_EQ(parts.driver->Stats().core.channels, 2u);
}

SND_TEST(ApiHostOutput, ConfigRules) {
  snd::mock::NullOutputDevice dev;
  snd_output_device dt = snd::ToC(dev);
  snd::mock::MemoryFileSystem fs;
  snd_file_system ft = snd::ToC(fs);
  {
    snd_config c;
    snd_config_init(&c);
    SND_CHECK_EQ(c.output_kind, SND_OUTPUT_PLATFORM);  // the default keeps the 1.1 behaviour
    SND_CHECK(c.file_system_root == nullptr);
  }
  for (const snd_output_kind kind : {snd_output_kind{SND_OUTPUT_SILENT}, snd_output_kind{SND_OUTPUT_MANUAL}}) {
    snd_config c = SmallConfig();
    c.output.device = &dt;  // a game device with a built-in kind: refused
    c.output_kind = kind;
    snd_system* sys = reinterpret_cast<snd_system*>(1);
    SND_CHECK_EQ(snd_create(&c, nullptr, &ft, &sys), SND_ERR_INVALID_ARGUMENT);
    SND_CHECK(sys == nullptr);
  }
  {
    snd_config c = SmallConfig();
    c.output_kind = 3u;  // unknown
    snd_system* sys = nullptr;
    SND_CHECK_EQ(snd_create(&c, nullptr, &ft, &sys), SND_ERR_INVALID_ARGUMENT);
  }
  {
    snd_config c = SmallConfig();
    c.output.device = &dt;  // a game device with PLATFORM: as before
    snd_system* sys = nullptr;
    SND_CHECK_EQ(snd_create(&c, nullptr, &ft, &sys), SND_OK);
    snd_destroy(sys);
  }
  // C++: the fields reach the C config.
  snd::Config cc;
  SND_CHECK(cc.outputKind == snd::OutputKind::Platform);
  SND_CHECK(cc.fileSystemRoot == nullptr);
  cc.outputKind = snd::OutputKind::Manual;
  cc.fileSystemRoot = "data";
  snd_config out;
  snd::detail::ToC(cc, &out);
  SND_CHECK_EQ(out.output_kind, SND_OUTPUT_MANUAL);
  SND_CHECK(out.file_system_root == cc.fileSystemRoot);
}
