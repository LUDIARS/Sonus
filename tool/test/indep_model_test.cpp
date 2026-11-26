// Independent tests (tool/test/indep_properties.md B, C, D, E): the mixer rules on a
// deeper bus tree, a randomized check that the override list always matches the cells,
// undo / redo round trips, the control mappings (fader law, wheel, knob, meter), peak
// hold through the profiler with analytic times, the recording file (layout, FNV-1a 64,
// damage) and project JSON fuzzing (no crash).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "indep_isolate.hpp"
#include "indep_scripted_game.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/meter_ballistics.hpp"
#include "snd/tool/recording_file.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/control_math.hpp"
#include "snd/tool/ui/raster_compositor.hpp"
#include "tool_fixture.hpp"
#include "ui_helpers.hpp"

using namespace snd::tool;
using tooltest::G;
using tooltest::ProjectDir;
namespace ll = snd::livelink;

namespace {

// Deterministic PRNG (splitmix64).
struct Rng {
  std::uint64_t s;
  std::uint64_t Next() {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint32_t Below(std::uint32_t n) { return static_cast<std::uint32_t>(Next() % n); }
  float Uniform(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(Next() >> 40) / 16777216.0f; }
};

// Tree: Master > A > {A1, A2}, Master > B (effects: B parameq, A1 delay); categories C,
// C2 (child of C); parameters P (global, -1..1, default 0.5), Q (global, 0..10).
void WriteTree(const ProjectDir& p) {
  p.Write("project.json", "{\"type\":\"project\",\"guid\":\"" + G(101) + "\",\"name\":\"Tree\",\"formatVersion\":1}");
  p.Write("bank.json", "{\"type\":\"bank\",\"guid\":\"" + G(102) + "\",\"name\":\"Main\"}");
  auto bus = [&](int g, const char* name, const char* parent, const char* extra) {
    std::string s = "{\"type\":\"bus\",\"guid\":\"" + G(static_cast<std::uint32_t>(g)) + "\",\"name\":\"" + name +
                    "\",\"bank\":\"Main\"";
    if (parent != nullptr) s += std::string(",\"parent\":\"") + parent + "\"";
    s += extra;
    s += "}";
    p.Write(std::string("buses/") + name + ".json", s);
  };
  bus(103, "Master", nullptr, ",\"master\":true");
  bus(104, "A", "Master", ",\"volumeDb\":-3");
  bus(105, "A1", "A", ",\"effects\":[{\"type\":\"snd.fx.delay\",\"params\":[10,0,0,-96,1000,0,100]}]");
  bus(106, "A2", "A", "");
  bus(107, "B", "Master", ",\"effects\":[{\"type\":\"snd.fx.parameq\"}]");
  p.Write("cats/C.json", "{\"type\":\"category\",\"guid\":\"" + G(108) + "\",\"name\":\"C\",\"bank\":\"Main\",\"volumeDb\":-2}");
  p.Write("cats/C2.json",
          "{\"type\":\"category\",\"guid\":\"" + G(109) + "\",\"name\":\"C2\",\"bank\":\"Main\",\"parent\":\"C\"}");
  p.Write("params/P.json", "{\"type\":\"parameter\",\"guid\":\"" + G(110) +
                               "\",\"name\":\"P\",\"bank\":\"Main\",\"min\":-1,\"max\":1,\"default\":0.5}");
  p.Write("params/Q.json",
          "{\"type\":\"parameter\",\"guid\":\"" + G(111) + "\",\"name\":\"Q\",\"bank\":\"Main\",\"min\":0,\"max\":10}");
}

struct Tree {
  ProjectDir dir{"indep-tree"};
  Project project;
  MixerModel m;
  bool ok = false;
  Tree() {
    WriteTree(dir);
    ok = project.Load(dir.dir().string());
    m.Bind(project);
  }
  std::int32_t B(const char* n) const { return project.FindBus(snd::HashId(n)); }
  Target F(const char* n) const { return Target{Control::BusFader, B(n), 0, 0}; }
  Target M(const char* n) const { return Target{Control::BusMute, B(n), 0, 0}; }
  Target S(const char* n) const { return Target{Control::BusSolo, B(n), 0, 0}; }
  bool Off(const char* n) const { return m.EffectiveBusDb(B(n)) == kFaderOffDb; }
};

// The override list must be exactly: every cell that differs from the bank, plus every
// enabled parameter override; values equal the cells' (B8).
void CheckOverridesMatchCells(const MixerModel& m) {
  std::map<std::string, float> expect;
  const std::vector<Cell> cells = m.Cells();
  std::uint32_t differing = 0;
  for (const Cell& c : cells) differing += c.value != c.bankValue ? 1u : 0u;
  std::uint32_t enabled = 0;
  for (const auto& p : m.parameters()) enabled += p.enabled ? 1u : 0u;
  const std::vector<OverrideItem> items = m.Overrides();
  SND_CHECK_EQ(items.size(), static_cast<std::size_t>(differing + enabled));
  std::set<std::string> keys;
  for (const OverrideItem& it : items) {
    SND_CHECK(keys.insert(it.key).second);  // unique keys
    if (it.target.control == Control::ParamOverrideOn) {
      SND_CHECK(m.parameters()[static_cast<std::size_t>(it.target.a)].enabled);
      SND_CHECK_EQ(it.value, m.parameters()[static_cast<std::size_t>(it.target.a)].value);
      continue;
    }
    bool found = false;
    for (const Cell& c : cells) {
      if (c.owner == it.target) {
        found = true;
        SND_CHECK_EQ(it.value, c.value);
        SND_CHECK_EQ(it.bankValue, c.bankValue);
        SND_CHECK(c.value != c.bankValue);
      }
    }
    SND_CHECK(found);
  }
}

bool SameCells(const std::vector<Cell>& a, const std::vector<Cell>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!(a[i].address == b[i].address) || a[i].value != b[i].value) return false;
  }
  return true;
}

}  // namespace

// --- B: mixer rules ------------------------------------------------------------------------

SND_TEST(IndepToolMixer, SoloKeepsTheSoloedBusItsAncestorsAndDescendants) {
  Tree t;
  SND_REQUIRE(t.ok);
  SND_REQUIRE(t.m.Set(t.S("A1"), 1.0f));
  SND_CHECK(!t.Off("Master") && !t.Off("A") && !t.Off("A1"));
  SND_CHECK(t.Off("A2") && t.Off("B"));
  SND_REQUIRE(t.m.Set(t.S("A1"), 0.0f));
  SND_REQUIRE(t.m.Set(t.S("A"), 1.0f));  // descendants stay
  SND_CHECK(!t.Off("Master") && !t.Off("A") && !t.Off("A1") && !t.Off("A2"));
  SND_CHECK(t.Off("B"));
  SND_REQUIRE(t.m.Set(t.S("B"), 1.0f));  // union of the solos
  for (const char* n : {"Master", "A", "A1", "A2", "B"}) SND_CHECK(!t.Off(n));
  SND_REQUIRE(t.m.Set(t.S("A"), 0.0f));
  SND_REQUIRE(t.m.Set(t.S("A1"), 1.0f));  // A1 + B: only A2 is silenced
  SND_CHECK(t.Off("A2"));
  SND_CHECK(!t.Off("A") && !t.Off("A1") && !t.Off("B") && !t.Off("Master"));
  // A soloed bus that is muted is silent; the solo of others still applies.
  SND_REQUIRE(t.m.Set(t.M("A1"), 1.0f));
  SND_CHECK(t.Off("A1") && t.Off("A2"));
  SND_CHECK(!t.Off("A"));
  // Releasing every solo and mute restores every fader value.
  SND_REQUIRE(t.m.Set(t.F("A2"), -7.0f));
  SND_REQUIRE(t.m.Set(t.S("A1"), 0.0f));
  SND_REQUIRE(t.m.Set(t.S("B"), 0.0f));
  SND_REQUIRE(t.m.Set(t.M("A1"), 0.0f));
  SND_CHECK_EQ(t.m.EffectiveBusDb(t.B("A")), -3.0f);
  SND_CHECK_EQ(t.m.EffectiveBusDb(t.B("A2")), -7.0f);
  SND_CHECK_EQ(t.m.EffectiveBusDb(t.B("B")), 0.0f);
  CheckOverridesMatchCells(t.m);
}

