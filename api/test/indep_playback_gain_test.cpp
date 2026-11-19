// Independent tests: per-playback volume and pitch (CONTRACTS.md D-104).
// Properties (from the spec, before reading the implementation):
//  V1 entry: sys NULL -> INVALID_ARGUMENT; handle 0 -> INVALID_HANDLE (before the value);
//     volume NaN / +-inf / < 0, pitch NaN / +-inf / <= 0 -> INVALID_ARGUMENT; a stale handle
//     -> INVALID_HANDLE; snd_play_user_buffer_ex with a refused value -> invalid handle and
//     nothing recorded.
//  V2 volume v multiplies the level exactly (DC buffer: v x value x centre pan); at most
//     1000; 0 is silence; a change ramps linearly over exactly one block.
//  V3 pitch p multiplies the frequency; clamped to [1/16, 16] and, with the source ratio, to
//     Config.max_pitch_ratio.
//  V4 a pending playback (start `when` in the future) and snd_play_user_buffer_ex start with
//     the value from their first sample (no ramp from 1).
//  V5 several playbacks of ONE static buffer at once, each with its own volume and pitch.
//  V6 an event playback (bank) takes both (they are not user-buffer specific).
//  V7 destroy of the static buffer under several readers (D-97): all end once, released; a
//     play_ex after the destroy ends DISCARDED.
//  V8 the coalescing fallback (D-11) keeps the LAST volume when the arena overflows.
//  V9 the reserved ids on other scopes / as switches do nothing.
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "indep_rig.hpp"
#include "music_fixture.hpp"
#include "snd/core/core.hpp"

using apitest::kCenter;
using apitest::SmallConfig;

namespace {

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

// Level of `hz` in channel 0 / centre over [from, from + n) (Hann window).
double Level(const indep::Rig& r, std::uint64_t from, std::size_t n, double hz) {
  const double w = 6.283185307179586 * hz / 48000.0;
  double re = 0, im = 0, ws = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(6.283185307179586 * double(i) / double(n - 1));
    const double v = r.Raw(from + i) / kCenter * win;
    re += v * std::cos(w * double(i));
    im -= v * std::sin(w * double(i));
    ws += win;
  }
  return 2.0 * std::sqrt(re * re + im * im) / ws;
}

// A static looping user buffer (memory owned here, outlives the rig when declared first).
struct Buf {
  std::vector<float> mem;
  snd_user_buffer_handle h{};
  void Make(snd_system* sys, std::vector<float> data) {
    mem = std::move(data);
    float* ch[1] = {mem.data()};
    const snd_user_buffer_desc d{ch, 1, static_cast<std::uint32_t>(mem.size()), 0, SND_USER_BUFFER_STATIC, 1, 0};
    h = snd_create_user_buffer(sys, &d);
  }
};

std::vector<float> Dc(float v, std::size_t n = 1024) { return std::vector<float>(n, v); }
// Whole cycles of `hz` over n frames (seamless loop).
std::vector<float> Sine(double hz, float amp, std::size_t n = 48000) {
  std::vector<float> s(n);
  for (std::size_t i = 0; i < n; ++i) s[i] = amp * static_cast<float>(std::sin(6.283185307179586 * hz * double(i) / 48000.0));
  return s;
}

float Last(const indep::Rig& r) { return r.Raw(r.Frames() - 1) / kCenter; }

}  // namespace

