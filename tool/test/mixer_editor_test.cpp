// Mixer structure model: buses and the graph order (against the build's BUSS order),
// sends, effects (dsp ranges, like MixerModel), snapshots (capture, weighted result),
// ducking, categories, voice limit groups, write-back of the live mixer, undo / redo, the
// build output.
#include <cmath>

#include "doc_fixture.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/project.hpp"

using namespace snd::tool;
using namespace doctest;
namespace bf = snd::bank_format;

namespace {

double Cell1(const DocBuild& b, const ProjectDocument& doc, const std::string& guid, const std::string& path) {
  const std::vector<CellRef> c = LocateField(b, doc, guid, path);
  if (c.size() != 1u) return NAN;
  return c[0].type == CellType::F32 ? BitsToFloat(c[0].bits) : static_cast<double>(c[0].bits);
}

// Every bus is used by an event so that it is built into "Main".
void UseAllBuses(ProjectDocument& doc) {
  json::Value init = json::MakeObject();
  json::Add(init, "bank", String(G(1)));
  json::Value tracks = json::MakeArray();
  for (const DocObject* b : doc.OfKind("bus")) {
    json::Value tr = json::MakeObject();
    json::Add(tr, "bus", String(b->guid));
    tracks.items.push_back(tr);
  }
  json::Add(init, "tracks", tracks);
  doc.Create("event", "AllBuses", &init);
}

}  // namespace

SND_TEST(MixerEditor, BusTreeRulesAndGraphOrderMatchTheBuild) {
  ProjectDocument doc;
  LoadBase(doc);
  MixerEditor m(doc);
  SND_CHECK_EQ(m.MasterBus(), G(2));
  SND_CHECK(m.AddBus("Second", "").empty());  // a second root
  const std::string ui = m.AddBus("UI", G(2));
  const std::string uiSub = m.AddBus("UI/Sub", ui);
  SND_REQUIRE(!ui.empty() && !uiSub.empty());
  SND_CHECK(!m.SetParent(ui, uiSub));   // cycle
  SND_CHECK(!m.SetParent(G(2), ui));    // the master stays the root
  SND_CHECK(m.SetParent(uiSub, G(3)));  // under Sfx
  SND_CHECK(!m.AddSend(G(2), G(3), -6));  // Master -> Sfx: Sfx -> Master is its parent: cycle
  SND_CHECK(!m.AddSend(G(3), G(3), -6));  // to itself
  SND_CHECK(m.AddSend(ui, G(4), -10, true));
  SND_CHECK(!m.RemoveBus(G(3)));  // has children
  SND_CHECK(!m.RemoveBus(G(4)));  // a send target
  // Graph order vs the build's BUSS rows.
  UseAllBuses(doc);
  const std::vector<std::string> order = m.GraphOrder();
  SND_REQUIRE_EQ(order.size(), std::size_t{5});
  SND_CHECK_EQ(order.back(), G(2));  // master last
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  const ParsedBank* v = b.banks[0].view();
  SND_REQUIRE(v != nullptr && v->Rows(bf::chunk::BUSS) == 5u);
  for (std::uint32_t r = 0; r < 5; ++r) {
    SND_CHECK_EQ(v->U32(bf::chunk::BUSS, bf::col::BUSS_id, r), snd::HashId(doc.Find(order[r])->name()));
  }
  // Flags, channels, volume clamps.
  SND_CHECK(m.SetFlag(ui, BusFlag::Meter, true));
  SND_CHECK(m.SetFlag(ui, BusFlag::Meter, false));
  SND_CHECK(!m.SetChannels(ui, 9));
  SND_CHECK(m.SetVolume(ui, 40));
  SND_CHECK_EQ(NumberAt(doc.Find(ui)->doc, "volumeDb", 0), 12.0);
  SND_CHECK(!m.SetVolume(ui, NAN));
}

