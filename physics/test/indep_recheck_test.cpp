// Independent tests of the second round (re-check after 51c7144): the new release of the
// held one-shot emitters ("released once the audio side is seen to have applied the plays"),
// the tracking order and the new-key area, under conditions the implementer's tests do not
// reach.
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/test/test.hpp"

using namespace indepphys;

namespace {

const snd_id kDc = snd_hash_id("I/Dc");
const snd_id kShort = snd_hash_id("I/Short");
const snd_id kLoop = snd_hash_id("I/Loop");

struct Fx {
  explicit Fx(snd_id impactEvent = kDc, snd_physics_config c = Cfg(nullptr), snd_config sc = phystest::SmallConfig())
      : s(sc) {
    loaded = s.LoadBank("phys.sndb", TestBank(), &bank);
    SetListener(s.sys);
    c.observer = &Recorder::Fn;
    c.observer_user = &rec;
    created = snd_physics_create(s.sys, &c, nullptr, &phys);
    if (phys == nullptr) return;
    single.assign(4, SND_ID_NONE);
    single[SND_CONTACT_IMPACT] = impactEvent;
    single[SND_CONTACT_SLIDE] = kLoop;
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = 1;
    d.single = single.data();
    tableResult = snd_physics_set_table(phys, &d);
  }
  ~Fx() { snd_physics_destroy(phys); }
  bool Ok() const { return loaded && created == SND_OK && tableResult == SND_OK; }
  snd_result Submit(std::vector<snd_contact> cs, float dt = 1.0f / 60.0f) {
    rec.Clear();
    return snd_phys_submit_contacts(phys, cs.empty() ? nullptr : cs.data(), static_cast<std::uint32_t>(cs.size()), dt);
  }
  // Emitters the game can still create (all destroyed again).
  std::uint32_t FreeEmitters(std::uint32_t upTo) {
    std::vector<snd_emitter_handle> mine;
    for (std::uint32_t i = 0; i < upTo; ++i) mine.push_back(snd_create_emitter(s.sys));
    std::uint32_t ok = 0;
    for (const snd_emitter_handle e : mine) {
      if (e.value != 0u) {
        ++ok;
        snd_destroy_emitter(s.sys, e);
      }
    }
    return ok;
  }
  Sys s;
  Recorder rec;
  snd_bank_handle bank{};
  bool loaded = false;
  snd_phys* phys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  snd_result tableResult = SND_ERR_INVALID_ARGUMENT;
  std::vector<snd_id> single;
};

snd_contact Hit(std::uint64_t obj, float x, float impulse = 5.0f) {
  return Contact(SND_CONTACT_IMPACT, obj, obj + 100000u, 0, 0, x, 0.0f, 0.0f, impulse);
}

snd_physics_config Open() {
  snd_physics_config c = Cfg(nullptr);
  c.cooldown_seconds = 0.0f;
  c.budget_per_second = 1.0e6f;
  return c;
}

// Sum of |L| and |R| over the captured frames from `from`.
void Energy(const Sys& s, std::uint64_t from, double* l, double* r) {
  *l = 0;
  *r = 0;
  for (std::uint64_t f = from; f < s.Frames(); ++f) {
    *l += std::fabs(s.L(f));
    *r += std::fabs(s.R(f));
  }
}

}  // namespace

// The observation physics relies on (physics.h): a fresh playback handle reads NONE until
// the audio side has applied its play, also when its slot was used by an earlier playback
// whose state was published (the snapshot is matched by generation), and also after an
// snd_update that published the command but before any block applied it.
SND_TEST(IndepRecheck, CoreStateIsNoneUntilApplied) {
  snd_config sc = phystest::SmallConfig();
  sc.max_playbacks = 2;  // slots are reused all the time
  Sys s(sc);
  snd_bank_handle bank{};
  SND_REQUIRE(s.LoadBank("phys.sndb", TestBank(), &bank));
  std::uint32_t early = 0, seenApplied = 0;
  for (int i = 0; i < 40; ++i) {
    const snd_playback_handle h = snd_play(s.sys, kShort, snd_emitter_handle{0}, nullptr);
    SND_REQUIRE(h.value != 0u);
    if (snd_get_playback_state(s.sys, h) != SND_PLAYBACK_STATE_NONE) ++early;
    snd_update(s.sys);  // published, not applied by a block yet (the manual device has not run)
    if (snd_get_playback_state(s.sys, h) != SND_PLAYBACK_STATE_NONE) ++early;
    for (int k = 0; k < 6; ++k) {
      s.Advance(1);
      if (snd_get_playback_state(s.sys, h) != SND_PLAYBACK_STATE_NONE) ++seenApplied;
    }
  }
  SND_CHECK_EQ(early, 0u);
  SND_CHECK(seenApplied > 0u);
}

