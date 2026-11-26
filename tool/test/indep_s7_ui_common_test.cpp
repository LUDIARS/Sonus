// Independent tests of group U, common parts (indep_s7_ui_properties.md A, B, C): screens and
// their keys, the document's history / save / dirty state per screen, the tool's own text entry.
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/ui/control_math.hpp"

using namespace iu;
using ui::KeyEvent;

namespace {

std::uint32_t F(int n) { return ui::kKeyF1 + static_cast<std::uint32_t>(n) - 1u; }

}  // namespace

// Discovery aid (no checks): SND_INDEP_DUMP=1 prints every screen's element tree.
SND_TEST(IndepS7Ui, DumpScreens) {
  Rig r;
  for (ui::Screen sc : kAllScreens) {
    r.Show(sc);
    Dump(r.rt(), ui::ScreenName(sc));
  }
  Rig rich(LargeFiles(3, 2, 6, 4, 12));
  for (std::uint32_t g : {3u, 21u, 24u, 22u, 23u, 25u, 26u}) {
    rich.app->Navigate(G(g));
    rich.Frame();
    Dump(rich.rt(), ("rich " + std::to_string(g) + " " + ui::ScreenName(rich.app->view().screen)).c_str());
  }
  rich.app->Navigate(G(24));
  for (std::uint32_t tab = 0; tab < 4; ++tab) {
    rich.app->view().music.tab = tab;
    rich.Frame();
    Dump(rich.rt(), ("music tab " + std::to_string(tab)).c_str());
  }
  SND_CHECK(true);
}

// A1: names round trip, the 12 names of the README, unknown names refused.
SND_TEST(IndepS7Ui, ScreenNamesRoundTripAndUnknownNamesAreRefused) {
  const std::set<std::string> expected = {"mixer", "buses", "event", "params",  "spatial", "music",
                                          "material", "patch", "profiler", "replay", "link", "problems"};
  std::set<std::string> names;
  for (ui::Screen sc : kAllScreens) {
    const char* n = ui::ScreenName(sc);
    SND_REQUIRE(n != nullptr);
    names.insert(n);
    ui::Screen back = ui::Screen::Problems;
    SND_CHECK(ui::ParseScreen(n, &back));
    SND_CHECK(back == sc);
    SND_CHECK(ui::ScreenLabel(sc) != nullptr && std::strlen(ui::ScreenLabel(sc)) > 0);
  }
  SND_CHECK(names == expected);
  for (const char* bad : {"", "Mixer", "MIXER", " mixer", "mixer ", "nope", "3d", "12", "mixer\n"}) {
    ui::Screen out = ui::Screen::Link;
    SND_CHECK_MSG(!ui::ParseScreen(bad, &out), "accepted '%s'", bad);
    SND_CHECK(out == ui::Screen::Link);
  }
}

// A2: the tab order is a permutation of the 12 screens.
SND_TEST(IndepS7Ui, TabOrderIsAPermutation) {
  std::set<int> seen;
  for (ui::Screen sc : ui::kTabOrder) seen.insert(static_cast<int>(sc));
  SND_CHECK_EQ(seen.size(), std::size_t{ui::kScreenCount});
  SND_CHECK_EQ(*seen.begin(), 0);
  SND_CHECK_EQ(*seen.rbegin(), static_cast<int>(ui::kScreenCount) - 1);
}

