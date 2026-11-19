// Per-playback volume and pitch (CONTRACTS.md D-104): snd_play_user_buffer_ex,
// snd_set_playback_volume, snd_set_playback_pitch. Checked analytically on the
// null device: the level of a looping sine is the volume times its amplitude, its
// frequency is the pitch times the written one, several playbacks read one STATIC
// buffer at once with their own values (no copy), synths take the volume and ignore
// the pitch, a pending playback starts with its values, a new playback in the same
// slot starts at 1 / 1, and the entry rules.
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#include "api_rig.hpp"
#include "snd/sonus.hpp"

using namespace apitest;

namespace {

constexpr float kAmp = 0.4f;
constexpr std::size_t kWindow = 4800;  // 0.1 s: 10 Hz bins

// One second of a 1 kHz sine at 48 kHz: whole cycles, so a loop is seamless at any pitch.
std::vector<float> Sine1k() {
  std::vector<float> s(48000);
  for (std::size_t i = 0; i < s.size(); ++i) {
    s[i] = kAmp * static_cast<float>(std::sin(6.283185307179586 * 1000.0 * double(i) / 48000.0));
  }
  return s;
}

// Level at `hz` over the last kWindow captured frames.
double Tail(const Rig& r, double hz) { return Level(r, r.Frames() - kWindow, kWindow, hz); }

void SND_CALL Dc(void*, float* const* out, std::uint32_t channels, std::uint32_t frames, const snd_render_context*) {
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = 0.5f;
  }
}

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

}  // namespace

SND_TEST(ApiPlaybackGain, VolumeScalesTheLevel) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  std::vector<float> sine = Sine1k();
  float* ch[1] = {sine.data()};
  snd_user_buffer_desc d{ch, 1, 48000, 48000, SND_USER_BUFFER_STATIC, 1, 0};
  const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &d);
  SND_REQUIRE(b.value != 0u);
  const snd_playback_handle p = snd_play_user_buffer_ex(r.sys, b, 0u, 0.5f, 1.0f, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  r.Step(30);
  SND_CHECK_NEAR(Tail(r, 1000.0), 0.5 * kAmp, 0.01 * kAmp);
  // Changed while playing.
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, 0.25f), SND_OK);
  r.Step(30);
  SND_CHECK_NEAR(Tail(r, 1000.0), 0.25 * kAmp, 0.01 * kAmp);
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, 0.0f), SND_OK);  // silence, the playback goes on
  r.Step(30);
  SND_CHECK(Tail(r, 1000.0) < 1e-4);
  SND_CHECK(snd_get_playback_state(r.sys, p) != SND_PLAYBACK_STATE_ENDED);
  SND_CHECK(snd::SetPlaybackVolume(r.sys, snd::PlaybackHandle::FromC(p), 2.0f) == snd::Result::Ok);  // C++
  r.Step(30);
  SND_CHECK_NEAR(Tail(r, 1000.0), 2.0 * kAmp, 0.02 * kAmp);
  // The change is ramped over one block: no sample jumps by more than the sine's own slope
  // allows plus the step spread over 256 samples.
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, 0.5f), SND_OK);
  const std::uint64_t from = r.Frames();
  r.Step(30);
  double worst = 0.0;
  for (std::uint64_t c = from + 1; c < r.Frames(); ++c) worst = std::fmax(worst, std::fabs(r.At(c) - r.At(c - 1)));
  const double slope = 2.0 * 3.14159265358979 * 1000.0 / 48000.0 * 2.0 * kAmp;  // max |d/dn| at volume 2
  SND_CHECK_MSG(worst <= slope * 1.05 + 1.5 * kAmp / 256.0, "worst step %f", worst);
  SND_CHECK_EQ(snd_stop(r.sys, p, 0.0f, nullptr), SND_OK);
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, b), SND_OK);
}