// Positions are kept under the release rule: one submit per frame, sub-steps (several
// submits per snd_update, many empty ones), a short event (1 block), and slots reused
// (max_playbacks small). Every hit is 2 m to the right: the left channel stays silent.
SND_TEST(IndepRecheck, HitsStayOnTheRightUnderEveryPattern) {
  struct Pattern {
    const char* what;
    snd_id event;
    std::uint32_t maxPlaybacks;
    int submitsPerUpdate;
    int hitsPerSubmit;
  };
  const Pattern patterns[] = {
      {"1 submit per frame", kDc, 256, 1, 1},
      {"4 sub-steps", kDc, 256, 4, 2},
      {"30 empty sub-steps", kDc, 256, 30, 0},
      {"short event", kShort, 256, 1, 3},
      {"short event, slots reused", kShort, 6, 3, 2},
  };
  for (const Pattern& pt : patterns) {
    snd_config sc = phystest::SmallConfig();
    sc.max_playbacks = pt.maxPlaybacks;
    Fx f(pt.event, Open(), sc);
    SND_REQUIRE(f.Ok());
    f.s.Advance(2);
    const std::uint64_t from = f.s.Frames();
    std::uint64_t obj = 0;
    for (int frame = 0; frame < 120; ++frame) {
      for (int sub = 0; sub < pt.submitsPerUpdate; ++sub) {
        std::vector<snd_contact> cs;
        const int n = pt.hitsPerSubmit > 0 ? pt.hitsPerSubmit : (sub == 0 ? 1 : 0);
        for (int i = 0; i < n; ++i, obj += 2) cs.push_back(Hit(obj, 2.0f + 0.001f * static_cast<float>(i)));
        SND_CHECK_EQ(f.Submit(cs, 1.0f / 240.0f), SND_OK);
      }
      f.s.Advance(1);
    }
    f.s.Advance(60);
    double l = 0, r = 0;
    Energy(f.s, from, &l, &r);
    const snd_physics_stats st = phystest::Stats(f.phys);
    snd::test::Note("%s: |L| %.3f |R| %.3f, requests %llu, no emitter %llu, api failures %llu", pt.what, l, r,
                    static_cast<unsigned long long>(st.group_requests),
                    static_cast<unsigned long long>(st.dropped_no_emitter),
                    static_cast<unsigned long long>(st.api_failures));
    SND_CHECK_MSG(r > 1.0, "%s: nothing heard", pt.what);
    SND_CHECK_MSG(l < 1e-3 * r, "%s: |L| %.4f against |R| %.4f (a hit was heard off its position)", pt.what, l, r);
    SND_CHECK_EQ(st.dropped_no_emitter, 0u);
  }
}

// Decision 2 of the final round: a full hold ring never destroys a held emitter early; the
// new group is dropped (dropped_no_emitter). 6 sub-steps of 20 hits in 20 cells before one
// snd_update (max_held_emitters 64): 64 hits are held and heard on the right, the other 56
// are dropped; nothing is heard at the listener. Once the audio side runs, the ring empties
// and new hits play again.
SND_TEST(IndepRecheck, RingFullBySubstepsDropsNewAndKeepsPositions) {
  Fx f(kDc, Open());
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  const std::uint64_t from = f.s.Frames();
  std::uint64_t obj = 0;
  for (int sub = 0; sub < 6; ++sub) {
    std::vector<snd_contact> cs;
    for (int i = 0; i < 20; ++i, obj += 2) cs.push_back(Hit(obj, 2.0f + 0.6f * static_cast<float>(i)));  // 20 cells
    SND_CHECK_EQ(f.Submit(cs, 1.0f / 60.0f), SND_OK);
  }
  f.s.Advance(20);
  double l = 0, r = 0;
  Energy(f.s, from, &l, &r);
  snd_physics_stats st = phystest::Stats(f.phys);
  snd::test::Note("ring full by sub-steps: requests %llu, no emitter %llu, |L| %.2f |R| %.2f",
                  static_cast<unsigned long long>(st.group_requests),
                  static_cast<unsigned long long>(st.dropped_no_emitter), l, r);
  SND_CHECK_EQ(st.group_requests, 64u);
  SND_CHECK_EQ(st.dropped_no_emitter, 56u);
  SND_CHECK(r > 1.0);
  SND_CHECK_EQ(l, 0.0);
  // The ring empties as soon as the plays are applied: a new hit plays.
  SND_CHECK_EQ(f.Submit({Hit(obj, 2.0f)}, 1.0f / 60.0f), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_DROP), 0u);
}

