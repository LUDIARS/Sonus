// The music screen: segments (the time line with draggable markers, tempo points, markers, stems,
// the grid), playlists, the music event's states and rule table (unreachable rows, auto sort refused
// then allowed), the transition preview against MusicEditor::Preview and its audition, stingers.
#include <cmath>

#include "snd/test/test.hpp"
#include "snd/tool/music_editor.hpp"
#include "ui_editor_helpers.hpp"
#include "ui_music_fixture.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {
const std::string kSegA = doctest::G(83), kPA = doctest::G(86), kMusic = doctest::G(88);

struct MusicEditorRig : Editor {
  explicit MusicEditorRig(std::uint32_t tab, ui::Workspace ws = ui::Workspace{})
      : Editor(ui::Screen::Music, 1280, 800, 1.0f, MusicFiles(), ws, &AddMusicWaves) {
    app.view().music.tab = tab;
    Frame();
  }
};
}  // namespace

SND_TEST(ToolUiMusic, SegmentTimeLineMarkersAndTempo) {
  MusicEditorRig t(0);
  t.app.view().music.segment = kSegA;
  t.Frame();
  for (const char* id : {"mu.seg.asset", "mu.seg.bars", "mu.seg.entry", "mu.seg.exit", "mu.seg.preload", "mu.seg.tap",
                         "mu.seg.grid.apply", "mu.seg.line", "mu.tp.0.bpm"}) {
    SND_CHECK_MSG(t.Has(id), "missing %s", id);
  }
  for (const char* m : {"Drop", "Out"}) {
    for (const char* part : {"mu.seg.mk.", "mu.mk."}) {
      const std::string id = part + ui::NameKey(m) + (std::string(part) == "mu.mk." ? ".name" : "");
      SND_CHECK_MSG(t.Has(id), "missing %s", id.c_str());
    }
  }
  SegmentEditor se(t.doc, kSegA);
  // 4 bars at 120 BPM 4/4: 8 s over the line's width / 1.05.
  const tela::PlacedElement* line = t.El("mu.seg.line");
  SND_REQUIRE(line != nullptr);
  const double pps = line->bounds.width / (8.0 * 1.05);
  const std::size_t undo0 = t.doc.UndoCount();
  t.Drag("mu.seg.mk." + ui::NameKey("Drop"), 20.0f, 0.0f, 4);
  SND_CHECK_NEAR(static_cast<double>(se.Markers()[0].frame), 30000.0 + 20.0 / pps * 48000.0, 1.0);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0 + 1u);
  SND_CHECK_EQ(t.app.view().music.marker, std::string("Drop"));  // selected by name
  // Tempo: + adds a point one bar later; the bpm field.
  SND_REQUIRE(t.Click("mu.tp.add"));
  SND_REQUIRE_EQ(se.Tempo().size(), 2u);
  SND_CHECK_EQ(se.Tempo()[1].tick, 3840u);
  SND_REQUIRE(t.Click("mu.tp.1.bpm.inc"));
  SND_CHECK_NEAR(se.Tempo()[1].bpm, 121.0, 1e-9);
  SND_REQUIRE(t.Click("mu.tp.1.remove"));
  // Markers: rename by typing, new marker, stems.
  SND_REQUIRE(t.Click("mu.mk." + ui::NameKey("Out") + ".name"));
  t.ClearEntry();
  t.Type("End");
  SND_CHECK_EQ(se.Markers()[1].name, std::string("End"));
  SND_REQUIRE(t.Click("mu.mk.add"));
  SND_CHECK_EQ(se.Markers().size(), 3u);
  SND_REQUIRE(t.Click("mu.st.add"));
  SND_CHECK(t.Has("mu.st.0.first"));
  // The grid: 120 BPM from frame 0, bars to fill the 8 s material.
  SND_REQUIRE(t.Click("mu.seg.grid.apply"));
  SND_CHECK_EQ(NumberAt(t.doc.Find(kSegA)->doc, "bars", 0.0), 4.0);
}

SND_TEST(ToolUiMusic, PlaylistsEntries) {
  MusicEditorRig t(1);
  t.app.view().music.playlist = kPA;
  t.Frame();
  PlaylistEditor pe(t.doc, kPA);
  SND_CHECK_EQ(pe.EntryCount(), 1u);
  SND_REQUIRE(t.Pick("mu.pl.add", "SegB"));
  SND_CHECK_EQ(pe.EntryCount(), 2u);
  SND_REQUIRE(t.Click("mu.pl.e1.w.inc"));
  SND_CHECK_NEAR(NumberAt(t.doc.Find(kPA)->doc, "entries[1].weight", 0.0), 1.1, 1e-9);
  SND_REQUIRE(t.Click("mu.pl.e1.up"));
  SND_CHECK_EQ(StringAt(t.doc.Find(kPA)->doc, "entries[0].segment"), doctest::G(84));
  SND_REQUIRE(t.Pick("mu.pl.mode", "shuffle"));
  SND_CHECK_EQ(StringAt(t.doc.Find(kPA)->doc, "mode"), std::string("shuffle"));
}