// A3: F2..F10 select their screens (README), Ctrl+PageDown / PageUp walk the tab order and wrap.
SND_TEST(IndepS7Ui, FunctionKeysAndCtrlPageWalkTheTabs) {
  Rig r;
  const ui::Screen byKey[] = {ui::Screen::Mixer, ui::Screen::Profiler, ui::Screen::Buses, ui::Screen::Event,
                              ui::Screen::Params, ui::Screen::Spatial, ui::Screen::Music, ui::Screen::Material,
                              ui::Screen::Patch};
  for (int i = 0; i < 9; ++i) {
    r.Show(ui::Screen::Problems);
    r.Key(F(i + 2));
    SND_CHECK_MSG(r.app->view().screen == byKey[i], "F%d -> %s", i + 2, ui::ScreenName(r.app->view().screen));
  }
  r.Show(ui::kTabOrder[0]);
  for (std::uint32_t i = 1; i <= ui::kScreenCount; ++i) {
    r.Key(ui::kKeyPageDown, ui::kModControl);
    SND_CHECK_MSG(r.app->view().screen == ui::kTabOrder[i % ui::kScreenCount], "PageDown step %u", i);
  }
  for (std::uint32_t i = 1; i <= ui::kScreenCount; ++i) {
    r.Key(ui::kKeyPageUp, ui::kModControl);
    SND_CHECK_MSG(r.app->view().screen == ui::kTabOrder[(ui::kScreenCount - i) % ui::kScreenCount], "PageUp step %u", i);
  }
  // Ctrl+Tab / Ctrl+Shift+Tab are the same walk.
  r.Show(ui::kTabOrder[ui::kScreenCount - 1]);
  r.Key(ui::kKeyTab, ui::kModControl);
  SND_CHECK(r.app->view().screen == ui::kTabOrder[0]);
  r.Key(ui::kKeyTab, ui::kModControl | ui::kModShift);
  SND_CHECK(r.app->view().screen == ui::kTabOrder[ui::kScreenCount - 1]);
}

// A4: changing the screen ends a text entry (without committing it).
SND_TEST(IndepS7Ui, ChangingTheScreenEndsTheTextEntry) {
  // D-186: while an entry is open it takes every key: the screen keys (F4, Ctrl+PageDown, Ctrl+Tab)
  // do nothing (the screen stays, the entry stays open, nothing is committed). After Esc closes the
  // entry they switch. A screen change by other means (the tab button, OnScreen) ends the entry
  // without committing it.
  Rig r;
  r.Show(ui::Screen::Event);
  int commits = 0;
  r.app->BeginText("indep.field", "abc", [&](const std::string&) { ++commits; return true; }, false);
  SND_CHECK(r.app->text().active());
  for (ui::KeyEvent k : {ui::KeyEvent{F(4), 0, 0}, ui::KeyEvent{ui::kKeyPageDown, 0, ui::kModControl},
                         ui::KeyEvent{ui::kKeyTab, 0, ui::kModControl}, ui::KeyEvent{F(2), 0, 0}}) {
    r.Key(k.key, k.modifiers);
    SND_CHECK(r.app->view().screen == ui::Screen::Event);
    SND_CHECK(r.app->text().active());
  }
  SND_CHECK(r.app->text().text() == "abc");
  SND_CHECK_EQ(commits, 0);
  r.Key(ui::kKeyEscape);
  SND_CHECK(!r.app->text().active());
  r.Key(F(4));
  SND_CHECK(r.app->view().screen == ui::Screen::Buses);
  SND_CHECK_EQ(commits, 0);
  r.app->BeginText("indep.field", "abc", [&](const std::string&) { ++commits; return true; }, false);
  r.app->OnScreen(ui::Screen::Params);
  r.Frame();
  SND_CHECK(!r.app->text().active());
  SND_CHECK_EQ(commits, 0);
  // The tab button too.
  r.app->BeginText("indep.field", "abc", [&](const std::string&) { ++commits; return true; }, false);
  r.ClickId("tab.event");
  SND_CHECK(r.app->view().screen == ui::Screen::Event);
  SND_CHECK(!r.app->text().active());
  SND_CHECK_EQ(commits, 0);
}

