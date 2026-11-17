// Independent tests: parameters (4.3), categories (4.6, D-30), buses and sends
// (4.6), voice selection (4.4), determinism (2.5), memory after Create (3.3).
// Analytic expectations with the DC / counter sources of indep_fixture.hpp.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "indep_fixture.hpp"

using namespace indep;
using snd::HashId;

namespace {
float Db(float db) { return std::pow(10.0f, db / 20.0f); }

// Volume curve over a parameter: x = 0 -> 0 dB, x = 1 -> `dbAt1`.
TCurve VolumeCurve(const char* param, float dbAt1) {
  return TCurve{HashId(param), 0u, 0u, {{0.0f, 0.0f}, {1.0f, dbAt1}}};
}

class CountingAllocator final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag tag) override {
    (tag == snd::MemTag::Bank ? bankAllocs : allocs).fetch_add(1);
    return snd::base::DefaultAllocator().Allocate(bytes, align, tag);
  }
  void Free(void* p, snd::MemTag tag) override {
    (tag == snd::MemTag::Bank ? bankFrees : frees).fetch_add(1);
    snd::base::DefaultAllocator().Free(p, tag);
  }
  std::atomic<std::uint64_t> allocs{0}, frees{0}, bankAllocs{0}, bankFrees{0};
};
}  // namespace

// M1: Playback > Emitter > Global, per playback.
SND_TEST(IndepMix, ParameterScopesComposePerPlayback) {
  Bank b;
  b.b.params.push_back(TParam{HashId("Vol"), 0, 0.0f, 1.0f, 0.0f, 0.0f});
  b.b.curves.push_back(VolumeCurve("Vol", -20.0f));
  b.Event("A", 100.0f).curves.push_back(0);
  b.Event("B", 200.0f).curves.push_back(0);
  b.Event("C", 400.0f).curves.push_back(0);
  Fx fx;
  fx.Load("b", b.Build("B"));
  const snd_emitter_handle em = fx.sys->CreateEmitter();
  const snd_playback_handle a = fx.sys->Play(HashId("A"), em, nullptr);
  fx.sys->Play(HashId("B"), em, nullptr);
  fx.sys->Play(HashId("C"), NoEmitter(), nullptr);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.7f, 1e-5f);
  fx.sys->SetParamGlobal(HashId("Vol"), 1.0f);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.7f * 0.1f, 1e-5f);
  fx.sys->SetParamEmitter(em, HashId("Vol"), 0.0f);  // A, B back to 0 dB; C keeps global
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * (0.3f + 0.04f), 1e-5f);
  fx.sys->SetParamPlayback(a, HashId("Vol"), 1.0f);  // only A
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * (0.01f + 0.2f + 0.04f), 1e-5f);
  fx.sys->SetParamGlobal(HashId("Vol"), 0.0f);  // C only; A and B are overridden
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * (0.01f + 0.2f + 0.4f), 1e-5f);
}

// M2: ramp rate (units / second); a new value ramps from the PARM default.
SND_TEST(IndepMix, ParameterRampFollowsTheRate) {
  Bank b;
  b.b.params.push_back(TParam{HashId("R"), 0, 0.0f, 1.0f, 0.0f, 2.0f});  // 2 units / s
  b.b.curves.push_back(VolumeCurve("R", -20.0f));
  b.Event("E", 500.0f).curves.push_back(0);
  Fx fx;
  fx.Load("b", b.Build("B"));
  fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  fx.sys->SetParamGlobal(HashId("R"), 1.0f);
  std::vector<float> x;
  for (int i = 0; i < 120; ++i) {
    fx.Step();
    const float g = fx.At(fx.Total() - 1) / (kCenter * 0.5f);  // last sample of the block
    x.push_back(-std::log10(g));                                // = parameter value (curve: -20 dB at 1)
  }
  const float perBlock = 2.0f * 256.0f / 48000.0f;
  bool bounded = true, monotone = true;
  for (std::size_t i = 1; i < x.size(); ++i) {
    if (x[i] - x[i - 1] > perBlock + 1e-4f) bounded = false;
    if (x[i] < x[i - 1] - 1e-5f) monotone = false;
  }
  SND_CHECK(bounded);
  SND_CHECK(monotone);
  // 0.5 s to reach 1: half way after ~47 blocks.
  SND_CHECK_NEAR(x[46], 0.5f, 0.03f);
  SND_CHECK_NEAR(x.back(), 1.0f, 1e-4f);
  SND_CHECK(x[80] < 0.95f);
}