SND_TEST(ApiPlaybackGain, PitchScalesTheFrequency) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  std::vector<float> sine = Sine1k();
  float* ch[1] = {sine.data()};
  snd_user_buffer_desc d{ch, 1, 48000, 48000, SND_USER_BUFFER_STATIC, 1, 0};
  const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &d);
  SND_REQUIRE(b.value != 0u);
  const snd_playback_handle p = snd_play_user_buffer_ex(r.sys, b, 0u, 1.0f, 1.5f, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  r.Step(30);
  SND_CHECK_NEAR(Tail(r, 1500.0), kAmp, 0.01 * kAmp);
  SND_CHECK(Tail(r, 1000.0) < 0.01 * kAmp);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, p, 0.5f), SND_OK);
  r.Step(30);
  SND_CHECK_NEAR(Tail(r, 500.0), kAmp, 0.01 * kAmp);
  SND_CHECK(Tail(r, 1500.0) < 0.01 * kAmp);
  SND_CHECK(snd::SetPlaybackPitch(r.sys, snd::PlaybackHandle::FromC(p), 2.0f) == snd::Result::Ok);
  r.Step(30);
  SND_CHECK_NEAR(Tail(r, 2000.0), kAmp, 0.01 * kAmp);
  SND_CHECK_EQ(snd_stop(r.sys, p, 0.0f, nullptr), SND_OK);
  // The buffer's own rate and the pitch multiply: 1 kHz samples declared 24 kHz are 500 Hz at
  // pitch 1, 1.5 kHz at pitch 3.
  std::vector<float> slow = Sine1k();
  float* sc[1] = {slow.data()};
  snd_user_buffer_desc sd{sc, 1, 48000, 24000, SND_USER_BUFFER_STATIC, 1, 0};
  const snd_user_buffer_handle sb = snd_create_user_buffer(r.sys, &sd);
  SND_REQUIRE(sb.value != 0u);
  const snd_playback_handle q = snd_play_user_buffer_ex(r.sys, sb, 0u, 1.0f, 3.0f, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(q.value != 0u);
  r.Step(40);
  SND_CHECK_NEAR(Tail(r, 1500.0), kAmp, 0.01 * kAmp);
  SND_CHECK_EQ(snd_stop(r.sys, q, 0.0f, nullptr), SND_OK);
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, b), SND_OK);
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, sb), SND_OK);
}

SND_TEST(ApiPlaybackGain, SeveralReadersOfOneStaticBuffer) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  std::vector<float> sine = Sine1k();
  const std::vector<float> original = sine;
  float* ch[1] = {sine.data()};
  snd_user_buffer_desc d{ch, 1, 48000, 48000, SND_USER_BUFFER_STATIC, 1, 0};
  const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &d);
  SND_REQUIRE(b.value != 0u);
  // Three playbacks of the same memory: 1 kHz at 0.5, 1.5 kHz at 0.25, 0.5 kHz at 0.125.
  const snd_playback_handle a = snd_play_user_buffer_ex(r.sys, b, 0u, 0.5f, 1.0f, snd_emitter_handle{0}, nullptr);
  const snd_playback_handle c = snd_play_user_buffer_ex(r.sys, b, 0u, 0.25f, 1.5f, snd_emitter_handle{0}, nullptr);
  const snd::PlaybackHandle e = snd::PlayUserBuffer(r.sys, snd::UserBufferHandle::FromC(b),
                                                    snd::UserBufferPlay{snd::kIdNone, 0.125f, 0.5f});
  SND_REQUIRE(a.value != 0u && c.value != 0u && e.value != 0u);
  r.Step(30);
  SND_CHECK_EQ(snd_get_playback_state(r.sys, a), SND_PLAYBACK_STATE_PLAYING);
  SND_CHECK_EQ(snd_get_playback_state(r.sys, c), SND_PLAYBACK_STATE_PLAYING);
  SND_CHECK_EQ(snd_get_playback_state(r.sys, e.ToC()), SND_PLAYBACK_STATE_PLAYING);
  SND_CHECK_NEAR(Tail(r, 1000.0), 0.5 * kAmp, 0.01 * kAmp);
  SND_CHECK_NEAR(Tail(r, 1500.0), 0.25 * kAmp, 0.01 * kAmp);
  SND_CHECK_NEAR(Tail(r, 500.0), 0.125 * kAmp, 0.01 * kAmp);
  // Changing one reader leaves the others.
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, c, 1.0f), SND_OK);
  r.Step(30);
  SND_CHECK_NEAR(Tail(r, 1000.0), 0.5 * kAmp, 0.01 * kAmp);
  SND_CHECK_NEAR(Tail(r, 1500.0), 1.0 * kAmp, 0.01 * kAmp);
  SND_CHECK_NEAR(Tail(r, 500.0), 0.125 * kAmp, 0.01 * kAmp);
  SND_CHECK(sine == original);  // only read
  // The buffer is released only after every reader ended.
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, b), SND_OK);
  for (int i = 0; i < 40 && snd_get_user_buffer_released(r.sys, b) == 0; ++i) r.Step();
  SND_CHECK_EQ(snd_get_user_buffer_released(r.sys, b), 1);
  std::uint32_t ended = 0;
  for (const End& x : r.ev.ends) ended += (x.handle == a.value || x.handle == c.value || x.handle == e.value) ? 1u : 0u;
  SND_CHECK_EQ(ended, 3u);
  // A stream buffer still has one reader at a time (D-94).
  std::vector<float> ring(4096, 0.0f);
  float* rc[1] = {ring.data()};
  snd_user_buffer_desc qd{rc, 1, 4096, 48000, SND_USER_BUFFER_STREAM, 0, 0};
  const snd_user_buffer_handle qb = snd_create_user_buffer(r.sys, &qd);
  SND_REQUIRE(qb.value != 0u);
  const snd_playback_handle q1 = snd_play_user_buffer_ex(r.sys, qb, 0u, 0.5f, 1.0f, snd_emitter_handle{0}, nullptr);
  r.Step(10);
  const snd_playback_handle q2 = snd_play_user_buffer_ex(r.sys, qb, 0u, 0.5f, 1.0f, snd_emitter_handle{0}, nullptr);
  r.Step(10);
  bool limited = false;
  for (const End& x : r.ev.ends) limited = limited || (x.handle == q2.value && x.detail == SND_END_DETAIL_VOICE_LIMIT);
  SND_CHECK(limited);
  SND_CHECK_EQ(snd_get_playback_state(r.sys, q1), SND_PLAYBACK_STATE_PLAYING);
  SND_CHECK_EQ(snd_stop(r.sys, q1, 0.0f, nullptr), SND_OK);
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, qb), SND_OK);
  r.Step(10);
}