SND_TEST(IndepToolMixer, FaderRangeEffectParametersAndInitOnly) {
  Tree t;
  SND_REQUIRE(t.ok);
  SND_CHECK(t.m.Set(t.F("B"), 40.0f));
  SND_CHECK_EQ(t.m.Get(t.F("B")), kFaderMaxDb);
  SND_CHECK(t.m.Set(t.F("B"), -1000.0f));
  SND_CHECK_EQ(t.m.Get(t.F("B")), -144.0f);
  SND_CHECK(t.m.Set(t.F("B"), 0.0f));
  SND_CHECK(t.m.Set(t.F("B"), -INFINITY));
  SND_CHECK_EQ(t.m.Get(t.F("B")), -144.0f);
  SND_CHECK(t.m.Set(t.F("B"), 0.0f));
  SND_CHECK(t.m.Set(t.F("B"), NAN));
  SND_CHECK_EQ(t.m.Get(t.F("B")), -144.0f);
  // parameq: Type1 is an integer 0..8 (Allpass), Freq1 20..20000 Hz.
  const Target type1{Control::EffectParam, t.B("B"), 0, 0};
  const Target freq1{Control::EffectParam, t.B("B"), 0, 1};
  SND_CHECK(t.m.Set(type1, 2.6f));
  SND_CHECK_EQ(t.m.Get(type1), 3.0f);
  SND_CHECK(t.m.Set(type1, 99.0f));
  SND_CHECK_EQ(t.m.Get(type1), 8.0f);
  SND_CHECK(t.m.Set(freq1, 1.0f));
  SND_CHECK_EQ(t.m.Get(freq1), 20.0f);
  const EffectDesc& eq = t.project.buses()[static_cast<std::size_t>(t.B("B"))].effects[0];
  SND_CHECK(eq.params[1].logScale);  // Hz with a positive minimum
  SND_CHECK(!eq.params[2].logScale);
  // delay MaxDelay (index 6) is init-only: not editable, not a cell.
  const Target maxDelay{Control::EffectParam, t.B("A1"), 0, 6};
  SND_CHECK(!t.m.IsEditable(maxDelay));
  SND_CHECK(!t.m.Set(maxDelay, 500.0f));
  for (const Cell& c : t.m.Cells()) SND_CHECK(!(c.owner == maxDelay));
  // Category mute is -inf; category volume keeps its fader.
  const Target cm{Control::CategoryMute, t.project.FindCategory(snd::HashId("C")), 0, 0};
  SND_CHECK(t.m.Set(cm, 1.0f));
  SND_CHECK_EQ(t.m.EffectiveCategoryDb(cm.a), kFaderOffDb);
  SND_CHECK_EQ(t.m.categories()[static_cast<std::size_t>(cm.a)].faderDb, -2.0f);
  CheckOverridesMatchCells(t.m);
}

// B8 / B9 / C3 / C5, randomized: after every edit, undo, redo, revert and revert all the
// override list matches the cells; undo + redo is the identity; revert all empties the
// list and its undo restores every cell.
SND_TEST(IndepToolMixer, RandomEditsKeepOverridesUndoAndRevertConsistent) {
  Tree t;
  SND_REQUIRE(t.ok);
  Rng rng{12345};
  const char* buses[] = {"Master", "A", "A1", "A2", "B"};
  const std::int32_t cats = static_cast<std::int32_t>(t.project.categories().size());
  for (int step = 0; step < 3000; ++step) {
    const std::vector<Cell> before = t.m.Cells();
    switch (rng.Below(12)) {
      case 0: t.m.Set(t.F(buses[rng.Below(5)]), rng.Uniform(-150.0f, 20.0f)); break;
      case 1: { const Target m = t.M(buses[rng.Below(5)]); t.m.Set(m, 1.0f - t.m.Get(m)); break; }
      case 2: { const Target s = t.S(buses[rng.Below(5)]); t.m.Set(s, 1.0f - t.m.Get(s)); break; }
      case 3: t.m.Set(Target{Control::EffectParam, t.B("B"), 0, static_cast<std::int32_t>(rng.Below(16))},
                      rng.Uniform(-50.0f, 30000.0f), rng.Below(3)); break;
      case 4: t.m.Set(Target{Control::CategoryFader, static_cast<std::int32_t>(rng.Below(static_cast<std::uint32_t>(cats))), 0, 0},
                      rng.Uniform(-100.0f, 12.0f)); break;
      case 5: { const Target p{Control::ParamOverrideOn, static_cast<std::int32_t>(rng.Below(2)), 0, 0};
                t.m.Set(p, 1.0f - t.m.Get(p)); break; }
      case 6: t.m.Set(Target{Control::ParamOverride, static_cast<std::int32_t>(rng.Below(2)), 0, 0},
                      rng.Uniform(-2.0f, 12.0f), 7u); break;
      case 7: {
        if (t.m.Undo()) {
          const std::vector<Cell> undone = t.m.Cells();
          SND_CHECK(t.m.Redo());
          SND_CHECK(SameCells(t.m.Cells(), before));
          SND_CHECK(t.m.Undo());
          SND_CHECK(SameCells(t.m.Cells(), undone));
        }
        break;
      }
      case 8: t.m.Redo(); break;
      case 9: {
        const std::vector<OverrideItem> items = t.m.Overrides();
        if (!items.empty()) {
          const OverrideItem it = items[rng.Below(static_cast<std::uint32_t>(items.size()))];
          t.m.Revert(it);
          for (const OverrideItem& o : t.m.Overrides()) SND_CHECK(o.key != it.key);  // that row is gone
        }
        break;
      }
      case 10: {
        if (t.m.RevertAll()) {
          SND_CHECK(t.m.Overrides().empty());
          SND_CHECK(t.m.Undo());
          SND_CHECK(SameCells(t.m.Cells(), before));
          SND_CHECK(t.m.Redo());
          SND_CHECK(t.m.Overrides().empty());
        }
        break;
      }
      case 11: {  // a drag: many moves, one undo step
        const std::size_t n0 = t.m.history().UndoCount();
        const Target f = t.F(buses[rng.Below(5)]);
        const float v0 = t.m.Get(f);
        t.m.BeginGroup();
        for (int i = 0; i < 5; ++i) t.m.Set(f, rng.Uniform(-100.0f, 12.0f));
        t.m.EndGroup();
        const bool changed = t.m.Get(f) != v0;
        SND_CHECK_EQ(t.m.history().UndoCount(), std::min<std::size_t>(512u, n0 + (changed ? 1u : 0u)));
        break;
      }
    }
    CheckOverridesMatchCells(t.m);
  }
}

// C1 / C2 / C4: groups, wheel merges, redo cleared by a new edit.
SND_TEST(IndepToolMixer, HistoryMergesAndRedoClearing) {
  Tree t;
  SND_REQUIRE(t.ok);
  const Target a = t.F("A"), b = t.F("B");
  t.m.Set(a, -4.0f, 9u);
  t.m.Set(a, -5.0f, 9u);
  t.m.Set(a, -6.0f, 9u);
  SND_CHECK_EQ(t.m.history().UndoCount(), 1u);
  t.m.Set(b, -1.0f, 10u);  // another control
  t.m.Set(a, -7.0f, 9u);   // A again after B: a new step
  SND_CHECK_EQ(t.m.history().UndoCount(), 3u);
  SND_CHECK(t.m.Undo());
  SND_CHECK_EQ(t.m.Get(a), -6.0f);
  SND_CHECK(t.m.Undo());
  SND_CHECK(t.m.Undo());
  SND_CHECK_EQ(t.m.Get(a), -3.0f);  // the bank value
  SND_CHECK_EQ(t.m.history().RedoCount(), 3u);
  t.m.Set(b, -2.0f);
  SND_CHECK_EQ(t.m.history().RedoCount(), 0u);
  SND_CHECK(!t.m.Redo());
  // A drag that ends where it started is no step.
  const std::size_t n = t.m.history().UndoCount();
  t.m.BeginGroup();
  t.m.Set(a, 0.0f);
  t.m.Set(a, -3.0f);
  t.m.EndGroup();
  SND_CHECK_EQ(t.m.history().UndoCount(), n);
}

// --- D: mappings ---------------------------------------------------------------------------

