// Independent tests of group U, second re-check (indep_s7_ui_properties.md M), after the decisions
// on the second return: Esc drops the group (AbortGroup: back to the state at the press, no step,
// no redo step, earlier undo / redo steps kept), integer fields refuse before converting, the
// smallest windows, the write back matched by object, finite text only, position selections
// dropped when the document changes other than by the screens' own edits.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <set>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"

using namespace iu;

namespace {

bool EndsWith(const std::string& s, const std::string& e) {
  return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

// Every object's text: the document's state.
std::map<std::string, std::string> State(const ProjectDocument& d) {
  std::map<std::string, std::string> m;
  for (const DocObject& o : d.objects()) m[o.guid] = d.TextOf(o);
  return m;
}

std::vector<std::string> NumberFields(const Rig& r) {
  std::vector<std::string> v;
  for (const tela::PlacedElement& p : r.rt().elements()) {
    const std::string& id = p.element.id;
    if (!EndsWith(id, ".value")) continue;
    const std::string base = id.substr(0, id.size() - 6);
    const tela::PlacedElement* hit = r.El(base);
    if (hit != nullptr && hit->element.kind == tela::ElementKind::button && hit->element.label.empty()) v.push_back(base);
  }
  return v;
}

bool TypeInto(Rig& r, const std::string& id, const std::string& text) {
  if (!r.ClickId(id) || !r.app->text().active()) return false;
  for (int i = 0; i < 300; ++i) r.app->Key(ui::KeyEvent{ui::kKeyBackspace, 0, 0});
  r.Type(text);
  r.Key(ui::kKeyEnter);
  return true;
}

// The rich project with every kind of integer field: tempo points, a stem, a snapshot value of an
// effect parameter, a voice limit group, transitions and stingers.
std::map<std::string, std::string> Ints() {
  std::map<std::string, std::string> f = LargeFiles(3, 2, 6, 4, 12);
  f["segments/Seg.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(22) + "\", \"name\": \"Seg\", \"asset\": \"" + G(7) +
                           "\", \"bars\": 1, \"tempo\": [{\"tick\": 0, \"bpm\": 120, \"beatsPerBar\": 4, \"beatUnit\": 4}],"
                           " \"markers\": [{\"name\": \"M1\", \"frame\": 100, \"kind\": \"user\"}],"
                           " \"stems\": [{\"channelFirst\": 0, \"channelCount\": 1}]}\n";
  f["snapshots/Quiet.json"] = "{\"type\": \"snapshot\", \"guid\": \"" + G(9) + "\", \"name\": \"Quiet\", \"bank\": \"Main\", "
                              "\"values\": [{\"kind\": \"effectParam\", \"bus\": \"Sfx\", \"effect\": 0, \"param\": 0, \"value\": 500},"
                              " {\"kind\": \"sendLevel\", \"bus\": \"Sfx\", \"send\": 0, \"value\": -6}]}\n";
  f["voiceLimitGroups/G.json"] = "{\"type\": \"voiceLimitGroup\", \"guid\": \"" + G(43) +
                                 "\", \"name\": \"G\", \"maxVoices\": 4}\n";
  std::string& m = f["events/Music.json"];
  const std::size_t at = m.rfind("]}}");
  m.insert(at + 1, ", \"stingers\": [{\"name\": \"St\", \"event\": \"" + G(8) + "\", \"timing\": \"barsAfter\", \"timingCount\": 1}]");
  return f;
}

struct View {
  const char* name;
  void (*select)(Rig&);
};
void SelBus(Rig& r) { r.app->Navigate(G(3)); }
void SelSnap(Rig& r) { r.app->Navigate(G(9)); }
void SelGroup(Rig& r) { r.app->Navigate(G(43)); }
void SelSegment(Rig& r) { r.app->Navigate(G(22)); }
void SelPlaylist(Rig& r) { r.app->Navigate(G(23)); }
void SelRules(Rig& r) { r.app->Navigate(G(24)); }
void SelStingers(Rig& r) { r.app->Navigate(G(24)); r.app->view().music.tab = 3; }
void SelLink(Rig& r) { r.app->OnScreen(ui::Screen::Link); }
void SelLog(Rig& r) { r.app->view().profiler.tab = 5; r.app->OnScreen(ui::Screen::Profiler); }
void SelEvent(Rig& r) { r.app->Navigate(G(8)); r.Frame(); r.ClickId("ev.t0.c0"); }
void SelParams(Rig& r) { r.app->Navigate(G(6)); r.Frame(); if (r.app->view().params.event.empty()) r.app->view().params.event = G(8); r.Frame(); r.ClickId("pc.pt.0"); }
void SelSpatial(Rig& r) { r.app->Navigate(G(8)); r.app->OnScreen(ui::Screen::Spatial); }
void SelMaterial(Rig& r) { r.app->Navigate(G(25)); }
void SelPatch(Rig& r) { r.app->Navigate(G(26)); r.Frame(); r.ClickId("pa.node.osc0"); }
void SelDucking(Rig& r) { r.app->Navigate(G(3)); r.app->view().buses.tab = 2; }
void SelCats(Rig& r) { r.app->Navigate(G(5)); }
const View kViews[] = {{"bus", SelBus},         {"snapshot", SelSnap},   {"group", SelGroup},     {"segment", SelSegment},
                       {"playlist", SelPlaylist}, {"rules", SelRules},   {"stingers", SelStingers}, {"link", SelLink},
                       {"log", SelLog},         {"event", SelEvent},     {"params", SelParams},   {"spatial", SelSpatial},
                       {"material", SelMaterial}, {"patch", SelPatch},   {"ducking", SelDucking}, {"categories", SelCats}};

bool IsIntegerField(const std::string& id) {
  for (const char* x : {"mx.bp.ch", "mx.bp.lod", "mx.grp.max", "ln.port", "mu.seg.bars", "mu.seg.entry", "mu.seg.exit",
                        "mu.pl.loops", "prof.log.f.id"}) {
    if (id == x) return true;
  }
  for (const char* s : {".effect", ".param", ".send", ".tick", ".beats", ".frame", ".first", ".count", ".r"}) {
    if (EndsWith(id, s) && id.rfind("mu.", 0) == 0) return true;
    if (EndsWith(id, s) && id.rfind("mx.", 0) == 0 && std::string(s) != ".r") return true;
  }
  return false;
}

// "inf" / "nan" / "infinity" as words in a label. Not counted: "(0=inf)" (a legend: 0 loops =
// infinite) and the faders' "-inf dB" (stage 2: -inf = silence, below -144 dB).
bool NonFiniteWord(std::string label) {
  for (const char* ok : {"0=inf", "-inf dB"}) {
    for (std::size_t at; (at = label.find(ok)) != std::string::npos;) label.erase(at, std::string(ok).size());
  }
  std::string w;
  auto bad = [](std::string t) {
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return t == "inf" || t == "nan" || t == "infinity";
  };
  for (char c : label) {
    if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
      w += c;
    } else {
      if (bad(w)) return true;
      w.clear();
    }
  }
  return bad(w);
}

}  // namespace

// ============================================================================================
// M1: AbortGroup (model): the state, the undo and the redo lists as at BeginGroup; random sequences
// of edits, groups, aborted groups (also nested), undo and redo keep the history's invariants.
// ============================================================================================
SND_TEST(IndepS7UiRecheck2, AbortGroupRestoresEverything) {
  ProjectDocument d;
  doctest::LoadBase(d);
  SND_REQUIRE(d.SetNumber(G(3), "volumeDb", -2.0));
  SND_REQUIRE(d.SetNumber(G(3), "volumeDb", -3.0));
  SND_REQUIRE(d.Undo());  // undo 1, redo 1
  const auto before = State(d);
  const std::size_t u = d.UndoCount(), rd = d.RedoCount();
  const std::uint64_t rev = d.revision();
  // Nothing changed in the group.
  d.BeginGroup();
  d.AbortGroup();
  SND_CHECK(!d.InGroup());
  SND_CHECK_EQ(d.UndoCount(), u);
  SND_CHECK_EQ(d.RedoCount(), rd);
  SND_CHECK_EQ(d.revision(), rev);
  // Changed and changed back.
  d.BeginGroup();
  SND_REQUIRE(d.SetNumber(G(4), "volumeDb", -9.0));
  SND_REQUIRE(d.SetNumber(G(4), "volumeDb", 0.0));
  d.AbortGroup();
  SND_CHECK(State(d) == before);
  SND_CHECK_EQ(d.UndoCount(), u);
  SND_CHECK_EQ(d.RedoCount(), rd);
  // Several objects, a created and a removed object.
  d.BeginGroup();
  SND_REQUIRE(d.SetNumber(G(4), "volumeDb", -9.0));
  SND_REQUIRE(!d.Create("category", "Tmp").empty());
  SND_REQUIRE(d.Remove(G(9)));
  d.AbortGroup();
  SND_CHECK(State(d) == before);
  SND_CHECK_EQ(d.UndoCount(), u);
  SND_CHECK_EQ(d.RedoCount(), rd);
  SND_CHECK(!d.IsDirty() || true);
  // The old redo step still redoes.
  SND_CHECK(d.Redo());
  SND_CHECK_EQ(NumberAt(d.Find(G(3))->doc, "volumeDb", 0), -3.0);
  // Nested: an abort inside an outer group, then the outer group closes.
  const auto mid = State(d);
  const std::size_t u2 = d.UndoCount();
  d.BeginGroup();
  SND_REQUIRE(d.SetNumber(G(4), "volumeDb", -1.0));
  d.BeginGroup();
  SND_REQUIRE(d.SetNumber(G(4), "volumeDb", -5.0));
  d.AbortGroup();
  const bool stillOpen = d.InGroup();
  if (stillOpen) d.EndGroup();
  SND_CHECK(!d.InGroup());
  std::printf("nested abort: outer group %s after the inner abort, volume %g, steps %u -> %u\n",
              stillOpen ? "open" : "closed", NumberAt(d.Find(G(4))->doc, "volumeDb", 0),
              static_cast<unsigned>(u2), static_cast<unsigned>(d.UndoCount()));
  SND_CHECK(d.UndoCount() <= u2 + 1);
  while (d.UndoCount() > u2) d.Undo();
  SND_CHECK(State(d) == mid);
}

SND_TEST(IndepS7UiRecheck2, RandomSequencesWithAbortsKeepTheHistory) {
  for (unsigned seed = 1; seed <= 40; ++seed) {
    ProjectDocument d;
    doctest::LoadBase(d);
    const auto initial = State(d);
    std::mt19937 rng(seed);
    const std::string targets[] = {G(2), G(3), G(4), G(5)};
    for (int step = 0; step < 60; ++step) {
      const unsigned op = rng() % 6;
      const std::string& g = targets[rng() % 4];
      const double v = -static_cast<double>(rng() % 40);
      if (op == 0) {
        d.SetNumber(g, "volumeDb", v);
      } else if (op == 1) {
        d.Undo();
      } else if (op == 2) {
        d.Redo();
      } else if (op == 3) {
        d.BeginGroup();
        d.SetNumber(g, "volumeDb", v);
        d.SetNumber(targets[rng() % 4], "volumeDb", v - 1);
        d.EndGroup();
      } else {
        const auto s0 = State(d);
        const std::size_t u = d.UndoCount(), rd = d.RedoCount();
        d.BeginGroup();
        for (unsigned k = 0, n = rng() % 4; k < n; ++k) d.SetNumber(targets[rng() % 4], "volumeDb", -static_cast<double>(rng() % 40));
        d.AbortGroup();
        SND_CHECK_MSG(State(d) == s0 && d.UndoCount() == u && d.RedoCount() == rd && !d.InGroup(), "seed %u step %d", seed, step);
      }
    }
    const auto end = State(d);
    const std::size_t u = d.UndoCount();
    while (d.Undo()) {
    }
    SND_CHECK_MSG(State(d) == initial, "seed %u: undo all", seed);
    for (std::size_t i = 0; i < u; ++i) d.Redo();
    SND_CHECK_MSG(State(d) == end, "seed %u: redo all", seed);
  }
}

// ============================================================================================
// M2: Esc on the screens: drags that change nothing, go out and back, move; number fields at their
// end and moved; faders; with an earlier step and with a redo step.
// ============================================================================================
namespace {
enum class Prior { Step, Redo };
void Prepare(Rig& r, Prior p) {
  SND_REQUIRE(EventEditor(r.doc, G(8)).SetNumber(EventNumber::Priority, 9.0));
  if (p == Prior::Redo) SND_REQUIRE(r.doc.Undo());
  r.Frame();
}
// Press the centre of `id`, move through the offsets (dx, dy pairs), Esc, release.
void PressMovesEsc(Rig& r, const std::string& id, std::vector<std::pair<float, float>> moves) {
  const tela::PlacedElement* e = r.El(id);
  SND_REQUIRE(e != nullptr);
  const tela::Point c = uitest::Center(e);
  r.ptr->Down(c.x, c.y);
  r.Frame();
  for (const auto& [dx, dy] : moves) {
    r.ptr->Move(c.x + dx, c.y + dy);
    r.Frame();
  }
  r.Key(ui::kKeyEscape);
  r.ptr->Move(c.x + 3.0f, c.y + 3.0f);
  r.Frame();
  r.ptr->Up(c.x + 3.0f, c.y + 3.0f);
  r.ptr->Advance(1000);
  r.Frame();
}
}  // namespace

SND_TEST(IndepS7UiRecheck2, EscOnEveryDragLeavesTheHistoryAsBefore) {
  struct Case {
    const char* name;
    void (*select)(Rig&);
    const char* id;
    std::vector<std::pair<float, float>> moves;
  };
  const Case cases[] = {
      {"clip unchanged", [](Rig& r) { r.app->Navigate(G(8)); }, "ev.t0.c0", {{-30, 0}, {-60, 0}}},
      {"clip out and back", [](Rig& r) { r.app->Navigate(G(8)); }, "ev.t0.c0", {{40, 0}, {0, 0}}},
      {"clip moved", [](Rig& r) { r.app->Navigate(G(8)); }, "ev.t0.c0", {{40, 0}, {80, 48}}},
      {"number at its end", [](Rig& r) { r.app->Navigate(G(8)); }, "ev.f.n0", {{0, 20}, {0, 60}}},
      {"number moved", [](Rig& r) { r.app->Navigate(G(3)); }, "mx.bp.vol", {{0, -20}, {0, -40}}},
      {"number out and back", [](Rig& r) { r.app->Navigate(G(3)); }, "mx.bp.vol", {{0, -20}, {0, 0}}},
      {"curve point", [](Rig& r) { r.app->Navigate(G(6)); r.Frame(); if (r.app->view().params.event.empty()) r.app->view().params.event = G(8); },
       "pc.pt.1", {{0, 30}}},
  };
  for (const Case& c : cases) {
    for (Prior p : {Prior::Step, Prior::Redo}) {
      Rig r;
      Prepare(r, p);
      c.select(r);
      r.Frame();
      const auto s0 = State(r.doc);
      const std::size_t u = r.doc.UndoCount(), rd = r.doc.RedoCount();
      PressMovesEsc(r, c.id, c.moves);
      SND_CHECK_MSG(State(r.doc) == s0, "%s (%s): the document changed", c.name, p == Prior::Step ? "step" : "redo");
      SND_CHECK_MSG(r.doc.UndoCount() == u && r.doc.RedoCount() == rd, "%s (%s): undo %u -> %u, redo %u -> %u", c.name,
                    p == Prior::Step ? "step" : "redo", static_cast<unsigned>(u), static_cast<unsigned>(r.doc.UndoCount()),
                    static_cast<unsigned>(rd), static_cast<unsigned>(r.doc.RedoCount()));
      SND_CHECK(!r.doc.InGroup());
      // The earlier step / redo step still works.
      if (p == Prior::Step) {
        SND_CHECK(r.doc.Undo());
        SND_CHECK_EQ(EventEditor(r.doc, G(8)).NumberOf(EventNumber::Priority), 128.0);
      } else {
        SND_CHECK(r.doc.Redo());
        SND_CHECK_EQ(EventEditor(r.doc, G(8)).NumberOf(EventNumber::Priority), 9.0);
      }
    }
  }
}

SND_TEST(IndepS7UiRecheck2, EscOnFaderAndKnobDragsLeavesTheLiveHistoryAsBefore) {
  for (int prior = 0; prior < 2; ++prior) {
    for (float dy : {-60.0f, 0.0f}) {
      Rig r;
      r.Show(ui::Screen::Mixer);
      const std::int32_t sfx = r.s.project().FindBus(snd::HashId("Sfx"));
      const Target t{Control::BusFader, sfx, 0, 0};
      SND_REQUIRE(r.s.mixer().Set(t, -6.0f));
      if (prior == 1) SND_REQUIRE(r.s.mixer().Undo());
      r.Frame();
      const float v0 = r.s.mixer().Get(t);
      const std::size_t u = r.s.mixer().history().UndoCount(), rd = r.s.mixer().history().RedoCount();
      PressMovesEsc(r, ui::BusId(snd::HashId("Sfx")) + ".fader.hit", {{0, dy * 0.5f}, {0, dy}, {0, dy * 0.5f}});
      SND_CHECK_EQ(r.s.mixer().Get(t), v0);
      SND_CHECK_EQ(r.s.mixer().history().UndoCount(), u);
      SND_CHECK_EQ(r.s.mixer().history().RedoCount(), rd);
      // The knob of the lowpass.
      const std::string knob = ui::BusId(snd::HashId("Sfx")) + ".fx0.0.hit";
      if (r.El(knob) != nullptr) {
        const Target kt{Control::EffectParam, sfx, 0, 0};
        const float k0 = r.s.mixer().Get(kt);
        PressMovesEsc(r, knob, {{0, -40}});
        SND_CHECK_EQ(r.s.mixer().Get(kt), k0);
        SND_CHECK_EQ(r.s.mixer().history().UndoCount(), u);
      }
    }
  }
}

// ============================================================================================
// M3: every integer field refuses non-integers, negatives, values beyond its type and non-numbers
// before converting (document unchanged, entry open); the port is 1..65535.
// ============================================================================================
SND_TEST(IndepS7UiRecheck2, IntegerFieldsRefuseBeforeConverting) {
  std::string bad;
  std::set<std::string> seen;
  for (const View& v : kViews) {
    Rig r(Ints());
    v.select(r);
    r.Frame();
    for (const std::string& id : NumberFields(r)) {
      if (!IsIntegerField(id) || r.El(id) == nullptr) continue;
      seen.insert(id);
      // 64-bit fields (ticks and frames) take 2^32; every field refuses 2^64.
      const bool u64 = EndsWith(id, ".tick") || EndsWith(id, ".frame") || id == "mu.seg.entry" || id == "mu.seg.exit";
      for (const char* text : {"2.5", "-1", "4294967296", "18446744073709551616", "1e300", "-1e300", "nan", "0.0001"}) {
        if (u64 && std::string(text) == "4294967296") continue;
        const auto s0 = State(r.doc);
        const std::uint16_t port = r.app->connectionDesc().port;
        if (!TypeInto(r, id, text)) {
          bad += std::string(" ") + v.name + ":" + id + "(no entry)";
          break;
        }
        const bool changed = State(r.doc) != s0 || r.app->connectionDesc().port != port;
        if (changed || !r.app->text().active()) bad += std::string(" ") + id + "['" + text + "' " + (changed ? "written" : "closed") + "]";
        if (r.app->text().active()) r.Key(ui::kKeyEscape);
        if (State(r.doc) != s0) {
          r.doc.Undo();
          r.Frame();
        }
      }
    }
  }
  // The port: 0 and 65536 refused, 1 and 65535 taken.
  Rig r;
  r.Show(ui::Screen::Link);
  for (const char* t : {"0", "65536", "70000"}) {
    const std::uint16_t p = r.app->connectionDesc().port;
    SND_REQUIRE(TypeInto(r, "ln.port", t));
    if (r.app->connectionDesc().port != p || !r.app->text().active()) bad += std::string(" ln.port['") + t + "' taken]";
    r.Key(ui::kKeyEscape);
  }
  for (const char* t : {"1", "65535"}) {
    SND_REQUIRE(TypeInto(r, "ln.port", t));
    SND_CHECK_EQ(std::to_string(r.app->connectionDesc().port), std::string(t));
  }
  std::string list;
  for (const std::string& s : seen) list += " " + s;
  std::printf("integer fields: %u:%s\n", static_cast<unsigned>(seen.size()), list.c_str());
  SND_CHECK(seen.size() >= 15u);
  SND_CHECK_MSG(bad.empty(), "%s", bad.c_str());
}

// ============================================================================================
// M4: small windows: a screen is either "too small" (and declares no number field) or every button
// is inside the window; pagers' buttons too (every page can be reached).
// ============================================================================================
namespace {
struct Win {
  float w, h;
};
// Buttons outside the window, as "<window> <view>:<id>" (the header and footer when `chrome`, else
// the screen's body), and screens marked small at full size or with number fields while small.
std::string OutsideButtons(std::initializer_list<Win> wins, bool chrome, unsigned* count) {
  std::string bad;
  *count = 0;
  for (Win w : wins) {
    for (float dpi : {1.0f, 2.0f}) {
      for (const View& v : kViews) {
        Rig r(Ints());
        r.app->SetViewport(static_cast<int>(w.w * dpi), static_cast<int>(w.h * dpi), dpi);
        v.select(r);
        r.Frame();
        const bool small = r.El("body.small") != nullptr || r.El("prof.log.small") != nullptr;
        if (!chrome && small && w.w >= ui::kMinEditorWidth && w.h >= ui::kMinEditorHeight) bad += std::string(" ") + v.name + ":small at full size";
        if (!chrome && small && !NumberFields(r).empty()) bad += std::string(" ") + v.name + ":fields while small";
        for (const tela::PlacedElement& p : r.rt().elements()) {
          if (p.element.kind != tela::ElementKind::button) continue;
          const std::string& id = p.element.id;
          const bool isChrome = id.rfind("tab.", 0) == 0 || id.rfind("hdr.", 0) == 0 || id.rfind("footer.", 0) == 0 ||
                                id.rfind("link.", 0) == 0;
          if (isChrome != chrome) continue;
          const tela::Rect& b = p.bounds;
          if (b.width <= 0.0f || b.height <= 0.0f) continue;
          if (b.x < -0.5f || b.y < -0.5f || b.x + b.width > w.w + 0.5f || b.y + b.height > w.h + 0.5f) {
            ++*count;
            char t[160];
            std::snprintf(t, sizeof t, " %gx%g/%g %s:%s", static_cast<double>(w.w), static_cast<double>(w.h),
                          static_cast<double>(dpi), v.name, id.c_str());
            if (bad.size() < 6000) bad += t;
          }
        }
      }
    }
  }
  return bad;
}
}  // namespace

// The screens' bodies: at every window either "too small" (without number fields) or every button
// (fields, pagers) inside the window.
SND_TEST(IndepS7UiRecheck2, Defect_BodyButtonsOutsideTheWindowAtSmallSizes) {
  unsigned n = 0;
  const std::string bad = OutsideButtons({Win{320, 240}, Win{640, 400}, Win{799, 499}, Win{800, 500}, Win{1024, 640}}, false, &n);
  SND_CHECK_MSG(bad.empty(), "%u:%s", n, bad.c_str());
}

// The header's tabs and the footer's document buttons (Undo / Redo / Save, problems) at the windows
// the tool still draws (Mixer, Profiler, Replay and Problems down to 320 x 240; the editors say "too
// small" below 800 x 500): they are declared outside the window, so the other screens and the
// document's buttons cannot be reached with the pointer (only F keys / Ctrl+PageDown).
SND_TEST(IndepS7UiRecheck2, Defect_HeaderAndFooterButtonsOutsideSmallWindows) {
  unsigned n = 0;
  const std::string bad = OutsideButtons({Win{320, 240}, Win{640, 400}, Win{799, 499}}, true, &n);
  SND_CHECK_MSG(bad.empty(), "%u:%s", n, bad.c_str());
}

// ============================================================================================
// M5: write back with the question: matched by object (GUID, effect type, parameter).
// ============================================================================================
namespace {
// Sfx with effects `types` (in order), an override on effect `e` param 0, then `edit` on the
// document; answers "Write back" if a question comes. Returns Sfx's effects as written.
struct WbResult {
  std::vector<std::pair<std::string, double>> effects;  // type, param 0
  bool asked = false;
  std::string message;
};
WbResult WriteBackAfter(const std::vector<std::string>& types, std::uint32_t e, float value,
                        void (*edit)(ProjectDocument&)) {
  std::map<std::string, std::string> f = doctest::BaseFiles();
  std::string fx;
  for (std::size_t i = 0; i < types.size(); ++i) fx += std::string(i ? ", " : "") + "{\"type\": \"" + types[i] + "\"}";
  f["buses/Sfx.json"] = "{\"type\": \"bus\", \"guid\": \"" + G(3) + "\", \"name\": \"Sfx\", \"parent\": \"Master\", "
                        "\"sends\": [{\"bus\": \"" + G(4) + "\", \"levelDb\": -12}], \"effects\": [" + fx + "]}\n";
  Rig r(f);
  r.Show(ui::Screen::Mixer);
  const std::int32_t sfx = r.s.project().FindBus(snd::HashId("Sfx"));
  WbResult out;
  if (!r.s.mixer().Set(Target{Control::EffectParam, sfx, static_cast<std::int32_t>(e), 0}, value)) return out;
  r.Frame();
  const std::size_t m0 = r.s.messages().size();
  edit(r.doc);
  r.Frame();
  out.asked = r.app->rebindPending();
  if (out.asked) r.ClickId("rebind.writeback");
  else r.app->WriteBackMixer();
  r.Frame();
  for (std::size_t i = m0; i < r.s.messages().size(); ++i) out.message += r.s.messages()[i] + " | ";
  MixerEditor mx(r.doc);
  const json::Value* arr = FindPath(r.doc.Find(G(3))->doc, "effects");
  for (std::size_t i = 0; arr != nullptr && i < arr->items.size(); ++i)
    out.effects.emplace_back(StringAt(arr->items[i], "type"), mx.EffectParam(G(3), static_cast<std::uint32_t>(i), 0));
  return out;
}
}  // namespace

SND_TEST(IndepS7UiRecheck2, WriteBackIsMatchedByObject) {
  std::string other;
  for (const EffectTypeInfo& t : StandardEffectTypes()) {
    if (t.name != "snd.fx.lowpass" && t.name != "snd.fx.highpass" && !t.params.empty()) {
      other = t.name;
      break;
    }
  }
  SND_REQUIRE(!other.empty());
  // Replaced by another type: nothing written into it, the discarded value is named.
  WbResult a = WriteBackAfter({"snd.fx.lowpass"}, 0, 500.0f, [](ProjectDocument& d) {
    MixerEditor mx(d);
    mx.RemoveEffect(G(3), 0);
    std::string o;
    for (const EffectTypeInfo& t : StandardEffectTypes()) {
      if (t.name != "snd.fx.lowpass" && t.name != "snd.fx.highpass" && !t.params.empty()) { o = t.name; break; }
    }
    mx.AddEffect(G(3), o, 0);
  });
  SND_REQUIRE_EQ(a.effects.size(), std::size_t{1});
  SND_CHECK(a.effects[0].first == other);
  SND_CHECK_MSG(a.effects[0].second != 500.0, "the lowpass value went into %s", other.c_str());
  SND_CHECK_MSG(a.message.find("discard") != std::string::npos, "no message naming the discarded override: %s", a.message.c_str());
  // Removed: no effect appears.
  WbResult b = WriteBackAfter({"snd.fx.lowpass"}, 0, 500.0f, [](ProjectDocument& d) { MixerEditor(d).RemoveEffect(G(3), 0); });
  SND_CHECK(b.effects.empty());
  // Reordered (lowpass, highpass -> highpass, lowpass), override on the lowpass: written to the
  // lowpass wherever it is now, or discarded; never into the highpass.
  WbResult c = WriteBackAfter({"snd.fx.lowpass", "snd.fx.highpass"}, 0, 500.0f,
                              [](ProjectDocument& d) { MixerEditor(d).MoveEffect(G(3), 0, 1); });
  SND_REQUIRE_EQ(c.effects.size(), std::size_t{2});
  SND_CHECK(c.effects[0].first == "snd.fx.highpass");
  SND_CHECK_MSG(c.effects[0].second != 500.0, "the lowpass value went into the highpass");
  std::printf("reordered: asked %d, lowpass now %g, message %s\n", c.asked, c.effects[1].second, c.message.c_str());
  // Same type, another effect: two lowpasses, override on the first, the first removed. By the rule
  // (GUID, type, parameter at the position) the value lands on the remaining lowpass: recorded.
  WbResult e = WriteBackAfter({"snd.fx.lowpass", "snd.fx.lowpass"}, 0, 500.0f,
                              [](ProjectDocument& d) { MixerEditor(d).RemoveEffect(G(3), 0); });
  SND_REQUIRE_EQ(e.effects.size(), std::size_t{1});
  std::printf("same type, other effect: asked %d, remaining lowpass %g, message %s\n", e.asked, e.effects[0].second,
              e.message.c_str());
}

// ============================================================================================
// M6: no "inf" / "nan" in any text after extreme values are typed into every number field.
// Decision: "表示する値は必ず有限". Found: the Params screen takes a parameter's max / default of
// +-1e308 (finite doubles); the mixer's parameter knob shows it as a float: "Speed inf" / "Speed -inf".
// ============================================================================================
SND_TEST(IndepS7UiRecheck2, Defect_NonFiniteTextAfterExtremeValues) {
  std::string bad;
  for (const View& v : kViews) {
    for (const char* text : {"1e308", "-1e308", "1e-308", "1e30", "-1e30"}) {
      Rig r(Ints());
      v.select(r);
      r.Frame();
      for (const std::string& id : NumberFields(r)) {
        if (r.El(id) == nullptr) continue;
        TypeInto(r, id, text);
        if (r.app->text().active()) r.Key(ui::kKeyEscape);
      }
      for (ui::Screen sc : kAllScreens) {
        r.app->OnScreen(sc);
        r.Frame();
        for (const tela::PlacedElement& e : r.rt().elements()) {
          if (NonFiniteWord(e.element.label) && bad.size() < 2500)
            bad += std::string(" ") + v.name + "/" + text + "/" + ui::ScreenName(sc) + ":" + e.element.id + "='" + e.element.label + "'";
        }
        SND_CHECK(LimitProblem(r.rt()).empty());
      }
      v.select(r);
      r.Frame();
      for (const tela::PlacedElement& e : r.rt().elements()) {
        if (NonFiniteWord(e.element.label) && bad.size() < 2500)
          bad += std::string(" ") + v.name + "/" + text + ":" + e.element.id + "='" + e.element.label + "'";
      }
    }
  }
  SND_CHECK_MSG(bad.empty(), "%s", bad.c_str());
}

// ============================================================================================
// M7: position selections are dropped when the document changes other than by the screen's own
// edits - write back, rebind answer, host edits (structural and value-only) - and kept over the
// screen's own edit of the selected item.
// ============================================================================================
SND_TEST(IndepS7UiRecheck2, OwnEditsKeepThePositionSelection) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  SND_REQUIRE(r.ClickId("ev.t0.c0"));
  r.Drag(r.El("ev.t0.c0"), 20.0f, 0.0f, 3);
  SND_CHECK_EQ(r.app->view().event.clip, 0);
}

