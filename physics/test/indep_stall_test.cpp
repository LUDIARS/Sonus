// Independent tests of D-125 (physics.h "Audio side stalled"): stall detection by
// block_index, what is (not) recorded while stalled, the end of the stall, long suspends,
// and the new end-of-playback detection of sliding contacts. Expectations from physics.h and
// CONTRACTS.md D-125.
#include <cmath>
#include <cstdint>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/test/test.hpp"

using namespace indepphys;

namespace {

const snd_id kDc = snd_hash_id("I/Dc");
const snd_id kLoop = snd_hash_id("I/Loop");
const snd_id kShort = snd_hash_id("I/Short");

struct Fx {
  explicit Fx(snd_physics_config c = OpenCfg(), snd_config sc = phystest::SmallConfig(), snd_id impact = kDc,
              snd_id slide = kLoop)
      : s(sc) {
    loaded = s.LoadBank("phys.sndb", TestBank(), &bank);
    SetListener(s.sys);
    c.observer = &Recorder::Fn;
    c.observer_user = &rec;
    created = snd_physics_create(s.sys, &c, nullptr, &phys);
    if (phys == nullptr) return;
    single.assign(4, SND_ID_NONE);
    single[SND_CONTACT_IMPACT] = impact;
    single[SND_CONTACT_SLIDE] = slide;
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = 1;
    d.single = single.data();
    d.footstep_default = kDc;
    tableResult = snd_physics_set_table(phys, &d);
  }
  ~Fx() { snd_physics_destroy(phys); }
  static snd_physics_config OpenCfg() {
    snd_physics_config c = Cfg(nullptr);
    c.cooldown_seconds = 0.0f;
    c.budget_per_second = 1.0e6f;
    return c;
  }
  bool Ok() const { return loaded && created == SND_OK && tableResult == SND_OK; }
  snd_result Submit(std::vector<snd_contact> cs, float dt = 1.0f / 60.0f) {
    rec.Clear();
    return snd_phys_submit_contacts(phys, cs.empty() ? nullptr : cs.data(), static_cast<std::uint32_t>(cs.size()), dt);
  }
  snd_physics_stats St() const { return phystest::Stats(phys); }
  Sys s;
  Recorder rec;
  snd_bank_handle bank{};
  bool loaded = false;
  snd_phys* phys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  snd_result tableResult = SND_ERR_INVALID_ARGUMENT;
  std::vector<snd_id> single;
};

snd_contact Hit(std::uint64_t obj, float x) {
  return Contact(SND_CONTACT_IMPACT, obj, obj + 100000u, 0, 0, x, 0.0f, 0.0f, 5.0f);
}
snd_contact Slide(std::uint64_t a, float x, float speed = 1.0f) {
  return Contact(SND_CONTACT_SLIDE, a, a + 1000u, 0, 0, x, 0.0f, 0.0f, 0.0f, speed);
}
void Energy(const Sys& s, std::uint64_t from, std::uint64_t to, double* l, double* r) {
  *l = 0;
  *r = 0;
  for (std::uint64_t f = from; f < to && f < s.Frames(); ++f) {
    *l += std::fabs(s.L(f));
    *r += std::fabs(s.R(f));
  }
}
std::uint32_t CoreOverflow(const Sys& s) {
  snd_stats ss{};
  snd_get_stats(s.sys, &ss);
  return ss.rejected_commands + ss.reserve_overflows + ss.fallback_commands;
}

}  // namespace