// B1: on the editor screens Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z undo and redo the document.
SND_TEST(IndepS7Ui, CtrlZUndoesTheDocumentOnEditorScreens) {
  for (ui::Screen sc : kAllScreens) {
    if (sc == ui::Screen::Mixer) continue;
    Rig r;
    r.Show(sc);
    SND_REQUIRE(r.doc.SetNumber(G(3), "volumeDb", -7.0));
    r.Frame();
    SND_REQUIRE_EQ(r.doc.UndoCount(), std::size_t{1});
    r.Key('Z', ui::kModControl);
    SND_CHECK_MSG(r.doc.UndoCount() == 0 && r.doc.RedoCount() == 1, "Ctrl+Z on %s", ui::ScreenName(sc));
    SND_CHECK_EQ(NumberAt(r.doc.Find(G(3))->doc, "volumeDb", 0), -1.5);
    r.Key('Y', ui::kModControl);
    SND_CHECK_MSG(r.doc.UndoCount() == 1, "Ctrl+Y on %s", ui::ScreenName(sc));
    r.Key('Z', ui::kModControl);
    r.Key('Z', ui::kModControl | ui::kModShift);
    SND_CHECK_MSG(r.doc.UndoCount() == 1, "Ctrl+Shift+Z on %s", ui::ScreenName(sc));
    SND_CHECK_EQ(NumberAt(r.doc.Find(G(3))->doc, "volumeDb", 0), -7.0);
  }
}

// B2: on the mixer Ctrl+Z is the live values' history; the document stays as it is.
SND_TEST(IndepS7Ui, CtrlZOnTheMixerUndoesLiveValuesOnly) {
  Rig r;
  r.Show(ui::Screen::Mixer);
  SND_REQUIRE(!r.s.mixer().buses().empty());
  SND_REQUIRE(r.doc.SetNumber(G(5), "volumeDb", -2.0));  // a category: an edit the mixer's bind follows
  r.Frame();
  const Target t{Control::BusFader, static_cast<std::int32_t>(r.s.mixer().buses().size() - 1), 0, 0};
  const float before = r.s.mixer().Get(t);
  SND_REQUIRE(r.s.mixer().Set(t, before - 5.0f));
  r.Frame();
  const std::size_t docUndos = r.doc.UndoCount();
  r.Key('Z', ui::kModControl);
  SND_CHECK_EQ(r.s.mixer().Get(t), before);
  SND_CHECK_EQ(r.doc.UndoCount(), docUndos);
}

// B3: an edit makes the App dirty, Ctrl+S saves (the document is clean and so is the App).
SND_TEST(IndepS7Ui, CtrlSSavesAndClearsTheDirtyMark) {
  doctest::TempDir dir("indep-ui-save");
  doctest::WriteAll(dir, doctest::BaseFiles());
  Rig r(doctest::BaseFiles(), true, 1280, 800, 1.0f, dir.path().string());
  r.Show(ui::Screen::Event);
  SND_CHECK(!r.app->dirty());
  SND_REQUIRE(r.doc.SetNumber(G(3), "volumeDb", -7.0));
  r.Frame();
  SND_CHECK(r.app->dirty());
  r.Key('S', ui::kModControl);
  SND_CHECK(!r.doc.IsDirty());
  SND_CHECK(!r.app->dirty());
  SND_CHECK(dir.Read("buses/Sfx.json").find("-7") != std::string::npos);
}

// B4: without a document nothing breaks; editor screens say how to start with a project.
SND_TEST(IndepS7Ui, NoDocumentEveryScreenAndKeyIsHarmless) {
  Rig r({}, false);
  for (ui::Screen sc : kAllScreens) {
    r.Show(sc);
    r.Key('Z', ui::kModControl);
    r.Key('Y', ui::kModControl);
    r.Key('S', ui::kModControl);
    r.Key(ui::kKeyDelete);
    SND_CHECK_MSG(FirstDuplicate(r.rt()).empty(), "duplicate on %s", ui::ScreenName(sc));
    SND_CHECK_MSG(LimitProblem(r.rt()).empty(), "%s", LimitProblem(r.rt()).c_str());
  }
  for (ui::Screen sc : {ui::Screen::Buses, ui::Screen::Event, ui::Screen::Params, ui::Screen::Spatial,
                        ui::Screen::Music, ui::Screen::Material, ui::Screen::Patch}) {
    r.Show(sc);
    SND_CHECK_MSG(r.AnyLabelContains("--project"), "%s does not say how to open a project", ui::ScreenName(sc));
  }
  SND_CHECK(!r.app->DocUndo());
  SND_CHECK(!r.app->DocRedo());
  SND_CHECK(!r.app->SaveDocument());
  SND_CHECK(!r.app->dirty());
}