SND_TEST(IndepToolMath, FaderLawKnotsContinuityMonotonicityAndInverse) {
  using namespace snd::tool::ui;
  const float pos[] = {0.05f, 0.25f, 0.5f, 0.75f, 1.0f};
  const float db[] = {-60.0f, -30.0f, -12.0f, 0.0f, 12.0f};
  for (int i = 0; i < 5; ++i) {
    SND_CHECK_NEAR(FaderPosToDb(pos[i]), db[i], 1e-4);
    SND_CHECK_NEAR(FaderDbToPos(db[i]), pos[i], 1e-6);
    // continuous at the knot
    if (pos[i] < 1.0f) SND_CHECK_NEAR(FaderPosToDb(pos[i] + 1e-5f), db[i], 0.01);
    SND_CHECK_NEAR(FaderPosToDb(pos[i] - 1e-5f), db[i], 0.01);
  }
  SND_CHECK_EQ(FaderPosToDb(0.0f), kFaderOffDb);
  SND_CHECK_NEAR(FaderPosToDb(1e-6f), -90.0f, 0.01);  // 0+ = -90
  SND_CHECK_EQ(FaderPosToDb(-0.5f), kFaderOffDb);
  SND_CHECK_EQ(FaderPosToDb(1.5f), 12.0f);
  SND_CHECK_EQ(FaderDbToPos(30.0f), 1.0f);
  SND_CHECK_EQ(FaderDbToPos(kFaderOffDb), 0.0f);
  float prev = FaderPosToDb(1e-4f);
  for (int i = 2; i <= 10000; ++i) {
    const float p = static_cast<float>(i) * 1e-4f;
    const float d = FaderPosToDb(p);
    SND_CHECK(d > prev);  // strictly increasing on (0, 1]
    prev = d;
    SND_CHECK_NEAR(FaderDbToPos(d), p, 2e-6);  // inverse
  }
  for (float d = -89.5f; d <= 12.0f; d += 0.25f) SND_CHECK_NEAR(FaderPosToDb(FaderDbToPos(d)), d, 2e-3);
}

SND_TEST(IndepToolMath, WheelKnobAndMeterMappings) {
  using namespace snd::tool::ui;
  SND_CHECK_EQ(FaderWheel(kFaderOffDb, 1.0f, false), -90.0f);
  SND_CHECK_EQ(FaderWheel(kFaderOffDb, -1.0f, false), kFaderOffDb);
  SND_CHECK_EQ(FaderWheel(kFaderOffDb, 3.0f, false), -88.0f);
  SND_CHECK_EQ(FaderWheel(-90.0f, -1.0f, false), kFaderOffDb);
  SND_CHECK_NEAR(FaderWheel(-90.0f, -1.0f, true), kFaderOffDb, 0);  // -90.1 < -90
  SND_CHECK_NEAR(FaderWheel(-6.0f, 2.0f, true), -5.8f, 1e-5);
  SND_CHECK_EQ(FaderWheel(11.5f, 1.0f, false), 12.0f);
  SND_CHECK_EQ(FaderWheel(0.0f, 0.0f, false), 0.0f);
  // Knob: linear, logarithmic (Hz), integer.
  const KnobRange lin{-96.0f, 0.0f, false, false};
  SND_CHECK_NEAR(KnobToNorm(-48.0f, lin), 0.5f, 1e-6);
  SND_CHECK_NEAR(KnobFromNorm(0.25f, lin), -72.0f, 1e-4);
  const KnobRange hz{20.0f, 20000.0f, true, false};
  SND_CHECK_NEAR(KnobFromNorm(0.5f, hz), std::sqrt(20.0f * 20000.0f), 0.01);
  SND_CHECK_NEAR(KnobToNorm(200.0f, hz), 1.0f / 3.0f, 1e-5);
  SND_CHECK_EQ(KnobFromNorm(0.0f, hz), 20.0f);
  SND_CHECK_EQ(KnobFromNorm(1.0f, hz), 20000.0f);
  const KnobRange in{0.0f, 7.0f, false, true};
  SND_CHECK_EQ(KnobFromNorm(0.5f, in), 4.0f);  // 3.5 rounds
  SND_CHECK_EQ(KnobFromNorm(0.4f, in), 3.0f);  // 2.8 rounds
  // Meter: (dB + 60) / 60 clamped.
  SND_CHECK_EQ(MeterFraction(-60.0f), 0.0f);
  SND_CHECK_EQ(MeterFraction(-90.0f), 0.0f);
  SND_CHECK_NEAR(MeterFraction(-18.0f), 0.7f, 1e-6);
  SND_CHECK_NEAR(MeterFraction(-6.0f), 0.9f, 1e-6);
  SND_CHECK_EQ(MeterFraction(0.0f), 1.0f);
  SND_CHECK_EQ(MeterFraction(6.0f), 1.0f);
  SND_CHECK_EQ(MeterFraction(NAN), 0.0f);
  // Graph scale: max(1.25 budget, 1.1 max).
  SND_CHECK_NEAR(GraphScale::For(1000.0f, 500.0f).yMax, 1250.0f, 1e-3);
  SND_CHECK_NEAR(GraphScale::For(1000.0f, 2000.0f).yMax, 2200.0f, 1e-3);
}

SND_TEST(IndepToolMath, PeakHoldHoldsFallsAndLatchesClip) {
  const std::uint64_t s = 1000000000ull;
  PeakHold h;
  const float db = 20.0f * std::log10(0.5f);
  h.Update(0.5f, 10 * s);
  SND_CHECK_NEAR(h.HeldDb(10 * s + s), db, 1e-5);
  SND_CHECK_NEAR(h.HeldDb(10 * s + s * 3 / 2), db, 1e-5);         // held 1.5 s
  SND_CHECK_NEAR(h.HeldDb(10 * s + s * 5 / 2), db - 20.0f, 1e-3);  // then 20 dB/s
  h.Update(0.1f, 10 * s + s);  // lower during the hold: no change
  SND_CHECK_NEAR(h.HeldDb(10 * s + s * 5 / 2), db - 20.0f, 1e-3);
  SND_CHECK(!h.clipped());
  h.Update(0.9999f, 20 * s);
  SND_CHECK(!h.clipped());
  h.Update(-1.0f, 21 * s);  // 0 dBFS (either sign)
  SND_CHECK(h.clipped());
  h.Update(0.01f, 40 * s);
  SND_CHECK(h.clipped());  // latched
  h.Reset();
  SND_CHECK(!h.clipped());
}

// D8 / D9 through the profiler with analytic times: the hold follows the meter messages,
// the clip latches until Reset clips.
SND_TEST(IndepToolProfiler, MeterHoldAndClipThroughTheProfiler) {
  tooltest::ToolProject tool;
  indep::ScriptedGame g;
  SND_REQUIRE(tool.ok && g.ok);
  Session s;
  s.UseProject(tool.project);
  SND_REQUIRE_EQ(s.Connect(indep::GameConn(g, tool.project.buildHash())), SND_OK);
  SND_REQUIRE(indep::Drive(s, g, [&] { return s.connection().state() == LinkState::Online; }));
  ll::Client* c = s.connection().client();
  auto meter = [&](float peak) {
    ll::MeterEntry e{};
    e.busId = 0;
    e.channels = 2;
    e.peak[0] = e.peak[1] = peak;
    e.rms[0] = e.rms[1] = peak * 0.5f;
    const std::uint64_t n0 = c->Received(ll::MessageType::BusMeters);
    g.SendList(ll::MessageType::BusMeters, std::vector<ll::MeterEntry>{e});
    return indep::Drive(s, g, [&] { return c->Received(ll::MessageType::BusMeters) > n0; });
  };
  Profiler p;  // polled only here, with analytic times (the session's own profiler uses real time)
  const std::uint64_t t0 = 1000ull * 1000000000ull;
  const std::uint64_t sec = 1000000000ull;
  SND_REQUIRE(meter(0.5f));
  p.Poll(c, t0);
  const BusMeter* m = p.FindMeter(0);
  SND_REQUIRE(m != nullptr);
  const float db = 20.0f * std::log10(0.5f);
  SND_CHECK_NEAR(m->holdDb[0], db, 1e-4);
  SND_REQUIRE(meter(0.01f));  // -40 dB
  p.Poll(c, t0 + sec);
  SND_CHECK_NEAR(p.FindMeter(0)->holdDb[0], db, 1e-4);
  p.Poll(c, t0 + 2 * sec);
  SND_CHECK_NEAR(p.FindMeter(0)->holdDb[1], db - 10.0f, 1e-3);
  SND_CHECK(!p.FindMeter(0)->clipped);
  SND_REQUIRE(meter(1.0f));
  p.Poll(c, t0 + 3 * sec);
  SND_CHECK(p.FindMeter(0)->clipped);
  SND_REQUIRE(meter(0.01f));
  p.Poll(c, t0 + 10 * sec);
  SND_CHECK(p.FindMeter(0)->clipped);
  p.ResetClips();
  p.Poll(c, t0 + 11 * sec);
  SND_CHECK(!p.FindMeter(0)->clipped);
}