// The ring cannot stay full for good while the audio side runs: with max_held_emitters 1
// and several hits every call (only one fits), the system keeps playing one hit per few
// calls for 2000 frames, never zero for long.
SND_TEST(IndepRecheck, FullRingAlwaysDrains) {
  snd_physics_config c = Open();
  c.max_held_emitters = 1;
  Fx f(kShort, c);
  SND_REQUIRE(f.Ok());
  std::uint64_t obj = 0;
  int sinceLastPlay = 0, longestGap = 0;
  for (int frame = 0; frame < 2000; ++frame) {
    std::vector<snd_contact> cs;
    for (int i = 0; i < 3; ++i, obj += 2) cs.push_back(Hit(obj, 1.0f + static_cast<float>(i)));
    SND_CHECK_EQ(f.Submit(cs, 1.0f / 60.0f), SND_OK);
    if (f.rec.Count(SND_PHYSICS_RECORD_PLAY) > 0u) {
      sinceLastPlay = 0;
    } else if (++sinceLastPlay > longestGap) {
      longestGap = sinceLastPlay;
    }
    f.s.Advance(1);
  }
  snd::test::Note("ring of 1: longest run of calls without a play %d", longestGap);
  SND_CHECK(longestGap <= 8);
}

// The volume recorded again by the release check (the volume the playback already has)
// changes nothing: a hit with volume 0.5 held through many submits without snd_update is
// heard at 0.5 of the level of the same hit with volume 1.
SND_TEST(IndepRecheck, ReleaseCheckDoesNotChangeTheVolume) {
  double level[2] = {0, 0};
  for (int v = 0; v < 2; ++v) {
    Fx f(kDc, Open());
    SND_REQUIRE(f.Ok());
    snd_physics_curves cv{};
    cv.impulse_to_volume = phystest::Linear({{0.0f, v == 0 ? 1.0f : 0.5f}});
    SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 0, &cv), SND_OK);
    f.s.Advance(2);
    SND_CHECK_EQ(f.Submit({Hit(1, 2.0f)}), SND_OK);
    for (int i = 0; i < 20; ++i) SND_CHECK_EQ(f.Submit({}), SND_OK);  // no snd_update in between
    f.s.Advance(1);
    for (int i = 0; i < 5; ++i) {
      SND_CHECK_EQ(f.Submit({}), SND_OK);
      f.s.Advance(1);
    }
    const std::uint64_t from = f.s.Frames();
    f.s.Advance(8);
    double l = 0, r = 0;
    Energy(f.s, from, &l, &r);
    level[v] = r;
  }
  SND_CHECK(level[0] > 1.0);
  SND_CHECK_NEAR(level[1] / level[0], 0.5, 0.01);
}

