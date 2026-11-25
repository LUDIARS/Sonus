// Model changes of the group U third rework, core only: numbers no bank column holds are refused,
// float-to-integer conversions are checked before converting, the rebind asks when a bus's effect
// chain changed, AbortGroup in nested groups discards the outermost group.
#include <cmath>
#include <limits>

#include "doc_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/document.hpp"
#include "snd/tool/mixer.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"

using namespace snd::tool;

namespace {
const std::string kSfx = doctest::G(3), kSpeed = doctest::G(6);
}

SND_TEST(ToolModelFix3, NumbersNoBankColumnHoldsAreRefused) {
  ProjectDocument doc;
  doctest::LoadBase(doc);
  const std::uint64_t rev = doc.revision();
  for (double v : {1e308, -1e308, 1e39, -3.5e38}) {
    SND_CHECK(!doc.SetNumber(kSfx, "volumeDb", v));
    SND_CHECK(v > 0 ? !ParameterEditor(doc, kSpeed).SetRange(0.0, v) : !ParameterEditor(doc, kSpeed).SetRange(v, 0.0));
    SND_CHECK(!ParameterEditor(doc, kSpeed).SetDefault(v));
    SND_CHECK(!doc.Insert(kSfx, "sends", 0, Number(v)));
  }
  SND_CHECK_EQ(doc.revision(), rev);
  // The largest float and the tiny numbers still go in.
  const double fmax = static_cast<double>(std::numeric_limits<float>::max());
  SND_CHECK(doc.SetNumber(kSfx, "x-editor.big", fmax));
  SND_CHECK(doc.SetNumber(kSfx, "x-editor.tiny", 1e-308));
  SND_CHECK(ParameterEditor(doc, kSpeed).SetRange(-1e30, 1e30));
  // Modify may keep a value a file brought, but not bring a new one.
  SND_CHECK(!doc.Modify(kSfx, [](json::Value& d) {
    d.members.emplace_back("x-huge", Number(1e300));
    return true;
  }));
}

SND_TEST(ToolModelFix3, ConversionsAreCheckedBeforeConverting) {
  SND_CHECK_EQ(ClampToU32(-5.0), 0u);
  SND_CHECK_EQ(ClampToU32(std::nan("")), 0u);
  SND_CHECK_EQ(ClampToU32(1e300), 4294967295u);
  SND_CHECK_EQ(ClampToU32(7.9), 7u);
  SND_CHECK_EQ(ClampToU64(-1e300), 0u);
  SND_CHECK_EQ(ClampToU64(1e300), std::numeric_limits<std::uint64_t>::max());
  SND_CHECK_EQ(ClampToI32(-1e300), std::numeric_limits<std::int32_t>::min());
  SND_CHECK_EQ(ClampToI32(1e300), std::numeric_limits<std::int32_t>::max());
  SND_CHECK_EQ(ClampToI32(std::nan("")), 0);
  SND_CHECK_EQ(ClampToI32(-3.5), -3);
  // TempoMap::Musical of a far or non-finite tick: saturated, no undefined conversion.
  const TempoMap map;
  SND_CHECK_EQ(map.Musical(1e300).bar, std::numeric_limits<std::int32_t>::max());
  const MusicalPosition n = map.Musical(std::nan(""));
  SND_CHECK(n.bar == 0 || n.bar == std::numeric_limits<std::int32_t>::max() || n.bar == std::numeric_limits<std::int32_t>::min());
  // A file's tempo point with tick -5 and a bus with channels -5: read as 0.
  std::map<std::string, std::string> f = doctest::BaseFiles();
  f["segments/Seg.json"] = "{\"type\": \"segment\", \"guid\": \"" + doctest::G(22) +
                           "\", \"name\": \"Seg\", \"tempo\": [{\"tick\": -5, \"bpm\": 120, \"beatsPerBar\": -1, \"beatUnit\": 1e300}],"
                           " \"markers\": [{\"name\": \"M\", \"frame\": -7}]}\n";
  f["buses/Sfx.json"] = "{\"type\": \"bus\", \"guid\": \"" + kSfx + "\", \"name\": \"Sfx\", \"parent\": \"Master\", \"channels\": -5}\n";
  ProjectDocument doc;
  doctest::LoadBase(doc, f);
  SegmentEditor seg(doc, doctest::G(22));
  SND_REQUIRE(seg.Tempo().size() == 1u);
  SND_CHECK_EQ(seg.Tempo()[0].tick, 0u);
  SND_CHECK_EQ(seg.Tempo()[0].beatsPerBar, 0u);
  SND_CHECK_EQ(seg.Tempo()[0].beatUnit, 4294967295u);
  SND_REQUIRE(seg.Markers().size() == 1u);
  SND_CHECK_EQ(seg.Markers()[0].frame, 0u);
  Project p;
  p.Load(doc);
  const std::int32_t sfx = p.FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(sfx >= 0);
  SND_CHECK_EQ(p.buses()[static_cast<std::size_t>(sfx)].channels, 0u);
}

