// Independent tests of group U, third re-check (indep_s7_ui_properties.md N), after the third return:
// the smallest window 1024 x 640 (logical, any DPI), selections dropped by revision, numbers that do
// not fit the bank refused, ranged conversions of broken files, the question when a bus's effect
// chain changes, names that read "inf" / "nan".
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"

using namespace iu;

namespace {

std::map<std::string, std::string> Rich() { return LargeFiles(3, 2, 6, 4, 12); }

bool TypeInto(Rig& r, const std::string& id, const std::string& text) {
  if (!r.ClickId(id) || !r.app->text().active()) return false;
  for (int i = 0; i < 300; ++i) r.app->Key(ui::KeyEvent{ui::kKeyBackspace, 0, 0});
  r.Type(text);
  r.Key(ui::kKeyEnter);
  return true;
}

// Every screen with its tabs and selections (also an open list and the rebind question).
struct Shot {
  const char* name;
  void (*set)(Rig&);
};
void ScreenOnly(Rig& r, ui::Screen s) { r.app->OnScreen(s); }
const Shot kShots[] = {
    {"mixer", [](Rig& r) { ScreenOnly(r, ui::Screen::Mixer); }},
    {"profiler0", [](Rig& r) { r.app->view().profiler.tab = 0; ScreenOnly(r, ui::Screen::Profiler); }},
    {"profiler1", [](Rig& r) { r.app->view().profiler.tab = 1; ScreenOnly(r, ui::Screen::Profiler); }},
    {"profiler2", [](Rig& r) { r.app->view().profiler.tab = 2; ScreenOnly(r, ui::Screen::Profiler); }},
    {"profiler3", [](Rig& r) { r.app->view().profiler.tab = 3; ScreenOnly(r, ui::Screen::Profiler); }},
    {"profiler4", [](Rig& r) { r.app->view().profiler.tab = 4; ScreenOnly(r, ui::Screen::Profiler); }},
    {"profiler5", [](Rig& r) { r.app->view().profiler.tab = 5; ScreenOnly(r, ui::Screen::Profiler); }},
    {"replay", [](Rig& r) { ScreenOnly(r, ui::Screen::Replay); }},
    {"problems", [](Rig& r) { ScreenOnly(r, ui::Screen::Problems); }},
    {"link0", [](Rig& r) { r.app->view().link.tab = 0; ScreenOnly(r, ui::Screen::Link); }},
    {"link1", [](Rig& r) { r.app->view().link.tab = 1; ScreenOnly(r, ui::Screen::Link); }},
    {"buses", [](Rig& r) { r.app->Navigate(G(3)); }},
    {"snapshots", [](Rig& r) { r.app->Navigate(G(9)); }},
    {"ducking", [](Rig& r) { r.app->Navigate(G(3)); r.app->view().buses.tab = 2; }},
    {"categories", [](Rig& r) { r.app->Navigate(G(5)); }},
    {"event", [](Rig& r) { r.app->Navigate(G(8)); r.Frame(); r.ClickId("ev.t0.c0"); }},
    {"big event", [](Rig& r) { r.app->Navigate(G(21)); }},
    {"params", [](Rig& r) { r.app->Navigate(G(6)); r.Frame(); if (r.app->view().params.event.empty()) r.app->view().params.event = G(8); r.Frame(); r.ClickId("pc.pt.0"); }},
    {"spatial", [](Rig& r) { r.app->Navigate(G(8)); r.app->OnScreen(ui::Screen::Spatial); }},
    {"segment", [](Rig& r) { r.app->Navigate(G(22)); }},
    {"playlist", [](Rig& r) { r.app->Navigate(G(23)); }},
    {"rules", [](Rig& r) { r.app->Navigate(G(24)); }},
    {"stingers", [](Rig& r) { r.app->Navigate(G(24)); r.app->view().music.tab = 3; }},
    {"material", [](Rig& r) { r.app->Navigate(G(25)); }},
    {"patch", [](Rig& r) { r.app->Navigate(G(26)); r.Frame(); r.ClickId("pa.node.osc0"); }},
    {"picker", [](Rig& r) { r.app->Navigate(G(8)); r.Frame(); r.ClickId("ev.f.bus"); }},
    {"rebind question", [](Rig& r) {
       r.app->OnScreen(ui::Screen::Mixer);
       const std::string tmp = MixerEditor(r.doc).AddBus("Tmp", G(2));
       r.Frame();
       r.s.mixer().Set(Target{Control::BusFader, r.s.project().FindBus(snd::HashId("Tmp")), 0, 0}, -20.0f);
       r.Frame();
       r.doc.Remove(tmp);
     }},
};

// Buttons (and drawn controls) outside the logical window.
std::string Outside(const Rig& r, float w, float h) {
  std::string bad;
  for (const tela::PlacedElement& p : r.rt().elements()) {
    if (p.element.kind != tela::ElementKind::button) continue;
    const tela::Rect& b = p.bounds;
    if (b.width <= 0.0f || b.height <= 0.0f) continue;
    if (b.x < -0.5f || b.y < -0.5f || b.x + b.width > w + 0.5f || b.y + b.height > h + 0.5f) bad += " " + p.element.id;
  }
  return bad;
}

}  // namespace

