// Independent tests of the fixes in 8d618bf (CONTRACTS.md D-97..D-99 and the notes on
// D-93..D-95), written against the accepted decisions:
//  - a synth's on_end comes after the last call of its callback;
//  - only real voices hold a ring; without one a buffered synth waits in silence;
//  - snd_destroy_user_buffer stops the playbacks reading the buffer, then releases it.
// Properties checked here (beyond the fixed defects' own tests):
//  X1 a buffered synth that becomes real again (devirtualised) or is demoted from
//     LowLatency takes a ring at its current position: lockstep still holds (every heard
//     sample is the value the callback wrote for that clock, or 0).
//  X2 a play of a user buffer recorded after its destroy (same arena) neither keeps the
//     buffer from being released nor plays the released memory: the buffer is released
//     and the play ends exactly once.
//  X3 on_end of a synth whose job is still running arrives exactly once, after the job,
//     also when several are held at once and when the callbacks are replaced meanwhile.
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "indep_rig.hpp"

using apitest::kCenter;
using apitest::SmallConfig;

namespace {

float RampAt(std::uint64_t c) { return 0.1f + 0.0008f * static_cast<float>(c % 1000u); }

struct RampUser {
  std::atomic<int> spinOnce{0};  // 1: the next call spins for > one block (LowLatency demotion)
};

void SND_CALL RampSynth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                        const snd_render_context* ctx) {
  auto* u = static_cast<RampUser*>(user);
  if (u != nullptr && u->spinOnce.exchange(0) != 0) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(12);
    while (std::chrono::steady_clock::now() < until) {
    }
  }
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = RampAt(ctx->clock_sample + f);
  }
}

snd_synth_desc RampDesc(RampUser* u, snd_synth_mode mode, float volume) {
  snd_synth_desc d{};
  d.callback = &RampSynth;
  d.user = u;
  d.channels = 1;
  d.mode = mode;
  d.volume = volume;
  d.ahead_blocks = 2;
  return d;
}

// Counts heard samples in [from, to) that are neither 0 nor gain x RampAt(clock).
std::uint64_t OffLockstep(const indep::Rig& r, std::uint64_t from, std::uint64_t to, float gain,
                          std::uint64_t* heard) {
  std::uint64_t wrong = 0;
  *heard = 0;
  for (std::uint64_t c = from; c < to && c < r.Frames(); ++c) {
    const float x = r.Raw(c) / kCenter;
    if (std::fabs(x) < 1e-7f) continue;
    ++*heard;
    if (std::fabs(x - gain * RampAt(c)) > 2e-5f) {
      if (wrong < 3u) {
        snd::test::Note("at %llu heard %g, want %g", static_cast<unsigned long long>(c), static_cast<double>(x),
                        static_cast<double>(gain * RampAt(c)));
      }
      ++wrong;
    }
  }
  return wrong;
}

}  // namespace

// X1: devirtualised.
SND_TEST(IndepFix, DevirtualisedBufferedSynthStaysInLockstep) {
  snd_config c = SmallConfig();
  c.max_real_voices = 2;
  c.fade_reserve_voices = 2;
  indep::Rig r(c);
  SND_REQUIRE_EQ(r.created, SND_OK);
  RampUser u;
  const snd_synth_desc loud = RampDesc(&u, SND_SYNTH_BUFFERED, 1.0f);
  const snd_synth_desc quiet = RampDesc(&u, SND_SYNTH_BUFFERED, 0.25f);
  const snd_playback_handle a = snd_play_synth(r.sys, &loud, snd_emitter_handle{0}, nullptr);
  const snd_playback_handle b = snd_play_synth(r.sys, &loud, snd_emitter_handle{0}, nullptr);
  r.Step(4);
  const snd_playback_handle q = snd_play_synth(r.sys, &quiet, snd_emitter_handle{0}, nullptr);  // virtual
  SND_REQUIRE(a.value != 0u && b.value != 0u && q.value != 0u);
  r.Step(30);
  snd_update(r.sys);
  snd_stats st{};
  SND_REQUIRE_EQ(snd_get_stats(r.sys, &st), SND_OK);
  SND_CHECK_EQ(st.real_voices, 2u);
  SND_CHECK(st.virtual_voices >= 1u);
  // The two loud ones go: the quiet one becomes real, takes a ring at its position.
  snd_stop(r.sys, a, 0.0f, nullptr);
  snd_stop(r.sys, b, 0.0f, nullptr);
  const std::uint64_t from = r.Frames() + 8u * 256u;  // after the stop fades
  r.Step(80);
  std::uint64_t heard = 0;
  const std::uint64_t wrong = OffLockstep(r, from, r.Frames(), 0.25f, &heard);
  SND_CHECK_EQ(wrong, 0u);
  SND_CHECK_MSG(heard >= 40u * 256u, "heard %llu", static_cast<unsigned long long>(heard));
  snd_stop(r.sys, q, 0.0f, nullptr);
  r.Step(6);
  SND_CHECK_EQ(r.EndsOf(q.value), 1u);
}

