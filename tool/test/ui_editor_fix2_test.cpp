// The second round of decisions after the independent re-check (N1..N6): Esc discards a drag's
// group and leaves the history (undo and redo) as it was, integer fields refuse what is not a whole
// number of their type, the screens with number fields ask for a window of at least
// kMinEditorWidth x kMinEditorHeight, write back maps the overrides by object, displayed values
// are finite, and selections by position go when the document changes shape elsewhere.
#include <cmath>
#include <string>

#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/spatial_preview.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {

const std::string kShot = doctest::G(8), kSfx = doctest::G(3);

// Types `text` into the number field `field` and presses Enter: true if the entry closed (taken).
bool Enter(Editor& t, const std::string& field, const std::string& text) {
  if (!t.Click(field) || !t.app.text().active()) return false;
  t.ClearEntry();
  t.Type(text);  // and Enter
  const bool taken = !t.app.text().active();
  if (!taken) t.Key(ui::kKeyEscape);
  return taken;
}

// No placed text shows a non-finite number.
bool AllTextFinite(const Editor& t) {
  for (const tela::PlacedElement& e : t.app.runtime().elements()) {
    const std::string& s = e.element.label;
    for (const char* bad : {"inf", "nan", "INF", "NAN"}) {
      if (s.find(bad) != std::string::npos) return false;
    }
  }
  return true;
}

}  // namespace

// N1 / N2: a drag cancelled with Esc files no step and keeps the redo list.
SND_TEST(ToolUiFix2, EscKeepsUndoAndRedoAsTheyWere) {
  Editor t(ui::Screen::Event);
  EventEditor ed(t.doc, kShot);
  SND_REQUIRE(ed.SetNumber(EventNumber::Priority, 9));
  SND_REQUIRE(ed.SetNumber(EventNumber::Priority, 10));
  SND_REQUIRE(t.app.DocUndo());  // one step to redo
  t.app.view().event.msPerPx = 1.0;
  t.Frame();
  const std::size_t undo0 = t.doc.UndoCount(), redo0 = t.doc.RedoCount();
  const std::uint64_t rev0 = t.doc.revision();
  SND_REQUIRE(redo0 == 1u);
  const double start = ed.ClipNumberOf(0, 0, ClipNumber::StartMs);
  const tela::Point c = Center(t.El("ev.t0.c0"));
  // A drag that goes and comes back, then Esc.
  t.ptr.Down(c.x, c.y);
  t.ptr.Move(c.x + 40.0f, c.y);
  t.Frame();
  t.ptr.Move(c.x, c.y);
  t.Frame();
  t.Key(ui::kKeyEscape);
  t.ptr.Up(c.x, c.y);
  t.Frame();
  SND_CHECK_EQ(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), start);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0);
  SND_CHECK_EQ(t.doc.RedoCount(), redo0);
  SND_CHECK(!t.doc.InGroup());
  // A press without motion, then Esc: nothing at all.
  t.ptr.Down(c.x, c.y);
  t.Key(ui::kKeyEscape);
  t.ptr.Up(c.x, c.y);
  t.Frame();
  SND_CHECK_EQ(t.doc.UndoCount(), undo0);
  SND_CHECK_EQ(t.doc.RedoCount(), redo0);
  // A number field dragged and cancelled.
  t.app.text().Cancel();
  t.Frame();
  const tela::Point v = Center(t.El("ev.f.n1.value"));
  const double prio = ed.NumberOf(EventNumber::Priority);
  t.ptr.Down(v.x, v.y);
  t.ptr.Move(v.x, v.y - 30.0f);
  t.Frame();
  SND_CHECK(ed.NumberOf(EventNumber::Priority) != prio);
  t.Key(ui::kKeyEscape);
  t.ptr.Up(v.x, v.y - 30.0f);
  t.Frame();
  SND_CHECK_EQ(ed.NumberOf(EventNumber::Priority), prio);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0);
  SND_CHECK_EQ(t.doc.RedoCount(), redo0);
  SND_CHECK(!t.app.text().active());
  // The redo is still the one from before.
  SND_REQUIRE(t.app.DocRedo());
  SND_CHECK_EQ(ed.NumberOf(EventNumber::Priority), 10.0);
  SND_CHECK(t.doc.revision() > rev0);
  // AbortGroup outside a group does nothing.
  t.doc.AbortGroup();
  SND_CHECK_EQ(ed.NumberOf(EventNumber::Priority), 10.0);
}