// ============================================================================================
// N1: the smallest window, exactly, one pixel less, portrait and landscape, DPI 0.25 .. 4.
// ============================================================================================
SND_TEST(IndepS7UiRecheck3, SmallestWindowEverywhere) {
  const float mw = ui::kMinEditorWidth, mh = ui::kMinEditorHeight;
  SND_CHECK_EQ(mw, 1024.0f);
  SND_CHECK_EQ(mh, 640.0f);
  struct Win {
    float w, h;
  };
  std::string bad;
  for (float dpi : {0.25f, 1.0f, 2.0f, 4.0f}) {
    // "One less" is one physical pixel less (at DPI 0.25 a physical pixel is 4 logical ones).
    const float px = 1.0f / dpi;
    for (Win w : {Win{mw, mh}, Win{mw - px, mh}, Win{mw, mh - px}, Win{mh, mw}, Win{mw, 2000}, Win{2400, mh}}) {
      if (w.w * dpi > 16000.0f || w.h * dpi > 16000.0f || w.w * dpi * w.h * dpi > 16.7e6f) continue;
      const bool big = w.w >= mw && w.h >= mh;
      for (const Shot& s : kShots) {
        Rig r(Rich());
        r.app->SetViewport(static_cast<int>(std::lround(w.w * dpi)), static_cast<int>(std::lround(w.h * dpi)), dpi);
        s.set(r);
        r.Frame();
        char tag[96];
        std::snprintf(tag, sizeof tag, "\n %gx%g/%g %s:", static_cast<double>(w.w), static_cast<double>(w.h),
                      static_cast<double>(dpi), s.name);
        if (!FirstDuplicate(r.rt()).empty()) bad += std::string(tag) + " duplicate " + FirstDuplicate(r.rt());
        const bool small = r.El("body.small") != nullptr;
        if (big) {
          if (small) bad += std::string(tag) + " too small at the minimum";
          const std::string out = Outside(r, w.w, w.h);
          if (!out.empty() && bad.size() < 4000) bad += std::string(tag) + out;
        } else {
          if (!small) bad += std::string(tag) + " drawn below the minimum";
          for (const tela::PlacedElement& p : r.rt().elements()) {
            const std::string& id = p.element.id;
            if (id != "root" && id.rfind("body.small", 0) != 0 && bad.size() < 4000) bad += std::string(tag) + " also " + id;
          }
        }
      }
    }
  }
  SND_CHECK_MSG(bad.empty(), "%s", bad.c_str());
}

// Resizing across the minimum keeps the state sound: a drag (held while the window shrinks and grows),
// a text entry, a selection; nothing edits the document while "too small".
SND_TEST(IndepS7UiRecheck3, ResizingAcrossTheMinimumKeepsTheState) {
  Rig r;
  r.app->SetViewport(1280, 800, 1.0f);
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  // A drag across a shrink.
  const tela::Point c = uitest::Center(r.El("ev.t0.c0"));
  r.ptr->Down(c.x, c.y);
  r.Frame();
  r.ptr->Move(c.x + 20.0f, c.y);
  r.Frame();
  r.app->SetViewport(800, 500, 1.0f);
  r.Frame();
  r.ptr->Move(c.x + 60.0f, c.y);
  r.Frame();
  r.ptr->Up(c.x + 60.0f, c.y);
  r.ptr->Advance(1000);
  r.Frame();
  SND_CHECK(!r.doc.InGroup());
  SND_CHECK(r.doc.UndoCount() <= 1u);
  // Keys while too small: Delete / Ctrl+Z do what? The document must not get an edit from Delete.
  const std::size_t u = r.doc.UndoCount();
  r.Key(ui::kKeyDelete);
  SND_CHECK_EQ(ed.ClipCount(0), 1u);
  SND_CHECK(r.doc.UndoCount() <= u);
  r.app->SetViewport(1280, 800, 1.0f);
  r.Frame();
  SND_CHECK(FirstDuplicate(r.rt()).empty());
  SND_CHECK(r.El("ev.t0.c0") != nullptr);
  // A text entry across a shrink: Esc still closes it, nothing is written.
  SND_REQUIRE(r.ClickId("ev.list.name"));
  SND_REQUIRE(r.app->text().active());
  r.Type("X");
  r.app->SetViewport(640, 400, 1.0f);
  r.Frame();
  r.Key(ui::kKeyEscape);
  r.app->SetViewport(1280, 800, 1.0f);
  r.Frame();
  SND_CHECK(!r.app->text().active());
  SND_CHECK(r.doc.Find(G(8))->name() == "Shot");
}