// Detection: with stall_calls = 4, the calls that see one block_index play up to the 4th;
// from the 5th on the audio side counts as stalled (dropped_audio_stalled, a DROP record with
// LIMIT_REACHED, audio_stalled_calls). A moved block_index ends the stall at once.
// stall_calls 0 never stalls.
SND_TEST(IndepStall, DetectionBoundaryAndEnd) {
  snd_physics_config c = Fx::OpenCfg();
  c.stall_calls = 4;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  std::uint64_t obj = 0;
  for (int round = 0; round < 3; ++round) {
    std::vector<int> played;
    for (int call = 1; call <= 8; ++call, obj += 2) {
      SND_CHECK_EQ(f.Submit({Hit(obj, 2.0f + static_cast<float>(call))}), SND_OK);
      if (f.rec.Count(SND_PHYSICS_RECORD_PLAY) == 1u) played.push_back(call);
      if (call > 4) {
        SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_DROP), 1u);
        SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_DROP)[0].r.result, SND_ERR_LIMIT_REACHED);
      }
    }
    SND_CHECK_EQ(played.size(), 4u);
    if (played.size() == 4u) SND_CHECK_EQ(played.back(), 4);
    f.s.Advance(1);  // the block_index moves: the next call plays again
  }
  const snd_physics_stats st = f.St();
  SND_CHECK_EQ(st.dropped_audio_stalled, 12u);
  SND_CHECK_EQ(st.audio_stalled_calls, 12u);
  SND_CHECK_EQ(st.group_requests, 12u);
  SND_CHECK_EQ(st.dropped_no_emitter, 0u);
  // stall_calls 0: never.
  snd_physics_config c0 = Fx::OpenCfg();
  c0.stall_calls = 0;
  Fx g(c0);
  SND_REQUIRE(g.Ok());
  for (int call = 0; call < 200; ++call) SND_CHECK_EQ(g.Submit({}), SND_OK);
  SND_CHECK_EQ(g.St().audio_stalled_calls, 0u);
}

// Sub-steps are not a stall: 64 submits per block_index (the first sees it new, 63 repeat it;
// default stall_calls 64) never count as stalled, for 40 blocks; the hit of every block plays.
// (Each round waits until snd_get_stats shows a new block_index: the rig's period does not
// always publish a new block by the next snd_update.)
SND_TEST(IndepStall, SubstepsAreNotAStall) {
  Fx f;
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  std::uint64_t obj = 0;
  snd_stats ss{};
  snd_get_stats(f.s.sys, &ss);
  std::uint64_t last = ss.block_index;
  for (int frame = 0; frame < 40; ++frame, obj += 2) {
    for (int i = 0; i < 50; ++i) {
      f.s.Advance(1);
      snd_get_stats(f.s.sys, &ss);
      if (ss.block_index != last) break;
    }
    SND_REQUIRE(ss.block_index != last);
    last = ss.block_index;
    SND_CHECK_EQ(f.Submit({Hit(obj, 2.0f)}, 1.0f / 960.0f), SND_OK);
    for (int sub = 1; sub < 64; ++sub) SND_CHECK_EQ(f.Submit({}, 1.0f / 960.0f), SND_OK);
  }
  const snd_physics_stats st = f.St();
  snd::test::Note("64 calls per block_index: stalled calls %llu", static_cast<unsigned long long>(st.audio_stalled_calls));
  SND_CHECK_EQ(st.audio_stalled_calls, 0u);
  SND_CHECK_EQ(st.group_requests, 40u);
  // One more call on the same block_index is the 65th: stalled.
  SND_CHECK_EQ(f.Submit({Hit(obj, 2.0f)}, 1.0f / 960.0f), SND_OK);
  SND_CHECK_EQ(f.St().audio_stalled_calls, 1u);
}

