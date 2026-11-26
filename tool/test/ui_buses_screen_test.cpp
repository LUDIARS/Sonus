// The mixer structure screen: the bus tree, bus members, sends, the effect chain, snapshots (values,
// capture, weighted result), ducking, categories and voice limit groups, and the live mixer's
// write-back. Every edit through MixerEditor (one undo step), the session's mixer follows the
// document's buses.
#include <cmath>

#include "snd/test/test.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {
const std::string kMaster = doctest::G(2), kSfx = doctest::G(3), kReverb = doctest::G(4), kQuiet = doctest::G(9);
}

SND_TEST(ToolUiBuses, TreeAndBusMembers) {
  Editor t(ui::Screen::Buses);
  for (const std::string& g : {kMaster, kSfx, kReverb}) SND_CHECK(t.Has("mx.bus." + Key(g)));
  SND_CHECK(t.app.view().buses.bus == kMaster);
  // Tree order: Master first, its children by name (Reverb, Sfx), indented.
  SND_CHECK(t.El("mx.bus." + Key(kMaster))->bounds.y < t.El("mx.bus." + Key(kReverb))->bounds.y);
  SND_CHECK(t.El("mx.bus." + Key(kReverb))->bounds.y < t.El("mx.bus." + Key(kSfx))->bounds.y);
  SND_CHECK(t.Label("mx.bus." + Key(kSfx)).find("  Sfx") != std::string::npos);
  SND_REQUIRE(t.Click("mx.bus." + Key(kSfx)));
  SND_CHECK(t.app.view().buses.bus == kSfx);
  SND_CHECK(t.Has("mx.bp.vol") && t.Has("mx.bp.parent") && t.Has("mx.send.0.level") && t.Has("mx.fx.0.p0"));
  // A drag up of 40 px on the volume: 10 steps of 0.5 dB, one undo step.
  const std::size_t undo0 = t.doc.UndoCount();
  t.Drag("mx.bp.vol", 0.0f, -40.0f, 4);
  SND_CHECK_NEAR(NumberAt(t.doc.Find(kSfx)->doc, "volumeDb", 0.0), 3.5, 1e-9);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0 + 1u);
  // The live mixer was bound to the new project value (no override was held).
  const std::int32_t sfx = t.s.project().FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(sfx >= 0);
  SND_CHECK_NEAR(t.s.mixer().BankValue(Target{Control::BusFader, sfx, 0, 0}), 3.5f, 1e-5f);
  // Flags and parent.
  SND_REQUIRE(t.Click("mx.bp.flag2"));
  SND_CHECK(BoolAt(t.doc.Find(kSfx)->doc, "meter", false));
  SND_REQUIRE(t.Pick("mx.bp.parent", "Reverb"));
  SND_CHECK_EQ(StringAt(t.doc.Find(kSfx)->doc, "parent"), kReverb);
  // Reverb under Sfx would now be a cycle: refused (message, nothing changed).
  SND_REQUIRE(t.Click("mx.bus." + Key(kReverb)));
  SND_REQUIRE(t.Pick("mx.bp.parent", "Sfx"));
  SND_CHECK(StringAt(t.doc.Find(kReverb)->doc, "parent") != kSfx);
}

SND_TEST(ToolUiBuses, NewBusSendsAndEffects) {
  Editor t(ui::Screen::Buses);
  SND_REQUIRE(t.Click("mx.bus." + Key(kSfx)));
  const std::size_t buses0 = t.s.project().buses().size();
  SND_REQUIRE(t.Click("mx.bus.add"));
  const std::string nb = t.app.view().buses.bus;
  SND_REQUIRE(t.doc.Find(nb) != nullptr);
  SND_CHECK_EQ(t.doc.Find(nb)->name(), std::string("Bus 1"));
  SND_CHECK_EQ(StringAt(t.doc.Find(nb)->doc, "parent"), kSfx);
  SND_CHECK_EQ(t.s.project().buses().size(), buses0 + 1u);  // the mixer follows the document
  SND_CHECK(t.Has("mx.bus." + Key(nb)));
  // A send from the new bus to Reverb, its level and pre-fader.
  SND_REQUIRE(t.Pick("mx.send.add", "Reverb"));
  MixerEditor me(t.doc);
  const json::Value* sends = t.doc.Find(nb)->doc.Find("sends");
  SND_REQUIRE(sends != nullptr && sends->items.size() == 1u);
  SND_REQUIRE(t.Click("mx.send.0.pre"));
  SND_CHECK(BoolAt(t.doc.Find(nb)->doc, "sends[0].preFader", false));
  SND_REQUIRE(t.Click("mx.send.0.level.inc"));
  SND_CHECK_NEAR(NumberAt(t.doc.Find(nb)->doc, "sends[0].levelDb", 0.0), -11.5, 1e-9);
  SND_REQUIRE(t.Click("mx.send.0.remove"));
  SND_CHECK(t.doc.Find(nb)->doc.Find("sends") == nullptr || t.doc.Find(nb)->doc.Find("sends")->items.empty());
  // Effects: add two, every parameter is a field; move and remove.
  SND_REQUIRE(t.Pick("mx.fx.add", "snd.fx.lowpass"));
  SND_REQUIRE(t.Pick("mx.fx.add", "snd.fx.delay"));
  SND_CHECK_EQ(StringAt(t.doc.Find(nb)->doc, "effects[1].type"), std::string("snd.fx.delay"));
  SND_CHECK_EQ(t.app.view().buses.effectPage, 1u);
  for (int p = 0; p < 7; ++p) SND_CHECK_MSG(t.Has("mx.fx.1.p" + std::to_string(p)), "delay param %d", p);
  SND_REQUIRE(t.Click("mx.fx.1.up"));
  SND_CHECK_EQ(StringAt(t.doc.Find(nb)->doc, "effects[0].type"), std::string("snd.fx.delay"));
  SND_REQUIRE(t.Click("mx.fx.0.p0.inc"));
  SND_CHECK(me.EffectParam(nb, 0, 0) > 0.0);
  SND_REQUIRE(t.Click("mx.fx.0.remove"));
  SND_CHECK_EQ(StringAt(t.doc.Find(nb)->doc, "effects[0].type"), std::string("snd.fx.lowpass"));
  // Remove the bus again.
  SND_REQUIRE(t.Click("mx.bus.remove"));
  SND_CHECK(t.doc.Find(nb) == nullptr);
  SND_CHECK_EQ(t.s.project().buses().size(), buses0);
}