// No leak of emitters over a long run: max_emitters 8, max_held_emitters 4, 6000 frames
// of random hits, some frames without snd_update, a bank unload in the middle, unloaded
// events, plays rejected for want of playback slots. At quiet points every emitter is back.
SND_TEST(IndepRecheck, NoEmitterLeakOverALongRun) {
  snd_config sc = phystest::SmallConfig();
  sc.max_emitters = 8;
  sc.max_playbacks = 6;
  snd_physics_config c = Open();
  c.max_held_emitters = 4;
  Fx f(kShort, c, sc);
  SND_REQUIRE(f.Ok());
  Rng rng(7);
  std::uint64_t obj = 0;
  std::uint32_t quietChecks = 0, quietOk = 0;
  for (int frame = 0; frame < 6000; ++frame) {
    if (frame == 2000) {
      SND_CHECK_EQ(snd_unload_bank(f.s.sys, f.bank, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
    }
    if (frame == 3000) {
      // Unloaded now: reload (the events exist again).
      f.loaded = f.s.LoadBank("phys2.sndb", TestBank(), &f.bank);
      SND_CHECK(f.loaded);
    }
    if (frame == 4000) SND_CHECK_EQ(snd_physics_set_single_event(f.phys, 0, SND_CONTACT_IMPACT, 0xDEAD01u), SND_OK);
    if (frame == 5000) SND_CHECK_EQ(snd_physics_set_single_event(f.phys, 0, SND_CONTACT_IMPACT, kDc), SND_OK);
    std::vector<snd_contact> cs;
    const std::uint32_t n = rng.Below(4);
    for (std::uint32_t i = 0; i < n; ++i, obj += 2) cs.push_back(Hit(obj, rng.Unit() * 4.0f - 2.0f));
    SND_CHECK_EQ(f.Submit(cs, 1.0f / 60.0f), SND_OK);
    if (rng.Below(4) != 0) f.s.Advance(1);  // a quarter of the frames: no snd_update
    if (frame % 500 == 499) {
      for (int q = 0; q < 8; ++q) {
        f.s.Advance(1);
        SND_CHECK_EQ(f.Submit({}, 1.0f / 60.0f), SND_OK);
      }
      f.s.Advance(2);
      ++quietChecks;
      const std::uint32_t got = f.FreeEmitters(8);
      if (got == 8u) ++quietOk;
      else snd::test::Note("frame %d: only %u of 8 emitters free", frame, got);
      f.s.Advance(2);
    }
  }
  SND_CHECK_EQ(quietOk, quietChecks);
  const snd_physics_stats st = phystest::Stats(f.phys);
  snd::test::Note("long run: requests %llu, no emitter %llu, api failures %llu",
                  static_cast<unsigned long long>(st.group_requests),
                  static_cast<unsigned long long>(st.dropped_no_emitter),
                  static_cast<unsigned long long>(st.api_failures));
  snd_physics_destroy(f.phys);
  f.phys = nullptr;
  f.s.Advance(4);
  SND_CHECK_EQ(f.FreeEmitters(8), 8u);
}

// snd_suspend: while suspended the plays are not applied, so the emitters stay held (no
// unsafe release); the game keeps recording; after snd_resume they come back.
SND_TEST(IndepRecheck, SuspendHoldsThenResumeReleases) {
  snd_config sc = phystest::SmallConfig();
  sc.max_emitters = 16;
  Fx f(kDc, Open(), sc);
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  SND_CHECK_EQ(snd_suspend(f.s.sys), SND_OK);
  std::vector<snd_contact> cs;
  for (std::uint64_t i = 0; i < 8; ++i) cs.push_back(Hit(i * 2, 2.0f));
  SND_CHECK_EQ(f.Submit(cs), SND_OK);
  for (int i = 0; i < 300; ++i) {
    snd_update(f.s.sys);
    SND_CHECK_EQ(f.Submit({}), SND_OK);
  }
  // The game can still record its own commands.
  SND_CHECK_EQ(snd_set_param_global(f.s.sys, 0x4444u, 1.0f), SND_OK);
  snd_stats ss{};
  snd_get_stats(f.s.sys, &ss);
  snd::test::Note("suspended 300 frames with 8 held: rejected_commands %u, reserve_overflows %u, fallback %u",
                  ss.rejected_commands, ss.reserve_overflows, ss.fallback_commands);
  SND_CHECK_EQ(ss.rejected_commands, 0u);
  SND_CHECK_EQ(snd_resume(f.s.sys), SND_OK);
  for (int i = 0; i < 10; ++i) {
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({}), SND_OK);
  }
  f.s.Advance(2);
  SND_CHECK_EQ(f.FreeEmitters(16), 16u);
  SND_CHECK_EQ(phystest::Stats(f.phys).dropped_no_emitter, 0u);
}

// Tracking order (decision): (1) reports, (2) timeouts, (3) new keys; eviction only when
// the table is full of entries that did not time out; new keys beyond the new-key area
// (max_tracked_pairs) are dropped_work_overflow; a table full of entries reported in this
// call drops the new key (tracking_dropped).
SND_TEST(IndepRecheck, TrackingOrderAndNewKeyArea) {
  snd_physics_config c = Cfg(nullptr);
  c.max_tracked_pairs = 2;
  c.tracking_timeout_seconds = 0.1f;
  Fx f(kDc, c);
  SND_REQUIRE(f.Ok());
  auto slide = [](std::uint64_t a) { return Contact(SND_CONTACT_SLIDE, a, a + 1000u, 0, 0, 0, 0, 1, 0, 1.0f); };
  SND_CHECK_EQ(f.Submit({slide(1)}, 0.05f), SND_OK);  // A at 50 ms
  SND_CHECK_EQ(f.Submit({slide(2)}, 0.05f), SND_OK);  // B at 100 ms
  // 150 ms: A timed out (100 ms), B not (50 ms). New C takes A's slot: no eviction.
  SND_CHECK_EQ(f.Submit({slide(3)}, 0.05f), SND_OK);
  snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.tracking_timeouts, 1u);
  SND_CHECK_EQ(st.tracking_evictions, 0u);
  SND_CHECK_EQ(st.tracking_started, 3u);
  // 160 ms: B (60 ms) and C (10 ms) alive, D new -> evicts B (the oldest report).
  SND_CHECK_EQ(f.Submit({slide(4)}, 0.01f), SND_OK);
  st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.tracking_evictions, 1u);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 1u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_STOP)[0].r.object_a, 2u);
  // C and D reported, E new: no room -> tracking_dropped; F and G beyond the new-key area
  // of 2 (E, F fit the area; G overflows).
  SND_CHECK_EQ(f.Submit({slide(3), slide(4), slide(5), slide(6), slide(7)}, 0.01f), SND_OK);
  st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.tracking_dropped, 2u);
  SND_CHECK_EQ(st.dropped_work_overflow, 1u);
  SND_CHECK_EQ(st.tracking_updated, 2u);
  SND_CHECK_EQ(st.slide_reports, st.tracking_started + st.tracking_updated + st.tracking_restarted + st.tracking_dropped +
                                     st.fallback_none);
  SND_CHECK_EQ(st.contacts_received, st.dropped_invalid + st.dropped_threshold + st.dropped_work_overflow + st.merged +
                                         st.groups + st.slide_reports);
}