// C1: UTF-8 writing of 1..4 byte code points, invalid ones ignored, Pop removes one character.
SND_TEST(IndepS7Ui, Utf8AppendAndPop) {
  std::string s;
  ui::AppendUtf8(&s, 'a');
  ui::AppendUtf8(&s, 0xE9);     // 2 bytes
  ui::AppendUtf8(&s, 0x3042);   // 3 bytes
  ui::AppendUtf8(&s, 0x1F600);  // 4 bytes
  SND_CHECK(s == std::string("a\xC3\xA9\xE3\x81\x82\xF0\x9F\x98\x80"));
  const std::string keep = s;
  for (std::uint32_t bad : {0xD800u, 0xDBFFu, 0xDC00u, 0xDFFFu, 0x110000u, 0xFFFFFFFFu}) {
    ui::AppendUtf8(&s, bad);
    SND_CHECK_MSG(s == keep, "code point %x was written", bad);
  }
  ui::PopUtf8(&s);
  SND_CHECK(s == std::string("a\xC3\xA9\xE3\x81\x82"));
  ui::PopUtf8(&s);
  SND_CHECK(s == std::string("a\xC3\xA9"));
  ui::PopUtf8(&s);
  SND_CHECK(s == "a");
  ui::PopUtf8(&s);
  SND_CHECK(s.empty());
  ui::PopUtf8(&s);
  SND_CHECK(s.empty());
  ui::AppendUtf8(&s, 0x10FFFF);
  SND_CHECK_EQ(s.size(), std::size_t{4});
}

// C2: what ParseNumber reads and refuses.
SND_TEST(IndepS7Ui, ParseNumberAcceptsFiniteNumbersOnly) {
  struct Ok {
    const char* t;
    double v;
  };
  for (const Ok& ok : {Ok{"12", 12.0}, Ok{"-3.5", -3.5}, Ok{"1e3", 1000.0}, Ok{"0", 0.0}, Ok{"2.5e-1", 0.25},
                       Ok{"-0", 0.0}}) {
    double v = 99.0;
    SND_CHECK_MSG(ui::ParseNumber(ok.t, &v), "refused '%s'", ok.t);
    SND_CHECK_EQ(v, ok.v);
  }
  for (const char* bad : {"", "-", ".", "1.2.3", "abc", "inf", "-inf", "nan", "NaN", "0x10", "1e999", "-1e999",
                          "1 2", " ", "1e", "e3", "--1", "12abc"}) {
    double v = 99.0;
    SND_CHECK_MSG(!ui::ParseNumber(bad, &v), "accepted '%s'", bad);
  }
}

// C3: FormatNumberText reads back (rounded to `decimals`).
SND_TEST(IndepS7Ui, FormatNumberTextReadsBack) {
  for (double v : {0.0, 1.0, -1.5, 0.125, 1234.5678, -0.0001, 1e6, 48000.0, 3.0e-4}) {
    for (int d : {0, 2, 4}) {
      const std::string t = ui::FormatNumberText(v, d);
      double back = 99.0;
      SND_CHECK_MSG(ui::ParseNumber(t, &back), "'%s' does not read", t.c_str());
      const double scale = std::pow(10.0, d);
      SND_CHECK_MSG(std::fabs(back - v) <= 0.5 / scale + 1e-12, "%g at %d -> %s", v, d,
                    t.c_str());
    }
  }
  // Values that need the decimals come back exactly.
  double back = 0.0;
  SND_CHECK(ui::ParseNumber(ui::FormatNumberText(-12.25, 4), &back));
  SND_CHECK_EQ(back, -12.25);
}

