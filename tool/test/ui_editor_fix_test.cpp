// The decisions after the independent tests (D-186..D-189): typed numbers go to the model as typed,
// steps add up between declarations, the clip's track follows the vertical motion, number fields
// keep room, Pick copies the value, the entry takes every key, Esc is the tool's (and cancels a drag),
// selections by position are dropped when the document changes elsewhere, the mixer's live values go
// over a rebind by object (and ask when they cannot), the cone and the 3D defaults, the curve's
// margins, the Link screen without a connection, problems once, name keys of named items.
#include <cmath>
#include <set>

#include "snd/test/test.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {

const std::string kShot = doctest::G(8), kSfx = doctest::G(3);

void Retype(Editor& t, const std::string& field, const std::string& text) {
  SND_REQUIRE(t.Click(field));
  SND_REQUIRE(t.app.text().active());
  t.ClearEntry();
  t.Type(text);
}

}  // namespace

SND_TEST(ToolUiFix, TypedNumbersGoToTheModelAsTyped) {
  Editor t(ui::Screen::Event);
  EventEditor ed(t.doc, kShot);
  const std::size_t undo0 = t.doc.UndoCount();
  Retype(t, "ev.f.n4", "-5");  // max distance: the model refuses a negative distance
  SND_CHECK_EQ(ed.NumberOf(EventNumber::MaxDistance), 50.0);
  SND_CHECK(t.app.text().active());
  SND_CHECK_EQ(t.doc.UndoCount(), undo0);
  t.Key(ui::kKeyEscape);
  Retype(t, "ev.f.n1", "300");  // priority 0..255
  SND_CHECK(ed.NumberOf(EventNumber::Priority) != 300.0 && ed.NumberOf(EventNumber::Priority) != 255.0);
  SND_CHECK(t.app.text().active());
  t.Key(ui::kKeyEscape);
  Retype(t, "ev.f.n1", "77");
  SND_CHECK_EQ(ed.NumberOf(EventNumber::Priority), 77.0);
  SND_CHECK(!t.app.text().active());
  // The buttons still stop at the field's range.
  Retype(t, "ev.f.n1", "255");
  SND_REQUIRE(t.Click("ev.f.n1.inc"));
  SND_CHECK_EQ(ed.NumberOf(EventNumber::Priority), 255.0);
}

SND_TEST(ToolUiFix, WheelNotchesAndClicksBetweenDeclarationsAddUp) {
  Editor t(ui::Screen::Buses);
  SND_REQUIRE(t.Click("mx.bus." + Key(kSfx)));
  auto vol = [&] { return NumberAt(t.doc.Find(kSfx)->doc, "volumeDb", 0.0); };
  const double v0 = vol();
  const std::size_t undo0 = t.doc.UndoCount();
  const tela::Point c = Center(t.El("mx.bp.vol"));
  for (int i = 0; i < 5; ++i) t.ptr.Wheel(c.x, c.y, 1.0f);  // no declaration in between
  t.Frame();
  SND_CHECK_NEAR(vol(), v0 + 2.5, 1e-9);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0 + 1u);
  // Three clicks on "+" without a declaration in between: three steps.
  const tela::Point inc = Center(t.El("mx.bp.vol.inc"));
  for (int i = 0; i < 3; ++i) t.ptr.Click(inc.x, inc.y);
  t.Frame();
  SND_CHECK_NEAR(vol(), v0 + 4.0, 1e-9);
}

SND_TEST(ToolUiFix, ClipTrackFollowsTheVerticalMotion) {
  Editor t(ui::Screen::Event);
  EventEditor ed(t.doc, kShot);
  SND_REQUIRE(ed.AddTrack(0));
  SND_REQUIRE(ed.AddTrack(2));
  t.Frame();
  // Pressed near the top edge and moved 5 px up / near the bottom and 10 px down: the same track.
  for (const auto& [atTop, dy] : {std::pair<bool, float>{true, -5.0f}, {false, 10.0f}, {true, -23.0f}}) {
    const tela::PlacedElement* c = t.El("ev.t1.c0");
    SND_REQUIRE(c != nullptr);
    t.DragFrom(tela::Point{c->bounds.x + c->bounds.width * 0.5f, atTop ? c->bounds.y + 2.0f : c->bounds.y + c->bounds.height - 2.0f},
               0.0f, dy, 2);
    SND_CHECK_MSG(ed.ClipCount(1) == 1u, "a motion of %g px moved the clip", static_cast<double>(dy));
  }
  // 30 px down (round(30 / 48) = 1): the next track.
  const tela::PlacedElement* c = t.El("ev.t1.c0");
  t.DragFrom(tela::Point{c->bounds.x + 10.0f, c->bounds.y + 10.0f}, 0.0f, 30.0f, 3);
  SND_CHECK_EQ(ed.ClipCount(2), 1u);
  // Far beyond the last track: clamped to it; far above the first: the first.
  t.Drag("ev.t2.c0", 0.0f, 500.0f, 3);
  SND_CHECK_EQ(ed.ClipCount(2), 1u);
  t.Drag("ev.t2.c0", 0.0f, -500.0f, 3);
  SND_CHECK_EQ(ed.ClipCount(0), 1u);
}

