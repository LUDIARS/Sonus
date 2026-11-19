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

// fef7a66 (D-97 note): plays applied after a destroy never start and end DISCARDED once;
// plays applied before it are stopped. Border cases, Stream and looping Static:
//  A destroy -> play -> play (one arena)    B play -> destroy -> play (one arena)
//  C a Pending play (NEXT_BAR on a slow transport) then destroy, same arena and next frame
//  E destroy -> play -> stop (one arena)
// Each: every issued play ends exactly once, the buffer is released, and nothing of the
// buffer (0.25) is heard after the destroy was applied (A, C, E) / after the stop fade (B).
namespace {

struct BufCase {
  indep::Rig* r;
  std::vector<float> mem;
  snd_user_buffer_handle b{};
  explicit BufCase(indep::Rig& rig, snd_user_buffer_mode mode) : r(&rig), mem(1000, 0.25f) {
    float* chans[1] = {mem.data()};
    const snd_user_buffer_desc bd{chans, 1, 1000, 0, mode, mode == SND_USER_BUFFER_STATIC ? 1u : 0u, 0};
    b = snd_create_user_buffer(r->sys, &bd);
    const float* src[1] = {mem.data()};
    snd_write_user_buffer(r->sys, b, src, 1000);
  }
  snd_playback_handle Play(const snd_when* w = nullptr) {
    return snd_play_user_buffer(r->sys, b, 0u, snd_emitter_handle{0}, w);
  }
  bool WaitReleased() {
    for (int i = 0; i < 80 && snd_get_user_buffer_released(r->sys, b) == 0; ++i) r->Step(1);
    return snd_get_user_buffer_released(r->sys, b) == 1;
  }
};

std::uint32_t ReasonOf(const indep::Rig& r, std::uint64_t h) {
  for (const apitest::End& e : r.ev.ends) {
    if (e.handle == h) return e.reason;
  }
  return 0u;
}

std::uint64_t HeardFrom(const indep::Rig& r, std::uint64_t from) {
  std::uint64_t n = 0;
  for (std::uint64_t c = from; c < r.Frames(); ++c) n += r.Raw(c) != 0.0f ? 1u : 0u;
  return n;
}

}  // namespace

SND_TEST(IndepFix, PlaysAroundADestroyInOneArena) {
  for (const snd_user_buffer_mode mode : {SND_USER_BUFFER_STREAM, SND_USER_BUFFER_STATIC}) {
    // A: destroy -> play -> play.
    {
      indep::Rig r;
      SND_REQUIRE_EQ(r.created, SND_OK);
      BufCase bc(r, mode);
      SND_REQUIRE(bc.b.value != 0u);
      r.Step(2);
      const std::uint64_t from = r.RenderClock();
      SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, bc.b), SND_OK);
      const snd_playback_handle p1 = bc.Play();
      const snd_playback_handle p2 = bc.Play();
      SND_CHECK_MSG(bc.WaitReleased(), "A mode %u: not released", mode);
      r.Step(4);
      for (const snd_playback_handle& p : {p1, p2}) {
        if (p.value == 0u) continue;
        SND_CHECK_MSG(r.EndsOf(p.value) == 1u, "A mode %u: ends %u", mode, r.EndsOf(p.value));
        SND_CHECK_EQ(ReasonOf(r, p.value), SND_END_DISCARDED);
      }
      SND_CHECK_MSG(HeardFrom(r, from) == 0u, "A mode %u: the buffer was heard", mode);
    }
    // B: play -> destroy -> play.
    {
      indep::Rig r;
      SND_REQUIRE_EQ(r.created, SND_OK);
      BufCase bc(r, mode);
      SND_REQUIRE(bc.b.value != 0u);
      r.Step(2);
      const snd_playback_handle p1 = bc.Play();
      SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, bc.b), SND_OK);
      const snd_playback_handle p2 = bc.Play();
      SND_REQUIRE(p1.value != 0u);
      SND_CHECK_MSG(bc.WaitReleased(), "B mode %u: not released", mode);
      r.Step(4);
      SND_CHECK_EQ(r.EndsOf(p1.value), 1u);
      if (p2.value != 0u) {
        SND_CHECK_EQ(r.EndsOf(p2.value), 1u);
        SND_CHECK_EQ(ReasonOf(r, p2.value), SND_END_DISCARDED);
      }
      const std::uint64_t quiet = r.Frames();
      r.Step(10);
      SND_CHECK_EQ(HeardFrom(r, quiet), 0u);
    }
    // E: destroy -> play -> stop.
    {
      indep::Rig r;
      SND_REQUIRE_EQ(r.created, SND_OK);
      BufCase bc(r, mode);
      r.Step(2);
      const std::uint64_t from = r.RenderClock();
      SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, bc.b), SND_OK);
      const snd_playback_handle p = bc.Play();
      SND_REQUIRE(p.value != 0u);
      SND_CHECK_EQ(snd_stop(r.sys, p, 0.0f, nullptr), SND_OK);
      SND_CHECK(bc.WaitReleased());
      r.Step(4);
      SND_CHECK_EQ(r.EndsOf(p.value), 1u);
      SND_CHECK_EQ(HeardFrom(r, from), 0u);
    }
  }
}