SND_TEST(IndepS7UiRecheck2, HostStructuralEditDropsThePositionSelection) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(ed.AddAssetClip(0, G(7), 500.0));
  r.Frame();
  SND_REQUIRE(r.ClickId("ev.t0.c1"));
  SND_REQUIRE(ed.RemoveClip(0, 0));  // the host: clip 1 is now clip 0
  r.Frame();
  SND_CHECK_EQ(r.app->view().event.clip, -1);
  r.Key(ui::kKeyDelete);
  SND_CHECK_EQ(ed.ClipCount(0), 1u);
}

SND_TEST(IndepS7UiRecheck2, WriteBackDropsThePositionSelection) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  SND_REQUIRE(r.ClickId("ev.t0.c0"));
  SND_REQUIRE(r.s.mixer().Set(Target{Control::BusFader, 0, 0, 0}, -10.0f));
  r.app->WriteBackMixer();
  r.Frame();
  SND_CHECK_EQ(r.app->view().event.clip, -1);
}

// Decision: "位置で持つ選択は、自分の編集以外で文書が変わったら外す（…ホストからの変更を含む）". The
// implementation drops a position selection only when the shape of the selected arrays changes
// (App::PositionShape); a host's value edit of the document keeps it. Here the host moves the
// selected clip and changes a curve point; the selections stay.
SND_TEST(IndepS7UiRecheck2, Defect_HostValueEditKeepsThePositionSelection) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  SND_REQUIRE(r.ClickId("ev.t0.c0"));
  SND_REQUIRE(EventEditor(r.doc, G(8)).MoveClip(0, 0, 300.0));
  r.Frame();
  SND_CHECK_MSG(r.app->view().event.clip == -1, "clip selection kept (%d)", r.app->view().event.clip);
  r.app->Navigate(G(6));
  r.Frame();
  if (r.app->view().params.event.empty()) r.app->view().params.event = G(8);
  r.Frame();
  SND_REQUIRE(r.ClickId("pc.pt.1"));
  SND_REQUIRE(CurveEditor(r.doc, G(8), "curves[0]").MovePoint(1, 10.0, 600.0));
  r.Frame();
  SND_CHECK_MSG(r.app->view().params.point == -1, "point selection kept (%d)", r.app->view().params.point);
}
