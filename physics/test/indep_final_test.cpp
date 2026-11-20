// Independent tests of the last round (report-A.md section 6 "not checked" items and the
// lines the tests did not reach): the volume of one-shots belongs to physics (D-119), the
// release check does not change the sound (sample by sample), a long stall of the audio side,
// child playbacks that start late (music segments, stingers), resource exhaustion paths and
// the C++ wrapper. Expectations come from physics.h and CONTRACTS.md D-110..D-124.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/ext/allocator.hpp"
#include "snd/test/test.hpp"

using namespace indepphys;

namespace {

const snd_id kDc = snd_hash_id("I/Dc");
const snd_id kLoop = snd_hash_id("I/Loop");
const snd_id kTone = snd_hash_id("F/Tone");
const snd_id kTone2 = snd_hash_id("F/Tone2");
const snd_id kMusic = snd_hash_id("F/Music");
const snd_id kMusicQuiet = snd_hash_id("F/MusicQuiet");

// A bank of 3D sine tones and music events (960 BPM 4/4: one bar = 0.25 s = 12000 frames).
//   F/Tone, F/Tone2      3D, sine 0.5 at 440 / 660 Hz, 0.5 s
//   F/Music              3D music event: playlist [Seg, Seg] (DC 0.5, one bar each)
//   F/MusicQuiet         3D music event: playlist [SegQ] (silence, 4 bars), stinger "Hit" -> F/St
//   F/St                 3D stinger event, DC 0.5, 0.25 s
const std::vector<std::uint8_t>& FinalBank() {
  static std::vector<std::uint8_t> bytes;
  if (!bytes.empty()) return bytes;
  mini::Project proj("phys-indep-final");
  proj.Add("banks/F.json", "bank", "F");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true, \"channels\": 2");
  auto tone = [](double hz) {
    snd::buildtool::PcmData p;
    p.channels = 1;
    p.sampleRate = 48000;
    p.frames = 24000;
    p.samples.resize(24000);
    for (std::size_t f = 0; f < 24000; ++f) {
      p.samples[f] = static_cast<float>(0.5 * std::sin(6.283185307179586 * hz * static_cast<double>(f) / 48000.0));
    }
    return p;
  };
  auto dc = [](std::uint64_t frames, float v) {
    snd::buildtool::PcmData p;
    p.channels = 1;
    p.sampleRate = 48000;
    p.frames = frames;
    p.samples.assign(static_cast<std::size_t>(frames), v);
    return p;
  };
  const std::string pcmf = "\"convert\": {\"default\": {\"codec\": \"PCMF\"}}";
  proj.Wav("audio/tone.wav", tone(440.0), snd::buildtool::WavSampleFormat::Float32);
  proj.Wav("audio/tone2.wav", tone(660.0), snd::buildtool::WavSampleFormat::Float32);
  proj.Wav("audio/seg.wav", dc(12000, 0.5f), snd::buildtool::WavSampleFormat::Float32);
  proj.Wav("audio/segq.wav", dc(48000, 0.0f), snd::buildtool::WavSampleFormat::Float32);
  proj.Wav("audio/st.wav", dc(12000, 0.5f), snd::buildtool::WavSampleFormat::Float32);
  proj.Add("assets/tone.json", "asset", "tone", "\"file\": \"audio/tone.wav\", " + pcmf);
  proj.Add("assets/tone2.json", "asset", "tone2", "\"file\": \"audio/tone2.wav\", " + pcmf);
  proj.Add("assets/seg.json", "asset", "seg", "\"file\": \"audio/seg.wav\", " + pcmf);
  proj.Add("assets/segq.json", "asset", "segq", "\"file\": \"audio/segq.wav\", " + pcmf);
  proj.Add("assets/st.json", "asset", "st", "\"file\": \"audio/st.wav\", " + pcmf);
  proj.Add("events/Tone.json", "event", "F/Tone", "\"bank\": \"F\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"tone\"}]}]");
  proj.Add("events/Tone2.json", "event", "F/Tone2", "\"bank\": \"F\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"tone2\"}]}]");
  proj.Add("events/St.json", "event", "F/St",
           "\"bank\": \"F\", \"is3D\": true, \"stinger\": true, \"tracks\": [{\"clips\": [{\"asset\": \"st\"}]}]");
  const std::string tempo = "\"tempo\": [{\"tick\": 0, \"bpm\": 960, \"beatsPerBar\": 4, \"beatUnit\": 4}]";
  proj.Add("music/Seg.json", "segment", "Seg", "\"asset\": \"seg\", \"bars\": 1, " + tempo);
  proj.Add("music/SegQ.json", "segment", "SegQ", "\"asset\": \"segq\", \"bars\": 4, " + tempo);
  proj.Add("music/PL.json", "playlist", "PL", "\"entries\": [{\"segment\": \"Seg\"}, {\"segment\": \"Seg\"}], \"loopCount\": 1");
  proj.Add("music/PLQ.json", "playlist", "PLQ", "\"entries\": [{\"segment\": \"SegQ\"}], \"loopCount\": 1");
  proj.Add("events/Music.json", "event", "F/Music",
           "\"bank\": \"F\", \"is3D\": true, \"music\": {\"initialState\": \"S\", \"states\": [{\"name\": \"S\", "
           "\"playlist\": \"PL\"}]}");
  proj.Add("events/MusicQ.json", "event", "F/MusicQuiet",
           "\"bank\": \"F\", \"is3D\": true, \"music\": {\"initialState\": \"S\", \"states\": [{\"name\": \"S\", "
           "\"playlist\": \"PLQ\"}], \"stingers\": [{\"name\": \"Hit\", \"event\": \"F/St\", \"timing\": \"immediate\"}]}");
  snd::buildtool::Diagnostics diag;
  snd::buildtool::BuildResult res;
  if (!proj.Build(&diag, &res)) {
    snd::test::Note("final bank build failed:\n%s", mini::Dump(diag).c_str());
    return bytes;
  }
  bytes = mini::ReadBytes(proj.out() / "F.sndb");
  return bytes;
}