// D8: the hold is measured from the peak. When no new meter arrives (the game paused or the
// connection dropped) the last message must not keep restarting the hold at every Poll.
SND_TEST(IndepToolProfiler, StaleMeterDoesNotRestartThePeakHold) {
  tooltest::ToolProject tool;
  indep::ScriptedGame g;
  SND_REQUIRE(tool.ok && g.ok);
  Session s;
  s.UseProject(tool.project);
  SND_REQUIRE_EQ(s.Connect(indep::GameConn(g, tool.project.buildHash())), SND_OK);
  SND_REQUIRE(indep::Drive(s, g, [&] { return s.connection().state() == LinkState::Online; }));
  ll::Client* c = s.connection().client();
  ll::MeterEntry e{};
  e.busId = 0;
  e.channels = 1;
  e.peak[0] = 0.5f;
  g.SendList(ll::MessageType::BusMeters, std::vector<ll::MeterEntry>{e});
  SND_REQUIRE(indep::Drive(s, g, [&] { return c->Received(ll::MessageType::BusMeters) == 1u; }));
  const std::uint64_t t0 = 1000ull * 1000000000ull;
  Profiler p;  // polled only here, with analytic times
  for (int f = 0; f <= 180; ++f) p.Poll(c, t0 + static_cast<std::uint64_t>(f) * 16666667ull);  // 3 s at 60 Hz
  const float db = 20.0f * std::log10(0.5f);
  // 1.5 s hold, then 1.5 s at 20 dB/s: 30 dB below the peak.
  SND_CHECK_NEAR(p.FindMeter(0)->holdDb[0], db - 30.0f, 0.5);
}

// --- E: recording file ---------------------------------------------------------------------

namespace {

std::vector<std::uint8_t> Records() {
  std::vector<std::uint8_t> r;
  auto put = [&](const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    r.insert(r.end(), b, b + n);
  };
  const ll::RecordLod lod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, 100u};
  const ll::RecordBankMap bm{static_cast<std::uint32_t>(ll::RecordKind::BankMap), 7u, 0xABCDull};
  const ll::RecordStall st{static_cast<std::uint32_t>(ll::RecordKind::Stall), 3u, 150u};
  const ll::RecordLod lod2{static_cast<std::uint32_t>(ll::RecordKind::Lod), 2u, 180u};
  put(&lod, sizeof lod);
  put(&bm, sizeof bm);
  put(&st, sizeof st);
  put(&lod2, sizeof lod2);
  return r;
}

std::uint32_t Get32(const std::vector<std::uint8_t>& b, std::size_t o) {
  return static_cast<std::uint32_t>(b[o]) | static_cast<std::uint32_t>(b[o + 1]) << 8 |
         static_cast<std::uint32_t>(b[o + 2]) << 16 | static_cast<std::uint32_t>(b[o + 3]) << 24;
}
std::uint64_t Get64(const std::vector<std::uint8_t>& b, std::size_t o) {
  return static_cast<std::uint64_t>(Get32(b, o)) | static_cast<std::uint64_t>(Get32(b, o + 4)) << 32;
}

}  // namespace

SND_TEST(IndepToolRecording, HeaderLayoutChecksumAndRoundTrip) {
  // FNV-1a 64 reference values.
  SND_CHECK_EQ(Fnv1a64(nullptr, 0), 0xcbf29ce484222325ull);
  const std::uint8_t a[] = {'a'};
  SND_CHECK_EQ(Fnv1a64(a, 1), 0xaf63dc4c8601ec8cull);
  const std::uint8_t foobar[] = {'f', 'o', 'o', 'b', 'a', 'r'};
  SND_CHECK_EQ(Fnv1a64(foobar, 6), 0x85944171f73967e8ull);
  Recording r;
  r.info = RecordingInfo{kRecordingOverflow, 0x1111222233334444ull, 0x5555666677778888ull, 48000, 256, 3, 0x10001};
  r.records = Records();
  const std::vector<std::uint8_t> f = EncodeRecording(r);
  SND_REQUIRE_EQ(f.size(), 64u + r.records.size());
  SND_CHECK(std::memcmp(f.data(), "SNRC", 4) == 0);
  SND_CHECK_EQ(Get32(f, 4), 2u);  // version 2 (D-67a)
  SND_CHECK_EQ(Get32(f, 8), 64u);
  SND_CHECK_EQ(Get32(f, 12), kRecordingOverflow);
  SND_CHECK_EQ(Get64(f, 16), 0x1111222233334444ull);
  SND_CHECK_EQ(Get64(f, 24), 0x5555666677778888ull);
  SND_CHECK_EQ(Get32(f, 32), 48000u);
  SND_CHECK_EQ(Get32(f, 36), 256u);
  SND_CHECK_EQ(Get32(f, 40), 3u);
  SND_CHECK_EQ(Get32(f, 44), 0x10001u);
  SND_CHECK_EQ(Get64(f, 48), r.records.size());
  // The checksum covers the header without its own field (56 bytes), then the records.
  std::vector<std::uint8_t> covered(f.begin(), f.begin() + 56);
  covered.insert(covered.end(), r.records.begin(), r.records.end());
  SND_CHECK_EQ(Get64(f, 56), Fnv1a64(covered.data(), covered.size()));
  Recording back;
  std::string err;
  SND_REQUIRE(DecodeRecording(f.data(), f.size(), &back, &err));
  SND_CHECK(back.info == r.info);
  SND_CHECK(back.records == r.records);
  // Summary (protocol §7): 2 LOD, 1 stall, 1 bank map, blocks 100..180.
  const RecordingSummary sm = Summarize(back.records.data(), back.records.size());
  SND_CHECK(sm.valid);
  SND_CHECK_EQ(sm.lods, 2u);
  SND_CHECK_EQ(sm.stalls, 1u);
  SND_CHECK_EQ(sm.bankMaps, 1u);
  SND_CHECK_EQ(sm.arenas, 0u);
  SND_CHECK_EQ(sm.firstBlock, 100u);
  SND_CHECK_EQ(sm.lastBlock, 180u);
  // Empty recording.
  Recording empty;
  const std::vector<std::uint8_t> e = EncodeRecording(empty);
  SND_CHECK_EQ(e.size(), 64u);
  SND_CHECK(DecodeRecording(e.data(), e.size(), &back, &err));
}

SND_TEST(IndepToolRecording, TruncatedOrDamagedFilesAreRefused) {
  Recording r;
  r.info.sampleRate = 48000;
  r.records = Records();
  std::vector<std::uint8_t> f = EncodeRecording(r);
  Recording out;
  std::string err;
  for (std::size_t n = 0; n < f.size(); ++n) {
    err.clear();
    SND_CHECK(!DecodeRecording(f.data(), n, &out, &err));
    SND_CHECK(!err.empty());
  }
  // Every bit of the file is checked (version 2: the checksum covers the header too).
  auto checked = [](std::size_t) { return true; };
  for (std::size_t o = 0; o < f.size(); ++o) {
    if (!checked(o)) continue;
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<std::uint8_t> d = f;
      d[o] = static_cast<std::uint8_t>(d[o] ^ (1u << bit));
      const bool ok = DecodeRecording(d.data(), d.size(), &out, &err);
      // headerBytes grown by a bit can still be "in range" only if the record size matches;
      // it cannot here (record size is fixed), so every flip must be refused.
      SND_CHECK_MSG(!ok, "flip at %zu bit %d accepted", o, bit);
    }
  }
  // Through the files: a damaged file is refused by Profiler::LoadRecording.
  ProjectDir dir("indep-rec");
  const std::string path = (dir.dir() / "x.snrec").string();
  SND_REQUIRE(SaveRecording(path, r, &err));
  Profiler p;
  SND_CHECK(p.LoadRecording(path, &err));
  std::vector<std::uint8_t> bad = f;
  bad[64 + 3] ^= 0x40;
  dir.WriteBytes("bad.snrec", bad);
  SND_CHECK(!p.LoadRecording((dir.dir() / "bad.snrec").string(), &err));
  SND_CHECK(!p.LoadRecording((dir.dir() / "missing.snrec").string(), &err));
  // Append to the original valid encoding after the checks that need it intact.
  f.push_back(0);
  SND_CHECK(!DecodeRecording(f.data(), f.size(), &out, &err));
}