// ============================================================================================
// N2: selections: host value edits, own edits, undo, write back. After a drop Delete removes nothing.
// ============================================================================================
SND_TEST(IndepS7UiRecheck3, SelectionsAcrossEditsOfEveryKind) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(ed.AddAssetClip(0, G(7), 500.0));
  r.Frame();
  auto select1 = [&] {
    SND_REQUIRE(r.ClickId("ev.t0.c1"));
    SND_REQUIRE_EQ(r.app->view().event.clip, 1);
  };
  auto deleteNothing = [&](const char* what) {
    SND_CHECK_MSG(r.app->view().event.clip == -1, "%s: selection kept", what);
    const std::size_t n = ed.ClipCount(0);
    r.Key(ui::kKeyDelete);
    SND_CHECK_MSG(ed.ClipCount(0) == n, "%s: Delete removed a clip", what);
  };
  // Own drag: kept; then a host value edit: dropped.
  select1();
  r.Drag(r.El("ev.t0.c1"), 20.0f, 0.0f, 3);
  SND_CHECK_EQ(r.app->view().event.clip, 1);
  SND_REQUIRE(ed.SetNumber(EventNumber::Priority, 5.0));  // the host, another member
  r.Frame();
  deleteNothing("host value edit of another member");
  // Own typed value: kept.
  select1();
  std::string start;
  for (const tela::PlacedElement* e : r.Prefixed("ev.clip.", ".n0")) {
    const std::string& id = e->element.id;
    if (id.size() > 3 && id.compare(id.size() - 3, 3, ".n0") == 0) start = id;
  }
  SND_REQUIRE(!start.empty());
  SND_REQUIRE(TypeInto(r, start, "700"));
  SND_CHECK_EQ(ed.ClipNumberOf(0, 1, ClipNumber::StartMs), 700.0);
  SND_CHECK_EQ(r.app->view().event.clip, 1);
  // Undo: dropped.
  r.Key('Z', ui::kModControl);
  deleteNothing("undo");
  // Redo (footer button): dropped.
  select1();
  r.ClickId("hdr.redo");
  deleteNothing("redo");
  // Write back: dropped.
  select1();
  SND_REQUIRE(r.s.mixer().Set(Target{Control::BusFader, 0, 0, 0}, -10.0f));
  r.app->WriteBackMixer();
  r.Frame();
  deleteNothing("write back");
  // A host edit between two of the screen's own edits, in the same frame as an own edit.
  select1();
  SND_REQUIRE(ed.SetNumber(EventNumber::Priority, 6.0));
  r.Drag(r.El("ev.t0.c1"), 10.0f, 0.0f, 2);  // the host edit came first: the selection goes, the drag selects again
  SND_CHECK(r.app->view().event.clip == 1 || r.app->view().event.clip == -1);
}