SND_TEST(ToolUiMusic, RuleTableUnreachableAutoSortAndPreview) {
  MusicEditorRig t(2);
  SND_REQUIRE(t.app.view().music.event == kMusic);  // the only event with "music"
  MusicEditor me(t.doc, kMusic);
  for (const std::string& id : {"mu.state." + ui::NameKey("Explore") + ".name", "mu.state." + ui::NameKey("Battle") + ".pl",
                                "mu.state." + ui::NameKey("Explore") + ".initial", std::string("mu.rule.0.from"), std::string("mu.rule.2.late"),
                         std::string("mu.rule.1.unreachable"), std::string("mu.rule.2.unreachable"), std::string("mu.rule.warn"),
                                std::string("mu.pv.text"), std::string("mu.pv.line"), std::string("mu.la.title"),
                                std::string("mu.pv.play"), std::string("mu.pv.why")}) {
    SND_CHECK_MSG(t.Has(id), "missing %s", id.c_str());
  }
  SND_CHECK(!t.Has("mu.rule.0.unreachable"));
  // Auto sort is refused while rows are unreachable.
  SND_REQUIRE(t.Click("mu.rule.sort"));
  SND_CHECK(me.Rules()[0].from == "*" && me.Rules()[0].to == "*");
  // Move the catch-all down twice: nothing unreachable, then auto sort works.
  SND_REQUIRE(t.Click("mu.rule.0.down"));
  SND_REQUIRE(t.Click("mu.rule.1.down"));
  SND_CHECK(me.Unreachable().empty());
  SND_CHECK(!t.Has("mu.rule.warn"));
  SND_REQUIRE(t.Click("mu.rule.sort"));
  SND_CHECK(me.Rules()[0].from == "Explore");
  // Preview Explore -> Battle at 1 s: the screen shows the model's numbers.
  const TransitionPreview p = me.Preview("Explore", "Battle", 1.0, MusicSettings{});
  SND_REQUIRE(p.valid);
  char b[64];
  std::snprintf(b, sizeof b, "fires at %.3f s", p.fireSeconds);
  SND_CHECK(t.Label("mu.pv.text").find(b) != std::string::npos);
  SND_CHECK(t.Label("mu.pv.text").find("row " + std::to_string(p.rule + 1)) == 0u);
  // Edit a row: timing and the fade.
  SND_REQUIRE(t.Pick("mu.rule.0.timing", "nextBar"));
  SND_CHECK(me.Rules()[0].timing == Timing::NextBar);
  SND_REQUIRE(t.Click("mu.rule.0.fo.inc"));
  SND_CHECK_NEAR(me.Rules()[0].fadeOutBeats, 1.0, 1e-9);
  SND_REQUIRE(t.Click("mu.rule.add"));
  SND_CHECK_EQ(me.Rules().size(), 4u);
  SND_REQUIRE(t.Click("mu.rule.3.remove"));
  SND_CHECK_EQ(me.Rules().size(), 3u);
  // States: rename follows into the rules.
  SND_REQUIRE(t.Click("mu.state." + ui::NameKey("Battle") + ".name"));
  t.ClearEntry();
  t.Type("Fight");
  SND_CHECK(me.Rules()[0].to == "Fight");
  SND_REQUIRE(t.Click("mu.state.add"));
  SND_CHECK_EQ(me.States().size(), 3u);
}

SND_TEST(ToolUiMusic, Stingers) {
  MusicEditorRig t(3);
  const std::string hit = "mu.sg." + ui::NameKey("Hit");
  SND_CHECK(t.Has(hit + ".name") && t.Has(hit + ".stack"));
  SND_REQUIRE(t.Click(hit + ".stack"));
  SND_CHECK_EQ(StringAt(t.doc.Find(kMusic)->doc, "music.stingers[0].overlap"), std::string("stack"));
  SND_REQUIRE(t.Pick("mu.sg.add", "Shot"));
  const std::string second = "mu.sg." + ui::NameKey("Stinger 2");
  SND_CHECK(t.Has(second + ".name"));
  SND_REQUIRE(t.Click(second + ".remove"));
  SND_CHECK(!t.Has(second + ".name"));
}