// V1
SND_TEST(IndepPlaybackGain, EntryRules) {
  Buf b;
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  b.Make(r.sys, Dc(0.1f));
  SND_REQUIRE(b.h.value != 0u);
  const snd_playback_handle p = snd_play_user_buffer(r.sys, b.h, 0u, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  SND_CHECK_EQ(snd_set_playback_volume(nullptr, p, 1.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_set_playback_pitch(nullptr, p, 1.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, snd_playback_handle{0}, kNaN), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, snd_playback_handle{0}, -1.0f), SND_ERR_INVALID_HANDLE);
  for (const float v : {kNaN, kInf, -kInf, -1.0f, -std::numeric_limits<float>::denorm_min()}) {
    SND_CHECK_MSG(snd_set_playback_volume(r.sys, p, v) == SND_ERR_INVALID_ARGUMENT, "volume %g", static_cast<double>(v));
  }
  for (const float v : {0.0f, -0.0f, std::numeric_limits<float>::denorm_min(), 1000.0f, 1e30f}) {
    SND_CHECK_MSG(snd_set_playback_volume(r.sys, p, v) == SND_OK, "volume %g", static_cast<double>(v));
  }
  for (const float v : {kNaN, kInf, -kInf, 0.0f, -0.0f, -2.0f}) {
    SND_CHECK_MSG(snd_set_playback_pitch(r.sys, p, v) == SND_ERR_INVALID_ARGUMENT, "pitch %g", static_cast<double>(v));
  }
  for (const float v : {std::numeric_limits<float>::denorm_min(), 1e-6f, 1.0f, 16.0f, 1e30f}) {
    SND_CHECK_MSG(snd_set_playback_pitch(r.sys, p, v) == SND_OK, "pitch %g", static_cast<double>(v));
  }
  // _ex with a refused value: invalid handle.
  SND_CHECK_EQ(snd_play_user_buffer_ex(r.sys, b.h, 0u, -1.0f, 1.0f, snd_emitter_handle{0}, nullptr).value, 0u);
  SND_CHECK_EQ(snd_play_user_buffer_ex(r.sys, b.h, 0u, 1.0f, 0.0f, snd_emitter_handle{0}, nullptr).value, 0u);
  SND_CHECK_EQ(snd_play_user_buffer_ex(r.sys, b.h, 0u, kNaN, 1.0f, snd_emitter_handle{0}, nullptr).value, 0u);
  SND_CHECK_EQ(snd_play_user_buffer_ex(nullptr, b.h, 0u, 1.0f, 1.0f, snd_emitter_handle{0}, nullptr).value, 0u);
  SND_CHECK_EQ(snd_play_user_buffer_ex(r.sys, snd_user_buffer_handle{0}, 0u, 1.0f, 1.0f, snd_emitter_handle{0}, nullptr).value,
               0u);
  // Stale handle.
  snd_stop(r.sys, p, 0.0f, nullptr);
  for (int i = 0; i < 20 && r.EndsOf(p.value) == 0u; ++i) r.Step(1);
  SND_REQUIRE_EQ(r.EndsOf(p.value), 1u);
  snd_update(r.sys);
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, 0.5f), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, p, 0.5f), SND_ERR_INVALID_HANDLE);
  // Nothing else ended (the refused _ex calls recorded no playback).
  r.Step(4);
  SND_CHECK_EQ(r.ev.ends.size(), 1u);
}

// V2: exact ratios, the bound, silence, and the one-block linear ramp.
SND_TEST(IndepPlaybackGain, VolumeIsExactAndRampsOverOneBlock) {
  Buf b;
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  b.Make(r.sys, Dc(0.001f));
  const snd_playback_handle p = snd_play_user_buffer(r.sys, b.h, 0u, snd_emitter_handle{0}, nullptr);
  r.Step(10);
  SND_CHECK(std::fabs(Last(r) - 0.001f) < 1e-6f);
  const float vols[] = {2.0f, 0.25f, 1000.0f, 5000.0f, 0.0f, 3.0f};
  const float want[] = {0.002f, 0.00025f, 1.0f, 1.0f, 0.0f, 0.003f};  // 5000 -> 1000
  float prev = 0.001f;
  for (std::size_t k = 0; k < sizeof(vols) / sizeof(vols[0]); ++k) {
    const std::uint64_t from = r.Frames();
    SND_REQUIRE_EQ(snd_set_playback_volume(r.sys, p, vols[k]), SND_OK);
    r.Step(12);
    SND_CHECK_MSG(std::fabs(Last(r) - want[k]) <= 1e-6f + 1e-5f * want[k], "volume %g: %g, want %g",
                  static_cast<double>(vols[k]), static_cast<double>(Last(r)), static_cast<double>(want[k]));
    // The ramp: find the first frame off `prev`; from that block boundary on, 256 frames move
    // linearly to the new value, then it holds.
    std::uint64_t a = from;
    while (a < r.Frames() && std::fabs(r.Raw(a) / kCenter - prev) <= 1e-7f + 1e-6f * prev) ++a;
    if (prev != want[k]) {
      SND_REQUIRE(a < r.Frames());
      const std::uint64_t blockStart = a - (a % 256u);
      SND_CHECK_MSG(a - blockStart <= 1u, "volume %g: the change starts mid-block (%llu)", static_cast<double>(vols[k]),
                    static_cast<unsigned long long>(a % 256u));
      double maxDev = 0.0;
      for (std::uint32_t f = 0; f < 256; ++f) {
        const double lin = prev + (want[k] - prev) * double(f + 1) / 256.0;
        const double lin0 = prev + (want[k] - prev) * double(f) / 256.0;
        const double x = r.Raw(blockStart + f) / kCenter;
        const double dev = std::min(std::fabs(x - lin), std::fabs(x - lin0));
        maxDev = std::max(maxDev, dev);
      }
      SND_CHECK_MSG(maxDev <= 1e-5 + 1e-4 * std::fabs(want[k] - prev), "volume %g: not a one-block line (%g)",
                    static_cast<double>(vols[k]), maxDev);
      for (std::uint64_t f = blockStart + 256; f < r.Frames(); ++f) {
        if (std::fabs(r.Raw(f) / kCenter - want[k]) > 1e-6f + 1e-5f * want[k]) {
          SND_CHECK_MSG(false, "volume %g: not held after the ramp", static_cast<double>(vols[k]));
          break;
        }
      }
    }
    prev = want[k];
  }
}

