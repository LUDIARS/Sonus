// Every screen within Tela's limits: a large project (hundreds of events, a long event with many
// tracks and clips, 100 transition rules, a patch at the node limit, 64 materials, many buses), at
// DPI 0.25, 1, 2 and 4 (D-67a): fewer than kMaxElements elements, unique ids, every drawing within
// 256 shapes and 4096 points, the composition paints. Paging keeps the lists in range.
#include <set>

#include "snd/buildtool/patch.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/patch_editor.hpp"
#include "snd/tool/ui/raster_compositor.hpp"
#include "ui_editor_helpers.hpp"
#include "ui_music_fixture.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {

std::map<std::string, std::string> BigFiles() {
  using doctest::G;
  std::map<std::string, std::string> f = MusicFiles();
  // 300 events with one clip each.
  for (int i = 0; i < 300; ++i) {
    f["events/many/E" + std::to_string(i) + ".json"] =
        "{\"type\": \"event\", \"guid\": \"" + G(1000 + i) + "\", \"name\": \"Many/E" + std::to_string(i) +
        "\", \"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"tone\", \"startMs\": " + std::to_string(i % 50) + "}]}]}";
  }
  // A long event: 40 tracks x 30 clips, actions.
  std::string tracks;
  for (int t = 0; t < 40; ++t) {
    std::string clips;
    for (int c = 0; c < 30; ++c) {
      clips += std::string(c ? ", " : "") + "{\"asset\": \"tone\", \"startMs\": " + std::to_string(c * 60) +
               ", \"fadeInMs\": 5, \"fadeOutMs\": 5}";
    }
    tracks += std::string(t ? ", " : "") + "{\"clips\": [" + clips + "], \"actions\": [{\"timeMs\": 100, \"action\": \"stop\", \"target\": \"Shot\"}]}";
  }
  f["events/Long.json"] = "{\"type\": \"event\", \"guid\": \"" + G(2000) + "\", \"name\": \"Long\", \"bank\": \"Main\", "
                          "\"tracks\": [" + tracks + "]}";
  // 100 transition rules on the music event (most unreachable behind the first).
  std::string rules = "{\"from\": \"*\", \"to\": \"*\", \"timing\": \"nextBar\"}";
  for (int i = 1; i < 100; ++i) rules += ", {\"from\": \"Explore\", \"to\": \"Battle\", \"timing\": \"nextBeat\"}";
  f["events/Music.json"] =
      "{\"type\": \"event\", \"guid\": \"" + G(88) + "\", \"name\": \"Music\", \"bank\": \"Main\", \"music\": {"
      "\"initialState\": \"Explore\", \"states\": [{\"name\": \"Explore\", \"playlist\": \"PA\"}, {\"name\": \"Battle\", \"playlist\": \"PB\"}],"
      "\"transitions\": [" + rules + "]}}";
  // 64 materials.
  std::string mats;
  for (int i = 0; i < 64; ++i) mats += std::string(i ? ", " : "") + "\"m" + std::to_string(i) + "\"";
  f["materials/Big.json"] = "{\"type\": \"materialTable\", \"guid\": \"" + G(2100) + "\", \"name\": \"Big\", \"bank\": \"Main\", "
                            "\"materials\": [" + mats + "]}";
  // 60 buses under the master, each with two effects and a send.
  for (int i = 0; i < 60; ++i) {
    f["buses/many/B" + std::to_string(i) + ".json"] =
        "{\"type\": \"bus\", \"guid\": \"" + G(2200 + i) + "\", \"name\": \"B" + std::to_string(i) + "\", \"parent\": \"Master\", "
        "\"effects\": [{\"type\": \"snd.fx.lowpass\"}, {\"type\": \"snd.fx.delay\"}], \"sends\": [{\"bus\": \"Reverb\"}]}";
  }
  f["patches/Full.json"] = "{\"type\": \"patch\", \"guid\": \"" + G(2300) + "\", \"name\": \"Full\", \"bank\": \"Main\"}";
  return f;
}

void FillPatch(ProjectDocument& doc) {
  AddMusicWaves(doc);
  PatchEditor pe(doc, doctest::G(2300));
  pe.AddNode("output", 900.0f, 100.0f);
  std::string prev;
  for (std::uint32_t i = 1; i < snd::buildtool::patch::kMaxNodes; ++i) {
    const std::string id = pe.AddNode(i == 1 ? "oscillator" : "amp", 30.0f * static_cast<float>(i), 40.0f + 9.0f * static_cast<float>(i));
    if (!prev.empty()) pe.Connect(prev, id, "in");
    prev = id;
  }
  pe.Connect(prev, pe.OutputNode(), "in");
}