SND_TEST(IndepFix, PendingPlayOfADestroyedBuffer) {
  for (const snd_user_buffer_mode mode : {SND_USER_BUFFER_STREAM, SND_USER_BUFFER_STATIC}) {
    for (const int gap : {0, 1}) {
      for (const bool playFirst : {true, false}) {
        if (!playFirst && gap > 0) continue;  // a play applied after the release: Defect_PendingPlayAppliedAfterTheReleaseEnds
        indep::Rig r;
        SND_REQUIRE_EQ(r.created, SND_OK);
        BufCase bc(r, mode);
        const snd_transport_desc td{20.0f, 4, 4, 1, 0};  // a bar = 12 s: the play stays Pending
        const snd_transport_handle t = snd_create_transport(r.sys, &td);
        r.Step(30);  // past bar 0's line
        snd_when w{};
        w.kind = SND_WHEN_NEXT_BAR;
        w.transport = t;
        const std::uint64_t from = r.RenderClock();
        snd_playback_handle p{0};
        if (playFirst) {
          p = bc.Play(&w);
          if (gap > 0) r.Step(static_cast<std::uint32_t>(gap));
          SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, bc.b), SND_OK);
        } else {
          SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, bc.b), SND_OK);
          if (gap > 0) r.Step(static_cast<std::uint32_t>(gap));
          if (snd_get_user_buffer_released(r.sys, bc.b) == 0) p = bc.Play(&w);
        }
        SND_CHECK_MSG(bc.WaitReleased(), "mode %u gap %d first %d: not released", mode, gap, playFirst ? 1 : 0);
        r.Step(4);
        if (p.value != 0u) {
          SND_CHECK_MSG(r.EndsOf(p.value) == 1u, "mode %u gap %d first %d: ends %u", mode, gap, playFirst ? 1 : 0,
                        r.EndsOf(p.value));
        }
        SND_CHECK_EQ(HeardFrom(r, from), 0u);
      }
    }
  }
}

// A virtual user-buffer voice (one real voice, taken by a loud synth) whose buffer is
// destroyed: it ends once and the buffer is released; a play after the destroy too.
SND_TEST(IndepFix, VirtualUserBufferVoiceAndDestroy) {
  for (const snd_user_buffer_mode mode : {SND_USER_BUFFER_STREAM, SND_USER_BUFFER_STATIC}) {
    snd_config c = SmallConfig();
    c.max_real_voices = 1;
    c.fade_reserve_voices = 2;
    indep::Rig r(c);
    SND_REQUIRE_EQ(r.created, SND_OK);
    static const float kLoud = 0.9f;
    const snd_synth_desc d = indep::DcDesc(&kLoud);
    const snd_playback_handle s = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
    r.Step(4);
    BufCase bc(r, mode);
    for (float& x : bc.mem) x = 0.001f;  // quieter than the synth: virtual
    const snd_playback_handle p1 = bc.Play();
    SND_REQUIRE(p1.value != 0u);
    r.Step(10);
    snd_update(r.sys);
    snd_stats st{};
    SND_REQUIRE_EQ(snd_get_stats(r.sys, &st), SND_OK);
    SND_CHECK_EQ(st.real_voices, 1u);
    SND_CHECK_EQ(r.EndsOf(p1.value), 0u);
    SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, bc.b), SND_OK);
    const snd_playback_handle p2 = bc.Play();
    SND_CHECK_MSG(bc.WaitReleased(), "mode %u: not released", mode);
    r.Step(4);
    SND_CHECK_EQ(r.EndsOf(p1.value), 1u);
    if (p2.value != 0u) SND_CHECK_EQ(r.EndsOf(p2.value), 1u);
    snd_stop(r.sys, s, 0.0f, nullptr);
    r.Step(6);
    SND_CHECK_EQ(r.EndsOf(s.value), 1u);
  }
}

// A play recorded while the game still sees the buffer as current, but applied after the
// audio side released it (destroy applied, no playback left, release published, the
// owner's update not yet run): with a When that has not come (NEXT_BAR on a STOPPED
// transport) it stays Pending, holds a reference on the slot, and never ends. D-97: a play
// applied while the buffer is being destroyed or after must not start and ends once; and
// a released slot must not keep references from old plays (a new buffer in that slot could
// then never be released).
SND_TEST(IndepFix, Defect_PendingPlayAppliedAfterTheReleaseEnds) {
  std::uint32_t issued = 0, stuck = 0, newNotReleased = 0;
  for (int trial = 0; trial < 6; ++trial) {
    indep::Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    BufCase bc(r, (trial & 1) == 0 ? SND_USER_BUFFER_STREAM : SND_USER_BUFFER_STATIC);
    const snd_transport_desc td{120.0f, 4, 4, 0, 0};  // Stopped: NEXT_BAR does not come
    const snd_transport_handle t = snd_create_transport(r.sys, &td);
    r.Step(2);
    SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, bc.b), SND_OK);
    r.Step(1);
    if (snd_get_user_buffer_released(r.sys, bc.b) != 0) continue;
    snd_when w{};
    w.kind = SND_WHEN_NEXT_BAR;
    w.transport = t;
    const snd_playback_handle p = bc.Play(&w);
    if (p.value == 0u) continue;
    ++issued;
    SND_CHECK(bc.WaitReleased());
    r.Step(20);
    if (r.EndsOf(p.value) != 1u) ++stuck;
    // A new buffer (it may take the same slot): destroyed, it must be released.
    BufCase nb(r, SND_USER_BUFFER_STATIC);
    SND_REQUIRE(nb.b.value != 0u);
    r.Step(2);
    SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, nb.b), SND_OK);
    if (!nb.WaitReleased()) ++newNotReleased;
    snd_stop(r.sys, p, 0.0f, nullptr);
    r.Step(4);
  }
  snd::test::Note("plays issued %u, still pending after the release %u, new buffers never released %u", issued, stuck,
                  newNotReleased);
  SND_CHECK(issued > 0u);
  SND_CHECK_EQ(stuck, 0u);
  SND_CHECK_EQ(newNotReleased, 0u);
}
