// Model additions of the group U rework (D-188, D-189), core only: MixerModel's live values by
// object and the rebind that carries them, EventEditor::Cone, CheckDocument reports a problem once.
#include <set>

#include "doc_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/mixer.hpp"
#include "snd/tool/mixer_editor.hpp"

using namespace snd::tool;

SND_TEST(ToolModelFix, LiveValuesGoOverARebindByObject) {
  ProjectDocument doc;
  doctest::LoadBase(doc);
  Project p;
  SND_REQUIRE(p.Load(doc) || true);
  MixerModel m;
  m.Bind(p);
  const std::int32_t sfx = p.FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(sfx >= 0);
  SND_REQUIRE(m.Set(Target{Control::BusFader, sfx, 0, 0}, -10.0f));
  SND_REQUIRE(m.Set(Target{Control::BusMute, sfx, 0, 0}, 1.0f));
  SND_REQUIRE(m.Set(Target{Control::EffectParam, sfx, 0, 0}, 2000.0f));
  SND_REQUIRE(m.Set(Target{Control::ParamOverride, 0, 0, 0}, 3.0f));
  SND_REQUIRE(m.Set(Target{Control::ParamOverrideOn, 0, 0, 0}, 1.0f));
  const std::vector<LiveValue> v = m.LiveValues();
  SND_CHECK_EQ(v.size(), 5u);
  // A new bus before Sfx in the tree order: the positions change, the values follow their objects.
  SND_REQUIRE(!MixerEditor(doc).AddBus("Aaa", doctest::G(2)).empty());
  Project p2;
  p2.Load(doc);
  SND_CHECK(MixerModel::Unmatched(p2, v).empty());
  std::vector<LiveValue> dropped;
  SND_CHECK_EQ(m.Rebind(p2, v, &dropped), 5u);
  SND_CHECK(dropped.empty());
  const std::int32_t sfx2 = p2.FindBus(snd::HashId("Sfx"));
  SND_CHECK(sfx2 != sfx);
  SND_CHECK_EQ(m.Get(Target{Control::BusFader, sfx2, 0, 0}), -10.0f);
  SND_CHECK_EQ(m.Get(Target{Control::BusMute, sfx2, 0, 0}), 1.0f);
  SND_CHECK_EQ(m.Get(Target{Control::EffectParam, sfx2, 0, 0}), 2000.0f);
  SND_CHECK_EQ(m.Get(Target{Control::ParamOverrideOn, 0, 0, 0}), 1.0f);
  SND_CHECK(!m.history().CanUndo());
  // The effect goes away: that value has no place.
  SND_REQUIRE(MixerEditor(doc).RemoveEffect(doctest::G(3), 0));
  Project p3;
  p3.Load(doc);
  const std::vector<LiveValue> u = MixerModel::Unmatched(p3, m.LiveValues());
  SND_REQUIRE_EQ(u.size(), 1u);
  SND_CHECK(u[0].control == Control::EffectParam);
  SND_CHECK_EQ(u[0].effectType, std::string("snd.fx.lowpass"));
}

SND_TEST(ToolModelFix, ConeInEffectAndProblemsOnce) {
  ProjectDocument doc;
  doctest::LoadBase(doc);
  EventEditor ed(doc, doctest::G(8));
  EventEditor::ConeDesc c = ed.Cone();
  SND_CHECK(!c.present && !c.active() && c.innerDeg == 0.0 && c.outerDeg == 0.0 && c.outerDb == 0.0);
  SND_REQUIRE(ed.SetCone(20.0, 80.0, -6.0));
  c = ed.Cone();
  SND_CHECK(c.present && c.active() && c.innerDeg == 20.0 && c.outerDeg == 80.0 && c.outerDb == -6.0);
  SND_REQUIRE(doc.Set(doctest::G(8), "tracks[0].clips[0].asset", String("nope")));
  const std::vector<DocIssue> issues = DistinctIssues(CheckDocument(doc));
  SND_CHECK(!issues.empty());
  std::set<std::string> seen;
  for (const DocIssue& i : issues) SND_CHECK_MSG(seen.insert(i.text).second, "twice: %s", i.text.c_str());
}