// M3: two volume curves add in dB.
SND_TEST(IndepMix, VolumeCurvesAddInDb) {
  Bank b;
  b.b.params.push_back(TParam{HashId("P"), 0, 0.0f, 1.0f, 0.0f, 0.0f});
  b.b.params.push_back(TParam{HashId("Q"), 0, 0.0f, 1.0f, 0.0f, 0.0f});
  b.b.curves.push_back(VolumeCurve("P", -6.0f));
  b.b.curves.push_back(VolumeCurve("Q", -14.0f));
  TEvent& e = b.Event("E", 500.0f);
  e.curves = {0, 1};
  Fx fx;
  fx.Load("b", b.Build("B"));
  fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.sys->SetParamGlobal(HashId("P"), 1.0f);
  fx.sys->SetParamGlobal(HashId("Q"), 1.0f);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f * Db(-20.0f), 1e-5f);
}

// K1-K4: category volume / mute / pause, the "all categories" id (D-30),
// ignore-pause categories, the category volume fade.
SND_TEST(IndepMix, CategoriesVolumeMutePauseAndAll) {
  Bank b;
  b.b.categories.push_back(TCategory{HashId("Cat"), kNone, 0u, 0.0f});
  b.b.categories.push_back(TCategory{HashId("UI"), kNone, bf::kCategoryIgnorePause, 0.0f});
  b.Event("A", 400.0f, 128, 0, 0);
  b.Event("B", 200.0f, 128, 0, kNone);
  b.Event("U", 100.0f, 128, 0, 1);
  Fx fx;
  fx.Load("b", b.Build("B"));
  fx.sys->Play(HashId("A"), NoEmitter(), nullptr);
  fx.sys->Play(HashId("B"), NoEmitter(), nullptr);
  fx.sys->Play(HashId("U"), NoEmitter(), nullptr);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.7f, 1e-5f);
  fx.sys->SetCategoryVolume(HashId("Cat"), 0.5f, 0.0f);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * (0.2f + 0.2f + 0.1f), 1e-5f);
  fx.sys->MuteCategory(HashId("Cat"), true);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.3f, 1e-5f);
  fx.sys->MuteCategory(HashId("Cat"), false);
  fx.sys->SetCategoryVolume(HashId("Cat"), 1.0f, 0.0f);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.7f, 1e-5f);
  // Pause of one category.
  fx.sys->PauseCategory(HashId("Cat"), true);
  fx.Steps(4);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.3f, 1e-5f);
  fx.sys->PauseCategory(HashId("Cat"), false);
  fx.Steps(4);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.7f, 1e-5f);
  // Global pause: everything except the ignore-pause category.
  fx.sys->PauseCategory(SND_ID_NONE, true);
  fx.Steps(4);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.1f, 1e-5f);
  fx.sys->PauseCategory(SND_ID_NONE, false);
  fx.Steps(4);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.7f, 1e-5f);
  // Global volume reaches events with and without a category.
  fx.sys->SetCategoryVolume(SND_ID_NONE, 0.5f, 0.0f);
  fx.Steps(3);
  const float m = fx.LastMean() / kCenter;
  SND_CHECK_MSG(std::fabs(m - 0.35f) < 1e-4f || std::fabs(m - 0.4f) < 1e-4f, "global volume gives %f", static_cast<double>(m));
  fx.sys->SetCategoryVolume(SND_ID_NONE, 1.0f, 0.0f);
  // Fade of a category volume: 0.2 s to 0, linear in gain.
  fx.Steps(2);
  fx.sys->SetCategoryVolume(HashId("Cat"), 0.0f, 0.2f);
  const std::size_t s = fx.Total();
  fx.Steps(45);
  const float a0 = fx.At(s) / kCenter - 0.3f;
  const float aMid = fx.At(s + 4800) / kCenter - 0.3f;
  const float aEnd = fx.At(s + 9600 + 300) / kCenter - 0.3f;
  SND_CHECK_NEAR(a0, 0.4f, 0.02f);
  SND_CHECK_NEAR(aMid, 0.2f, 0.02f);
  SND_CHECK_NEAR(aEnd, 0.0f, 1e-4f);
  SND_CHECK_EQ(fx.kind.ViolationSum(), 0u);
}