// V3: frequency ratio and the clamps.
SND_TEST(IndepPlaybackGain, PitchRatiosAndClamps) {
  struct Case {
    float maxRatio;
    float pitch;
    double hz;  // expected frequency of a 300 Hz buffer
  };
  const Case cases[] = {{4.0f, 2.0f, 600.0},    {4.0f, 0.5f, 150.0},   {4.0f, 100.0f, 1200.0},  // 16 -> max ratio 4
                        {32.0f, 100.0f, 4800.0}, {32.0f, 16.0f, 4800.0}, {4.0f, 1e-4f, 18.75}};  // 1/16
  for (const Case& k : cases) {
    Buf b;
    snd_config c = SmallConfig();
    c.max_pitch_ratio = k.maxRatio;
    indep::Rig r(c);
    SND_REQUIRE_EQ(r.created, SND_OK);
    b.Make(r.sys, Sine(300.0, 0.4f));
    const snd_playback_handle p = snd_play_user_buffer_ex(r.sys, b.h, 0u, 1.0f, k.pitch, snd_emitter_handle{0}, nullptr);
    SND_REQUIRE(p.value != 0u);
    const std::size_t n = k.hz < 100.0 ? 19200u : 9600u;
    r.Step(static_cast<std::uint32_t>(n / 256u + 12u));
    const std::uint64_t from = r.Frames() - n;
    const double at = Level(r, from, n, k.hz);
    const double at1 = Level(r, from, n, 300.0);
    SND_CHECK_MSG(std::fabs(at - 0.4) < 0.02, "max %g pitch %g: %g at %g Hz", static_cast<double>(k.maxRatio),
                  static_cast<double>(k.pitch), at, k.hz);
    if (std::fabs(k.hz - 300.0) > 50.0) SND_CHECK(at1 < 0.02);
  }
}

// V4: pending (NEXT_BAR on a transport) and _ex: the first heard sample already has the value.
SND_TEST(IndepPlaybackGain, PendingAndExStartWithTheirValues) {
  for (const int variant : {0, 1, 2}) {
    Buf b;
    indep::Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    b.Make(r.sys, Dc(0.1f));
    const snd_transport_desc td{240.0f, 4, 4, 1, 0};  // a bar = 48000 samples
    const snd_transport_handle t = snd_create_transport(r.sys, &td);
    r.Step(20);
    snd_when w{};
    w.kind = SND_WHEN_NEXT_BAR;
    w.transport = t;
    snd_playback_handle p{0};
    if (variant == 0) {  // _ex, pending
      p = snd_play_user_buffer_ex(r.sys, b.h, 0u, 3.0f, 1.0f, snd_emitter_handle{0}, &w);
    } else if (variant == 1) {  // play, then set in the same arena
      p = snd_play_user_buffer(r.sys, b.h, 0u, snd_emitter_handle{0}, &w);
      snd_set_playback_volume(r.sys, p, 3.0f);
    } else {  // play now, then set a few frames later while it is still pending
      p = snd_play_user_buffer(r.sys, b.h, 0u, snd_emitter_handle{0}, &w);
      r.Step(4);
      SND_REQUIRE_EQ(snd_get_playback_state(r.sys, p), SND_PLAYBACK_STATE_PENDING);
      snd_set_playback_volume(r.sys, p, 3.0f);
    }
    SND_REQUIRE(p.value != 0u);
    r.Step(220);
    std::uint64_t first = 0;
    while (first < r.Frames() && r.Raw(first) == 0.0f) ++first;
    SND_REQUIRE(first < r.Frames());
    // The resampler may take a few frames to reach the DC; none of them may be at volume 1
    // (0.1): the first 4 heard samples after the start transient are 0.3.
    int atOne = 0;
    for (std::uint64_t f = first; f < first + 64; ++f) {
      const float x = r.Raw(f) / kCenter;
      if (std::fabs(x - 0.1f) < 1e-3f) ++atOne;
    }
    SND_CHECK_MSG(atOne == 0, "variant %d: %d frames at volume 1 at the start", variant, atOne);
    SND_CHECK_NEAR(r.Raw(first + 100) / kCenter, 0.3f, 1e-4f);
  }
}