// A burst of tracked-key reports beyond any capacity (each key many times in one call) does
// not touch the work areas: merged, no overflow.
SND_TEST(IndepRecheck, RepeatedReportsOfTrackedKeysNeedNoArea) {
  snd_physics_config c = Cfg(nullptr);
  c.max_tracked_pairs = 2;
  c.max_groups_per_call = 1;
  Fx f(kDc, c);
  SND_REQUIRE(f.Ok());
  auto slide = [](std::uint64_t a, float v) { return Contact(SND_CONTACT_SLIDE, a, a + 1000u, 0, 0, 0, 0, 1, 0, v); };
  SND_CHECK_EQ(f.Submit({slide(1, 1.0f), slide(2, 1.0f)}), SND_OK);
  std::vector<snd_contact> cs;
  for (int i = 0; i < 500; ++i) cs.push_back(slide(static_cast<std::uint64_t>(1 + i % 2), 1.0f + static_cast<float>(i)));
  for (int i = 0; i < 5; ++i) cs.push_back(Hit(static_cast<std::uint64_t>(50 + 2 * i), static_cast<float>(i)));
  SND_CHECK_EQ(f.Submit(cs), SND_OK);
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.tracking_updated, 2u);
  SND_CHECK_EQ(st.merged, 498u);
  SND_CHECK_EQ(st.dropped_work_overflow, 4u);  // the impacts beyond the 1-group work area
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_UPDATE)) SND_CHECK(r.r.speed >= 499.0f);  // the largest of each key
}