SND_TEST(ToolUiBuses, SnapshotsValuesCaptureAndResult) {
  Editor t(ui::Screen::Buses);
  t.app.view().buses.tab = 1;
  t.Frame();
  SND_CHECK(t.app.view().buses.snapshot == kQuiet);
  MixerEditor me(t.doc);
  SND_CHECK_EQ(me.SnapshotValues(kQuiet).size(), 1u);
  SND_CHECK(t.Has("mx.sv.0.value") && t.Has("mx.sr.w." + Key(kQuiet)));
  // Weight 0.5: Sfx -1.5 dB -> -1.5 + 0.5 x -6 = -4.5.
  for (int i = 0; i < 5; ++i) SND_REQUIRE(t.Click("mx.sr.w." + Key(kQuiet) + ".inc"));
  SND_CHECK(t.Label("mx.sr.0").find("-1.5 -> -4.5") != std::string::npos);
  SND_REQUIRE(t.Click("mx.sv.add"));
  SND_CHECK_EQ(me.SnapshotValues(kQuiet).size(), 2u);
  SND_REQUIRE(t.Pick("mx.sv.1.bus", "Sfx"));  // Sfx has a send (the master has none)
  SND_REQUIRE(t.Pick("mx.sv.1.kind", "sendLevel"));
  SND_CHECK(me.SnapshotValues(kQuiet)[1].kind == SnapshotKind::SendLevel);
  SND_REQUIRE(t.Click("mx.sv.1.remove"));
  // Capture: the live mixer's Sfx at -10 dB -> busVolume -8.5.
  const std::int32_t sfx = t.s.project().FindBus(snd::HashId("Sfx"));
  t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -10.0f);
  SND_REQUIRE(t.Click("mx.sv.capture"));
  const std::vector<SnapshotValue> v = me.SnapshotValues(kQuiet);
  SND_REQUIRE_EQ(v.size(), 1u);
  SND_CHECK_NEAR(v[0].value, -8.5, 1e-5);
  // New snapshot.
  SND_REQUIRE(t.Click("mx.snap.list.new"));
  SND_CHECK(t.app.view().buses.snapshot != kQuiet);
}

SND_TEST(ToolUiBuses, DuckingCategoriesGroupsAndWriteBack) {
  Editor t(ui::Screen::Buses);
  t.app.view().buses.tab = 2;
  t.Frame();
  SND_REQUIRE(t.Click("mx.duck.list.new"));
  const std::string dk = t.app.view().buses.ducking;
  SND_REQUIRE(!dk.empty() && t.doc.Find(dk) != nullptr);
  SND_REQUIRE(t.Click("mx.dk.f0.dec"));
  SND_CHECK_NEAR(NumberAt(t.doc.Find(dk)->doc, "amountDb", 0.0), -6.5, 1e-9);
  SND_REQUIRE(t.Click("mx.dk.kind"));  // to a rule: the source becomes the first category
  SND_CHECK_EQ(StringAt(t.doc.Find(dk)->doc, "kind"), std::string("rule"));
  SND_CHECK_EQ(StringAt(t.doc.Find(dk)->doc, "source"), doctest::G(5));
  // Categories.
  SND_REQUIRE(t.Click("mx.tab.3"));
  SND_REQUIRE(t.Click("mx.cat.list.new"));
  const std::string cat = t.app.view().buses.category;
  SND_CHECK_EQ(t.doc.Find(cat)->name(), std::string("Category 1"));
  SND_REQUIRE(t.Pick("mx.ct.parent", "SE"));
  SND_CHECK_EQ(StringAt(t.doc.Find(cat)->doc, "parent"), doctest::G(5));
  SND_REQUIRE(t.Click("mx.ct.focus"));
  SND_CHECK(BoolAt(t.doc.Find(cat)->doc, "muteOnFocusLoss", false));
  SND_REQUIRE(t.Click("mx.grp.list.new"));
  const std::string grp = t.app.view().buses.group;
  SND_REQUIRE(t.Click("mx.grp.max.inc"));
  SND_CHECK_EQ(NumberAt(t.doc.Find(grp)->doc, "maxVoices", 0.0), 9.0);
  SND_REQUIRE(t.Pick("mx.grp.steal", "quietest"));
  SND_CHECK_EQ(StringAt(t.doc.Find(grp)->doc, "steal"), std::string("quietest"));
  // Write back: the live Sfx fader into the document, one step; the mixer then holds no override.
  const std::int32_t sfx = t.s.project().FindBus(snd::HashId("Sfx"));
  t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -7.0f);
  t.Frame();
  const std::size_t undo0 = t.doc.UndoCount();
  SND_REQUIRE(t.Click("mx.writeback"));
  SND_CHECK_NEAR(NumberAt(t.doc.Find(kSfx)->doc, "volumeDb", 0.0), -7.0, 1e-5);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0 + 1u);
  SND_CHECK(t.s.mixer().Overrides().empty());
}