SND_TEST(ToolUiFix, NumberFieldsKeepRoom) {
  for (const auto& [w, h] : {std::pair<int, int>{1280, 800}, {640, 400}}) {
    Editor t(ui::Screen::Mixer, w, h);
    t.app.view().patch.patch.clear();
    for (ui::Screen s : ui::kTabOrder) {
      for (std::uint32_t tab = 0; tab < 4u; ++tab) {
        t.app.view().screen = s;
        t.app.view().buses.tab = tab;
        t.app.view().music.tab = tab;
        t.Frame();
        for (const tela::PlacedElement& p : t.app.runtime().elements()) {
          const std::string& id = p.element.id;
          if (id.size() < 7 || id.compare(id.size() - 6, 6, ".value") != 0) continue;
          const std::string base = id.substr(0, id.size() - 6);
          bool hit = false;
          for (const auto& b : t.app.router().controls()) hit = hit || b.hitId == base;
          if (!hit) continue;  // a disabled field (text only)
          SND_CHECK_MSG(p.bounds.width >= 24.0f, "%dx%d %s: %s is %.0f px", w, h, ui::ScreenName(s), id.c_str(),
                        static_cast<double>(p.bounds.width));
        }
      }
    }
  }
}

SND_TEST(ToolUiFix, PickCopiesTheValue) {
  Editor t(ui::Screen::Event);
  std::string got;
  const std::string want = "a value long enough to live on the heap, item two";
  ui::Picker p;
  p.items = {ui::PickerItem{"one", "a value long enough to live on the heap, item one"}, ui::PickerItem{"two", want}};
  p.pick = [&got](const std::string& v) { got = v; };
  t.app.OpenPicker(std::move(p));
  t.app.Pick(t.app.picker()->items[1].value);
  SND_CHECK_EQ(got, want);
}

SND_TEST(ToolUiFix, TheEntryTakesEveryKeyAndEscIsTheTools) {
  Editor t(ui::Screen::Event);
  EventEditor ed(t.doc, kShot);
  SND_REQUIRE(ed.SetNumber(EventNumber::Priority, 9));
  t.Frame();
  const std::size_t undo0 = t.doc.UndoCount();
  SND_REQUIRE(t.Click("ev.list.name"));
  SND_REQUIRE(t.app.text().active());
  for (std::uint32_t key : {static_cast<std::uint32_t>('Z'), static_cast<std::uint32_t>('Y')}) {
    ui::KeyEvent k;
    k.key = key;
    k.modifiers = ui::kModControl;
    SND_CHECK(t.app.Key(k));
  }
  t.Key(ui::kKeyDelete);
  t.Key(ui::kKeyF1 + 1u);  // F2
  t.Key(ui::kKeyPageDown, ui::kModControl);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0);
  SND_CHECK(t.app.view().screen == ui::Screen::Event);
  SND_CHECK(t.app.text().active());
  t.Key(ui::kKeyEscape);
  SND_CHECK(!t.app.text().active());
  // Esc with nothing open: the tool takes it (true), nothing changes.
  ui::KeyEvent esc;
  esc.key = ui::kKeyEscape;
  SND_CHECK(t.app.Key(esc));
  // Esc during a drag: the drag's step is taken back.
  t.app.view().event.msPerPx = 1.0;
  t.Frame();
  const double start = ed.ClipNumberOf(0, 0, ClipNumber::StartMs);
  const tela::Point c = Center(t.El("ev.t0.c0"));
  t.ptr.Down(c.x, c.y);
  t.ptr.Move(c.x + 30.0f, c.y);
  t.Frame();
  SND_CHECK_NEAR(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), start + 30.0, 1e-9);
  SND_CHECK(t.app.Key(esc));
  SND_CHECK_EQ(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), start);
  SND_CHECK(!t.doc.InGroup());
  SND_CHECK_EQ(t.doc.UndoCount(), undo0);
  t.ptr.Up(c.x + 30.0f, c.y);
  t.ptr.Advance(1000);
  t.Frame();
  SND_CHECK_EQ(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), start);
}