namespace {

// A bank whose 3D events start their voice late: a clip at 100 ms, a streamed asset (the
// playback primes first), a nested event behind a 100 ms action.
struct LateBank {
  std::vector<std::uint8_t> bank;
  std::string streamName;
  std::vector<std::uint8_t> stream;
};
const LateBank& BuildLateBank() {
  static LateBank lb;
  if (!lb.bank.empty()) return lb;
  mini::Project proj("phys-indep-late");
  proj.Add("banks/Late.json", "bank", "Late");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true, \"channels\": 2");
  snd::buildtool::PcmData dc;
  dc.channels = 1;
  dc.sampleRate = 48000;
  dc.frames = 24000;
  dc.samples.assign(24000, 0.5f);
  proj.Wav("audio/dc.wav", dc, snd::buildtool::WavSampleFormat::Float32);
  proj.Add("assets/ldc.json", "asset", "ldc", "\"file\": \"audio/dc.wav\", \"convert\": {\"default\": {\"codec\": \"PCMF\"}}");
  proj.Add("assets/sdc.json", "asset", "sdc",
           "\"file\": \"audio/dc.wav\", \"loadForm\": \"stream\", \"convert\": {\"default\": {\"codec\": \"PCMF\"}}");
  proj.Add("events/LateClip.json", "event", "L/LateClip",
           "\"bank\": \"Late\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"ldc\", \"startMs\": 100}]}]");
  proj.Add("events/Stream.json", "event", "L/Stream",
           "\"bank\": \"Late\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"sdc\"}]}]");
  proj.Add("events/Inner.json", "event", "L/Inner",
           "\"bank\": \"Late\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"ldc\"}]}]");
  proj.Add("events/Outer.json", "event", "L/Outer",
           "\"bank\": \"Late\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"ldc\", \"volumeDb\": -120}], "
           "\"actions\": [{\"timeMs\": 100, \"action\": \"play\", \"target\": \"L/Inner\"}]}]");
  snd::buildtool::Diagnostics diag;
  snd::buildtool::BuildResult res;
  if (!proj.Build(&diag, &res)) {
    snd::test::Note("late bank build failed:\n%s", mini::Dump(diag).c_str());
    return lb;
  }
  lb.bank = mini::ReadBytes(proj.out() / "Late.sndb");
  if (!res.streams.empty()) {
    lb.streamName = res.streams[0];
    lb.stream = mini::ReadBytes(proj.out() / res.streams[0]);
  }
  return lb;
}

}  // namespace

namespace {
void CheckHitPositioned(const LateBank& lb, const char* name, double* outL = nullptr, double* outR = nullptr);
}  // namespace

// The release rule assumes that a playback state other than NONE means its 3D values are
// computed. Events whose voice starts later than the play (a clip at 100 ms, a streamed
// asset, an action that plays a nested event at 100 ms) must still sound at the hit's
// position after physics has released the emitter.
SND_TEST(IndepRecheck, LateStartingVoicesKeepThePosition) {
  const LateBank& lb = BuildLateBank();
  SND_REQUIRE(!lb.bank.empty());
  const char* names[] = {"L/LateClip", "L/Stream"};
  for (const char* name : names) {
    CheckHitPositioned(lb, name);
  }
}

// Record of a core limit (A-004 §5; physics.h "known limits"; acceptance decision: core
// side, the physics rule stays): an event whose action plays a nested event 100 ms after the
// play. Physics releases the emitter once the play is applied (~3 frames); the nested
// playback starts afterwards. Passes when the hit is heard either
//   (a) on the right (core fixed: the child keeps the parent's transform), or
//   (b) at the listener, centred (the known limit),
// and says which. Anything else fails. (Was Defect_ReleasedEmitterLosesNestedActionPlay.)
SND_TEST(IndepRecheck, CoreA004_NestedActionPlayAfterEmitterRelease) {
  const LateBank& lb = BuildLateBank();
  SND_REQUIRE(!lb.bank.empty());
  double l = 0, r = 0;
  CheckHitPositioned(lb, "L/Outer", &l, &r);
  const bool fixed = r > 1.0 && l < 1e-3 * r;
  const bool knownLimit = r > 1.0 && std::fabs(l - r) < 0.01 * r;
  snd::test::Note("core A-004 section 5: %s (|L| %.2f |R| %.2f)",
                  fixed ? "(a) fixed: heard on the right" : knownLimit ? "(b) known limit: at the listener" : "neither", l, r);
  SND_CHECK(fixed || knownLimit);
}