// ============================================================================================
// N3: numbers that do not fit the bank: FLT_MAX exactly is taken, the next double and 1e300 refused
// (typed and set), an existing large value in a file stays when another member is edited.
// ============================================================================================
SND_TEST(IndepS7UiRecheck3, NumbersThatDoNotFitTheBankAreRefused) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  const double fmax = static_cast<double>(FLT_MAX);
  const double above = std::nextafter(fmax, INFINITY);
  char t[64];
  std::snprintf(t, sizeof t, "%.17g", above);
  SND_REQUIRE(TypeInto(r, "ev.f.n4", t));
  SND_CHECK(r.app->text().active());
  SND_CHECK_EQ(ed.NumberOf(EventNumber::MaxDistance), 50.0);
  r.Key(ui::kKeyEscape);
  for (const char* s : {"1e300", "3.5e38", "-3.5e38"}) {
    SND_REQUIRE(TypeInto(r, "ev.f.n4", s));
    SND_CHECK_MSG(r.app->text().active() && ed.NumberOf(EventNumber::MaxDistance) == 50.0, "%s taken", s);
    r.Key(ui::kKeyEscape);
  }
  std::snprintf(t, sizeof t, "%.17g", fmax);
  SND_REQUIRE(TypeInto(r, "ev.f.n4", t));
  SND_CHECK_MSG(!r.app->text().active() && ed.NumberOf(EventNumber::MaxDistance) == fmax, "FLT_MAX: open %d value %.17g",
                r.app->text().active(), ed.NumberOf(EventNumber::MaxDistance));
  if (r.app->text().active()) r.Key(ui::kKeyEscape);
  // The model directly.
  SND_CHECK(!r.doc.SetNumber(G(8), "maxDistance", above));
  SND_CHECK(r.doc.SetNumber(G(8), "maxDistance", -fmax));
  SND_CHECK(!r.doc.SetNumber(G(3), "volumeDb", -1e300));
  // A file that already holds a value beyond float: editing another member keeps it.
  std::map<std::string, std::string> f = doctest::BaseFiles();
  f["events/Wide.json"] = "{\"type\": \"event\", \"guid\": \"" + G(44) +
                          "\", \"name\": \"Wide\", \"bank\": \"Main\", \"maxDistance\": 1e300, \"tracks\": [{\"clips\": []}]}\n";
  Rig w(f);
  w.app->Navigate(G(44));
  w.Frame();
  EventEditor we(w.doc, G(44));
  SND_CHECK(we.SetNumber(EventNumber::Priority, 7.0));
  SND_CHECK(w.doc.Modify(G(44), [](json::Value& v) {
    SetMember(v, "retriggerMs", Number(3.0), nullptr);
    return true;
  }));
  SND_CHECK_EQ(NumberAt(w.doc.Find(G(44))->doc, "maxDistance", 0.0), 1e300);
  SND_CHECK_EQ(we.NumberOf(EventNumber::Priority), 7.0);
  // Modify that brings the large value in: refused.
  SND_CHECK(!w.doc.Modify(G(8), [](json::Value& v) {
    SetMember(v, "maxDistance", Number(1e300), nullptr);
    return true;
  }));
  // The screen with the large value: finite text, no limit problem.
  for (const tela::PlacedElement& e : w.rt().elements()) {
    SND_CHECK_MSG(e.element.label.find("inf") == std::string::npos || e.element.label.find("(0=inf)") != std::string::npos,
                  "%s: %s", e.element.id.c_str(), e.element.label.c_str());
  }
  SND_CHECK(LimitProblem(w.rt()).empty());
}