SND_TEST(MixerEditor, SendsAndEffects) {
  ProjectDocument doc;
  LoadBase(doc);
  MixerEditor m(doc);
  SND_REQUIRE(m.SetSendLevel(G(3), 0, -3));
  SND_REQUIRE(m.SetSendPreFader(G(3), 0, true));
  SND_CHECK(!m.SetSendLevel(G(3), 1, -3));
  SND_REQUIRE(m.AddEffect(G(3), "snd.fx.delay", 0));  // before the lowpass
  SND_CHECK_EQ(StringAt(doc.Find(G(3))->doc, "effects[1].type"), std::string("snd.fx.lowpass"));
  // Every delay parameter at the dsp default.
  const std::vector<EffectTypeInfo> types = StandardEffectTypes();
  const EffectTypeInfo* delay = nullptr;
  for (const EffectTypeInfo& t : types) {
    if (t.name == "snd.fx.delay") delay = &t;
  }
  SND_REQUIRE(delay != nullptr);
  SND_CHECK_EQ(FindPath(doc.Find(G(3))->doc, "effects[0].params")->items.size(), delay->params.size());
  // Range and rounding as MixerModel::Normalize.
  SND_REQUIRE(m.SetEffectParam(G(3), 1, 0, 1e9));  // lowpass cutoff beyond the range
  const json::Value& bus = doc.Find(G(3))->doc;
  SND_CHECK_EQ(NumberAt(bus, "effects[1].params[0]", 0), MixerEditor::NormalizeEffectParam("snd.fx.lowpass", 0, 1e9));
  SND_CHECK(MixerEditor::NormalizeEffectParam("snd.fx.lowpass", 0, 1e9) < 1e9);
  SND_REQUIRE(m.SetEffectParam(G(3), 1, 1, 0.9));  // the second lowpass parameter (absent): filled
  SND_CHECK_EQ(FindPath(bus, "effects[1].params")->items.size(), std::size_t{2});
  SND_CHECK(!m.SetEffectParam(G(3), 1, 7, 1));  // lowpass has 2 parameters
  Project p;
  SND_REQUIRE(p.Load(doc));
  MixerModel mix;
  mix.Bind(p);
  const std::int32_t sfx = p.FindBus(snd::HashId("Sfx"));
  const Target t{Control::EffectParam, sfx, 1, 0};
  SND_CHECK_EQ(mix.Normalize(t, 1e9f), static_cast<float>(NumberAt(bus, "effects[1].params[0]", 0)));
  SND_CHECK_EQ(mix.BankValue(t), static_cast<float>(NumberAt(bus, "effects[1].params[0]", 0)));
  // Move, flags, blob, remove; plugin types take any value.
  SND_REQUIRE(m.MoveEffect(G(3), 0, 1));
  SND_CHECK_EQ(StringAt(bus, "effects[1].type"), std::string("snd.fx.delay"));
  SND_REQUIRE(m.SetEffectFlags(G(3), 1, 1));
  SND_REQUIRE(m.SetEffectBlob(G(3), 1, "ir/hall.bin"));
  SND_REQUIRE(m.SetEffectBlob(G(3), 1, ""));
  SND_REQUIRE(m.AddEffect(G(3), "my.plugin", 2));
  SND_REQUIRE(m.SetEffectParam(G(3), 2, 3, 12345));
  SND_CHECK_EQ(NumberAt(bus, "effects[2].params[3]", 0), 12345.0);
  SND_REQUIRE(m.RemoveEffect(G(3), 2));
  SND_REQUIRE(m.RemoveSend(G(3), 0));
  // The build takes the result; the effect parameter is in EFPV.
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  SND_CHECK_NEAR(Cell1(b, doc, G(3), "effects[0].params[1]"), MixerEditor::NormalizeEffectParam("snd.fx.lowpass", 1, 0.9), 1e-6);
  // Undo all.
  while (doc.Undo()) {
  }
  SND_CHECK(doc.PendingChanges().empty());
}