// X1: demoted from LowLatency (a call over the block's duration).
SND_TEST(IndepFix, DemotedLowLatencySynthStaysInLockstep) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  RampUser u;
  const snd_synth_desc d = RampDesc(&u, SND_SYNTH_LOW_LATENCY, 1.0f);
  const snd_playback_handle p = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  r.Step(10);
  u.spinOnce.store(1);
  r.Step(80);
  snd::compose::Parts parts = snd::compose::PartsOf(r.sys);
  SND_REQUIRE(parts.sources != nullptr);
  SND_CHECK_EQ(parts.sources->Stats().callbackDemotions, 1u);
  std::uint64_t heard = 0;
  const std::uint64_t wrong = OffLockstep(r, 0, r.Frames(), 1.0f, &heard);
  SND_CHECK_EQ(wrong, 0u);
  SND_CHECK(heard >= 60u * 256u);
  snd_stop(r.sys, p, 0.0f, nullptr);
  r.Step(6);
  SND_CHECK_EQ(r.EndsOf(p.value), 1u);
}

// X2: destroy, then play in the same arena (gap 0) or in the next frames (gap 1, 2) before
// the release reached the game.
SND_TEST(IndepFix, Defect_PlayRecordedAfterDestroyDoesNotKeepTheBuffer) {
  for (int k = 0; k < 6; ++k) {
    const snd_user_buffer_mode mode = (k & 1) == 0 ? SND_USER_BUFFER_STREAM : SND_USER_BUFFER_STATIC;
    const int gap = k / 2;
    indep::Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    std::vector<float> mem(1000, 0.25f);
    float* chans[1] = {mem.data()};
    const snd_user_buffer_desc bd{chans, 1, 1000, 0, mode, mode == SND_USER_BUFFER_STATIC ? 1u : 0u, 0};
    const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &bd);
    SND_REQUIRE(b.value != 0u);
    const float* src[1] = {mem.data()};
    snd_write_user_buffer(r.sys, b, src, 1000);
    r.Step(2);
    SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, b), SND_OK);
    if (gap > 0) r.Step(static_cast<std::uint32_t>(gap));
    const snd_playback_handle p = snd_get_user_buffer_released(r.sys, b) == 0
                                      ? snd_play_user_buffer(r.sys, b, 0u, snd_emitter_handle{0}, nullptr)
                                      : snd_playback_handle{0};
    snd::test::Note("mode %u gap %d: play handle %s", mode, gap, p.value != 0u ? "issued" : "refused");
    const std::uint64_t from = r.Frames();
    for (int i = 0; i < 60 && snd_get_user_buffer_released(r.sys, b) == 0; ++i) r.Step(1);
    SND_CHECK_MSG(snd_get_user_buffer_released(r.sys, b) == 1, "mode %u gap %d: never released", mode, gap);
    r.Step(4);
    if (p.value != 0u) {
      SND_CHECK_MSG(r.EndsOf(p.value) == 1u, "mode %u gap %d: the play recorded after the destroy did not end once", mode, gap);
    }
    // Once released the game may free the memory: nothing may be heard from it any more.
    const std::uint64_t releasedAt = r.Frames();
    r.Step(10);
    std::uint64_t loud = 0;
    for (std::uint64_t c = releasedAt; c < r.Frames(); ++c) loud += r.Raw(c) != 0.0f ? 1u : 0u;
    SND_CHECK_MSG(loud == 0u, "mode %u gap %d: %llu frames heard after the release", mode, gap,
                  static_cast<unsigned long long>(loud));
    (void)from;
    if (p.value != 0u) snd_stop(r.sys, p, 0.0f, nullptr);
    r.Step(2);
  }
}