// While stalled: the statistics identity holds with dropped_audio_stalled; a dropped group uses
// its budget place (the next group in budget order is dropped_budget) and records no cooldown
// (the same key plays as soon as the stall ends, even with a long cooldown).
SND_TEST(IndepStall, StalledGroupsAccounting) {
  snd_physics_config c = Fx::OpenCfg();
  c.stall_calls = 2;
  c.cooldown_seconds = 10.0f;
  c.budget_per_second = 60.0f;  // 1 place per 1/60 s call
  Fx f(c);
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  SND_CHECK_EQ(f.Submit({}), SND_OK);
  SND_CHECK_EQ(f.Submit({}), SND_OK);
  SND_CHECK_EQ(f.Submit({Hit(1, 2.0f), Hit(3, 3.0f)}), SND_OK);  // stalled (3rd call, same block)
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_DROP), 1u);
  snd_physics_stats st = f.St();
  SND_CHECK_EQ(st.dropped_audio_stalled, 1u);
  SND_CHECK_EQ(st.dropped_budget, 1u);
  SND_CHECK_EQ(st.groups, st.dropped_no_event + st.dropped_cooldown + st.dropped_budget + st.dropped_audio_stalled +
                              st.dropped_no_emitter + st.group_requests);
  f.s.Advance(1);
  SND_CHECK_EQ(f.Submit({Hit(1, 2.0f)}), SND_OK);  // no cooldown was recorded
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  st = f.St();
  SND_CHECK_EQ(st.dropped_cooldown, 0u);
}

// Sliding contacts while stalled: the reports update the entries (tracking_updated) without
// recording (the command area does not grow even with a tiny arena); a new key is dropped
// (tracking_dropped); a key no longer reported does not time out during the stall but at the
// first call after it; the first report after the stall records the latest position and
// speed, and the loop is heard there.
SND_TEST(IndepStall, SlidesDuringAStall) {
  snd_config sc = phystest::SmallConfig();
  sc.commands.arena_bytes = 4096;
  sc.commands.reserve_bytes = 1024;
  snd_physics_config c = Fx::OpenCfg();
  c.tracking_timeout_seconds = 2.0f;  // 120 calls: slide 2 times out inside the stall (from call ~64)
  Fx f(c, sc);
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  SND_CHECK_EQ(f.Submit({Slide(1, -2.0f), Slide(2, -3.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 2u);
  f.s.Advance(2);
  // Stall: no snd_update and no output for 2000 calls; slide 1 moves from the left to the
  // right with a rising speed, slide 2 is no longer reported, slide 3 is new.
  std::uint32_t overflowAt100 = 0;
  std::size_t recordsDuringStall = 0;
  for (int call = 0; call < 2000; ++call) {
    const float x = -2.0f + 4.0f * static_cast<float>(call) / 1999.0f;
    std::vector<snd_contact> cs = {Slide(1, x, 1.0f + static_cast<float>(call) * 0.001f)};
    if (call == 1000) cs.push_back(Slide(3, 0.0f));
    SND_CHECK_EQ(f.Submit(cs), SND_OK);
    if (call >= 100) {
      for (const Rec& r : f.rec.recs) {
        if (r.r.kind != SND_PHYSICS_RECORD_FALLBACK) ++recordsDuringStall;
      }
    }
    if (call == 100) overflowAt100 = CoreOverflow(f.s);
  }
  snd_physics_stats st = f.St();
  snd::test::Note("stall: stalled calls %llu, tracking_updated %llu, dropped %llu, timeouts %llu, records %llu",
                  static_cast<unsigned long long>(st.audio_stalled_calls),
                  static_cast<unsigned long long>(st.tracking_updated),
                  static_cast<unsigned long long>(st.tracking_dropped),
                  static_cast<unsigned long long>(st.tracking_timeouts), static_cast<unsigned long long>(recordsDuringStall));
  SND_CHECK(st.audio_stalled_calls >= 2000u - 70u);
  SND_CHECK_EQ(CoreOverflow(f.s), overflowAt100);  // nothing recorded meanwhile
  SND_CHECK_EQ(st.tracking_dropped, 1u);         // slide 3
  SND_CHECK_EQ(st.tracking_timeouts, 0u);        // slide 2 waits for the end of the stall
  SND_CHECK_EQ(st.tracking_updated + st.tracking_restarted, 2000u);
  // The stall ends (snd_update + output): slide 2 times out, slide 1 records its latest values.
  f.s.Advance(1);
  const float lastX = 2.0f;
  const float lastSpeed = 1.0f + 1999.0f * 0.001f;
  SND_CHECK_EQ(f.Submit({Slide(1, lastX, lastSpeed)}), SND_OK);
  st = f.St();
  SND_CHECK_EQ(st.tracking_timeouts, 1u);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 1u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_STOP)[0].r.object_a, 2u);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_UPDATE), 1u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_UPDATE)[0].r.position.x, lastX);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_UPDATE)[0].Param(P(SND_PHYSICS_PARAM_SPEED_NAME)), lastSpeed);
  for (int i = 0; i < 20; ++i) {
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({Slide(1, lastX, lastSpeed)}), SND_OK);
  }
  const std::uint64_t from = f.s.Frames();
  for (int i = 0; i < 10; ++i) {
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({Slide(1, lastX, lastSpeed)}), SND_OK);
  }
  double l = 0, r = 0;
  Energy(f.s, from, f.s.Frames(), &l, &r);
  snd::test::Note("slide 1 after the stall: |L| %.2f |R| %.2f", l, r);
  SND_CHECK(r > 1.0);
  SND_CHECK(l < 1e-3 * r);
}