SND_TEST(ToolModelFix3, AChangedEffectChainIsNotCarriedOver) {
  std::map<std::string, std::string> f = doctest::BaseFiles();
  f["buses/Sfx.json"] = "{\"type\": \"bus\", \"guid\": \"" + kSfx + "\", \"name\": \"Sfx\", \"parent\": \"Master\", "
                        "\"effects\": [{\"type\": \"snd.fx.lowpass\"}, {\"type\": \"snd.fx.lowpass\"}]}\n";
  ProjectDocument doc;
  doctest::LoadBase(doc, f);
  Project p;
  p.Load(doc);
  MixerModel m;
  m.Bind(p);
  const std::int32_t sfx = p.FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(m.Set(Target{Control::EffectParam, sfx, 0, 0}, 500.0f));
  const std::vector<LiveValue> v = m.LiveValues();
  SND_REQUIRE(v.size() == 1u);
  SND_CHECK_EQ(v[0].effectChain.size(), 2u);
  // The same chain (another bus added): carried over.
  SND_REQUIRE(!MixerEditor(doc).AddBus("Aaa", doctest::G(2)).empty());
  Project p2;
  p2.Load(doc);
  SND_CHECK(MixerModel::Unmatched(p2, v).empty());
  // The first lowpass removed: the remaining one has the same type at index 0, but the chain changed.
  SND_REQUIRE(MixerEditor(doc).RemoveEffect(kSfx, 0));
  Project p3;
  p3.Load(doc);
  const std::vector<LiveValue> lost = MixerModel::Unmatched(p3, v);
  SND_REQUIRE(lost.size() == 1u);
  SND_CHECK(lost[0].name.find("lowpass") != std::string::npos);
  std::vector<LiveValue> dropped;
  SND_CHECK_EQ(m.Rebind(p3, v, &dropped), 0u);
  SND_CHECK_EQ(dropped.size(), 1u);
  // An effect appended: the order and count changed too.
  SND_REQUIRE(MixerEditor(doc).AddEffect(kSfx, "snd.fx.lowpass", 1));
  SND_REQUIRE(MixerEditor(doc).AddEffect(kSfx, "snd.fx.highpass", 2));
  Project p4;
  p4.Load(doc);
  SND_CHECK_EQ(MixerModel::Unmatched(p4, v).size(), 1u);
}

SND_TEST(ToolModelFix3, NestedAbortDiscardsTheOutermostGroup) {
  ProjectDocument doc;
  doctest::LoadBase(doc);
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -3.0));
  const std::size_t undo0 = doc.UndoCount(), redo0 = doc.RedoCount();
  doc.BeginGroup();
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -4.0));
  doc.BeginGroup();
  SND_REQUIRE(doc.SetNumber(doctest::G(2), "volumeDb", -5.0));
  doc.AbortGroup();
  SND_CHECK(!doc.InGroup());
  doc.EndGroup();  // the matching calls do nothing
  doc.EndGroup();
  SND_CHECK_EQ(NumberAt(doc.Find(kSfx)->doc, "volumeDb", 0.0), -3.0);
  SND_CHECK_EQ(NumberAt(doc.Find(doctest::G(2))->doc, "volumeDb", 0.0), 0.0);
  SND_CHECK_EQ(doc.UndoCount(), undo0);
  SND_CHECK_EQ(doc.RedoCount(), redo0);
}