SND_TEST(MixerEditor, SnapshotsCaptureAndWeightedResult) {
  ProjectDocument doc;
  LoadBase(doc);
  MixerEditor m(doc);
  const std::string loud = m.AddSnapshot("Loud", G(1));
  SND_REQUIRE(!loud.empty());
  SnapshotValue v;
  v.bus = G(3);
  v.value = 6;
  SND_REQUIRE(m.AddSnapshotValue(loud, v));
  SnapshotValue fx;
  fx.kind = SnapshotKind::EffectParam;
  fx.bus = G(3);
  fx.effect = 0;
  fx.param = 0;
  fx.value = 3000;
  SND_REQUIRE(m.AddSnapshotValue(loud, fx));
  SnapshotValue send;
  send.kind = SnapshotKind::SendLevel;
  send.bus = G(3);
  send.send = 0;
  send.value = 0;
  SND_REQUIRE(m.AddSnapshotValue(loud, send));
  SnapshotValue bad = send;
  bad.send = 5;
  SND_CHECK(!m.AddSnapshotValue(loud, bad));
  // Quiet: Sfx -6 dB. Weighted: Sfx = -1.5 + 0.5*(-6) + 0.25*6 = -3.0.
  const std::vector<SnapshotTarget> r = m.ApplySnapshots({{G(9), 0.5}, {loud, 0.25}});
  bool vol = false, eff = false, snd = false;
  for (const SnapshotTarget& t : r) {
    if (t.kind == SnapshotKind::BusVolume && t.bus == G(3)) {
      vol = true;
      SND_CHECK_NEAR(t.value, -3.0, 1e-12);
      SND_CHECK_NEAR(t.projectValue, -1.5, 1e-12);
    }
    if (t.kind == SnapshotKind::EffectParam) {
      eff = true;
      SND_CHECK_NEAR(t.value, 1000 + 0.25 * (3000 - 1000), 1e-9);
    }
    if (t.kind == SnapshotKind::SendLevel) {
      snd = true;
      SND_CHECK_NEAR(t.value, -12 + 0.25 * 12, 1e-9);
    }
  }
  SND_CHECK(vol && eff && snd);
  // Weights are clamped to 0..1.
  const std::vector<SnapshotTarget> r2 = m.ApplySnapshots({{G(9), 5.0}});
  SND_CHECK_NEAR(r2[0].value, -7.5, 1e-12);
  // Capture from the live mixer: Sfx fader at -4.5 (offset -3), lowpass at 500.
  Project p;
  SND_REQUIRE(p.Load(doc));
  MixerModel mix;
  mix.Bind(p);
  const std::int32_t sfx = p.FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(mix.Set(Target{Control::BusFader, sfx, 0, 0}, -4.5f));
  SND_REQUIRE(mix.Set(Target{Control::EffectParam, sfx, 0, 0}, 500.0f));
  SND_REQUIRE(m.CaptureSnapshot(loud, mix));
  const std::vector<SnapshotValue> got = m.SnapshotValues(loud);
  SND_REQUIRE_EQ(got.size(), std::size_t{2});
  SND_CHECK(got[0].kind == SnapshotKind::BusVolume && got[0].bus == G(3) && got[0].value == -3.0);
  SND_CHECK(got[1].kind == SnapshotKind::EffectParam && got[1].value == 500.0);
  // The build writes SNPV in order; the result of weight 1 reproduces the mixer's fader.
  const std::vector<SnapshotTarget> r3 = m.ApplySnapshots({{loud, 1.0}});
  SND_CHECK_NEAR(r3[0].value, -4.5, 1e-12);
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  SND_CHECK_EQ(Cell1(b, doc, loud, "values[0].value"), -3.0);
  SND_REQUIRE(m.RemoveSnapshotValue(loud, 1));
  SND_CHECK_EQ(m.SnapshotValues(loud).size(), std::size_t{1});
}