namespace {
void CheckHitPositioned(const LateBank& lb, const char* name, double* outL, double* outR) {
  {
    Sys s;
    if (!lb.stream.empty()) s.fs.AddFile(lb.streamName, lb.stream);
    snd_bank_handle bank{};
    SND_REQUIRE(s.LoadBank("late.sndb", lb.bank, &bank));
    SetListener(s.sys);
    Recorder rec;
    snd_physics_config c = Open();
    c.observer = &Recorder::Fn;
    c.observer_user = &rec;
    snd_phys* p = nullptr;
    SND_REQUIRE_EQ(snd_physics_create(s.sys, &c, nullptr, &p), SND_OK);
    std::vector<snd_id> single(4, SND_ID_NONE);
    single[SND_CONTACT_IMPACT] = snd_hash_id(name);
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = 1;
    d.single = single.data();
    SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
    s.Advance(2);
    const snd_contact x = Hit(1, 2.0f);
    SND_CHECK_EQ(snd_phys_submit_contacts(p, &x, 1, 1.0f / 60.0f), SND_OK);
    const std::uint64_t from = s.Frames();
    std::uint32_t releasedAt = 0;
    for (int frame = 0; frame < 120; ++frame) {
      s.Advance(1);
      SND_CHECK_EQ(snd_phys_submit_contacts(p, nullptr, 0, 1.0f / 60.0f), SND_OK);
      if (releasedAt == 0u && s.Frames() > 0) {
        // The game can create max_emitters (128) again once physics released its one.
        std::vector<snd_emitter_handle> mine;
        for (int i = 0; i < 128; ++i) mine.push_back(snd_create_emitter(s.sys));
        bool all = true;
        for (const snd_emitter_handle e : mine) {
          all = all && e.value != 0u;
          if (e.value != 0u) snd_destroy_emitter(s.sys, e);
        }
        if (all) releasedAt = static_cast<std::uint32_t>(frame + 1);
      }
    }
    double l = 0, r = 0;
    Energy(s, from, &l, &r);
    snd::test::Note("%s: emitter back after %u frames (%u samples of audio), |L| %.2f |R| %.2f", name, releasedAt,
                    releasedAt * 256u, l, r);
    snd_physics_destroy(p);
    if (outL != nullptr) {  // the caller judges
      *outL = l;
      *outR = r;
      return;
    }
    SND_CHECK_MSG(r > 1.0, "%s: nothing heard", name);
    SND_CHECK_MSG(l < 1e-3 * r, "%s: |L| %.2f against |R| %.2f: heard off its position", name, l, r);
  }
}
}  // namespace

// Core (outside physics), public API only: an event whose action plays a nested event at
// 100 ms. variant 0: the game keeps the emitter; variant 1: destroys it 3 frames after the
// play (after the play was applied, like physics' release). sonus.h: "Playbacks on the
// emitter keep the last transform" -- the nested playback started by the action after the
// destroy: recorded here (which result core gives).
SND_TEST(IndepRecheck, CoreNestedPlayAfterEmitterDestroyed) {
  const LateBank& lb = BuildLateBank();
  SND_REQUIRE(!lb.bank.empty());
  double res[2][2] = {};
  for (int variant = 0; variant < 2; ++variant) {
    Sys s;
    snd_bank_handle bank{};
    SND_REQUIRE(s.LoadBank("late.sndb", lb.bank, &bank));
    SetListener(s.sys);
    s.Advance(2);
    const snd_emitter_handle e = snd_create_emitter(s.sys);
    const snd_vec3 right{2.0f, 0.0f, 0.0f};
    SND_CHECK_EQ(snd_set_transform(s.sys, e, &right, nullptr, nullptr), SND_OK);
    const snd_playback_handle h = snd_play(s.sys, snd_hash_id("L/Outer"), e, nullptr);
    SND_REQUIRE(h.value != 0u);
    const std::uint64_t from = s.Frames();
    s.Advance(3);
    SND_CHECK(snd_get_playback_state(s.sys, h) != SND_PLAYBACK_STATE_NONE);
    if (variant == 1) SND_CHECK_EQ(snd_destroy_emitter(s.sys, e), SND_OK);
    s.Advance(80);
    Energy(s, from, &res[variant][0], &res[variant][1]);
    snd::test::Note("nested play at 100 ms, %s: |L| %.2f |R| %.2f", variant == 0 ? "emitter kept" : "emitter destroyed at 3 frames",
                    res[variant][0], res[variant][1]);
  }
  SND_CHECK(res[0][1] > 1.0 && res[0][0] < 1e-3 * res[0][1]);
}