// D-67a: version 2's checksum covers the header, so damage to any header field (build
// hashes, rate, block size, platform, contract version, flags) is found; a version 1 file
// (checksum over the records only) is not read.
SND_TEST(IndepToolRecording, HeaderFieldsAreCoveredAndVersion1IsRefused) {
  Recording r;
  r.info = RecordingInfo{0, 0x1111222233334444ull, 0x5555666677778888ull, 48000, 256, 3, 0x10001};
  r.records = Records();
  const std::vector<std::uint8_t> f = EncodeRecording(r);
  Recording out;
  std::string err;
  SND_REQUIRE(DecodeRecording(f.data(), f.size(), &out, &err));
  struct Field {
    const char* name;
    std::size_t offset;
  };
  for (const Field fd : {Field{"flags", 12}, Field{"projectBuildHash", 16}, Field{"projectBuildHash.hi", 23},
                         Field{"runtimeBuildHash", 24}, Field{"sampleRate", 32}, Field{"blockSize", 36},
                         Field{"platform", 40}, Field{"contractVersion", 44}}) {
    std::vector<std::uint8_t> d = f;
    d[fd.offset] = static_cast<std::uint8_t>(d[fd.offset] + 1u);  // a plausible other value
    err.clear();
    SND_CHECK_MSG(!DecodeRecording(d.data(), d.size(), &out, &err), "%s change accepted", fd.name);
    SND_CHECK(err.find("checksum") != std::string::npos);
  }
  // The same file written as version 1 (checksum over the records only).
  std::vector<std::uint8_t> v1 = f;
  v1[4] = 1;
  const std::uint64_t old = Fnv1a64(r.records.data(), r.records.size());
  for (int i = 0; i < 8; ++i) v1[56 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(old >> (8 * i));
  SND_CHECK(!DecodeRecording(v1.data(), v1.size(), &out, &err));
}

// E5 / E6: "Load: ... 壊れていれば拒否". A file whose checksum is right but whose records
// cannot be read (unknown record kind, written by a broken game or edited and re-summed)
// is broken and must be refused, not loaded with an invalid summary.
SND_TEST(IndepToolRecording, LoadRefusesUnreadableRecordsAndKeepsThePrevious) {
  Recording r;
  r.records = Records();
  r.records[16] = 99;  // the second record's kind
  ProjectDir dir("indep-rec2");
  std::string err;
  const std::string path = (dir.dir() / "y.snrec").string();
  SND_REQUIRE(SaveRecording(path, r, &err));
  SND_CHECK(!Summarize(r.records.data(), r.records.size()).valid);
  Recording out;
  SND_CHECK(!LoadRecording(path, &out, &err));
  // The profiler keeps what it had loaded before.
  Recording good;
  good.info.sampleRate = 44100;
  good.records = Records();
  const std::string goodPath = (dir.dir() / "good.snrec").string();
  SND_REQUIRE(SaveRecording(goodPath, good, &err));
  Profiler p;
  SND_REQUIRE(p.LoadRecording(goodPath, &err));
  SND_CHECK(!p.LoadRecording(path, &err));
  SND_CHECK(p.loadedPath() == goodPath);
  SND_CHECK_EQ(p.loaded().info.sampleRate, 44100u);
  SND_CHECK(p.loaded().records == good.records);
  SND_CHECK(p.loadedSummary().valid);
  SND_CHECK_EQ(p.loadedSummary().lods, 2u);
}

// --- E7: project JSON fuzzing --------------------------------------------------------------

namespace {

std::vector<std::pair<std::string, std::string>> MixFiles() {
  ProjectDir d("indep-mix-src");
  tooltest::WriteMixProject(d);
  std::vector<std::pair<std::string, std::string>> out;
  for (const char* rel : {"project.json", "banks/Main.json", "buses/Master.json", "buses/Sfx.json", "buses/Music.json",
                          "categories/SE.json", "parameters/Knob.json", "events/Tone.json"}) {
    const std::vector<std::uint8_t> b = tooltest::ReadAll((d.dir() / rel).string());
    out.emplace_back(rel, std::string(b.begin(), b.end()));
  }
  return out;
}

std::string Mutate(std::string s, Rng& rng) {
  static const char* tokens[] = {"1e999", "-1e999", "-1", "4294967296", "null", "\"\"", "[]", "{}", "true",
                                 "\"Master\"", "\"Sfx\"", "\"Music\"", "NaN", "0.0", "1e-45", "[1,2,3]",
                                 "\"snd.fx.parameq\"", "\"snd.fx.unknown\"", "\"\\u0000\"", "\"\\ud800\""};
  const std::uint32_t n = 1 + rng.Below(4);
  for (std::uint32_t i = 0; i < n && !s.empty(); ++i) {
    const std::size_t at = rng.Below(static_cast<std::uint32_t>(s.size()));
    switch (rng.Below(6)) {
      case 0: s.resize(at); break;
      case 1: s[at] = static_cast<char>(rng.Below(256)); break;
      case 2: s.insert(at, tokens[rng.Below(20)]); break;
      case 3: s.erase(at, rng.Below(8)); break;
      case 4: {  // replace a value after a ':'
        const std::size_t colon = s.find(':', at);
        if (colon != std::string::npos) {
          const std::size_t end = s.find_first_of(",}", colon);
          s.replace(colon + 1, end == std::string::npos ? 0 : end - colon - 1, tokens[rng.Below(20)]);
        }
        break;
      }
      case 5: s.insert(at, std::string(rng.Below(3000), '[')); break;
    }
  }
  return s;
}

}  // namespace

namespace {

using Files = std::vector<std::pair<std::string, std::string>>;

// Loads the project, edits everything, ticks, revert all + undo, declares both screens
// and draws them. Child process: 0 fine, 2 the override list does not match the cells.
int Exercise(const Files& files, bool compose, bool checkKeys = true) {
  ProjectDir dir("indep-fuzz");
  for (const auto& f : files) dir.Write(f.first, f.second);
  Session s;
  s.LoadProject(dir.dir().string());
  MixerModel& m = s.mixer();
  for (std::size_t b = 0; b < s.project().buses().size(); ++b) {
    const std::int32_t bi = static_cast<std::int32_t>(b);
    m.Set(Target{Control::BusFader, bi, 0, 0}, -6.0f);
    m.Set(Target{Control::BusSolo, bi, 0, 0}, 1.0f);
    const auto& fx = s.project().buses()[b].effects;
    for (std::size_t e = 0; e < fx.size() && e < 4; ++e) {
      for (std::size_t p = 0; p < fx[e].params.size() && p < 8; ++p) {
        m.Set(Target{Control::EffectParam, bi, static_cast<std::int32_t>(e), static_cast<std::int32_t>(p)}, 0.5f);
      }
    }
  }
  for (std::size_t c = 0; c < s.project().categories().size(); ++c) {
    m.Set(Target{Control::CategoryMute, static_cast<std::int32_t>(c), 0, 0}, 1.0f);
  }
  for (std::size_t p = 0; p < s.project().parameters().size(); ++p) {
    m.Set(Target{Control::ParamOverride, static_cast<std::int32_t>(p), 0, 0}, 3.0f);
    m.Set(Target{Control::ParamOverrideOn, static_cast<std::int32_t>(p), 0, 0}, 1.0f);
  }
  s.Tick(indep::NowNs());
  std::set<std::string> keys;
  for (const OverrideItem& it : m.Overrides()) {
    if (!keys.insert(it.key).second && checkKeys) return 2;
  }
  snd::tool::ui::App app(s, snd::tool::ui::AppDesc{});
  app.Frame(indep::NowNs());
  tela::PixelSurface px;
  if (compose && !snd::tool::ui::Compose(app.runtime(), snd::tool::ui::ComposeOptions{}, &px)) return 3;
  app.OnScreen(snd::tool::ui::Screen::Profiler);
  app.Frame(indep::NowNs());
  m.RevertAll();
  m.Undo();
  app.OnScreen(snd::tool::ui::Screen::Mixer);
  app.Frame(indep::NowNs());
  if (compose && !snd::tool::ui::Compose(app.runtime(), snd::tool::ui::ComposeOptions{}, &px)) return 3;
  return 0;
}

// Two objects of one kind with the same name have the same runtime id (HashId(name)).
bool DuplicateIds(const Files& files) {
  ProjectDir dir("indep-fuzz-ids");
  for (const auto& f : files) dir.Write(f.first, f.second);
  Project p;
  p.Load(dir.dir().string());
  std::set<snd::Id> b, c, q;
  for (const BusDesc& d : p.buses()) {
    if (!b.insert(d.id).second) return true;
  }
  for (const CategoryDesc& d : p.categories()) {
    if (!c.insert(d.id).second) return true;
  }
  for (const ParameterDesc& d : p.parameters()) {
    if (!q.insert(d.id).second) return true;
  }
  return false;
}

}  // namespace

// E7: malformed projects (truncated, wrong types, extreme numbers, cycles, many parameters,
// random mutations) neither crash the models nor the UI; the override list stays
// consistent (projects with duplicate names included).
SND_TEST(IndepToolProject, FuzzedProjectsNeverCrashTheModelsOrTheUi) {
  const Files base = MixFiles();
  Rng rng{777};
  std::vector<Files> cases;
  {
    Files c = base;  // parent cycle X <-> Y, a second master
    c.emplace_back("buses/X.json", "{\"type\":\"bus\",\"guid\":\"" + G(50) + "\",\"name\":\"X\",\"parent\":\"Y\"}");
    c.emplace_back("buses/Y.json", "{\"type\":\"bus\",\"guid\":\"" + G(51) + "\",\"name\":\"Y\",\"parent\":\"X\"}");
    c.emplace_back("buses/Z.json", "{\"type\":\"bus\",\"guid\":\"" + G(52) + "\",\"name\":\"Z\",\"master\":true}");
    cases.push_back(c);
    Files d = base;  // an unknown effect with very many / huge parameters, extreme numbers
    std::string params;
    for (int i = 0; i < 70000; ++i) params += (i ? "," : "") + std::string(i % 3 == 0 ? "1e38" : "-3.4e38");
    d.emplace_back("buses/W.json", "{\"type\":\"bus\",\"guid\":\"" + G(53) +
                                       "\",\"name\":\"W\",\"parent\":\"Master\",\"volumeDb\":1e300,\"channels\":-5,"
                                       "\"effects\":[{\"type\":\"x\",\"params\":[" + params + "]},{\"params\":7}]}");
    d.emplace_back("parameters/R.json", "{\"type\":\"parameter\",\"guid\":\"" + G(54) +
                                            "\",\"name\":\"R\",\"min\":5,\"max\":-5,\"default\":1e39}");
    d.emplace_back("categories/D.json", "{\"type\":\"category\",\"guid\":\"" + G(55) +
                                            "\",\"name\":\"D\",\"parent\":\"D\",\"volumeDb\":\"loud\"}");
    cases.push_back(d);
    Files e = base;  // 60 buses in a chain, 40 categories, 30 parameters (paging, lists)
    std::string prev = "Master";
    for (int i = 0; i < 60; ++i) {
      const std::string n = "Bus" + std::to_string(i);
      e.emplace_back("buses/" + n + ".json", "{\"type\":\"bus\",\"guid\":\"" + G(static_cast<std::uint32_t>(200 + i)) +
                                                 "\",\"name\":\"" + n + "\",\"parent\":\"" + prev +
                                                 "\",\"effects\":[{\"type\":\"snd.fx.parameq\"}]}");
      prev = n;
    }
    for (int i = 0; i < 40; ++i) {
      e.emplace_back("categories/K" + std::to_string(i) + ".json",
                     "{\"type\":\"category\",\"guid\":\"" + G(static_cast<std::uint32_t>(300 + i)) + "\",\"name\":\"K" +
                         std::to_string(i) + "\"}");
    }
    for (int i = 0; i < 30; ++i) {
      e.emplace_back("parameters/V" + std::to_string(i) + ".json",
                     "{\"type\":\"parameter\",\"guid\":\"" + G(static_cast<std::uint32_t>(400 + i)) + "\",\"name\":\"V" +
                         std::to_string(i) + "\"}");
    }
    cases.push_back(e);
  }
  for (int i = 0; i < 300; ++i) {
    Files c = base;
    const std::uint32_t files = 1 + rng.Below(3);
    for (std::uint32_t k = 0; k < files; ++k) {
      auto& f = c[rng.Below(static_cast<std::uint32_t>(c.size()))];
      f.second = Mutate(f.second, rng);
    }
    cases.push_back(std::move(c));
  }
  std::uint32_t run = 0, skipped = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    if (DuplicateIds(cases[i])) ++skipped;  // run as well since D-67a (unique keys)
    const bool compose = i < 3 || i % 25 == 0;
    int r = indep::Isolated([&] { return Exercise(cases[i], compose); });
    if (r == indep::kNotIsolated) r = Exercise(cases[i], compose);
    ++run;
    SND_CHECK_MSG(r == 0, "case %zu: result %d", i, r);
  }
  SND_CHECK(run > 250u);
  std::printf("fuzz: %u cases run, %u of them with duplicate names\n", run, skipped);
}