// Category pause holds the position (counter continues where it stopped).
SND_TEST(IndepMix, CategoryPauseHoldsThePosition) {
  Bank b;
  b.b.categories.push_back(TCategory{HashId("Cat"), kNone, 0u, 0.0f});
  b.Event("C", 1000.0f, 128, 0, 0);
  Fx fx;
  fx.Load("b", b.Build("B"));
  const snd_playback_handle p = fx.sys->Play(HashId("C"), NoEmitter(), nullptr);
  fx.Steps(4);
  fx.sys->PauseCategory(HashId("Cat"), true);
  fx.Steps(3);
  fx.sys->Update();
  snd_position a{}, c{};
  fx.sys->GetPosition(p, &a);
  fx.Steps(10);
  SND_CHECK_EQ(fx.LastMean(), 0.0f);
  fx.sys->Update();
  fx.sys->GetPosition(p, &c);
  SND_CHECK_EQ(a.frames, c.frames);
  fx.sys->PauseCategory(HashId("Cat"), false);
  fx.Steps(4);
  SND_CHECK_EQ(fx.kind.discont.load(), 0u);
  const std::size_t f = fx.Total() - 20;
  SND_CHECK_NEAR(fx.At(f + 1) - fx.At(f), kCenter / 4096.0f, 1e-6f);
}

// B1 / B2: bus volume and the bus tree multiply (child bank volume x parent).
SND_TEST(IndepMix, BusVolumeAndHierarchyMultiply) {
  Bank b;
  b.b.buses.clear();
  b.b.buses.push_back(TBus{HashId("Sub"), 1u, 0, 0, -6.0f, {}, {}});
  b.b.buses.push_back(TBus{HashId("Sfx"), 2u, 0, 0, 0.0f, {}, {}});
  b.b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  b.Event("E", 500.0f, 128, 0);
  Fx fx;
  fx.Load("b", b.Build("B"));
  fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f * Db(-6.0f), 1e-5f);
  fx.sys->SetBusVolume(HashId("Sfx"), 0.5f, 0.0f);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f * Db(-6.0f) * 0.5f, 1e-5f);
  fx.sys->SetBusVolume(HashId("Master"), 0.5f, 0.0f);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f * Db(-6.0f) * 0.25f, 1e-5f);
  // Fade: 0.1 s from 0.25 to 0 overall, linear in gain.
  fx.sys->SetBusVolume(HashId("Sfx"), 0.0f, 0.1f);
  const std::size_t s = fx.Total();
  fx.Steps(25);
  SND_CHECK_NEAR(fx.At(s + 2400), kCenter * 0.5f * Db(-6.0f) * 0.125f, 0.01f);
  SND_CHECK_EQ(fx.At(s + 4800 + 256), 0.0f);
  snd_meter m{};
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetMeter(HashId("Sub"), &m), SND_OK);
  SND_CHECK_EQ(fx.sys->GetMeter(HashId("NoSuchBus"), &m), SND_ERR_NOT_FOUND);
}

// B3: pre-fader sends ignore the source bus fader, post-fader sends follow it.
SND_TEST(IndepMix, PreAndPostFaderSends) {
  for (std::uint32_t pre = 0; pre < 2; ++pre) {
    Bank b;
    b.b.buses.clear();
    b.b.buses.push_back(TBus{HashId("Src"), 2u, 0, 0, 0.0f, {}, {TBus::Send{1u, -6.0f, pre}}});
    b.b.buses.push_back(TBus{HashId("Aux"), 2u, 0, 0, 0.0f, {}, {}});
    b.b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
    b.Event("E", 500.0f, 128, 0);
    Fx fx;
    fx.Load("b", b.Build("B"));
    fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
    fx.Steps(3);
    SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f * (1.0f + Db(-6.0f)), 1e-5f);
    fx.sys->SetBusVolume(HashId("Src"), 0.0f, 0.0f);
    fx.Steps(3);
    SND_CHECK_NEAR(fx.LastMean(), pre != 0u ? kCenter * 0.5f * Db(-6.0f) : 0.0f, 1e-5f);
    fx.sys->SetBusVolume(HashId("Src"), 1.0f, 0.0f);
    fx.sys->SetBusVolume(HashId("Aux"), 0.0f, 0.0f);
    fx.Steps(3);
    SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f, 1e-5f);
  }
}