// V5: one static buffer, four readers, each its own volume / pitch.
SND_TEST(IndepPlaybackGain, IndependentReadersOfOneStaticBuffer) {
  Buf b;
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  b.Make(r.sys, Sine(400.0, 0.1f));
  const snd_playback_handle p1 = snd_play_user_buffer_ex(r.sys, b.h, 0u, 1.0f, 1.0f, snd_emitter_handle{0}, nullptr);
  const snd_playback_handle p2 = snd_play_user_buffer_ex(r.sys, b.h, 0u, 2.0f, 1.5f, snd_emitter_handle{0}, nullptr);
  const snd_playback_handle p3 = snd_play_user_buffer_ex(r.sys, b.h, 0u, 3.0f, 0.5f, snd_emitter_handle{0}, nullptr);
  const snd_playback_handle p4 = snd_play_user_buffer_ex(r.sys, b.h, 0u, 0.5f, 2.5f, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p1.value != 0u && p2.value != 0u && p3.value != 0u && p4.value != 0u);
  r.Step(60);
  const std::uint64_t from = r.Frames() - 9600;
  SND_CHECK_NEAR(Level(r, from, 9600, 400.0), 0.1, 0.005);
  SND_CHECK_NEAR(Level(r, from, 9600, 600.0), 0.2, 0.005);
  SND_CHECK_NEAR(Level(r, from, 9600, 200.0), 0.3, 0.005);
  SND_CHECK_NEAR(Level(r, from, 9600, 1000.0), 0.05, 0.005);
  // Changing one changes only that one.
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p2, 0.0f), SND_OK);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, p3, 1.25f), SND_OK);  // 200 -> 500 Hz
  r.Step(50);
  const std::uint64_t f2 = r.Frames() - 9600;
  SND_CHECK_NEAR(Level(r, f2, 9600, 400.0), 0.1, 0.005);
  SND_CHECK(Level(r, f2, 9600, 600.0) < 0.005);
  SND_CHECK(Level(r, f2, 9600, 200.0) < 0.005);
  SND_CHECK_NEAR(Level(r, f2, 9600, 500.0), 0.3, 0.005);
  SND_CHECK_NEAR(Level(r, f2, 9600, 1000.0), 0.05, 0.005);
  // V7: destroy under the four readers; a play_ex afterwards (same arena) is DISCARDED.
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, b.h), SND_OK);
  const snd_playback_handle late = snd_play_user_buffer_ex(r.sys, b.h, 0u, 2.0f, 2.0f, snd_emitter_handle{0}, nullptr);
  for (int i = 0; i < 40 && snd_get_user_buffer_released(r.sys, b.h) == 0; ++i) r.Step(1);
  SND_CHECK_EQ(snd_get_user_buffer_released(r.sys, b.h), 1);
  r.Step(4);
  for (const snd_playback_handle& p : {p1, p2, p3, p4}) SND_CHECK_EQ(r.EndsOf(p.value), 1u);
  if (late.value != 0u) {
    SND_CHECK_EQ(r.EndsOf(late.value), 1u);
    for (const apitest::End& e : r.ev.ends) {
      if (e.handle == late.value) SND_CHECK_EQ(e.reason, SND_END_DISCARDED);
    }
  }
  const std::uint64_t quiet = r.Frames();
  r.Step(6);
  for (std::uint64_t f = quiet; f < r.Frames(); ++f) SND_CHECK_EQ(r.Raw(f), 0.0f);
}