SND_TEST(ToolUiFix, PositionSelectionsDropWhenTheDocumentChangesElsewhere) {
  Editor t(ui::Screen::Event);
  EventEditor ed(t.doc, kShot);
  SND_REQUIRE(ed.AddAssetClip(0, doctest::G(7), 500.0));  // A at 0, B at 500
  SND_REQUIRE(ed.RemoveClip(0, 0));                      // B
  t.Frame();
  SND_REQUIRE(t.Click("ev.t0.c0"));  // select B
  SND_CHECK_EQ(t.app.view().event.clip, 0);
  t.Key('Z', ui::kModControl);  // A comes back before B: the selection is dropped
  SND_CHECK_EQ(t.app.view().event.clip, -1);
  t.Key(ui::kKeyDelete);  // nothing selected: nothing removed
  SND_CHECK_EQ(ed.ClipCount(0), 2u);
  // Another screen's edit: back on the Event screen nothing is selected by position.
  SND_REQUIRE(t.Click("ev.t0.c0"));
  t.app.OnScreen(ui::Screen::Buses);
  t.Frame();
  t.app.OnScreen(ui::Screen::Event);
  t.Frame();
  SND_CHECK_EQ(t.app.view().event.clip, -1);
  // A selection by GUID stays while the object is there, and goes when it is removed.
  SND_CHECK(t.app.view().event.event == kShot);
}

SND_TEST(ToolUiFix, MixerOverridesGoOverARebindOrAsk) {
  Editor t(ui::Screen::Mixer);
  const std::int32_t sfx = t.s.project().FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -10.0f));
  SND_REQUIRE(t.s.mixer().Set(Target{Control::EffectParam, sfx, 0, 0}, 2000.0f));
  t.Frame();
  // A new bus: the overrides go over by GUID, no question.
  SND_REQUIRE(!MixerEditor(t.doc).AddBus("New", doctest::G(2)).empty());
  t.Frame();
  SND_CHECK_EQ(t.s.project().buses().size(), 4u);
  SND_CHECK(!t.app.rebindPending());
  const std::int32_t sfx2 = t.s.project().FindBus(snd::HashId("Sfx"));
  SND_CHECK_EQ(t.s.mixer().Get(Target{Control::BusFader, sfx2, 0, 0}), -10.0f);
  SND_CHECK_EQ(t.s.mixer().Get(Target{Control::EffectParam, sfx2, 0, 0}), 2000.0f);
  // The overridden effect goes away: the rebind waits and asks; the override stays meanwhile.
  SND_REQUIRE(MixerEditor(t.doc).RemoveEffect(kSfx, 0));
  t.Frame();
  SND_CHECK(t.app.rebindPending());
  SND_CHECK_EQ(t.app.rebindDropped().size(), 1u);
  SND_CHECK(t.Has("rebind.writeback") && t.Has("rebind.discard"));
  SND_CHECK_EQ(t.s.project().buses()[static_cast<std::size_t>(sfx2)].effects.size(), 1u);  // not yet
  // Undo brings the effect back: nothing to ask.
  SND_REQUIRE(t.app.DocUndo());
  t.Frame();
  SND_CHECK(!t.app.rebindPending());
  SND_REQUIRE(MixerEditor(t.doc).RemoveEffect(kSfx, 0));
  t.Frame();
  SND_REQUIRE(t.Click("rebind.discard"));
  SND_CHECK(!t.app.rebindPending());
  SND_CHECK(t.s.project().buses()[static_cast<std::size_t>(t.s.project().FindBus(snd::HashId("Sfx")))].effects.empty());
  SND_CHECK_EQ(t.s.mixer().Get(Target{Control::BusFader, t.s.project().FindBus(snd::HashId("Sfx")), 0, 0}), -10.0f);
  // Write back as the answer: the values into the document, no override left.
  SND_REQUIRE(t.s.mixer().Set(Target{Control::BusFader, 0, 0, 0}, -3.0f));
  t.Frame();
  const DocObject* nb = t.doc.FindByName("bus", "New");
  SND_REQUIRE(nb != nullptr);
  const std::int32_t nbi = t.s.project().FindBus(snd::HashId("New"));
  SND_REQUIRE(t.s.mixer().Set(Target{Control::BusFader, nbi, 0, 0}, -4.0f));
  t.Frame();
  SND_REQUIRE(t.doc.Remove(nb->guid, true));
  t.Frame();
  SND_REQUIRE(t.app.rebindPending());
  SND_REQUIRE(t.Click("rebind.writeback"));
  SND_CHECK(!t.app.rebindPending());
  SND_CHECK(t.s.mixer().Overrides().empty());
  SND_CHECK_EQ(NumberAt(t.doc.Find(doctest::G(2))->doc, "volumeDb", 0.0), -3.0);
}

