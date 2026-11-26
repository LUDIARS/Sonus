// Independent tests of snd_write_input / snd_play_input (stage 7-D, D-194, D-195) through
// the C API: the entry rules, a writer thread calling snd_write_input while the system's
// audio thread renders (null output device, api_rig.hpp) - run under ThreadSanitizer with
//   ctest -j1 --repeat until-fail:20 -R indep_src1_api_test
// - the port released by a stop and played again, a playback of an unknown port ending.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "api_rig.hpp"
#include "indep_src1_util.hpp"

namespace {

const apitest::End* EndOf(const apitest::Rig& r, snd_playback_handle h) {
  for (const apitest::End& e : r.ev.ends) {
    if (e.handle == h.value) return &e;
  }
  return nullptr;
}

}  // namespace

// D-195 entry rules: NULL system / channels, 0 channels, 0 frames, unknown ports -> 0;
// the last port works; more channels than the port has are accepted (ignored).
SND_TEST(IndepSrc1Api, WriteInputEntryRules) {
  apitest::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd::source::Runtime* rt = snd::compose::PartsOf(r.sys).sources;
  SND_REQUIRE(rt != nullptr);
  const std::uint32_t ports = rt->InputPortCount();
  SND_CHECK(ports >= 1u);
  std::vector<float> a(64, 0.5f);
  const float* ch[9] = {a.data(), a.data(), a.data(), a.data(), a.data(), a.data(), a.data(), a.data(), a.data()};
  SND_CHECK_EQ(snd_write_input(nullptr, 0, ch, 1, 64), 0u);
  SND_CHECK_EQ(snd_write_input(r.sys, 0, nullptr, 1, 64), 0u);
  SND_CHECK_EQ(snd_write_input(r.sys, 0, ch, 0, 64), 0u);
  SND_CHECK_EQ(snd_write_input(r.sys, 0, ch, 1, 0), 0u);
  SND_CHECK_EQ(snd_write_input(r.sys, ports, ch, 1, 64), 0u);
  SND_CHECK_EQ(snd_write_input(r.sys, 0xFFFFFFFFu, ch, 1, 64), 0u);
  SND_CHECK_EQ(snd_write_input(r.sys, ports - 1u, ch, 9, 64), 64u);
  SND_CHECK_EQ(rt->InputPortOf(ports - 1u)->Buffered(), 64u);
  SND_CHECK_EQ(rt->InputPortOf(0)->Buffered(), 0u);
  // The C++ wrapper (D-195) is the same function.
  SND_CHECK_EQ(snd::WriteInput(r.sys, 0, ch, 2, 32), 32u);
  SND_CHECK_EQ(snd::WriteInput(r.sys, ports, ch, 2, 32), 0u);
  SND_CHECK_EQ(rt->InputPortOf(0)->Buffered(), 32u);
}

// A writer thread feeds port 0 with a 600 Hz sine at about the real rate while the audio
// thread renders an input playback: the tone comes out, the port's counters add up, and
// TSan sees no race between snd_write_input and the render.
SND_TEST(IndepSrc1Api, WriterThreadAgainstTheAudioThread) {
  apitest::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd::source::Runtime* rt = snd::compose::PartsOf(r.sys).sources;
  SND_REQUIRE(rt != nullptr);
  snd_input_desc d{};
  d.device_index = 0;
  d.channels = 1;
  const snd_playback_handle h = snd_play_input(r.sys, &d, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(h.value != 0u);
  r.Step(4);
  std::atomic<std::uint64_t> pumped{0};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> accepted{0};
  std::thread writer([&] {
    std::vector<float> c(128);
    const float* ch[1] = {c.data()};
    std::uint64_t n = 0;
    while (!stop.load(std::memory_order_acquire)) {
      // Keep about 4 blocks ahead of what the device has taken.
      if (n > pumped.load(std::memory_order_acquire) * 256u + 1024u) {
        std::this_thread::yield();
        continue;
      }
      for (float& v : c) v = static_cast<float>(0.5 * std::sin(isrc::kTwoPi * 600.0 * double(n++) / 48000.0));
      accepted.fetch_add(snd_write_input(r.sys, 0, ch, 1, 128), std::memory_order_relaxed);
    }
  });
  for (int i = 0; i < 300; ++i) {
    r.Step();
    pumped.store(r.Frames() / 256u, std::memory_order_release);
  }
  stop.store(true, std::memory_order_release);
  writer.join();
  const std::uint64_t from = r.Frames() - 4800;
  const double a = apitest::Level(r, from, 4800, 600.0);
  SND_CHECK_MSG(a > 0.2, "600 Hz level %f", a);
  SND_CHECK_EQ(snd_get_playback_state(r.sys, h), SND_PLAYBACK_STATE_PLAYING);
  snd_stop(r.sys, h, 0.0f, nullptr);
  r.Step(8);
  SND_CHECK(EndOf(r, h) != nullptr);
  const snd::source::InputPortStats s = rt->InputPortOf(0)->Stats();
  SND_CHECK_EQ(s.writtenFrames, accepted.load());
  SND_CHECK_EQ(s.writtenFrames, s.readFrames + s.skippedFrames + rt->InputPortOf(0)->Buffered());
}

// The port is free again after a stop (a new playback of it plays), a second concurrent
// playback of a port ends with VOICE_LIMIT, a playback of an unknown port ends; the ends
// come once each and the other playbacks go on.
SND_TEST(IndepSrc1Api, PortReuseAndRefusedPlaybacks) {
  apitest::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd::source::Runtime* rt = snd::compose::PartsOf(r.sys).sources;
  snd_input_desc d{};
  d.device_index = 1;
  d.channels = 2;
  const snd_playback_handle a = snd_play_input(r.sys, &d, snd_emitter_handle{0}, nullptr);
  r.Step(4);
  const snd_playback_handle b = snd_play_input(r.sys, &d, snd_emitter_handle{0}, nullptr);
  snd_input_desc bad = d;
  bad.device_index = rt->InputPortCount();
  const snd_playback_handle c = snd_play_input(r.sys, &bad, snd_emitter_handle{0}, nullptr);
  r.Step(6);
  SND_CHECK(EndOf(r, a) == nullptr);
  const apitest::End* eb = EndOf(r, b);
  SND_REQUIRE(eb != nullptr);
  SND_CHECK_EQ(eb->detail, snd_end_detail{SND_END_DETAIL_VOICE_LIMIT});
  SND_CHECK(EndOf(r, c) != nullptr);
  snd_stop(r.sys, a, 0.0f, nullptr);
  r.Step(6);
  SND_CHECK(EndOf(r, a) != nullptr);
  const snd_playback_handle e = snd_play_input(r.sys, &d, snd_emitter_handle{0}, nullptr);
  r.Step(6);
  SND_CHECK(EndOf(r, e) == nullptr);
  SND_CHECK_EQ(snd_get_playback_state(r.sys, e), SND_PLAYBACK_STATE_PLAYING);
  std::size_t endsOfB = 0;
  for (const apitest::End& x : r.ev.ends) endsOfB += x.handle == b.value ? 1u : 0u;
  SND_CHECK_EQ(endsOfB, std::size_t{1});
}