SND_TEST(MixerEditor, DuckingCategoriesAndLimitGroups) {
  ProjectDocument doc;
  LoadBase(doc);
  MixerEditor m(doc);
  SND_CHECK(m.AddDucking(G(1), DuckKind::Rule, G(3), G(4), -6).empty());  // a rule's source is a category
  const std::string d = m.AddDucking(G(1), DuckKind::Sidechain, G(3), G(4), -6);
  SND_REQUIRE(!d.empty());
  SND_CHECK(m.SetDucking(d, DuckField::AttackMs, 5));
  SND_CHECK(!m.SetDucking(d, DuckField::ReleaseMs, -1));
  SND_CHECK(m.SetDucking(d, DuckField::ThresholdDb, -24));
  SND_CHECK(m.SetDuckingKind(d, DuckKind::Rule, G(5)));
  SND_CHECK(!m.SetDuckingKind(d, DuckKind::Sidechain, G(5)));
  SND_CHECK(m.SetDuckingTarget(d, G(3)));
  const std::string ui = m.AddCategory("UI", "");
  const std::string menu = m.AddCategory("UI/Menu", ui);
  SND_REQUIRE(!ui.empty() && !menu.empty());
  SND_CHECK(!m.SetCategoryParent(ui, menu));  // cycle
  SND_CHECK(m.SetCategoryParent(menu, G(5)));
  SND_CHECK(m.SetCategoryParent(menu, ""));
  SND_CHECK(m.SetCategoryVolume(menu, -3));
  SND_CHECK(m.SetCategoryFlag(menu, true, true));
  SND_CHECK(m.SetCategoryFlag(menu, false, true));
  SND_CHECK(!BoolAt(doc.Find(menu)->doc, "ignorePause", false) && BoolAt(doc.Find(menu)->doc, "muteOnFocusLoss", false));
  SND_CHECK(!m.RemoveCategory(G(5)));  // referenced (Shot, the ducking rule)
  SND_CHECK(m.SetCategoryParent(menu, ui));
  SND_CHECK(!m.RemoveCategory(ui));    // has a child
  SND_CHECK(m.RemoveCategory(menu));
  SND_CHECK(m.AddLimitGroup("Steps", 0, StealMode::Oldest).empty());
  const std::string g = m.AddLimitGroup("Steps", 8, StealMode::Quietest);
  SND_REQUIRE(!g.empty());
  SND_CHECK(!m.SetMaxVoices(g, 0));
  SND_CHECK(m.SetMaxVoices(g, 4));
  SND_CHECK(m.SetSteal(g, StealMode::Never));
  SND_REQUIRE(doc.Set(G(8), "limitGroup", String(g)));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  SND_CHECK_EQ(Cell1(b, doc, d, "thresholdDb"), -24.0);
  SND_CHECK_EQ(Cell1(b, doc, d, "attackMs"), 5.0);
  SND_CHECK_EQ(Cell1(b, doc, g, "maxVoices"), 4.0);
  SND_CHECK_EQ(Cell1(b, doc, g, "steal"), 3.0);
  // The ducking cell's live address: the target bus and its rank.
  const std::vector<CellRef> c = LocateField(b, doc, d, "amountDb");
  SND_REQUIRE_EQ(c.size(), std::size_t{1});
  SND_CHECK(c[0].addressable && c[0].runtimeId == snd::HashId("Sfx") && c[0].index == 0u && c[0].liveWritable);
}

SND_TEST(MixerEditor, WriteBackOfTheLiveMixerIsOneStep) {
  ProjectDocument doc;
  LoadBase(doc);
  Project p;
  SND_REQUIRE(p.Load(doc));
  MixerModel mix;
  mix.Bind(p);
  const std::int32_t sfx = p.FindBus(snd::HashId("Sfx"));
  const std::int32_t se = p.FindCategory(snd::HashId("SE"));
  SND_REQUIRE(mix.Set(Target{Control::BusFader, sfx, 0, 0}, -8.0f));
  SND_REQUIRE(mix.Set(Target{Control::BusMute, 0, 0, 0}, 1.0f));  // monitoring state: not written
  SND_REQUIRE(mix.Set(Target{Control::EffectParam, sfx, 0, 0}, 750.0f));
  SND_REQUIRE(mix.Set(Target{Control::CategoryFader, se, 0, 0}, -2.0f));
  const std::size_t mixerSteps = mix.history().UndoCount();
  SND_CHECK_EQ(WriteBackMixer(doc, mix), 3u);
  SND_CHECK_EQ(doc.UndoCount(), std::size_t{1});
  SND_CHECK_EQ(mix.history().UndoCount(), mixerSteps);  // the mixer's history is its own
  SND_CHECK_EQ(NumberAt(doc.Find(G(3))->doc, "volumeDb", 0), -8.0);
  SND_CHECK_EQ(NumberAt(doc.Find(G(3))->doc, "effects[0].params[0]", 0), 750.0);
  SND_CHECK_EQ(NumberAt(doc.Find(G(5))->doc, "volumeDb", 0), -2.0);
  SND_CHECK(FindPath(doc.Find(G(2))->doc, "volumeDb") == nullptr);
  // Rebinding: the written values are the mixer's bank values, so no overrides remain
  // except the mute.
  Project p2;
  SND_REQUIRE(p2.Load(doc));
  MixerModel mix2;
  mix2.Bind(p2);
  SND_CHECK_EQ(mix2.BankValue(Target{Control::BusFader, sfx, 0, 0}), -8.0f);
  SND_CHECK(mix2.Overrides().empty());
  SND_CHECK(doc.Undo());
  SND_CHECK(doc.PendingChanges().empty());
  // Nothing to write: no step.
  MixerModel fresh;
  fresh.Bind(p);
  SND_CHECK_EQ(WriteBackMixer(doc, fresh), 0u);
  SND_CHECK_EQ(doc.UndoCount(), std::size_t{0});
}