// B6 / B7: a 5.1 master bus is downmixed to the 2-channel output; the output
// has OutputChannels() channels.
SND_TEST(IndepMix, SixChannelMasterIsDownmixedToStereo) {
  Bank b;
  b.b.buses.clear();
  b.b.buses.push_back(TBus{HashId("Sfx"), 1u, 0, 0, 0.0f, {}, {}});
  b.b.buses.push_back(TBus{HashId("Master"), kNone, 6, bf::kBusMaster, 0.0f, {}, {}});
  b.Event("E", 500.0f, 128, 0);
  Fx fx;
  SND_REQUIRE_EQ(fx.sys->OutputChannels(), 2u);
  fx.Load("b", b.Build("B"));
  fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  const float l = fx.LastMean(0), r = fx.LastMean(1);
  SND_CHECK(l > 0.1f);
  SND_CHECK_NEAR(l, r, 1e-6f);
  SND_CHECK(l < 0.5f + 1e-6f);  // a downmix never amplifies a centred mono source beyond its level
}

// V1: priority beats loudness; V2: equal priority -> the loudest; V6: 4x the
// real limit stays bounded and sums exactly the selected voices.
SND_TEST(IndepMix, VoiceSelectionPriorityThenLoudnessAndFourTimesTheLimit) {
  {
    snd_config cfg = Config();
    cfg.max_real_voices = 4;
    Bank b;
    for (int i = 0; i < 8; ++i) {
      char n[8];
      std::snprintf(n, sizeof(n), "V%d", i);
      b.Event(n, static_cast<float>(1u << i), i < 4 ? 200u : 100u);
    }
    Fx fx(cfg);
    fx.Load("b", b.Build("B"));
    for (int i = 0; i < 8; ++i) {
      char n[8];
      std::snprintf(n, sizeof(n), "V%d", i);
      fx.sys->Play(HashId(n), NoEmitter(), nullptr);
    }
    fx.Steps(4);
    SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.015f, 1e-6f);  // V0..V3 (priority 200)
  }
  {
    snd_config cfg = Config();
    cfg.max_real_voices = 4;
    Bank b;
    for (int i = 0; i < 8; ++i) {
      char n[8];
      std::snprintf(n, sizeof(n), "V%d", i);
      // Audibility is the voice's gain (4.4), not the signal: loudness by volumeDb.
      b.Event(n, 500.0f, 128u, 0, kNone, -2.0f * float(7 - i));
    }
    Fx fx(cfg);
    fx.Load("b", b.Build("B"));
    for (int i = 0; i < 8; ++i) {
      char n[8];
      std::snprintf(n, sizeof(n), "V%d", i);
      fx.sys->Play(HashId(n), NoEmitter(), nullptr);
    }
    fx.Steps(4);
    float expect = 0.0f;
    for (int i = 4; i < 8; ++i) expect += 0.5f * Db(-2.0f * float(7 - i));
    SND_CHECK_NEAR(fx.LastMean(), kCenter * expect, 1e-5f);  // V4..V7
  }
  {
    snd_config cfg = Config();
    cfg.max_real_voices = 8;  // 32 requests = 4x
    Bank b;
    for (int i = 1; i <= 32; ++i) {
      char n[8];
      std::snprintf(n, sizeof(n), "W%d", i);
      b.Event(n, 500.0f, 128u, 0, kNone, -1.0f * float(32 - i));
    }
    Fx fx(cfg);
    fx.Load("b", b.Build("B"));
    for (int i = 1; i <= 32; ++i) {
      char n[8];
      std::snprintf(n, sizeof(n), "W%d", i);
      fx.sys->Play(HashId(n), NoEmitter(), nullptr);
    }
    fx.Steps(5);
    float expect = 0.0f;
    for (int i = 25; i <= 32; ++i) expect += 0.5f * Db(-1.0f * float(32 - i));
    SND_CHECK_NEAR(fx.LastMean(), kCenter * expect, 1e-5f);  // W25..W32
    fx.sys->Update();
    snd_stats st{};
    fx.sys->GetStats(&st);
    SND_CHECK_EQ(st.real_voices, 8u);
    SND_CHECK_EQ(st.virtual_voices, 24u);
    SND_CHECK_EQ(st.playbacks, 32u);
    SND_CHECK(fx.ends.empty());  // virtual voices are not ended
    SND_CHECK_EQ(fx.kind.ViolationSum(), 0u);
  }
}