// System + test bank + final bank + physics with a one- or two-material table.
struct Fx {
  explicit Fx(snd_physics_config c = OpenCfg(), snd_config sc = phystest::SmallConfig()) : s(sc) {
    snd_bank_handle b{};
    loaded = s.LoadBank("phys.sndb", TestBank(), &b) && s.LoadBank("final.sndb", FinalBank(), &bank);
    SetListener(s.sys);
    c.observer = &Recorder::Fn;
    c.observer_user = &rec;
    created = snd_physics_create(s.sys, &c, nullptr, &phys);
  }
  ~Fx() { snd_physics_destroy(phys); }
  static snd_physics_config OpenCfg() {
    snd_physics_config c = Cfg(nullptr);
    c.cooldown_seconds = 0.0f;
    c.budget_per_second = 1.0e6f;
    return c;
  }
  // Material m's impact event = impact[m], slide event = slide[m].
  snd_result Table(std::vector<snd_id> impact, std::vector<snd_id> slide = {}) {
    const std::uint32_t n = static_cast<std::uint32_t>(impact.size());
    single.assign(std::size_t{n} * 4u, SND_ID_NONE);
    for (std::uint32_t m = 0; m < n; ++m) {
      single[m * 4u + SND_CONTACT_IMPACT] = impact[m];
      if (m < slide.size()) {
        single[m * 4u + SND_CONTACT_SLIDE] = slide[m];
        single[m * 4u + SND_CONTACT_ROLL] = slide[m];
      }
    }
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = n;
    d.single = single.data();
    return snd_physics_set_table(phys, &d);
  }
  bool Ok() const { return loaded && created == SND_OK; }
  snd_result Submit(std::vector<snd_contact> cs, float dt = 1.0f / 60.0f) {
    rec.Clear();
    return snd_phys_submit_contacts(phys, cs.empty() ? nullptr : cs.data(), static_cast<std::uint32_t>(cs.size()), dt);
  }
  // Emitters the game can create right now (all destroyed again; two periods let core apply
  // the destroys so the slots are free again afterwards).
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
    s.Advance(2);
    return ok;
  }
  // Frames until the game can create `upTo` emitters again (advancing and submitting empty).
  int FramesUntilEmittersBack(std::uint32_t upTo, int limit = 60) {
    for (int i = 0; i < limit; ++i) {
      if (FreeEmitters(upTo) == upTo) return i;
      s.Advance(1);
      Submit({});
    }
    return -1;
  }
  Sys s;
  Recorder rec;
  snd_bank_handle bank{};
  bool loaded = false;
  snd_phys* phys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  std::vector<snd_id> single;
};

snd_contact Hit(std::uint64_t obj, float x, std::uint16_t ma = 0, std::uint16_t mb = 0, float impulse = 5.0f,
                float mass = 1.0f) {
  return Contact(SND_CONTACT_IMPACT, obj, obj + 100000u, ma, mb, x, 0.0f, 0.0f, impulse, 1.0f, mass, mass);
}
snd_contact Slide(std::uint64_t a, float x = 2.0f, std::uint16_t ma = 0, std::uint16_t mb = 0, float speed = 1.0f) {
  return Contact(SND_CONTACT_SLIDE, a, a + 1000u, ma, mb, x, 0.0f, 0.0f, 0.0f, speed);
}

// Mean of R over the frames captured during the next `periods` periods.
double MeanR(Sys& s, std::uint32_t periods) {
  const std::uint64_t from = s.Frames();
  s.Advance(periods);
  double r = 0;
  for (std::uint64_t f = from; f < s.Frames(); ++f) r += s.R(f);
  return r / static_cast<double>(s.Frames() - from);
}
void Energy(const Sys& s, std::uint64_t from, std::uint64_t to, double* l, double* r) {
  *l = 0;
  *r = 0;
  for (std::uint64_t f = from; f < to && f < s.Frames(); ++f) {
    *l += std::fabs(s.L(f));
    *r += std::fabs(s.R(f));
  }
}
std::uint64_t Onset(const Sys& s, std::uint64_t from) {
  for (std::uint64_t f = from; f < s.Frames(); ++f) {
    if (s.L(f) != 0.0f || s.R(f) != 0.0f) return f;
  }
  return s.Frames();
}

}  // namespace

// ---------------------------------------------------------------------------------------
// (a) D-119: the volume and pitch of a one-shot belong to physics.
// ---------------------------------------------------------------------------------------