SND_TEST(ApiPlaybackGain, SynthTakesTheVolumeAndIgnoresThePitch) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  snd_synth_desc sd{};
  sd.callback = &Dc;
  sd.channels = 1;
  sd.mode = SND_SYNTH_BUFFERED;
  sd.volume = 1.0f;
  const snd_playback_handle p = snd_play_synth(r.sys, &sd, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  r.Step(20);
  SND_CHECK_NEAR(r.At(r.Frames() - 1), 0.5f, 1e-5f);
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, 0.5f), SND_OK);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, p, 2.0f), SND_OK);  // accepted, no effect on a synth
  r.Step(20);
  for (std::uint64_t c = r.Frames() - 1024; c < r.Frames(); ++c) SND_CHECK_NEAR(r.At(c), 0.25f, 1e-5f);
  SND_CHECK_EQ(snd_stop(r.sys, p, 0.0f, nullptr), SND_OK);
  r.Step(10);
}

SND_TEST(ApiPlaybackGain, PendingPlaybackStartsWithItsValues) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  std::vector<float> dc(1024, 0.5f);
  float* ch[1] = {dc.data()};
  snd_user_buffer_desc d{ch, 1, 1024, 48000, SND_USER_BUFFER_STATIC, 1, 0};
  const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &d);
  SND_REQUIRE(b.value != 0u);
  snd_when w{};
  w.kind = SND_WHEN_DELAY_US;
  w.value = 50000;  // 50 ms = 2400 samples later
  const std::uint64_t from = r.Frames();
  const snd_playback_handle p = snd_play_user_buffer_ex(r.sys, b, 0u, 0.5f, 1.0f, snd_emitter_handle{0}, &w);
  SND_REQUIRE(p.value != 0u);
  r.Step(40);
  // From the first sound on, every frame is 0.5 x 0.5 (no block at volume 1 in between).
  std::uint64_t first = from;
  while (first < r.Frames() && std::fabs(r.At(first)) < 1e-6f) ++first;
  SND_REQUIRE(first < r.Frames());
  SND_CHECK(first >= from + 2400u);
  // (The resampler rings by a few percent at the DC onset; a block at volume 1 would be 0.5.)
  double worst = 0.0;
  for (std::uint64_t c = first; c < r.Frames(); ++c) worst = std::fmax(worst, std::fabs(r.At(c)));
  SND_CHECK(worst < 0.25 * 1.1);
  std::uint64_t off = 0;
  for (std::uint64_t c = first + 64u; c < r.Frames(); ++c) off += std::fabs(r.At(c) - 0.25f) > 1e-3f ? 1u : 0u;
  SND_CHECK_EQ(off, 0u);
  SND_CHECK_EQ(snd_stop(r.sys, p, 0.0f, nullptr), SND_OK);
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, b), SND_OK);
}

SND_TEST(ApiPlaybackGain, ANewPlaybackInTheSlotStartsAtOne) {
  snd_config cfg = SmallConfig();
  cfg.max_playbacks = 1;  // the second playback reuses the first one's slot
  Rig r(cfg);
  SND_REQUIRE_EQ(r.created, SND_OK);
  std::vector<float> dc(2048, 0.5f);
  float* ch[1] = {dc.data()};
  snd_user_buffer_desc d{ch, 1, 2048, 48000, SND_USER_BUFFER_STATIC, 0, 0};
  const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &d);
  SND_REQUIRE(b.value != 0u);
  const snd_playback_handle p = snd_play_user_buffer_ex(r.sys, b, 0u, 0.25f, 1.0f, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  bool ended = false;
  for (int i = 0; i < 60 && !ended; ++i) {
    r.Step();
    for (const End& x : r.ev.ends) ended = ended || x.handle == p.value;
  }
  SND_REQUIRE(ended);
  r.Step(2);
  const std::uint64_t from = r.Frames();
  const snd_playback_handle q = snd_play_user_buffer(r.sys, b, 0u, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(q.value != 0u);
  SND_CHECK_EQ(snd::HandleRef::FromValue(q.value).slot, snd::HandleRef::FromValue(p.value).slot);
  r.Step(20);
  // 2048 frames of 0.5 after the onset (the resampler rings a few percent at both edges).
  std::uint64_t first = from;
  while (first < r.Frames() && std::fabs(r.At(first)) < 1e-6f) ++first;
  SND_REQUIRE(first + 2048u <= r.Frames());
  std::uint64_t off = 0;
  for (std::uint64_t c = first + 64u; c < first + 1984u; ++c) off += std::fabs(r.At(c) - 0.5f) > 1e-3f ? 1u : 0u;
  SND_CHECK_EQ(off, 0u);
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, b), SND_OK);
  r.Step(10);
}