SND_TEST(ToolUiFix2, EscOnAFaderKeepsTheLiveHistory) {
  Editor t(ui::Screen::Mixer);
  const std::int32_t sfx = t.s.project().FindBus(snd::HashId("Sfx"));
  const Target fader{Control::BusFader, sfx, 0, 0};
  SND_REQUIRE(t.s.mixer().Set(fader, -6.0f));
  SND_REQUIRE(t.s.mixer().Set(fader, -8.0f));
  SND_REQUIRE(t.s.mixer().Undo());
  t.Frame();
  const std::size_t undo0 = t.s.mixer().history().UndoCount(), redo0 = t.s.mixer().history().RedoCount();
  SND_REQUIRE(redo0 == 1u);
  const tela::PlacedElement* hit = t.El(ui::BusId(snd::HashId("Sfx")) + ".fader.hit");
  SND_REQUIRE(hit != nullptr);
  const tela::Point c = Center(hit);
  t.ptr.Down(c.x, c.y);
  t.ptr.Move(c.x, c.y - 40.0f);
  t.Frame();
  SND_CHECK(t.s.mixer().Get(fader) != -6.0f);
  t.Key(ui::kKeyEscape);
  t.ptr.Up(c.x, c.y - 40.0f);
  t.Frame();
  SND_CHECK_EQ(t.s.mixer().Get(fader), -6.0f);
  SND_CHECK_EQ(t.s.mixer().history().UndoCount(), undo0);
  SND_CHECK_EQ(t.s.mixer().history().RedoCount(), redo0);
}

// N3: integer fields take whole numbers of their type only; the entry stays open otherwise.
SND_TEST(ToolUiFix2, IntegerFieldsRefuseWhatIsNotAWholeNumberOfTheirType) {
  Editor t(ui::Screen::Buses);
  SND_REQUIRE(t.Click("mx.bus." + Key(kSfx)));
  const std::uint64_t rev0 = t.doc.revision();
  for (const char* bad : {"2.5", "-1", "1e300", "4294967296", "nan", "inf"}) {
    SND_CHECK_MSG(!Enter(t, "mx.bp.ch", bad), "channels took %s", bad);
  }
  SND_CHECK_EQ(t.doc.revision(), rev0);
  SND_CHECK(Enter(t, "mx.bp.ch", "2"));
  // The Link screen's port: 1 .. 65535.
  t.app.OnScreen(ui::Screen::Link);
  t.Frame();
  const std::uint16_t port0 = t.app.connectionDesc().port;
  for (const char* bad : {"0", "70000", "65536", "-5", "80.5", "1e300"}) {
    SND_CHECK_MSG(!Enter(t, "ln.port", bad), "port took %s", bad);
    SND_CHECK_EQ(t.app.connectionDesc().port, port0);
  }
  SND_CHECK(Enter(t, "ln.port", "65535"));
  SND_CHECK_EQ(t.app.connectionDesc().port, 65535u);
  // The helpers themselves.
  std::uint32_t u = 7;
  SND_CHECK(!ui::WholeU32(4294967296.0, &u) && u == 7u);
  SND_CHECK(!ui::WholeU32(std::nan(""), &u) && !ui::WholeU32(-1.0, &u) && !ui::WholeU32(0.5, &u));
  SND_CHECK(ui::WholeU32(4294967295.0, &u) && u == 4294967295u);
  SND_CHECK_EQ(ui::SatU32(1e300), 4294967295u);
  SND_CHECK_EQ(ui::SatU32(-1e300), 0u);
  SND_CHECK_EQ(ui::SatU32(std::nan("")), 0u);
}

// N4: below the smallest window the editors show only "window too small"; from it up every number
// field lies inside the window.
SND_TEST(ToolUiFix2, EditorsAskForTheirSmallestWindow) {
  Editor t(ui::Screen::Event, 640, 400);
  SND_CHECK(t.Has("body.small"));
  SND_CHECK(!t.Has("ev.f.n0"));
  t.app.OnScreen(ui::Screen::Mixer);  // one smallest window for every screen (third round)
  t.Frame();
  SND_CHECK(t.Has("body.small"));
  SND_CHECK(!t.Has("tab.mixer") && !t.Has("footer.problems") && !t.Has("footer.msg"));
  const int w = static_cast<int>(ui::kMinEditorWidth), h = static_cast<int>(ui::kMinEditorHeight);
  for (ui::Screen sc : {ui::Screen::Buses, ui::Screen::Event, ui::Screen::Params, ui::Screen::Spatial,
                        ui::Screen::Music, ui::Screen::Material, ui::Screen::Patch, ui::Screen::Link}) {
    t.app.SetViewport(w, h, 1.0f);
    t.app.OnScreen(sc);
    t.Frame();
    SND_CHECK(!t.Has("body.small"));
    for (const tela::PlacedElement& e : t.app.runtime().elements()) {
      const std::string& id = e.element.id;
      if (id.size() < 6 || id.compare(id.size() - 6, 6, ".value") != 0) continue;
      SND_CHECK_MSG(e.bounds.x + e.bounds.width <= static_cast<float>(w) + 0.5f &&
                        e.bounds.y + e.bounds.height <= static_cast<float>(h) + 0.5f,
                    "%s outside at %dx%d", id.c_str(), w, h);
    }
    t.app.SetViewport(w - 1, h, 1.0f);
    t.Frame();
    SND_CHECK(t.Has("body.small"));
  }
}