namespace {
struct Slow {
  std::atomic<int> inCall{0};
  std::atomic<int> slow{0};
  std::atomic<int> ended{0};
  std::atomic<int> callsAfterEnd{0};
};
void SND_CALL SlowSynth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                        const snd_render_context*) {
  auto* s = static_cast<Slow*>(user);
  s->inCall.fetch_add(1);
  if (s->ended.load() != 0) s->callsAfterEnd.fetch_add(1);
  if (s->slow.load() != 0) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    while (std::chrono::steady_clock::now() < until) {
    }
  }
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = 0.01f;
  }
  s->inCall.fetch_sub(1);
}
struct EndLog {
  Slow* users = nullptr;
  std::uint64_t* handles = nullptr;
  int n = 0;
  std::atomic<int> ends{0};
  std::atomic<int> endsDuringCall{0};
  std::atomic<int> twice{0};
  int perHandle[8] = {};
};
void SND_CALL OnEndLog(void* u, snd_playback_handle h, snd_end_reason, snd_end_detail) {
  auto* l = static_cast<EndLog*>(u);
  for (int i = 0; i < l->n; ++i) {
    if (l->handles[i] != h.value) continue;
    if (l->users[i].inCall.load() != 0) l->endsDuringCall.fetch_add(1);
    l->users[i].ended.store(1);
    if (++l->perHandle[i] > 1) l->twice.fetch_add(1);
  }
  l->ends.fetch_add(1);
}
}  // namespace

// X3: several held at once; the callbacks are replaced while they are held.
SND_TEST(IndepFix, HeldEndsArriveOnceAfterTheJobsAlsoWhenCallbacksChange) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  r.settle = false;
  constexpr int kN = 4;
  Slow users[kN];
  std::uint64_t handles[kN] = {};
  EndLog first, second;
  for (EndLog* l : {&first, &second}) {
    l->users = users;
    l->handles = handles;
    l->n = kN;
  }
  snd_callbacks cb{};
  cb.user = &first;
  cb.on_end = &OnEndLog;
  SND_REQUIRE_EQ(snd_set_callbacks(r.sys, &cb), SND_OK);
  std::vector<snd_playback_handle> ps;
  for (int i = 0; i < kN; ++i) {
    snd_synth_desc d{};
    d.callback = &SlowSynth;
    d.user = &users[i];
    d.channels = 1;
    d.mode = SND_SYNTH_BUFFERED;
    d.volume = 1.0f;
    ps.push_back(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr));
    handles[i] = ps.back().value;
  }
  r.Step(10);
  for (Slow& s : users) s.slow.store(1);
  r.Step(2);
  for (const snd_playback_handle& p : ps) snd_stop(r.sys, p, 0.0f, nullptr);
  r.Step(1);
  // Switch to the second set of callbacks while ends may be held.
  cb.user = &second;
  SND_REQUIRE_EQ(snd_set_callbacks(r.sys, &cb), SND_OK);
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (first.ends.load() + second.ends.load() < kN && std::chrono::steady_clock::now() < until) r.Step(1);
  for (Slow& s : users) s.slow.store(0);
  r.Step(10);
  SND_CHECK_EQ(first.ends.load() + second.ends.load(), kN);
  SND_CHECK_EQ(first.endsDuringCall.load() + second.endsDuringCall.load(), 0);
  int calls = 0;
  for (Slow& s : users) calls += s.callsAfterEnd.load();
  SND_CHECK_EQ(calls, 0);
  for (int i = 0; i < kN; ++i) SND_CHECK_EQ(first.perHandle[i] + second.perHandle[i], 1);
}