// C4: TextEntry keys: append, Backspace, Enter (commit, refused stays open), Escape, the byte cap,
// numeric entries, a new Begin replaces the old entry without committing it.
SND_TEST(IndepS7Ui, TextEntryKeys) {
  ui::TextEntry e;
  std::string committed;
  bool accept = false;
  int calls = 0;
  e.Begin("f", "ab", [&](const std::string& t) { ++calls; committed = t; return accept; }, false);
  SND_CHECK(e.active());
  SND_CHECK(e.text() == "ab");
  KeyEvent c;
  c.text = 'c';
  SND_CHECK(e.Key(c));
  SND_CHECK(e.text() == "abc");
  KeyEvent bs;
  bs.key = ui::kKeyBackspace;
  e.Key(bs);
  SND_CHECK(e.text() == "ab");
  KeyEvent enter;
  enter.key = ui::kKeyEnter;
  bool refused = false;
  e.Key(enter, &refused);
  SND_CHECK(refused);
  SND_CHECK(e.active());
  SND_CHECK_EQ(calls, 1);
  accept = true;
  refused = true;
  e.Key(enter, &refused);
  SND_CHECK(!refused);
  SND_CHECK(!e.active());
  SND_CHECK(committed == "ab");
  SND_CHECK_EQ(calls, 2);

  // Escape: no commit.
  e.Begin("f", "x", [&](const std::string&) { ++calls; return true; }, false);
  KeyEvent esc;
  esc.key = ui::kKeyEscape;
  e.Key(esc);
  SND_CHECK(!e.active());
  SND_CHECK_EQ(calls, 2);

  // The cap.
  e.Begin("f", "", [&](const std::string&) { return true; }, false);
  KeyEvent z;
  z.text = 0x3042;  // 3 bytes
  for (int i = 0; i < 400; ++i) e.Key(z);
  SND_CHECK(e.text().size() <= ui::TextEntry::kMaxBytes);
  SND_CHECK(e.text().size() >= ui::TextEntry::kMaxBytes - 3);
  SND_CHECK_EQ(e.text().size() % 3, std::size_t{0});  // never a cut character

  // Numeric: only digits, sign, '.', 'e'.
  e.Begin("n", "", [&](const std::string&) { return true; }, true);
  for (char ch : std::string("1a2b.x5-e+3 Q")) {
    KeyEvent k;
    k.text = static_cast<unsigned char>(ch);
    e.Key(k);
  }
  for (char ch : e.text()) {
    SND_CHECK_MSG(std::strchr("0123456789+-.eE", ch) != nullptr, "numeric entry took '%c'", ch);
  }
  SND_CHECK(e.text().find('1') != std::string::npos && e.text().find('5') != std::string::npos);

  // A new Begin replaces without committing.
  int firstCalls = 0;
  e.Begin("a", "one", [&](const std::string&) { ++firstCalls; return true; }, false);
  e.Begin("b", "two", [&](const std::string&) { return true; }, false);
  SND_CHECK_EQ(firstCalls, 0);
  SND_CHECK(e.field() == "b");
  SND_CHECK(e.text() == "two");
  // Keys while inactive are not used.
  e.Cancel();
  SND_CHECK(!e.Key(c));
}

// C4 through the App: Escape over an open entry is the entry's (the document is untouched).
SND_TEST(IndepS7Ui, AppEscapeCancelsTheEntryWithoutCommit) {
  Rig r;
  r.Show(ui::Screen::Event);
  int commits = 0;
  r.app->BeginText("indep.f", "Shot", [&](const std::string&) { ++commits; return true; }, false);
  r.Key(0, 0, 'X');
  SND_CHECK(r.app->text().text() == "ShotX");
  r.Key(ui::kKeyEscape);
  SND_CHECK(!r.app->text().active());
  SND_CHECK_EQ(commits, 0);
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{0});
  // Ctrl+Z inside an entry must not undo the document underneath it.
  SND_REQUIRE(r.doc.SetNumber(G(3), "volumeDb", -7.0));
  r.app->BeginText("indep.f", "", [&](const std::string&) { return true; }, false);
  r.Key('Z', ui::kModControl);
  // D-186: the entry takes the key; the document keeps its step.
  SND_CHECK(r.app->text().active());
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{1});
}