// A game volume set on the observed handle while the emitter is held may be overwritten by
// the release check ("上書きしうる"): the level heard is either physics' (0.5) or the game's
// (1.0); the test says which. After the emitter is returned, a game volume stays.
SND_TEST(IndepFinal, VolumeOwnershipWhileHeldAndAfterRelease) {
  Fx f;
  SND_REQUIRE(f.Ok());
  SND_REQUIRE_EQ(f.Table({kDc}), SND_OK);
  snd_physics_curves cv{};
  cv.impulse_to_volume = phystest::Linear({{0.0f, 0.5f}});
  SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 0, &cv), SND_OK);
  f.s.Advance(2);
  // Held: the game changes the volume before the play is applied, then a submit runs the
  // release check.
  SND_CHECK_EQ(f.Submit({Hit(1, 2.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  const snd_playback_handle h = f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.playback;
  SND_REQUIRE(h.value != 0u);
  SND_CHECK_EQ(snd_set_playback_volume(f.s.sys, h, 1.0f), SND_OK);
  SND_CHECK_EQ(f.Submit({}), SND_OK);
  f.s.Advance(4);
  const double held = MeanR(f.s, 4);  // DC 0.5 at 2 m: 0.25 x volume
  const bool physicsWon = std::fabs(held - 0.125) < 0.005;
  const bool gameWon = std::fabs(held - 0.25) < 0.005;
  snd::test::Note("game volume 1 set while held: heard %.4f (%s)", held,
                  physicsWon ? "physics' 0.5 was recorded again" : gameWon ? "the game's 1 stayed" : "neither");
  SND_CHECK(physicsWon || gameWon);
  // Returned: wait until physics gave the emitter back, then the game's volume stays even
  // across more submits.
  SND_CHECK(f.FramesUntilEmittersBack(128) >= 0);  // 128 in the rig: physics' one is back
  SND_CHECK_EQ(snd_set_playback_volume(f.s.sys, h, 0.2f), SND_OK);
  f.s.Advance(8);  // past the render-ahead and the 1-block ramp
  for (int i = 0; i < 5; ++i) SND_CHECK_EQ(f.Submit({}), SND_OK);
  const double after = MeanR(f.s, 4);
  snd::test::Note("game volume 0.2 after the release: heard %.4f", after);
  SND_CHECK_NEAR(after, 0.25 * 0.2, 0.003);
}

// Using the handle to stop a held one-shot and to query its state works; the emitter of a
// stopped playback is returned.
SND_TEST(IndepFinal, StopAndStateOfAHeldOneShot) {
  snd_config sc = phystest::SmallConfig();
  sc.max_emitters = 4;
  Fx f(Fx::OpenCfg(), sc);
  SND_REQUIRE(f.Ok());
  SND_REQUIRE_EQ(f.Table({kLoop}), SND_OK);  // a looping one-shot: only a stop ends it
  f.s.Advance(2);
  SND_CHECK_EQ(f.Submit({Hit(1, 2.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  const snd_playback_handle h = f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.playback;
  SND_CHECK_EQ(snd_get_playback_state(f.s.sys, h), SND_PLAYBACK_STATE_NONE);  // not applied yet
  f.s.Advance(3);
  SND_CHECK_EQ(snd_get_playback_state(f.s.sys, h), SND_PLAYBACK_STATE_PLAYING);
  SND_CHECK_EQ(snd_stop(f.s.sys, h, 0.0f, nullptr), SND_OK);
  f.s.Advance(4);
  const snd_playback_state st = snd_get_playback_state(f.s.sys, h);
  SND_CHECK(st == SND_PLAYBACK_STATE_ENDED || st == SND_PLAYBACK_STATE_NONE);
  SND_CHECK(f.FramesUntilEmittersBack(4) >= 0);
  // Stopped before it was ever applied: the emitter comes back too.
  SND_CHECK_EQ(f.Submit({Hit(3, 2.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  const snd_playback_handle h2 = f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.playback;
  SND_CHECK_EQ(snd_stop(f.s.sys, h2, 0.0f, nullptr), SND_OK);
  SND_CHECK(f.FramesUntilEmittersBack(4) >= 0);
  f.s.Advance(4);
  const snd_playback_state st2 = snd_get_playback_state(f.s.sys, h2);
  SND_CHECK(st2 == SND_PLAYBACK_STATE_ENDED || st2 == SND_PLAYBACK_STATE_NONE);
}

// ---------------------------------------------------------------------------------------
// (b) The release check (the same volume recorded again) does not change the sound: the
// same input with the check run many times before each snd_update gives the same samples
// as with one submit per update (aligned on the onset, compared bit for bit).
// ---------------------------------------------------------------------------------------
namespace {
std::vector<float> RunHits(int extraSubmits, int variant) {
  Fx f;
  if (!f.Ok()) return {};
  if (variant == 2) {
    f.Table({kTone, kTone2});
  } else {
    f.Table({kTone});
  }
  snd_physics_curves cv[2] = {};
  cv[0].impulse_to_volume = phystest::Linear({{0.0f, 0.5f}});
  cv[1].impulse_to_volume = phystest::Linear({{0.0f, 0.8f}});
  if (variant == 1) cv[0].mass_to_pitch = phystest::Linear({{0.0f, 1.5f}});
  snd_physics_set_curves(f.phys, 0, &cv[0]);
  if (variant == 2) snd_physics_set_curves(f.phys, 1, &cv[1]);
  f.s.Advance(2);
  const std::uint64_t from = f.s.Frames();
  f.Submit({variant == 2 ? Hit(1, 2.0f, 0, 1) : Hit(1, 2.0f)});
  for (int frame = 0; frame < 110; ++frame) {
    for (int i = 0; i < extraSubmits; ++i) f.Submit({}, 0.0f);
    f.s.Advance(1);
    f.Submit({}, 0.0f);
  }
  const std::uint64_t on = Onset(f.s, from);
  std::vector<float> v;
  for (std::uint64_t i = on; i < on + 20000u && i < f.s.Frames(); ++i) {
    v.push_back(f.s.L(i));
    v.push_back(f.s.R(i));
  }
  return v;
}
}  // namespace

SND_TEST(IndepFinal, ReleaseCheckLeavesTheSamplesUnchanged) {
  const char* what[3] = {"volume 0.5", "pitch 1.5", "two voices (0.5, 0.8)"};
  for (int variant = 0; variant < 3; ++variant) {
    const std::vector<float> once = RunHits(0, variant);
    const std::vector<float> many = RunHits(12, variant);
    SND_REQUIRE(once.size() == 40000u);
    SND_REQUIRE_EQ(many.size(), once.size());
    std::size_t diff = 0, first = once.size();
    for (std::size_t i = 0; i < once.size(); ++i) {
      if (std::memcmp(&once[i], &many[i], sizeof(float)) != 0) {
        if (diff == 0u) first = i;
        ++diff;
      }
    }
    double energy = 0;
    for (float x : once) energy += std::fabs(x);
    snd::test::Note("%s: %llu of %llu samples differ (first %llu), energy %.1f", what[variant],
                    static_cast<unsigned long long>(diff), static_cast<unsigned long long>(once.size()),
                    static_cast<unsigned long long>(first), energy);
    SND_CHECK(energy > 100.0);
    SND_CHECK_EQ(diff, 0u);
  }
}

// ---------------------------------------------------------------------------------------
// (c) The audio side stopped for a long time (thousands of frames).
// ---------------------------------------------------------------------------------------

// Output not advanced for 4000 frames (snd_update and the submits go on): the hold ring (64)
// fills and stays full, every later group is dropped (none is played without its position),
// the tracked slide is updated, the one not reported times out; when the output runs again
// the held emitters are all released and new hits play on the right.
SND_TEST(IndepFinal, LongStallWithoutAdvanceKeepsDroppingThenRecovers) {
  Fx f;
  SND_REQUIRE(f.Ok());
  SND_REQUIRE_EQ(f.Table({kDc}, {kLoop}), SND_OK);
  f.s.Advance(2);
  std::uint64_t obj = 10;
  for (int frame = 0; frame < 4000; ++frame) {
    std::vector<snd_contact> cs = {Slide(1, -2.0f)};
    if (frame == 0) cs.push_back(Slide(2, -3.0f));
    for (int i = 0; i < 2; ++i, obj += 2) cs.push_back(Hit(obj, 2.0f + 0.6f * static_cast<float>(i)));
    SND_CHECK_EQ(f.Submit(cs), SND_OK);
    snd_update(f.s.sys);
  }
  snd_stats ss{};
  SND_CHECK_EQ(snd_get_stats(f.s.sys, &ss), SND_OK);
  snd_physics_stats st = phystest::Stats(f.phys);
  snd::test::Note("not advanced 4000 frames: requests %llu, no emitter %llu, api failures %llu, core rejected %u, "
                  "reserve overflows %u, fallback %u (overflow: see Defect_LongSuspendWithPhysicsCannotResume)",
                  static_cast<unsigned long long>(st.group_requests),
                  static_cast<unsigned long long>(st.dropped_no_emitter), static_cast<unsigned long long>(st.api_failures),
                  ss.rejected_commands, ss.reserve_overflows, ss.fallback_commands);
  SND_CHECK_EQ(st.group_requests, 64u);
  SND_CHECK_EQ(st.dropped_no_emitter, 2u * 4000u - 64u);
  SND_CHECK_EQ(st.tracking_timeouts, 1u);  // slide 2
  SND_CHECK_EQ(st.tracking_updated + st.tracking_restarted, 3999u);
  // The output runs again.
  const std::uint64_t from = f.s.Frames();
  int frames = 0;
  for (; frames < 60 && phystest::Stats(f.phys).group_requests == 64u; ++frames) {
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({Hit(9000000, 2.0f)}), SND_OK);
  }
  SND_CHECK(frames < 60);
  snd::test::Note("after the stall the first new hit played after %d frames", frames);
  for (int i = 0; i < 80; ++i) {
    SND_CHECK_EQ(f.Submit({}), SND_OK);
    f.s.Advance(1);
  }
  st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.tracking_timeouts, 2u);  // slide 1 too, once not reported
  double l = 0, r = 0;
  Energy(f.s, f.s.Frames() - 1024, f.s.Frames(), &l, &r);
  SND_CHECK(l + r < 1e-3);  // everything stopped
  SND_CHECK_EQ(f.FreeEmitters(128), 128u);
  (void)from;
}

// Defect (requested property of report-A.md 6 / this round: "コマンドの領域が溢れないこと、
// 再開後に保持が全部解けて新しい衝突が正しい位置で鳴ること"): snd_suspend, then the game keeps
// calling snd_update and snd_phys_submit_contacts every frame (physics.h: "Call
// snd_phys_submit_contacts every game frame, also while the game is paused"), with one hit a
// frame, for 3000 frames. Expected: snd_resume succeeds and new hits play on the right.
// Actual: the release check records snd_set_playback_volume on every held, not yet applied
// playback at every call (D-119), i.e. up to max_held_emitters commands a frame; while
// suspended nothing consumes them, the command reserve overflows and snd_resume itself is
// refused with SND_ERR_COMMAND_BUFFER_FULL - for good (retrying with snd_update does not
// help). Core shares the blame (Defect_CoreResumeRefusedForeverAfterRecordingWhileSuspended).
SND_TEST(IndepFinal, Defect_LongSuspendWithPhysicsCannotResume) {
  Fx f;
  SND_REQUIRE(f.Ok());
  SND_REQUIRE_EQ(f.Table({kDc}), SND_OK);
  f.s.Advance(2);
  SND_CHECK_EQ(snd_suspend(f.s.sys), SND_OK);
  std::uint64_t obj = 0;
  for (int frame = 0; frame < 3000; ++frame, obj += 2) {
    SND_CHECK_EQ(f.Submit({Hit(obj, 2.0f + 0.6f * static_cast<float>(frame % 8))}), SND_OK);
    snd_update(f.s.sys);
  }
  snd_stats ss{};
  SND_CHECK_EQ(snd_get_stats(f.s.sys, &ss), SND_OK);
  snd::test::Note("3000 suspended frames, 1 hit a frame: core rejected %u, reserve overflows %u, fallback %u",
                  ss.rejected_commands, ss.reserve_overflows, ss.fallback_commands);
  SND_CHECK_EQ(ss.rejected_commands, 0u);
  SND_CHECK_EQ(ss.reserve_overflows, 0u);
  snd_result rr = snd_resume(f.s.sys);
  for (int i = 0; i < 100 && rr != SND_OK; ++i) {
    snd_update(f.s.sys);
    rr = snd_resume(f.s.sys);
  }
  SND_CHECK_EQ(rr, SND_OK);
  const std::uint64_t from = f.s.Frames();
  for (int frame = 0; frame < 60; ++frame, obj += 2) {
    SND_CHECK_EQ(f.Submit({Hit(obj, 2.0f)}), SND_OK);
    f.s.Advance(1);
  }
  f.s.Advance(60);
  double l = 0, r = 0;
  Energy(f.s, from, f.s.Frames(), &l, &r);
  snd::test::Note("after the resume: |L| %.3f |R| %.3f", l, r);
  SND_CHECK(r > 1.0);
  SND_CHECK_EQ(l, 0.0);
}

// Core (outside physics), public API only: snd_suspend, then 64 snd_set_param_global a frame
// with snd_update for 300 frames (a game that keeps updating its parameters while
// suspended). Expected: snd_resume succeeds (sonus.h: "Recorded like other commands"; nothing
// says a resume can be refused for good). Actual: SND_ERR_COMMAND_BUFFER_FULL, and retrying
// after snd_update never succeeds: nothing consumes the commands while suspended, so the
// system cannot be resumed any more.
SND_TEST(IndepFinal, Defect_CoreResumeRefusedForeverAfterRecordingWhileSuspended) {
  Sys s;
  SND_REQUIRE_EQ(s.created, SND_OK);
  s.Advance(2);
  SND_CHECK_EQ(snd_suspend(s.sys), SND_OK);
  for (int frame = 0; frame < 300; ++frame) {
    for (int i = 0; i < 64; ++i) snd_set_param_global(s.sys, 0x5000u + static_cast<snd_id>(i), static_cast<float>(frame));
    snd_update(s.sys);
  }
  snd_result rr = snd_resume(s.sys);
  int retries = 0;
  for (; retries < 100 && rr != SND_OK; ++retries) {
    snd_update(s.sys);
    rr = snd_resume(s.sys);
  }
  snd::test::Note("resume after 300 suspended frames of 64 global parameters: %d (after %d retries)", static_cast<int>(rr),
                  retries);
  SND_CHECK_EQ(rr, SND_OK);
}

// For comparison (passes): a short suspend (300 frames) with physics holding 8 hits and the
// game submitting every frame resumes, and the held emitters are released afterwards
// (IndepRecheck.SuspendHoldsThenResumeReleases covers the same at 300 frames). Here the same
// with tracked slides too, and the positions after the resume.
SND_TEST(IndepFinal, ShortSuspendWithSlidesResumesAndKeepsPositions) {
  Fx f;
  SND_REQUIRE(f.Ok());
  SND_REQUIRE_EQ(f.Table({kDc}, {kLoop}), SND_OK);
  f.s.Advance(2);
  SND_CHECK_EQ(snd_suspend(f.s.sys), SND_OK);
  std::vector<snd_contact> cs = {Slide(1, -2.0f), Slide(2, -3.0f)};
  for (std::uint64_t i = 0; i < 8; ++i) cs.push_back(Hit(100 + 2 * i, 2.0f + 0.6f * static_cast<float>(i)));
  SND_CHECK_EQ(f.Submit(cs), SND_OK);
  for (int frame = 0; frame < 100; ++frame) {
    SND_CHECK_EQ(f.Submit({Slide(1, -2.0f)}), SND_OK);  // slide 2 times out while suspended
    snd_update(f.s.sys);
  }
  SND_CHECK_EQ(phystest::Stats(f.phys).tracking_timeouts, 1u);
  SND_CHECK_EQ(snd_resume(f.s.sys), SND_OK);
  for (int frame = 0; frame < 10; ++frame) {
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({Slide(1, -2.0f)}), SND_OK);
  }
  // New hits on the right while slide 1 sounds on the left: compare left/right energy with
  // slide 1 stopped first.
  for (int frame = 0; frame < 30; ++frame) {
    SND_CHECK_EQ(f.Submit({}), SND_OK);
    f.s.Advance(1);
  }
  SND_CHECK_EQ(phystest::Stats(f.phys).tracking_timeouts, 2u);
  f.s.Advance(40);
  const std::uint64_t from = f.s.Frames();
  std::uint64_t obj = 500;
  for (int frame = 0; frame < 30; ++frame, obj += 2) {
    SND_CHECK_EQ(f.Submit({Hit(obj, 2.0f)}), SND_OK);
    f.s.Advance(1);
  }
  f.s.Advance(60);
  double l = 0, r = 0;
  Energy(f.s, from, f.s.Frames(), &l, &r);
  SND_CHECK(r > 1.0);
  SND_CHECK_EQ(l, 0.0);
  // The last hits' emitters are released by the next submits (physics.h: "a later
  // snd_phys_submit_contacts").
  SND_CHECK(f.FramesUntilEmittersBack(128) >= 0);
}

// ---------------------------------------------------------------------------------------
// (d) Child playbacks that start after physics returned the emitter (A-004 §5 family).
// Records, like CoreA004_*: (a) heard on the right (core keeps the parent's transform) or
// (b) at the listener (the known limit); anything else fails.
// ---------------------------------------------------------------------------------------
namespace {
const char* Classify(double l, double r, bool* ok) {
  const bool right = r > 1.0 && l < 1e-3 * r;
  const bool listener = r > 1.0 && std::fabs(l - r) < 0.01 * r;
  *ok = right || listener;
  return right ? "(a) on the right" : listener ? "(b) at the listener" : "neither";
}
}  // namespace

// A music event registered as an impact event: the first segment starts with the play; the
// second starts a bar (12000 frames) later, after the emitter was returned. Compared with the
// same music on an emitter the game keeps: core does not spatialise music events at all
// (centred on a kept emitter too), so the release cannot change anything there; the test
// records that and checks physics gives the same as the kept emitter for both segments.
namespace {
void MusicSegments(Sys& s, std::uint64_t from, const char** c1, const char** c2, bool* ok1, bool* ok2) {
  const std::uint64_t on = Onset(s, from);
  double l1 = 0, r1 = 0, l2 = 0, r2 = 0;
  Energy(s, on + 512, on + 11500, &l1, &r1);
  Energy(s, on + 12512, on + 23500, &l2, &r2);
  *c1 = Classify(l1, r1, ok1);
  *c2 = Classify(l2, r2, ok2);
}
}  // namespace

SND_TEST(IndepFinal, CoreA004_MusicSecondSegmentAfterEmitterRelease) {
  const char *k1 = "", *k2 = "", *p1 = "", *p2 = "";
  bool ok[4] = {};
  {
    Sys s;
    snd_bank_handle bank{};
    SND_REQUIRE(s.LoadBank("final.sndb", FinalBank(), &bank));
    SetListener(s.sys);
    s.Advance(2);
    const snd_emitter_handle e = snd_create_emitter(s.sys);
    const snd_vec3 right{2.0f, 0.0f, 0.0f};
    snd_set_transform(s.sys, e, &right, nullptr, nullptr);
    s.Advance(1);
    const std::uint64_t from = s.Frames();
    SND_REQUIRE(snd_play(s.sys, kMusic, e, nullptr).value != 0u);
    s.Advance(150);
    MusicSegments(s, from, &k1, &k2, &ok[0], &ok[1]);
  }
  Fx f;
  SND_REQUIRE(f.Ok());
  SND_REQUIRE_EQ(f.Table({kMusic}), SND_OK);
  f.s.Advance(2);
  const std::uint64_t from = f.s.Frames();
  SND_CHECK_EQ(f.Submit({Hit(1, 2.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  for (int i = 0; i < 150; ++i) {
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({}), SND_OK);
  }
  MusicSegments(f.s, from, &p1, &p2, &ok[2], &ok[3]);
  snd::test::Note("music, kept emitter: segment 1 %s, segment 2 %s; through physics: segment 1 %s, segment 2 %s", k1, k2,
                  p1, p2);
  for (bool b : ok) SND_CHECK(b);
  SND_CHECK(std::strcmp(k1, p1) == 0);  // segment 1 (the play itself): as on a kept emitter
  SND_CHECK(std::strcmp(k2, p2) == 0 || std::strcmp(p2, "(b) at the listener") == 0);  // segment 2: (a) / (b)
}

// A stinger the game plays (snd_play_stinger, immediate) on a music playback physics started,
// after physics returned the emitter. The music itself is silent; the stinger is heard.
SND_TEST(IndepFinal, CoreA004_StingerAfterEmitterRelease) {
  Fx f;
  SND_REQUIRE(f.Ok());
  SND_REQUIRE_EQ(f.Table({kMusicQuiet}), SND_OK);
  f.s.Advance(2);
  SND_CHECK_EQ(f.Submit({Hit(1, 2.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  const snd_playback_handle music = f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.playback;
  SND_CHECK(f.FramesUntilEmittersBack(128) >= 0);  // physics returned its emitter
  SND_CHECK_EQ(snd_get_playback_state(f.s.sys, music), SND_PLAYBACK_STATE_PLAYING);
  const std::uint64_t from = f.s.Frames();
  SND_CHECK_EQ(snd_play_stinger(f.s.sys, music, snd_hash_id("Hit"), nullptr), SND_OK);
  f.s.Advance(40);
  double l = 0, r = 0;
  Energy(f.s, from, f.s.Frames(), &l, &r);
  bool ok = false;
  const char* c = Classify(l, r, &ok);
  snd::test::Note("stinger after the release: %s (|L| %.1f |R| %.1f)", c, l, r);
  SND_CHECK(ok);
}

// ---------------------------------------------------------------------------------------
// Paths the earlier tests did not reach (coverage).
// ---------------------------------------------------------------------------------------

// Resource exhaustion (D-119, D-122, physics.h "result: the failure of the API call"): no
// playback slot for a one-shot / a slide / a footstep / play_at, no emitter for a new slide.
// Failures are counted in api_failures and reported in the records; nothing crashes; the
// module recovers when resources come back.
SND_TEST(IndepFinal, ResourceExhaustionPaths) {
  snd_config sc = phystest::SmallConfig();
  sc.max_playbacks = 2;
  sc.max_emitters = 3;
  Fx f(Fx::OpenCfg(), sc);
  SND_REQUIRE(f.Ok());
  SND_REQUIRE_EQ(f.Table({kDc}, {kLoop}), SND_OK);
  SND_REQUIRE_EQ(snd_physics_set_footstep_event(f.phys, 0, SND_PHYSICS_ANY_MOTION, kDc), SND_OK);
  f.s.Advance(2);
  // Two looping slides take both playback slots.
  SND_CHECK_EQ(f.Submit({Slide(1), Slide(2)}), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 2u);
  f.s.Advance(2);
  // A third slide: an emitter is left (3), no playback slot -> START with a failure.
  SND_CHECK_EQ(f.Submit({Slide(1), Slide(2), Slide(3)}), SND_OK);
  const std::vector<Rec> starts = f.rec.Of(SND_PHYSICS_RECORD_START);
  SND_REQUIRE_EQ(starts.size(), 1u);
  SND_CHECK_EQ(starts[0].r.playback.value, 0u);
  SND_CHECK(starts[0].r.result != SND_OK);
  const std::uint64_t failures0 = phystest::Stats(f.phys).api_failures;
  SND_CHECK(failures0 >= 1u);
  // Its later reports find no playback: restart attempts fail again, counted, no crash.
  for (int i = 0; i < 3; ++i) {
    SND_CHECK_EQ(f.Submit({Slide(1), Slide(2), Slide(3)}), SND_OK);
    f.s.Advance(1);
  }
  SND_CHECK(phystest::Stats(f.phys).api_failures > failures0);
  // No emitter for a fourth slide: tracking_dropped (D-122).
  const std::uint64_t dropped0 = phystest::Stats(f.phys).tracking_dropped;
  SND_CHECK_EQ(f.Submit({Slide(1), Slide(2), Slide(3), Slide(4)}), SND_OK);
  SND_CHECK_EQ(phystest::Stats(f.phys).tracking_dropped, dropped0 + 1u);
  // A one-shot: no emitter either -> dropped_no_emitter.
  SND_CHECK_EQ(f.Submit({Slide(1), Slide(2), Slide(3), Hit(50, 2.0f)}), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_DROP), 1u);
  // A footstep without a playback slot: the record carries the failure.
  f.rec.Clear();
  const snd_surface_weight w{0, 0, 1.0f};
  const snd_result fr = snd_phys_play_footstep(f.phys, snd_emitter_handle{0}, 0, &w, 1, SND_MOTION_WALK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  snd::test::Note("footstep without a playback slot: result %d, record result %d", static_cast<int>(fr),
                  static_cast<int>(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.result));
  SND_CHECK(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.result != SND_OK);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.playback.value, 0u);
  // play_at without a playback slot: its result is returned (physics.h: "and the result of
  // snd_play_at") and counted.
  const std::uint64_t failures1 = phystest::Stats(f.phys).api_failures;
  const snd_vec3 pos{1, 0, 0};
  const snd_result pr = snd_physics_play_at(f.phys, kDc, &pos);
  snd::test::Note("play_at without a playback slot: %d", static_cast<int>(pr));
  if (pr != SND_OK) SND_CHECK_EQ(phystest::Stats(f.phys).api_failures, failures1 + 1u);
  // Timeout of all slides (the failed one included): stopped, emitters back.
  for (int i = 0; i < 20; ++i) {
    SND_CHECK_EQ(f.Submit({}), SND_OK);
    f.s.Advance(1);
  }
  SND_CHECK_EQ(phystest::Stats(f.phys).tracking_timeouts, 3u);
  f.s.Advance(40);
  SND_CHECK(f.FramesUntilEmittersBack(3) >= 0);
  // Recovered: a new slide starts and plays.
  SND_CHECK_EQ(f.Submit({Slide(7)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 1u);
  SND_CHECK(f.rec.Of(SND_PHYSICS_RECORD_START)[0].r.playback.value != 0u);
}

// A full command arena: the recording calls of physics fail (COMMAND_BUFFER_FULL); they are
// counted in api_failures, the records carry the failure, nothing crashes, and the module
// works again after snd_update.
SND_TEST(IndepFinal, CommandArenaFullPaths) {
  snd_config sc = phystest::SmallConfig();
  sc.commands.arena_bytes = 2048;
  sc.commands.reserve_bytes = 1024;
  Fx f(Fx::OpenCfg(), sc);
  SND_REQUIRE(f.Ok());
  SND_REQUIRE_EQ(f.Table({kDc}, {kLoop}), SND_OK);
  f.s.Advance(2);
  std::vector<snd_contact> cs;
  for (std::uint64_t i = 0; i < 60; ++i) cs.push_back(Hit(i * 2, 2.0f + 0.6f * static_cast<float>(i)));
  for (std::uint64_t i = 0; i < 10; ++i) cs.push_back(Slide(500 + i));
  SND_CHECK_EQ(f.Submit(cs), SND_OK);
  const snd_physics_stats st = phystest::Stats(f.phys);
  std::uint32_t failedRecords = 0;
  for (const Rec& r : f.rec.recs) failedRecords += r.r.result != SND_OK ? 1u : 0u;
  snd::test::Note("arena of 2 KiB: api failures %llu, records with a failure %u, requests %llu, no emitter %llu, started "
                  "%llu, dropped %llu",
                  static_cast<unsigned long long>(st.api_failures), failedRecords,
                  static_cast<unsigned long long>(st.group_requests),
                  static_cast<unsigned long long>(st.dropped_no_emitter),
                  static_cast<unsigned long long>(st.tracking_started),
                  static_cast<unsigned long long>(st.tracking_dropped));
  SND_CHECK(st.api_failures > 0u);
  SND_CHECK_EQ(st.groups, st.dropped_no_event + st.dropped_cooldown + st.dropped_budget + st.dropped_no_emitter +
                              st.group_requests);
  // Next frames: the module recovers, the emitters come back, all slides time out.
  for (int i = 0; i < 30; ++i) {
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({}), SND_OK);
  }
  f.s.Advance(40);
  SND_CHECK(f.FramesUntilEmittersBack(128) >= 0);
  SND_CHECK_EQ(f.Submit({Hit(9000, 2.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.result, SND_OK);
}

// Small paths: footstep with an unknown surface and no default (nothing, fallback_none);
// slides of one object on itself with the materials swapped (the orientation of the start
// is kept); a slide resolved by a pair override (START, no FALLBACK record); play_at with a
// non-finite y / z; set_table with a struct_size not a multiple of 4; curves with a
// non-finite x or a velocity below -1e6.
SND_TEST(IndepFinal, SmallPaths) {
  Fx f;
  SND_REQUIRE(f.Ok());
  SND_REQUIRE_EQ(f.Table({kDc, kDc}, {kLoop, kLoop}), SND_OK);
  // Footstep: unknown surface, no footstep default -> nothing.
  const snd_surface_weight w{9, 0, 1.0f};
  SND_CHECK_EQ(snd_phys_play_footstep(f.phys, snd_emitter_handle{0}, 0, &w, 1, SND_MOTION_WALK), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 0u);
  SND_CHECK_EQ(phystest::Stats(f.phys).footstep_fallback_none, 1u);
  // One object sliding on itself, materials (0, 1) then (1, 0): the same key; the start's
  // orientation is kept (material_a stays 0).
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_SLIDE, 5, 5, 0, 1, 2, 0, 0, 0, 1.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 2u);
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_SLIDE, 5, 5, 1, 0, 2, 0, 0, 0, 2.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_UPDATE), 2u);
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_UPDATE)) {
    SND_CHECK_EQ(r.r.material_a, 0u);
    SND_CHECK_EQ(r.r.material_b, 1u);
    SND_CHECK_EQ(r.r.speed, 2.0f);
  }
  // A slide resolved by a pair override: no FALLBACK record.
  SND_REQUIRE_EQ(snd_physics_set_pair_event(f.phys, 0, 1, SND_CONTACT_ROLL, kLoop), SND_OK);
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_ROLL, 7, 8, 1, 0, 2, 0, 0, 0, 1.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 1u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_START)[0].r.stage, SND_PHYSICS_STAGE_PAIR);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_FALLBACK), 0u);
  // play_at with a non-finite y or z.
  const snd_vec3 by{0, kNaN, 0};
  const snd_vec3 bz{0, 0, kInf};
  SND_CHECK_EQ(snd_physics_play_at(f.phys, kDc, &by), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_play_at(f.phys, kDc, &bz), SND_ERR_INVALID_ARGUMENT);
  // set_table: struct_size not a multiple of 4.
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.struct_size = 10;
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &d), SND_ERR_INVALID_ARGUMENT);
  // Curves: a non-finite x, a velocity y below -1e6.
  snd_physics_curves cv{};
  cv.speed_to_volume = phystest::Linear({{kNaN, 0.5f}});
  SND_CHECK_EQ(snd_physics_set_curves(f.phys, 0, &cv), SND_ERR_INVALID_ARGUMENT);
  cv = snd_physics_curves{};
  cv.impulse_to_velocity = phystest::Linear({{0.0f, -1.5e6f}});
  SND_CHECK_EQ(snd_physics_set_curves(f.phys, 0, &cv), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_material_count(f.phys), 2u);
}

// Creation details the header leaves open, recorded (Undecided_): an allocator table with a
// NULL function, and max_game_materials above the u16 range (65536 game materials at most).
SND_TEST(IndepFinal, Undecided_AllocatorTableAndGameMaterialRange) {
  Sys s;
  snd_physics_config c = Cfg(nullptr);
  snd_phys* p = nullptr;
  const snd_allocator half{nullptr, nullptr, nullptr};
  const snd_result r1 = snd_physics_create(s.sys, &c, &half, &p);
  snd::test::Note("allocator with NULL functions: %d", static_cast<int>(r1));
  SND_CHECK_EQ(r1, SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(p == nullptr);
  c.max_game_materials = 0x10001u;
  const snd_result r2 = snd_physics_create(s.sys, &c, nullptr, &p);
  snd::test::Note("max_game_materials 65537: %d", static_cast<int>(r2));
  SND_CHECK_EQ(r2, SND_ERR_INVALID_ARGUMENT);
  c.max_game_materials = 0x10000u;
  SND_CHECK_EQ(snd_physics_create(s.sys, &c, nullptr, &p), SND_OK);
  snd_physics_destroy(p);
}

// The C++ wrapper parts no other test calls: snd::phys::DefaultDesc, Create(desc, IAllocator),
// LoadBankTable.
namespace {
struct CountingIAllocator final : snd::IAllocator {
  int live = 0;
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag) override {
    void* q = nullptr;
#if defined(_WIN32)
    q = _aligned_malloc(bytes, align);
#else
    if (posix_memalign(&q, align < sizeof(void*) ? sizeof(void*) : align, bytes) != 0) q = nullptr;
#endif
    if (q != nullptr) ++live;
    return q;
  }
  void Free(void* q, snd::MemTag) override {
    if (q == nullptr) return;
    --live;
#if defined(_WIN32)
    _aligned_free(q);
#else
    std::free(q);
#endif
  }
};
}  // namespace

SND_TEST(IndepFinal, CppWrapperDescAllocatorAndBankTable) {
  Sys s;
  snd_bank_handle bank{};
  SND_REQUIRE(s.LoadBank("phys.sndb", TestBank(), &bank));
  CountingIAllocator a;
  const snd::phys::PhysicsDesc d = snd::phys::DefaultDesc(snd::BankHandle::FromC(bank), snd_hash_id("I/Table"));
  SND_CHECK_EQ(d.budget_per_second, 300.0f);
  SND_CHECK_EQ(d.bank.value, bank.value);
  snd::phys::PhysicsAudio* p = nullptr;
  SND_REQUIRE(snd::phys::Create(s.sys, d, &a, &p) == snd::Result::Ok);
  SND_CHECK(a.live >= 1);
  SND_CHECK_EQ(snd::phys::MaterialCount(p), 3u);
  SND_CHECK(snd::phys::LoadBankTable(p, snd::BankHandle::FromC(bank), snd_hash_id("I/Table")) == snd::Result::Ok);
  SND_CHECK(snd::phys::LoadBankTable(p, snd::BankHandle::FromC(bank), 0x1234u) == snd::Result::NotFound);
  SND_CHECK(snd::phys::GetStats(p, nullptr) == snd::Result::InvalidArgument);
  snd::phys::Destroy(p);
  SND_CHECK_EQ(a.live, 0);
  // The frozen Create without an allocator (system default).
  snd::phys::PhysicsAudio* q = nullptr;
  SND_CHECK(snd::phys::Create(s.sys, d, nullptr, &q) == snd::Result::Ok);
  SND_CHECK_EQ(snd::phys::MaterialCount(q), 3u);
  snd::phys::Destroy(q);
}