// A project with two buses (or categories, parameters) of the same name is reported as an
// error by the build, but the tool still opens it ("the build reports errors, the tool
// still shows the project"). Their runtime ids and therefore the Tela element ids are the
// same, and Tela throws on duplicate ids: the tool ends in std::terminate.
SND_TEST(IndepToolProject, DuplicateNamesDoNotCrashTheUi) {
  Files c = MixFiles();
  c.emplace_back("buses/Sfx2.json", "{\"type\":\"bus\",\"guid\":\"" + G(60) + "\",\"name\":\"Sfx\",\"parent\":\"Master\"}");
  SND_REQUIRE(DuplicateIds(c));
  const int r = indep::Isolated([&] { return Exercise(c, true, false); });
  SND_CHECK_MSG(r == 0 || r == indep::kNotIsolated, "child result %d (abort = %d)", r, indep::kSignalBase + 6);
  // The override list has two rows with the same key: Revert(key) can only reach the first.
  const int k = indep::Isolated([&] {
    ProjectDir dir("indep-dup");
    for (const auto& f : c) dir.Write(f.first, f.second);
    Session s;
    s.LoadProject(dir.dir().string());
    for (std::size_t b = 0; b < s.project().buses().size(); ++b) {
      s.mixer().Set(Target{Control::BusFader, static_cast<std::int32_t>(b), 0, 0}, -6.0f);
    }
    std::set<std::string> keys;
    for (const OverrideItem& it : s.mixer().Overrides()) {
      if (!keys.insert(it.key).second) return 2;
    }
    return 0;
  });
  SND_CHECK_MSG(k == 0 || k == indep::kNotIsolated, "duplicate override keys (child result %d)", k);
}

// --- E1 / 11.3: voice order and the recording states with a scripted game ------------------

namespace {

struct Linked {
  tooltest::ToolProject tool;
  indep::ScriptedGame g;
  Session s;
  ll::Client* c = nullptr;
  Linked() {
    SND_REQUIRE(tool.ok && g.ok);
    s.UseProject(tool.project);
    SND_REQUIRE_EQ(s.Connect(indep::GameConn(g, tool.project.buildHash())), SND_OK);
    SND_REQUIRE(indep::Drive(s, g, [&] { return s.connection().state() == LinkState::Online; }));
    c = s.connection().client();
  }
};

}  // namespace