SND_TEST(ToolUiFix, ConeCurveMarginsHandlesLinkAndProblems) {
  // An event without a cone: both screens say "no cone" alike (0 / 0 / 0 dB).
  std::map<std::string, std::string> f = doctest::BaseFiles();
  Editor t(ui::Screen::Event, 1280, 800, 1.0f, f);
  EventEditor ed(t.doc, kShot);
  SND_CHECK(!ed.Cone().present && !ed.Cone().active());
  SND_CHECK_EQ(ed.Cone().outerDeg, 0.0);
  SND_CHECK(t.Label("ev.f.cone.out.value").find("0 deg") == 0u);
  t.app.OnScreen(ui::Screen::Spatial);
  t.Frame();
  SND_CHECK(t.Label("sp.info").find("cone none") != std::string::npos);
  SND_REQUIRE(ed.SetCone(30.0, 90.0, -12.0));
  t.Frame();
  SND_CHECK(t.Label("sp.info").find("cone 30 / 90 deg -12 dB") != std::string::npos);
  // 3D: the default handles do not overlap the bodies.
  auto overlap = [&](const char* a, const char* b) {
    const tela::Rect ra = t.El(a)->bounds, rb = t.El(b)->bounds;
    return ra.x < rb.x + rb.width && rb.x < ra.x + ra.width && ra.y < rb.y + rb.height && rb.y < ra.y + ra.height;
  };
  SND_CHECK(!overlap("sp.listener.yaw", "sp.emitter"));
  SND_CHECK(!overlap("sp.emitter.yaw", "sp.listener"));
  // Curves: the end points sit inside the plot.
  t.app.OnScreen(ui::Screen::Params);
  t.Frame();
  const tela::PlacedElement* plot = t.El("pc.plot");
  for (const char* pt : {"pc.pt.0", "pc.pt.1"}) {
    const tela::Rect r = t.El(pt)->bounds;
    SND_CHECK(r.x >= plot->bounds.x && r.x + r.width <= plot->bounds.x + plot->bounds.width);
  }
  // Link without a connection (a document attached for the live reflection): no version.
  t.s.AttachDocument(&t.doc);
  t.app.OnScreen(ui::Screen::Link);
  t.Frame();
  SND_CHECK_EQ(t.Label("ln.check"), std::string("not connected"));
  SND_CHECK(t.Label("ln.features").find("not connected") != std::string::npos);
  t.s.AttachDocument(nullptr);
  // Problems: each problem once (a clip naming no asset).
  SND_REQUIRE(t.doc.Set(kShot, "tracks[0].clips[0].asset", String("nope")));
  const std::vector<DocIssue> issues = CheckDocument(t.doc);
  std::set<std::string> seen;
  for (const DocIssue& i : issues) SND_CHECK_MSG(seen.insert(i.text).second, "twice: %s", i.text.c_str());
  SND_CHECK(!issues.empty());
}

SND_TEST(ToolUiFix, NamedItemsKeepTheirIdsWhenMoved) {
  std::map<std::string, std::string> f = doctest::BaseFiles();
  f["materials/T.json"] = "{\"type\": \"materialTable\", \"guid\": \"" + doctest::G(300) + "\", \"name\": \"T\", \"bank\": \"Main\", "
                          "\"materials\": [\"a\", \"b\", \"c\"]}";
  Editor t(ui::Screen::Material, 1280, 800, 1.0f, f);
  const std::string b = "ma.mat." + ui::NameKey("b");
  const std::string bc = "ma.pair." + ui::NameKey("b") + "." + ui::NameKey("c");
  SND_REQUIRE(t.Has(b) && t.Has(bc));
  const float y0 = t.El(b)->bounds.y;
  SND_REQUIRE(MaterialEditor(t.doc, doctest::G(300)).MoveMaterial(1, 0));
  t.Frame();
  SND_CHECK(t.Has(b) && t.Has(bc));
  SND_CHECK(t.El(b)->bounds.y < y0);  // the same id, now first
  SND_REQUIRE(MaterialEditor(t.doc, doctest::G(300)).RemoveMaterial("a"));
  t.Frame();
  SND_CHECK(t.Has(b) && !t.Has("ma.mat." + ui::NameKey("a")));
}