// A long suspend (3000 frames, snd_update every frame) with hits and slides sent every frame
// (the condition where the reserve overflowed from frame 129 before D-125): snd_resume is
// accepted, nothing overflowed, the held emitters come back and new hits play on the right.
SND_TEST(IndepStall, LongSuspendWithHitsAndSlidesResumes) {
  Fx f;
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  SND_CHECK_EQ(snd_suspend(f.s.sys), SND_OK);
  std::uint64_t obj = 10;
  for (int frame = 0; frame < 3000; ++frame) {
    std::vector<snd_contact> cs = {Slide(1, -2.0f)};
    if (frame == 0) cs.push_back(Slide(2, -3.0f));
    for (int i = 0; i < 2; ++i, obj += 2) cs.push_back(Hit(obj, 2.0f + 0.6f * static_cast<float>(i)));
    SND_CHECK_EQ(f.Submit(cs), SND_OK);
    snd_update(f.s.sys);
  }
  snd_stats ss{};
  snd_get_stats(f.s.sys, &ss);
  snd::test::Note("3000 suspended frames: rejected %u, overflows %u, fallback %u", ss.rejected_commands,
                  ss.reserve_overflows, ss.fallback_commands);
  SND_CHECK_EQ(ss.rejected_commands, 0u);
  SND_CHECK_EQ(ss.reserve_overflows, 0u);
  SND_CHECK_EQ(snd_resume(f.s.sys), SND_OK);
  for (int i = 0; i < 20; ++i) {  // slide 1 times out 0.2 s (12 calls) after its last report
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({}), SND_OK);
  }
  f.s.Advance(40);
  const std::uint64_t from = f.s.Frames();
  for (int frame = 0; frame < 40; ++frame, obj += 2) {
    SND_CHECK_EQ(f.Submit({Hit(obj, 2.0f)}), SND_OK);
    f.s.Advance(1);
  }
  f.s.Advance(60);
  double l = 0, r = 0;
  Energy(f.s, from, f.s.Frames(), &l, &r);
  snd::test::Note("after the resume: |L| %.3f |R| %.3f", l, r);
  SND_CHECK(r > 1.0);
  SND_CHECK_EQ(l, 0.0);
  SND_CHECK_EQ(f.St().tracking_timeouts, 2u);
}