void CheckLimits(const tela::Runtime& rt, const char* what, float dpi) {
  const auto& els = rt.elements();
  SND_CHECK_MSG(els.size() < ui::kMaxElements, "%s at dpi %g: %zu elements", what, static_cast<double>(dpi), els.size());
  std::set<std::string> ids;
  for (const tela::PlacedElement& p : els) {
    SND_CHECK_MSG(ids.insert(p.element.id).second, "%s: duplicate id %s", what, p.element.id.c_str());
    if (p.element.kind != tela::ElementKind::canvas) continue;
    std::size_t points = 0;
    for (const tela::Shape& s : p.element.drawing.shapes()) points += s.points.size();
    SND_CHECK_MSG(p.element.drawing.shapes().size() <= 256u && points <= 4096u, "%s: drawing %s over the limits", what,
                  p.element.id.c_str());
  }
}

}  // namespace

SND_TEST(ToolUiLimits, EveryScreenOfALargeProjectAtEveryDpi) {
  struct Case {
    float dpi;
    int w, h;
    bool compose;
  };
  const Case cases[] = {{0.25f, 640, 400, true}, {1.0f, 1280, 800, true}, {2.0f, 2560, 1600, false}, {4.0f, 1600, 1200, true}};
  for (const Case& c : cases) {
    Editor t(ui::Screen::Mixer, c.w, c.h, c.dpi, BigFiles(), ui::Workspace{}, &FillPatch);
    // Selections that show the large objects.
    t.app.view().event.event = doctest::G(2000);
    t.app.view().event.msPerPx = 1.0;
    t.app.view().music.event = doctest::G(88);
    t.app.view().material.table = doctest::G(2100);
    t.app.view().patch.patch = doctest::G(2300);
    t.app.view().spatial.event = doctest::G(8);
    t.app.view().buses.bus = doctest::G(2200);
    for (ui::Screen s : ui::kTabOrder) {
      for (std::uint32_t tab = 0; tab < 6u; ++tab) {
        t.app.view().screen = s;
        t.app.view().buses.tab = tab % 4u;
        t.app.view().music.tab = tab % 4u;
        t.app.view().profiler.tab = tab;
        t.app.view().link.tab = tab % 2u;
        t.Frame();
        CheckLimits(t.app.runtime(), ui::ScreenName(s), c.dpi);
        if (c.compose && tab == 0u) {
          tela::PixelSurface img;
          SND_CHECK_MSG(ui::Compose(t.app.runtime(), ui::ComposeOptions{}, &img), "compose %s", ui::ScreenName(s));
        }
        if (s != ui::Screen::Buses && s != ui::Screen::Music && s != ui::Screen::Profiler && s != ui::Screen::Link) break;
      }
    }
  }
}

SND_TEST(ToolUiLimits, ListsPageAndTheLongEventFits) {
  Editor t(ui::Screen::Event, 1280, 800, 1.0f, BigFiles(), ui::Workspace{}, &FillPatch);
  // 300 + events: the list pages.
  SND_REQUIRE(t.Has("ev.list.pager.next"));
  const std::string first = t.Label("ev.list.pager.page");
  SND_REQUIRE(t.Click("ev.list.pager.next"));
  SND_CHECK(t.Label("ev.list.pager.page") != first);
  // The long event: the lanes page, only visible clips get hits.
  t.app.view().event.event = doctest::G(2000);
  t.app.view().event.msPerPx = 1.0;
  t.Frame();
  SND_CHECK(t.Has("ev.lanes.next"));
  SND_CHECK(t.Has("ev.t0.c0") && !t.Has("ev.t39.c0"));
  std::uint32_t hits = 0;
  for (const auto& b : t.app.router().controls()) hits += b.hitId.rfind("ev.t0.c", 0) == 0u ? 1u : 0u;
  SND_CHECK(hits > 0u && hits < 30u);  // 30 clips of 60 ms each over 1800 ms: not all fit at 1 ms / px
  t.app.view().event.track = 39;
  t.Frame();
  SND_CHECK(t.Has("ev.t39.c0"));
  // 100 rules page.
  t.app.view().screen = ui::Screen::Music;
  t.app.view().music.tab = 2;
  t.app.view().music.event = doctest::G(88);
  t.Frame();
  SND_CHECK(t.Has("mu.rule.pager.next") && t.Has("mu.rule.0.from") && !t.Has("mu.rule.99.from"));
  SND_CHECK(t.Has("mu.rule.1.unreachable"));
  for (int i = 0; i < 60 && !t.Has("mu.rule.99.from"); ++i) t.Click("mu.rule.pager.next");
  SND_CHECK(t.Has("mu.rule.99.from"));
  // A candidate list of 300 + events pages too.
  t.app.view().screen = ui::Screen::Event;
  t.app.view().event.event = doctest::G(8);
  t.Frame();
  SND_REQUIRE(t.Click("ev.clip.addevent"));
  SND_REQUIRE(t.app.picker() != nullptr);
  SND_CHECK(t.app.picker()->items.size() > 300u);
  SND_CHECK(t.Has("picker.next"));
  SND_REQUIRE(t.Click("picker.next"));
  SND_CHECK_EQ(t.app.picker()->page, 1u);
  SND_REQUIRE(t.Click("picker.scrim"));
  SND_CHECK(t.app.picker() == nullptr);
}