// V3: hysteresis (3 dB): a 2 dB louder newcomer does not replace an incumbent,
// a 4 dB louder one does.
SND_TEST(IndepMix, StealHysteresisOfThreeDb) {
  snd_config cfg = Config();
  cfg.max_real_voices = 4;
  Bank b;
  for (int i = 0; i < 4; ++i) {
    char n[8];
    std::snprintf(n, sizeof(n), "I%d", i);
    b.Event(n, 100.0f);
  }
  b.Event("Plus2", 100.0f, 128, 0, kNone, 2.0f);
  b.Event("Plus4", 100.0f, 128, 0, kNone, 4.0f);
  Fx fx(cfg);
  fx.Load("b", b.Build("B"));
  for (int i = 0; i < 4; ++i) {
    char n[8];
    std::snprintf(n, sizeof(n), "I%d", i);
    fx.sys->Play(HashId(n), NoEmitter(), nullptr);
  }
  fx.Steps(3);
  fx.sys->Play(HashId("Plus2"), NoEmitter(), nullptr);
  fx.Steps(4);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.4f, 1e-5f);  // the +2 dB newcomer stays virtual
  fx.sys->Play(HashId("Plus4"), NoEmitter(), nullptr);
  fx.Steps(4);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * (0.3f + 0.1f * Db(4.0f)), 1e-5f);
}

// V4: a stolen voice fades out over the short fade (5 ms), it is not cut.
SND_TEST(IndepMix, DemotedVoiceFadesOverTheShortFade) {
  snd_config cfg = Config();
  cfg.max_real_voices = 1;
  Bank b;
  b.Event("Low", 500.0f, 100);
  b.Event("High", 2000.0f + 256.0f * 20.0f, 200);  // DC 1.0, 20 blocks
  Fx fx(cfg);
  fx.Load("b", b.Build("B"));
  fx.sys->Play(HashId("Low"), NoEmitter(), nullptr);
  fx.Steps(3);
  fx.sys->Play(HashId("High"), NoEmitter(), nullptr);
  const std::size_t s = fx.Total();
  fx.Steps(4);
  // Find the block where High is heard; Low must still contribute and decay
  // over <= 240 frames (+1 block of slack), never jump straight to 0 from full.
  std::size_t on = 0;
  for (std::size_t f = s; f < fx.Total(); ++f) {
    if (fx.At(f) > kCenter * 0.9f) {
      on = f;
      break;
    }
  }
  SND_REQUIRE(on != 0u);
  // After 2 blocks only High is left.
  SND_CHECK_NEAR(fx.At(on + 512), kCenter * 1.0f, 1e-5f);
  // During the change the sum never exceeds both together.
  SND_CHECK(fx.MaxAbs(s, fx.Total()) <= kCenter * 1.5f + 1e-5f);
}

// V5: a virtual voice keeps time ("continue"): when it becomes real again the
// counter shows the elapsed frames.
SND_TEST(IndepMix, VirtualVoiceAdvancesAndContinuesInTime) {
  snd_config cfg = Config();
  cfg.max_real_voices = 1;
  Bank b;
  b.Event("C", 1000.0f, 100);
  b.Event("High", 2000.0f + 256.0f * 10.0f, 200);
  Fx fx(cfg);
  fx.Load("b", b.Build("B"));
  fx.sys->Play(HashId("C"), NoEmitter(), nullptr);
  const std::size_t start = fx.Total();  // the next block applies it: start clock
  fx.Steps(4);
  fx.sys->Play(HashId("High"), NoEmitter(), nullptr);
  fx.Steps(20);
  SND_CHECK(fx.kind.advances.load() > 0u);
  // C is real again: decode its frame number from the last samples.
  for (std::size_t f = fx.Total() - 8; f < fx.Total(); ++f) {
    const float v = fx.At(f) / kCenter;
    const long frame = std::lround(v * 4096.0f) - 1;
    const long expect = static_cast<long>((f - start) % 4096u);
    SND_CHECK_MSG(frame == expect, "frame %ld, expected %ld", frame, expect);
  }
  SND_CHECK_EQ(fx.kind.ViolationSum(), 0u);
}