// Footsteps and play_at record while stalled (physics.h: "the game calls them itself"). A game
// that keeps calling them every frame during a long suspend runs into the core limit of
// request A-005. Record: (a) the resume is accepted, or (b) COMMAND_BUFFER_FULL (A-005); the
// footsteps and play_at are recorded in both cases.
SND_TEST(IndepStall, CoreA005_FootstepsAndPlayAtWhileSuspended) {
  Fx f;
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  SND_CHECK_EQ(snd_suspend(f.s.sys), SND_OK);
  const snd_surface_weight w{0, 0, 1.0f};
  const snd_vec3 pos{2.0f, 0.0f, 0.0f};
  for (int frame = 0; frame < 2000; ++frame) {
    SND_CHECK_EQ(f.Submit({}), SND_OK);
    f.rec.Clear();
    snd_phys_play_footstep(f.phys, snd_emitter_handle{0}, 0, &w, 1, SND_MOTION_WALK);
    snd_physics_play_at(f.phys, kDc, &pos);
    snd_update(f.s.sys);
  }
  const snd_physics_stats st = f.St();
  SND_CHECK_EQ(st.requested_footstep, 2000u);
  SND_CHECK_EQ(st.requested_play_at, 2000u);
  snd_result rr = snd_resume(f.s.sys);
  for (int i = 0; i < 100 && rr != SND_OK; ++i) {
    snd_update(f.s.sys);
    rr = snd_resume(f.s.sys);
  }
  const bool fixed = rr == SND_OK;
  const bool known = rr == SND_ERR_COMMAND_BUFFER_FULL;
  snd::test::Note("footsteps and play_at for 2000 suspended frames: %s (resume %d, api failures %llu)",
                  fixed ? "(a) the resume is accepted" : known ? "(b) A-005: COMMAND_BUFFER_FULL" : "neither",
                  static_cast<int>(rr), static_cast<unsigned long long>(st.api_failures));
  SND_CHECK(fixed || known);
}

// The new end-of-playback detection of sliding contacts (3956b9d): a slide whose event is
// short (I/Short, one block) and that is reported every frame must keep being restarted
// (ENDED, NONE after seen, or refused as stale), whether its values change or not; it must
// not stay silent. And a slide stopped by the game (snd_stop on the observed handle) is
// restarted by the next report.
SND_TEST(IndepStall, ShortSlideKeepsRestarting) {
  for (int changing = 0; changing < 2; ++changing) {
    snd_physics_config c = Fx::OpenCfg();
    c.tracking_timeout_seconds = 10.0f;
    Fx f(c, phystest::SmallConfig(), kDc, kShort);
    SND_REQUIRE(f.Ok());
    f.s.Advance(2);
    std::uint64_t restarts = 0;
    int lastRestart = -1, longestGap = 0;
    const std::uint64_t from = f.s.Frames();
    for (int frame = 0; frame < 300; ++frame) {
      const float speed = changing ? 1.0f + 0.01f * static_cast<float>(frame) : 1.0f;
      SND_CHECK_EQ(f.Submit({Slide(1, 2.0f, speed)}), SND_OK);
      for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_START)) {
        if (r.r.flags & SND_PHYSICS_RECORD_FLAG_RESTART) {
          ++restarts;
          if (lastRestart >= 0 && frame - lastRestart > longestGap) longestGap = frame - lastRestart;
          lastRestart = frame;
        }
      }
      f.s.Advance(1);
    }
    double l = 0, r = 0;
    Energy(f.s, f.s.Frames() - 100u * 256u, f.s.Frames(), &l, &r);
    snd::test::Note("short slide, values %s: %llu restarts, longest gap %d frames, |R| of the last 100 frames %.1f",
                    changing ? "changing" : "constant", static_cast<unsigned long long>(restarts), longestGap, r);
    SND_CHECK(restarts >= 50u);
    SND_CHECK(longestGap <= 10);
    SND_CHECK(r > 1.0);
    (void)from;
  }
  // Stopped by the game: restarted by the next report.
  Fx f;
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  SND_CHECK_EQ(f.Submit({Slide(1, 2.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 1u);
  const snd_playback_handle h = f.rec.Of(SND_PHYSICS_RECORD_START)[0].r.playback;
  for (int i = 0; i < 4; ++i) {
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({Slide(1, 2.0f)}), SND_OK);
  }
  SND_CHECK_EQ(snd_stop(f.s.sys, h, 0.0f, nullptr), SND_OK);
  bool restarted = false;
  for (int i = 0; i < 10 && !restarted; ++i) {
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({Slide(1, 2.0f)}), SND_OK);
    restarted = f.rec.Count(SND_PHYSICS_RECORD_START) == 1u;
  }
  SND_CHECK(restarted);
}