SND_TEST(ApiPlaybackGain, EntryRules) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  std::vector<float> dc(1024, 0.5f);
  float* ch[1] = {dc.data()};
  snd_user_buffer_desc d{ch, 1, 1024, 48000, SND_USER_BUFFER_STATIC, 1, 0};
  const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &d);
  SND_REQUIRE(b.value != 0u);
  const snd_playback_handle p = snd_play_user_buffer(r.sys, b, 0u, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  // sys NULL, handle 0 first, then the value.
  SND_CHECK_EQ(snd_set_playback_volume(nullptr, p, 1.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_set_playback_pitch(nullptr, p, 1.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, snd_playback_handle{0}, kNan), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, snd_playback_handle{0}, 0.0f), SND_ERR_INVALID_HANDLE);
  for (const float v : {kNan, kInf, -kInf, -0.001f}) {
    SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, v), SND_ERR_INVALID_ARGUMENT);
  }
  for (const float v : {kNan, kInf, -kInf, 0.0f, -1.0f}) {
    SND_CHECK_EQ(snd_set_playback_pitch(r.sys, p, v), SND_ERR_INVALID_ARGUMENT);
  }
  // Extremes that are accepted (core clamps: volume <= 1000, pitch [1/16, 16]).
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, 0.0f), SND_OK);
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, 1.0e6f), SND_OK);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, p, 1.0e-6f), SND_OK);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, p, 1.0e6f), SND_OK);
  // A stale handle: INVALID_HANDLE from core.
  const snd_playback_handle stale{SND_HANDLE_MAKE(5u, 0x1234u)};
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, stale, 1.0f), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, stale, 1.0f), SND_ERR_INVALID_HANDLE);
  // An unregistered thread records nothing.
  snd_result other = SND_OK;
  std::thread t([&] { other = snd_set_playback_volume(r.sys, p, 0.5f); });
  t.join();
  SND_CHECK_EQ(other, SND_ERR_THREAD_NOT_REGISTERED);
  // snd_play_user_buffer_ex refuses the same values with an invalid handle.
  SND_CHECK_EQ(snd_play_user_buffer_ex(nullptr, b, 0u, 1.0f, 1.0f, snd_emitter_handle{0}, nullptr).value, 0u);
  SND_CHECK_EQ(snd_play_user_buffer_ex(r.sys, b, 0u, -1.0f, 1.0f, snd_emitter_handle{0}, nullptr).value, 0u);
  SND_CHECK_EQ(snd_play_user_buffer_ex(r.sys, b, 0u, 1.0f, 0.0f, snd_emitter_handle{0}, nullptr).value, 0u);
  SND_CHECK_EQ(snd_play_user_buffer_ex(r.sys, b, 0u, kNan, 1.0f, snd_emitter_handle{0}, nullptr).value, 0u);
  SND_CHECK_EQ(snd_play_user_buffer_ex(r.sys, snd_user_buffer_handle{0}, 0u, 1.0f, 1.0f, snd_emitter_handle{0},
                                       nullptr).value, 0u);
  // Back to 1 / 1; the reserved ids reach nothing in the other scopes (no parameter row).
  SND_CHECK_EQ(snd_set_playback_volume(r.sys, p, 1.0f), SND_OK);
  SND_CHECK_EQ(snd_set_playback_pitch(r.sys, p, 1.0f), SND_OK);
  SND_CHECK_EQ(snd_set_param_global(r.sys, 0xFFFFFFFDu, 0.0f), SND_OK);
  SND_CHECK_EQ(snd_set_param_global(r.sys, 0xFFFFFFFCu, 4.0f), SND_OK);
  r.Step(30);
  for (std::uint64_t c = r.Frames() - 1024; c < r.Frames(); ++c) SND_CHECK_NEAR(r.At(c), 0.5f, 1e-4f);
  SND_CHECK_EQ(snd_stop(r.sys, p, 0.0f, nullptr), SND_OK);
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, b), SND_OK);
  r.Step(10);
}