// N5: a live override whose effect was replaced by another type is not written into the new one.
SND_TEST(ToolUiFix2, WriteBackMapsOverridesByObject) {
  Editor t(ui::Screen::Mixer);
  const std::int32_t sfx = t.s.project().FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(t.s.mixer().Set(Target{Control::EffectParam, sfx, 0, 0}, 2000.0f));
  SND_REQUIRE(t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -9.0f));
  t.Frame();
  MixerEditor me(t.doc);
  t.doc.BeginGroup();
  SND_REQUIRE(me.RemoveEffect(kSfx, 0));
  SND_REQUIRE(me.AddEffect(kSfx, "snd.fx.highpass", 0));
  t.doc.EndGroup();
  t.Frame();
  SND_REQUIRE(t.app.rebindPending());
  const std::size_t msgs = t.s.messages().size();
  SND_REQUIRE(t.Click("rebind.writeback"));
  SND_CHECK(!t.app.rebindPending());
  const json::Value* fx = FindPath(t.doc.Find(kSfx)->doc, "effects");
  SND_REQUIRE(fx != nullptr && fx->IsArray() && fx->items.size() == 1u);
  const json::Value* p = FindPath(fx->items[0], "params");
  double p0 = 0.0;
  SND_CHECK(p == nullptr || !p->IsArray() || p->items.empty() || !p->items[0].AsDouble(&p0) || p0 != 2000.0);
  SND_CHECK_EQ(NumberAt(t.doc.Find(kSfx)->doc, "volumeDb", 0.0), -9.0);
  bool told = false;
  for (std::size_t i = msgs; i < t.s.messages().size(); ++i) told = told || t.s.messages()[i].find("discarded") != std::string::npos;
  SND_CHECK(told);
  SND_CHECK(t.s.mixer().Overrides().empty());
}

// N6: positions in the 3D preview stay within kMaxCoordinate; no text shows inf or nan.
SND_TEST(ToolUiFix2, SpatialValuesStayFinite) {
  Editor t(ui::Screen::Event);
  t.app.Navigate(kShot);
  t.app.OnScreen(ui::Screen::Spatial);
  t.Frame();
  SpatialPreview* sp = t.app.spatial();
  SND_REQUIRE(sp != nullptr);
  const float x0 = sp->listener().position.x;
  for (const char* bad : {"1e300", "2000000", "-1e7"}) {
    SND_CHECK_MSG(!Enter(t, "sp.l.x", bad), "listener x took %s", bad);
    SND_CHECK_EQ(sp->listener().position.x, x0);
  }
  SND_CHECK(Enter(t, "sp.l.x", "1000000"));
  SND_CHECK(Enter(t, "sp.e.z", "-1000000"));
  SND_CHECK(!Enter(t, "sp.e.yaw", "1e300"));
  t.Frame();
  SND_CHECK(AllTextFinite(t));
  SND_CHECK(!sp->PlaceEmitter(std::nan(""), 0.0));
  SND_CHECK(!sp->TurnListener(INFINITY));
  SND_CHECK_EQ(ui::FiniteText("x inf y -nan z"), std::string("x - y - z"));
}

// Decision 6 (third round): any change that does not come from the screens drops the selections by
// position, a value only too (judged by the revision); write back drops them too.
SND_TEST(ToolUiFix2, HostChangesDropSelections) {
  Editor t(ui::Screen::Event);
  EventEditor ed(t.doc, kShot);
  SND_REQUIRE(ed.AddAssetClip(0, doctest::G(7), 500.0));
  t.Frame();
  SND_REQUIRE(t.Click("ev.t0.c1"));
  SND_CHECK_EQ(t.app.view().event.clip, 1);
  t.app.view().event.msPerPx = 1.0;
  t.Frame();
  const tela::Point c = Center(t.El("ev.t0.c1"));  // the screen's own drag keeps it
  t.ptr.Down(c.x, c.y);
  t.ptr.Move(c.x + 10.0f, c.y);
  t.ptr.Up(c.x + 10.0f, c.y);
  t.Frame();
  SND_CHECK_EQ(t.app.view().event.clip, 1);
  SND_REQUIRE(ed.MoveClip(0, 1, 900.0));  // a host moves the selected clip: a value only
  t.Frame();
  SND_CHECK_EQ(t.app.view().event.clip, -1);
  SND_REQUIRE(t.Click("ev.t0.c0"));
  SND_CHECK_EQ(t.app.view().event.clip, 0);
  SND_REQUIRE(ed.RemoveClip(0, 1));  // structural
  t.Frame();
  SND_CHECK_EQ(t.app.view().event.clip, -1);
  SND_REQUIRE(t.Click("ev.t0.c0"));
  t.app.WriteBackMixer();
  t.Frame();
  SND_CHECK_EQ(t.app.view().event.clip, -1);
}