// ============================================================================================
// N4: broken files (negative, huge, beyond every integer type) are read and shown without a crash
// (the sanitizer build with float-cast-overflow reports nothing: no unchecked conversion).
// ============================================================================================
SND_TEST(IndepS7UiRecheck3, BrokenNumbersInFilesAreShownWithoutUndefinedConversions) {
  for (const char* v : {"-5", "-1e300", "1e300", "4294967296", "1.8446744073709552e19", "2.5", "-0.5"}) {
    std::map<std::string, std::string> f = LargeFiles(2, 1, 4, 3, 12);
    const std::string n = v;
    f["buses/Sfx.json"] = "{\"type\": \"bus\", \"guid\": \"" + G(3) + "\", \"name\": \"Sfx\", \"parent\": \"Master\", \"channels\": " + n +
                          ", \"lodPriority\": " + n + ", \"volumeDb\": " + n + ", \"effects\": [{\"type\": \"snd.fx.lowpass\", \"params\": [" + n + "]}]}\n";
    f["segments/Seg.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(22) + "\", \"name\": \"Seg\", \"asset\": \"" + G(7) +
                             "\", \"bars\": " + n + ", \"entryFrame\": " + n + ", \"exitFrame\": " + n +
                             ", \"tempo\": [{\"tick\": " + n + ", \"bpm\": " + n + ", \"beatsPerBar\": " + n + ", \"beatUnit\": " + n + "}],"
                             " \"markers\": [{\"name\": \"M1\", \"frame\": " + n + "}], \"stems\": [{\"channelFirst\": " + n +
                             ", \"channelCount\": " + n + "}]}\n";
    f["playlists/PL.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(23) + "\", \"name\": \"PL\", \"loopCount\": " + n +
                             ", \"entries\": [{\"segment\": \"" + G(22) + "\", \"weight\": " + n + ", \"repeat\": " + n + "}]}\n";
    f["snapshots/Quiet.json"] = "{\"type\": \"snapshot\", \"guid\": \"" + G(9) + "\", \"name\": \"Quiet\", \"bank\": \"Main\", "
                                "\"values\": [{\"kind\": \"effectParam\", \"bus\": \"Sfx\", \"effect\": " + n + ", \"param\": " + n +
                                ", \"value\": " + n + "}, {\"kind\": \"sendLevel\", \"bus\": \"Sfx\", \"send\": " + n + ", \"value\": " + n + "}]}\n";
    f["voiceLimitGroups/G.json"] = "{\"type\": \"voiceLimitGroup\", \"guid\": \"" + G(43) + "\", \"name\": \"G\", \"maxVoices\": " + n + "}\n";
    f["events/Shot.json"] = "{\"type\": \"event\", \"guid\": \"" + G(8) + "\", \"name\": \"Shot\", \"bank\": \"Main\", \"is3D\": true,"
                            " \"maxDistance\": " + n + ", \"priority\": " + n + ", \"maxInstances\": " + n + ", \"cone\": {\"innerDeg\": " + n +
                            ", \"outerDeg\": " + n + ", \"outerDb\": " + n + "}, \"tracks\": [{\"clips\": [{\"asset\": \"" + G(7) +
                            "\", \"startMs\": " + n + ", \"loopCount\": " + n + ", \"switchValue\": " + n + "}]}],"
                            " \"curves\": [{\"parameter\": \"Speed\", \"target\": \"pitch\", \"points\": [[" + n + ", " + n + "], [10, 1]]}]}\n";
    Rig r(f);
    for (const Shot& s : kShots) {
      s.set(r);
      r.Frame();
      SND_CHECK_MSG(FirstDuplicate(r.rt()).empty(), "%s %s", v, s.name);
      SND_CHECK_MSG(LimitProblem(r.rt()).empty(), "%s %s: %s", v, s.name, LimitProblem(r.rt()).c_str());
    }
    r.app->Navigate(G(24));
    r.Frame();
    r.app->RefreshProblems();
    r.Show(ui::Screen::Problems);
  }
}

// ============================================================================================
// N5: the rebind question when an overridden bus's effect chain changes (added, removed, swapped,
// two of the same type): never carried silently onto another effect.
// ============================================================================================
namespace {
struct ChainCase {
  const char* name;
  std::vector<std::string> types;
  std::uint32_t overridden;
  void (*edit)(ProjectDocument&);
};
}  // namespace

SND_TEST(IndepS7UiRecheck3, EffectChainChangesAsk) {
  const ChainCase cases[] = {
      {"added at the end", {"snd.fx.lowpass"}, 0, [](ProjectDocument& d) { MixerEditor(d).AddEffect(G(3), "snd.fx.highpass", 1); }},
      {"added before", {"snd.fx.lowpass"}, 0, [](ProjectDocument& d) { MixerEditor(d).AddEffect(G(3), "snd.fx.highpass", 0); }},
      {"other removed", {"snd.fx.highpass", "snd.fx.lowpass"}, 1, [](ProjectDocument& d) { MixerEditor(d).RemoveEffect(G(3), 0); }},
      {"swapped", {"snd.fx.lowpass", "snd.fx.highpass"}, 0, [](ProjectDocument& d) { MixerEditor(d).MoveEffect(G(3), 0, 1); }},
      {"same type, first removed", {"snd.fx.lowpass", "snd.fx.lowpass"}, 0, [](ProjectDocument& d) { MixerEditor(d).RemoveEffect(G(3), 0); }},
      {"same type, second removed", {"snd.fx.lowpass", "snd.fx.lowpass"}, 0, [](ProjectDocument& d) { MixerEditor(d).RemoveEffect(G(3), 1); }},
  };
  for (const ChainCase& c : cases) {
    std::map<std::string, std::string> f = doctest::BaseFiles();
    std::string fx;
    for (std::size_t i = 0; i < c.types.size(); ++i) fx += std::string(i ? ", " : "") + "{\"type\": \"" + c.types[i] + "\"}";
    f["buses/Sfx.json"] = "{\"type\": \"bus\", \"guid\": \"" + G(3) + "\", \"name\": \"Sfx\", \"parent\": \"Master\", "
                          "\"sends\": [{\"bus\": \"" + G(4) + "\", \"levelDb\": -12}], \"effects\": [" + fx + "]}\n";
    Rig r(f);
    r.Show(ui::Screen::Mixer);
    const std::int32_t sfx = r.s.project().FindBus(snd::HashId("Sfx"));
    const Target t{Control::EffectParam, sfx, static_cast<std::int32_t>(c.overridden), 0};
    SND_REQUIRE(r.s.mixer().Set(t, 444.0f));
    r.Frame();
    c.edit(r.doc);
    r.Frame();
    SND_CHECK_MSG(r.app->rebindPending(), "%s: no question", c.name);
    // Discard: no effect of the bus has the value.
    if (r.app->rebindPending()) r.ClickId("rebind.discard");
    r.Frame();
    SND_CHECK(!r.app->rebindPending());
    const std::int32_t s2 = r.s.project().FindBus(snd::HashId("Sfx"));
    for (std::int32_t e = 0; e < 3; ++e) {
      SND_CHECK_MSG(r.s.mixer().Get(Target{Control::EffectParam, s2, e, 0}) != 444.0f, "%s: effect %d got the value", c.name, e);
    }
  }
  // No change of the chain (a value edit of another bus): no question, carried.
  Rig r;
  r.Show(ui::Screen::Mixer);
  const std::int32_t sfx = r.s.project().FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(r.s.mixer().Set(Target{Control::EffectParam, sfx, 0, 0}, 444.0f));
  r.Frame();
  SND_REQUIRE(!MixerEditor(r.doc).AddBus("New", G(2)).empty());
  r.Frame();
  SND_CHECK(!r.app->rebindPending());
  SND_CHECK_EQ(r.s.mixer().Get(Target{Control::EffectParam, r.s.project().FindBus(snd::HashId("Sfx")), 0, 0}), 444.0f);
}

// ============================================================================================
// N6: objects named "inf", "nan", "-inf", "Infinity" are shown by their names (the finite-text
// filter is for numbers, not for names).
// ============================================================================================
SND_TEST(IndepS7UiRecheck3, Defect_NamesThatReadInfOrNanAreShownAsADash) {
  std::map<std::string, std::string> f = doctest::BaseFiles();
  f["parameters/Speed.json"] = "{\"type\": \"parameter\", \"guid\": \"" + G(6) +
                               "\", \"name\": \"inf\", \"min\": 0, \"max\": 10, \"default\": 1, \"rampRate\": 5}\n";
  std::string& shot = f["events/Shot.json"];
  for (std::size_t at; (at = shot.find("\"parameter\": \"Speed\"")) != std::string::npos;) shot.replace(at, 20, "\"parameter\": \"inf\"");
  f["buses/Reverb.json"] = "{\"type\": \"bus\", \"guid\": \"" + G(4) + "\", \"name\": \"nan\", \"parent\": \"" + G(2) + "\"}\n";
  f["categories/SE.json"] = "{\"type\": \"category\", \"guid\": \"" + G(5) + "\", \"name\": \"Infinity\"}\n";
  Rig r(f);
  std::string dashes;
  // The places a name is shown as a label: parameter title, composition rows, the 3D / Event fields'
  // pickers, the mixer strips, the buses' graph order, the categories.
  for (const Shot& s : kShots) {
    s.set(r);
    r.Frame();
    for (const tela::PlacedElement& e : r.rt().elements()) {
      const std::string& l = e.element.label;
      // A label that should carry one of the names but shows "-" in its place: the composition
      // rows "curve 0: <name> = ...", the parameter's title, the graph order, the strip names.
      if ((e.element.id == "pc.par.title" && l != "inf") ||
          (e.element.id.rfind("pc.cmp.c.", 0) == 0 && l.find("inf =") == std::string::npos) ||
          (e.element.id == "mx.graphorder" && l.find("nan") == std::string::npos)) {
        if (dashes.size() < 2000) dashes += std::string(" ") + s.name + ":" + e.element.id + "='" + l + "'";
      }
    }
  }
  SND_CHECK_MSG(dashes.empty(), "%s", dashes.c_str());
}