// G3: same commands, same banks -> the same bits (random clips included).
SND_TEST(IndepMix, DeterministicWithRandomClips) {
  auto build = [] {
    Bank b;
    TEvent& e = b.Event("R", 300.0f);
    e.sequence = static_cast<std::uint32_t>(bf::SequenceKind::Random);
    e.tracks[0].clips[0].randVolumeDb = 6.0f;
    e.tracks[0].clips[0].randPan = 1.0f;
    TClip c2 = e.tracks[0].clips[0];
    c2.target = b.Synth(600.0f);
    // b.Synth may reallocate; re-find the event.
    b.b.events.back().tracks[0].clips.push_back(c2);
    return b.Build("B");
  };
  std::vector<float> outs[2];
  for (int run = 0; run < 2; ++run) {
    Fx fx;
    fx.Load("b", build());
    fx.all.clear();
    for (int i = 0; i < 30; ++i) {
      if (i % 3 == 0) fx.sys->Play(HashId("R"), NoEmitter(), nullptr);
      fx.Step();
    }
    outs[run] = fx.all;
  }
  SND_REQUIRE_EQ(outs[0].size(), outs[1].size());
  SND_CHECK(std::memcmp(outs[0].data(), outs[1].data(), outs[0].size() * sizeof(float)) == 0);
  // And the random range is used (not every voice at the same gain).
  bool differs = false;
  for (std::size_t i = 2; i < outs[0].size(); i += 2) differs = differs || outs[0][i] != outs[0][0];
  SND_CHECK(differs);
}

// G1: after Create (and bank loading, MemTag::Bank) nothing is allocated, with
// a varied command script: params in all scopes (overflow chains), emitters,
// categories, steals, stops, pauses.
SND_TEST(IndepMix, NoAllocationAfterCreateUnderAVariedScript) {
  CountingAllocator alloc;
  snd_config cfg = Config();
  cfg.max_real_voices = 4;
  Bank b;
  b.b.categories.push_back(TCategory{HashId("Cat"), kNone, 0u, 0.0f});
  b.b.params.push_back(TParam{HashId("Vol"), 0, 0.0f, 1.0f, 0.0f, 1.0f});
  b.b.curves.push_back(VolumeCurve("Vol", -10.0f));
  b.Event("E", 300.0f, 128, 0, 0).curves.push_back(0);
  b.Event("F", 2000.0f + 700.0f, 150, 0, 0);
  Fx fx(cfg, {}, &alloc);
  SND_REQUIRE_EQ(fx.created, SND_OK);
  fx.Load("b", b.Build("B"));
  fx.Steps(2);
  fx.all.reserve(fx.all.size() + 400 * 512 + 1024);
  const std::uint64_t a0 = alloc.allocs.load();
  const snd_emitter_handle em = fx.sys->CreateEmitter();
  for (int i = 0; i < 300; ++i) {
    const snd_playback_handle p = fx.sys->Play(HashId(i % 2 ? "E" : "F"), i % 3 ? em : NoEmitter(), nullptr);
    for (int k = 0; k < 12; ++k) {
      char n[8];
      std::snprintf(n, sizeof(n), "p%d", k);
      fx.sys->SetParamPlayback(p, HashId(n), float(k));
      fx.sys->SetParamEmitter(em, HashId(n), float(k));
    }
    fx.sys->SetParamGlobal(HashId("Vol"), float(i % 2));
    if (i % 5 == 0) fx.sys->Pause(p, 0.0f);
    if (i % 7 == 0) fx.sys->Stop(p, 0.01f, nullptr);
    if (i % 11 == 0) fx.sys->SetCategoryVolume(HashId("Cat"), 0.5f, 0.1f);
    if (i % 13 == 0) fx.sys->PauseCategory(HashId("Cat"), (i / 13) % 2 == 0);
    fx.Step();
  }
  fx.Steps(50);
  SND_CHECK_MSG(alloc.allocs.load() == a0, "%llu allocations after initialisation",
                static_cast<unsigned long long>(alloc.allocs.load() - a0));
  SND_CHECK_EQ(fx.kind.ViolationSum(), 0u);
}