SND_TEST(IndepToolProfiler, VoicesRealFirstThenLoudestFirst) {
  Linked l;
  SND_REQUIRE(l.c != nullptr);
  std::vector<ll::VoiceInfo> v;
  const float aud[] = {0.2f, 0.9f, 0.5f, 0.7f, 0.1f, 0.95f, NAN, 0.3f};
  for (std::uint32_t i = 0; i < 8; ++i) {
    ll::VoiceInfo x{};
    x.eventId = 100 + i;
    x.audibility = aud[i];
    x.isVirtual = (i % 2 == 0) ? 1u : 0u;
    v.push_back(x);
  }
  SND_REQUIRE(l.g.SendList(ll::MessageType::VoiceList, v));
  SND_REQUIRE(indep::Drive(l.s, l.g, [&] { return l.c->Received(ll::MessageType::VoiceList) >= 1u; }));
  Profiler p;
  p.Poll(l.c, indep::NowNs());
  const std::vector<ll::VoiceInfo>& out = p.voices();
  SND_REQUIRE_EQ(out.size(), 8u);
  SND_CHECK_EQ(p.realVoices(), 4u);
  for (std::size_t i = 0; i < 4; ++i) SND_CHECK_EQ(out[i].isVirtual, 0u);
  for (std::size_t i = 4; i < 8; ++i) SND_CHECK(out[i].isVirtual != 0u);
  // real: 0.95, 0.9, 0.7, 0.3; virtual (finite ones in order): 0.5, 0.2, 0.1 (NaN anywhere)
  SND_CHECK_EQ(out[0].eventId, 105u);
  SND_CHECK_EQ(out[1].eventId, 101u);
  SND_CHECK_EQ(out[2].eventId, 103u);
  SND_CHECK_EQ(out[3].eventId, 107u);
  float last = 2.0f;
  for (std::size_t i = 4; i < 8; ++i) {
    if (std::isnan(out[i].audibility)) continue;
    SND_CHECK(out[i].audibility <= last);
    last = out[i].audibility;
  }
}

// 11.3 / README: Record -> (Ack) Recording; Stop -> (Ack) Stopping -> Ready once no data
// arrived for 100 ms; a refused start is Failed; Save writes the game's numbers.
SND_TEST(IndepToolProfiler, RecordingStatesFollowAcksAndSettle) {
  Linked l;
  SND_REQUIRE(l.c != nullptr);
  Profiler p;  // analytic times
  const std::uint64_t ms = 1000000ull, t0 = 5000ull * 1000ull * ms;
  l.g.holdAcks = true;
  SND_REQUIRE(p.StartRecording(l.c));
  SND_CHECK(p.recordingState() == RecordingState::Starting);
  SND_REQUIRE(indep::Drive(l.s, l.g, [&] { return l.g.heldAcks.size() == 1u; }));
  p.Poll(l.c, t0);
  SND_CHECK(p.recordingState() == RecordingState::Starting);  // no Ack yet
  l.g.ReleaseAcks();
  auto pollUntil = [&](std::uint64_t now, RecordingState st) {
    return indep::Drive(l.s, l.g, [&] {
      p.Poll(l.c, now);
      return p.recordingState() == st;
    });
  };
  SND_REQUIRE(pollUntil(t0 + 1 * ms, RecordingState::Recording));
  auto data = [&](std::uint64_t offset, std::uint32_t n) {
    std::vector<std::uint8_t> b(sizeof(ll::RecordData) + n, 0);
    const ll::RecordData rd{offset, n};
    std::memcpy(b.data(), &rd, sizeof rd);
    const ll::RecordLod lod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 1u, 10u + offset};
    for (std::uint32_t o = 0; o + 16u <= n; o += 16u) std::memcpy(b.data() + sizeof rd + o, &lod, 16);
    const std::uint64_t before = l.c->RecordingBytes();
    l.g.Send(ll::MessageType::RecordData, b.data(), static_cast<std::uint32_t>(b.size()));
    return indep::Drive(l.s, l.g, [&] { return l.c->RecordingBytes() == before + n; });
  };
  SND_REQUIRE(data(0, 32));
  SND_REQUIRE(p.StopRecording(l.c));
  SND_CHECK(p.recordingState() == RecordingState::Stopping);
  SND_REQUIRE(indep::Drive(l.s, l.g, [&] { return l.g.heldAcks.size() == 1u; }));
  l.g.ReleaseAcks();
  SND_REQUIRE(indep::Drive(l.s, l.g, [&] {
    p.Poll(l.c, t0 + 10 * ms);
    return l.g.heldAcks.empty() && p.recordedBytes() == 32u;
  }));
  // Data still arriving after the stop's Ack keeps it Stopping; Ready 100 ms after the last.
  SND_REQUIRE(data(32, 16));
  p.Poll(l.c, t0 + 60 * ms);
  SND_CHECK(p.recordingState() == RecordingState::Stopping);
  p.Poll(l.c, t0 + 150 * ms);
  SND_CHECK(p.recordingState() == RecordingState::Stopping);
  p.Poll(l.c, t0 + 160 * ms);
  SND_CHECK(p.recordingState() == RecordingState::Ready);
  SND_CHECK_EQ(p.recordedBytes(), 48u);
  // Save: the game's numbers in the header, the records as received.
  ProjectDir dir("indep-rec3");
  std::string err;
  const std::string path = (dir.dir() / "r.snrec").string();
  SND_REQUIRE(p.SaveRecording(l.c, l.s.CurrentRecordingInfo(), path, &err));
  Recording r;
  SND_REQUIRE(LoadRecording(path, &r, &err));
  SND_CHECK_EQ(r.info.sampleRate, 48000u);
  SND_CHECK_EQ(r.info.blockSize, 256u);
  SND_CHECK_EQ(r.info.runtimeBuildHash, l.g.runtimeHash);
  SND_CHECK_EQ(r.info.projectBuildHash, l.tool.project.buildHash());
  SND_CHECK_EQ(r.records.size(), 48u);
  SND_CHECK_EQ(Summarize(r.records.data(), r.records.size()).lods, 3u);
  // A refused start is Failed.
  l.g.holdAcks = false;
  l.g.ackResult = [](const indep::GameFrame& f) {
    return static_cast<std::int32_t>(f.Is(ll::MessageType::RecordControl) ? SND_ERR_INVALID_ARGUMENT : SND_OK);
  };
  SND_REQUIRE(p.StartRecording(l.c));
  SND_REQUIRE(pollUntil(t0 + 300 * ms, RecordingState::Failed));
  SND_CHECK_EQ(p.recordingError(), SND_ERR_INVALID_ARGUMENT);
}

// --- D-67a: stale meters ------------------------------------------------------------------

namespace {

bool SendMeter(Linked& l, float peak, float rms) {
  ll::MeterEntry e{};
  e.busId = 0;
  e.channels = 1;
  e.peak[0] = peak;
  e.rms[0] = rms;
  const std::uint64_t n0 = l.c->Received(ll::MessageType::BusMeters);
  l.g.SendList(ll::MessageType::BusMeters, std::vector<ll::MeterEntry>{e});
  return indep::Drive(l.s, l.g, [&] { return l.c->Received(ll::MessageType::BusMeters) > n0; });
}

}  // namespace

// A meter is stale only after more than 500 ms without a new one (500 ms exactly is fresh);
// then peak and rms fall at 20 dB/s from the 500 ms mark down to -inf, and the hold falls
// to the floor.
SND_TEST(IndepToolProfiler, StaleMeterBoundaryAt500MsAndFallToSilence) {
  Linked l;
  SND_REQUIRE(l.c != nullptr);
  SND_REQUIRE(SendMeter(l, 0.5f, 0.25f));
  Profiler p;
  const std::uint64_t ms = 1000000ull;
  p.Poll(l.c, indep::NowNs());
  const std::uint64_t r = p.FindMeter(0)->receivedNs;
  SND_REQUIRE(r != 0u);
  const float peakDb = LinearToDb(0.5f), rmsDb = LinearToDb(0.25f);
  p.Poll(l.c, r + 500 * ms);
  SND_CHECK(!p.FindMeter(0)->stale);
  SND_CHECK_EQ(p.FindMeter(0)->peak[0], 0.5f);
  SND_CHECK_EQ(p.FindMeter(0)->rms[0], 0.25f);
  p.Poll(l.c, r + 500 * ms + 1u);
  SND_CHECK(p.FindMeter(0)->stale);
  SND_CHECK_NEAR(LinearToDb(p.FindMeter(0)->peak[0]), peakDb, 1e-4);
  p.Poll(l.c, r + 1500 * ms);  // 1 s after the 500 ms mark: -20 dB
  SND_CHECK_NEAR(LinearToDb(p.FindMeter(0)->peak[0]), peakDb - 20.0f, 1e-3);
  SND_CHECK_NEAR(LinearToDb(p.FindMeter(0)->rms[0]), rmsDb - 20.0f, 1e-3);
  p.Poll(l.c, r + 10000 * ms);  // far below the floor: -inf
  SND_CHECK_EQ(p.FindMeter(0)->peak[0], 0.0f);
  SND_CHECK_EQ(p.FindMeter(0)->rms[0], 0.0f);
  SND_CHECK_EQ(p.FindMeter(0)->holdDb[0], kMeterFloorDb);
}