// V6: an event from a bank.
SND_TEST(IndepPlaybackGain, EventPlaybacksTakeVolumeAndPitch) {
  mt::MusicProject proj("indep-gain");
  mt::bt::PcmData s;
  s.channels = 1;
  s.sampleRate = 48000;
  s.frames = 96000;
  s.samples = Sine(500.0, 0.2f, 96000);
  proj.Asset("s", s);
  proj.Event("Fx", "\"tracks\": [{\"clips\": [{\"asset\": \"s\"}]}]");
  std::string diag;
  const std::vector<std::uint8_t> bank = proj.Build(&diag);
  SND_CHECK_MSG(!bank.empty(), "%s", diag.c_str());
  SND_REQUIRE(!bank.empty());
  apitest::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE(r.LoadBank("Main.sndb", bank));
  const snd_playback_handle p = snd_play(r.sys, snd::HashId("Fx"), snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, 2.0f), SND_OK);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, p, 1.5f), SND_OK);
  r.RunTo(r.Frames() + 24000);
  const std::uint64_t from = r.Frames() - 9600;
  SND_CHECK_NEAR(apitest::Level(r, from, 9600, 750.0), 0.4, 0.01);
  SND_CHECK(apitest::Level(r, from, 9600, 500.0) < 0.01);
  snd_stop(r.sys, p, 0.0f, nullptr);
  r.Step(4);
}

// V8: the arena overflows; the coalescing fallback must keep the last value.
SND_TEST(IndepPlaybackGain, OverflowKeepsTheLastVolume) {
  Buf b;
  snd_config c = SmallConfig();
  c.commands.arena_bytes = 2048;
  c.commands.reserve_bytes = 512;
  c.commands.overflow_param_slots = 32;
  indep::Rig r(c);
  SND_REQUIRE_EQ(r.created, SND_OK);
  b.Make(r.sys, Dc(0.01f));
  const snd_playback_handle p = snd_play_user_buffer(r.sys, b.h, 0u, snd_emitter_handle{0}, nullptr);
  r.Step(8);
  // One frame: thousands of volume and pitch commands; the last values are 7 and 1.
  for (int i = 0; i < 3000; ++i) {
    snd_set_playback_volume(r.sys, p, static_cast<float>(i % 50) * 0.1f);
    snd_set_playback_pitch(r.sys, p, 1.0f + static_cast<float>(i % 7) * 0.1f);
  }
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, 7.0f), SND_OK);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, p, 1.0f), SND_OK);
  r.Step(12);
  snd_update(r.sys);
  snd_stats st{};
  SND_REQUIRE_EQ(snd_get_stats(r.sys, &st), SND_OK);
  // 6000 commands of a few dozen bytes into 2.5 KiB of arena: most went to the coalescing
  // fallback. (stats.reserve_overflows counts only values lost without a compact form, so
  // it stays 0 here; the coalesced ones are stats.fallback_commands.)
  snd::test::Note("reserve_overflows %u, fallback_commands %u", st.reserve_overflows, st.fallback_commands);
  SND_CHECK(st.fallback_commands > 0u);  // fc38dea: the coalesced commands are counted
  SND_CHECK_NEAR(Last(r), 0.07f, 1e-5f);
}

// V9: the reserved ids elsewhere do nothing.
SND_TEST(IndepPlaybackGain, ReservedIdsElsewhereDoNothing) {
  Buf b;
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  b.Make(r.sys, Dc(0.1f));
  const snd_emitter_handle e = snd_create_emitter(r.sys);
  const snd_playback_handle p = snd_play_user_buffer(r.sys, b.h, 0u, e, nullptr);
  r.Step(8);
  const float before = Last(r);
  SND_CHECK(before > 0.0f);
  for (const snd_id id : {snd::core::kParamPlaybackVolume, snd::core::kParamPlaybackPitch}) {
    snd_set_param_global(r.sys, id, 0.0f);
    snd_set_param_emitter(r.sys, e, id, 0.0f);
    snd_set_switch_global(r.sys, id, 1);
    snd_set_switch_emitter(r.sys, e, id, 1);
    snd_set_switch_playback(r.sys, p, id, 1);
  }
  r.Step(10);
  SND_CHECK_NEAR(Last(r), before, 1e-6f);
}