// The hold falls while the meters stop and restarts from the first new peak when they
// come back (hold 200 ms, 100 dB/s here; times are the real receive and poll times).
SND_TEST(IndepToolProfiler, MeterStopThenResumeRestartsTheHold) {
  Linked l;
  SND_REQUIRE(l.c != nullptr);
  Profiler::Desc d;
  d.hold.holdNs = 200000000ull;
  d.hold.fallDbPerSecond = 100.0f;
  Profiler p(d);
  const double sec = 1e9;
  SND_REQUIRE(SendMeter(l, 0.5f, 0.5f));
  const std::uint64_t t1 = indep::NowNs();
  p.Poll(l.c, t1);
  const std::uint64_t r1 = p.FindMeter(0)->receivedNs;
  const float a = LinearToDb(0.5f), b = LinearToDb(0.25f);
  SND_CHECK_NEAR(p.FindMeter(0)->holdDb[0], a, 1e-4);
  // The meters stop: wait past the stale mark (real time, the next meter must be newer).
  while (indep::NowNs() < r1 + 700000000ull) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const std::uint64_t t2 = indep::NowNs();
  p.Poll(l.c, t2);
  const BusMeter* m = p.FindMeter(0);
  SND_CHECK(m->stale);
  SND_CHECK_NEAR(m->holdDb[0], std::max(kMeterFloorDb, static_cast<float>(a - (static_cast<double>(t2 - t1) / sec - 0.2) * 100.0)), 0.05);
  const float fallen = static_cast<float>(a - (static_cast<double>(t2 - r1) / sec - 0.5) * 100.0);
  if (fallen > kMeterFloorDb) {
    SND_CHECK_NEAR(LinearToDb(m->peak[0]), fallen, 0.05);
  } else {
    SND_CHECK_EQ(m->peak[0], 0.0f);
  }
  // They come back with a lower peak: fresh again, the hold restarts at it.
  SND_REQUIRE(SendMeter(l, 0.25f, 0.25f));
  const std::uint64_t t3 = indep::NowNs();
  p.Poll(l.c, t3);
  m = p.FindMeter(0);
  SND_CHECK(m->receivedNs > r1);
  SND_CHECK(!m->stale || t3 - m->receivedNs > 500000000ull);
  SND_CHECK_NEAR(m->holdDb[0], b, 1e-4);
  if (!m->stale) SND_CHECK_EQ(m->peak[0], 0.25f);
  p.Poll(l.c, t3 + 150000000ull);  // inside the 200 ms hold
  SND_CHECK_NEAR(p.FindMeter(0)->holdDb[0], b, 1e-4);
  p.Poll(l.c, t3 + 400000000ull);  // 200 ms of fall at 100 dB/s
  SND_CHECK_NEAR(p.FindMeter(0)->holdDb[0], b - 20.0f, 1e-3);
}

// --- D-67a: keys of objects that share a name -----------------------------------------------

// Three levels: a unique runtime id keeps its hex key; the same name with another GUID gets
// "g" + GUID; the same name and the same GUID get "n" + position. Every row of the override
// list has its own key and Revert(key) reaches exactly that object; the UI declares and draws.
SND_TEST(IndepToolProject, SharedNamesGetDistinctKeysAndRevertReachesEachRow) {
  Files c = MixFiles();
  c.emplace_back("buses/Sfx2.json", "{\"type\":\"bus\",\"guid\":\"" + G(60) + "\",\"name\":\"Sfx\",\"parent\":\"Master\"}");
  c.emplace_back("buses/Sfx3.json", "{\"type\":\"bus\",\"guid\":\"" + G(60) + "\",\"name\":\"Sfx\",\"parent\":\"Master\"}");
  c.emplace_back("categories/SE2.json", "{\"type\":\"category\",\"guid\":\"" + G(61) + "\",\"name\":\"SE\"}");
  c.emplace_back("parameters/Knob2.json", "{\"type\":\"parameter\",\"guid\":\"" + G(8) + "\",\"name\":\"Knob\"}");
  ProjectDir dir("indep-keys");
  for (const auto& f : c) dir.Write(f.first, f.second);
  Session s;
  s.LoadProject(dir.dir().string(), dir.out().string());  // with the build, as the tool does
  const Project& p = s.project();
  std::vector<std::int32_t> sfx;
  std::set<std::string> busKeys;
  for (std::size_t i = 0; i < p.buses().size(); ++i) {
    SND_CHECK(busKeys.insert(p.buses()[i].key).second);
    if (p.buses()[i].name == "Sfx") sfx.push_back(static_cast<std::int32_t>(i));
    if (p.buses()[i].name == "Music") {
      char hex[16];
      std::snprintf(hex, sizeof hex, "%08x", p.buses()[i].id);
      SND_CHECK(p.buses()[i].key == hex);  // unique: unchanged
    }
  }
  SND_REQUIRE_EQ(sfx.size(), 3u);
  std::uint32_t gKeys = 0, nKeys = 0;
  for (std::int32_t i : sfx) {
    const std::string& k = p.buses()[static_cast<std::size_t>(i)].key;
    gKeys += k[0] == 'g' ? 1u : 0u;
    nKeys += k[0] == 'n' ? 1u : 0u;
    if (k[0] == 'g') SND_CHECK_EQ(k.size(), 33u);
  }
  SND_CHECK_EQ(gKeys, 1u);
  SND_CHECK_EQ(nKeys, 2u);
  SND_CHECK(p.categories().size() == 2u && p.categories()[0].key != p.categories()[1].key);
  SND_CHECK(p.parameters().size() == 2u && p.parameters()[0].key != p.parameters()[1].key);
  SND_CHECK(p.errorCount() > 0u);  // the build refuses the shared names

  MixerModel& m = s.mixer();
  float v = -3.0f;
  for (std::int32_t i : sfx) m.Set(Target{Control::BusFader, i, 0, 0}, v--);
  for (std::int32_t i = 0; i < 2; ++i) m.Set(Target{Control::CategoryFader, i, 0, 0}, -10.0f - static_cast<float>(i));
  for (std::int32_t i = 0; i < 2; ++i) m.Set(Target{Control::ParamOverrideOn, i, 0, 0}, 1.0f);
  std::set<std::string> keys;
  for (const OverrideItem& it : m.Overrides()) SND_CHECK(keys.insert(it.key).second);
  SND_CHECK_EQ(keys.size(), 7u);
  snd::tool::ui::App app(s, snd::tool::ui::AppDesc{});
  app.Frame(indep::NowNs());
  const tela::PlacedElement* hdr = uitest::Find(app.runtime(), "hdr.project");
  SND_CHECK(hdr != nullptr && hdr->element.label.find("errors") != std::string::npos);
  // Revert each row by key: that one object goes back, the others stay.
  while (!m.Overrides().empty()) {
    const std::vector<OverrideItem> before = m.Overrides();
    const OverrideItem first = before.front();
    app.OnRevert(first.key);
    const std::vector<OverrideItem> after = m.Overrides();
    SND_CHECK_EQ(after.size() + 1u, before.size());
    for (const OverrideItem& it : after) {
      SND_CHECK(it.key != first.key);
      for (const OverrideItem& o : before) {
        if (o.key == it.key) SND_CHECK_EQ(o.value, it.value);
      }
    }
    if (after.size() + 1u != before.size()) break;
  }
  const int r = indep::Isolated([&] {
    s.mixer().Set(Target{Control::BusSolo, sfx[1], 0, 0}, 1.0f);
    app.Frame(indep::NowNs());
    std::set<std::string> ids;
    for (const tela::PlacedElement& e : app.runtime().elements()) {
      if (!ids.insert(e.element.id).second) return 2;
    }
    tela::PixelSurface px;
    return snd::tool::ui::Compose(app.runtime(), snd::tool::ui::ComposeOptions{}, &px) ? 0 : 3;
  });
  SND_CHECK_MSG(r == 0 || r == indep::kNotIsolated, "child result %d", r);
}
