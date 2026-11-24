// The input layer of the stage 5 screens: the tool's text entry (UTF-8, numbers, Enter / Escape /
// Backspace, refusal), keys (screens, undo of the document vs the mixer, save, Delete), the custom
// gestures of the pointer router (press / drag / release across redeclarations, a vanished element,
// double click, wheel series as one undo step), number fields (drag, Shift, wheel), pickers.
#include <cmath>

#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/ui/text_entry.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

SND_TEST(ToolUiInput, TextEntryUtf8NumbersAndKeys) {
  std::string s;
  ui::AppendUtf8(&s, 'a');
  ui::AppendUtf8(&s, 0xE9);     // é
  ui::AppendUtf8(&s, 0x3042);   // あ
  ui::AppendUtf8(&s, 0x1F600);  // emoji
  ui::AppendUtf8(&s, 0xD800);   // a surrogate: not a character
  SND_CHECK_EQ(s, std::string("a\xC3\xA9\xE3\x81\x82\xF0\x9F\x98\x80"));
  ui::PopUtf8(&s);
  ui::PopUtf8(&s);
  SND_CHECK_EQ(s, std::string("a\xC3\xA9"));
  double v = 0.0;
  SND_CHECK(ui::ParseNumber(" -12.5 ", &v) && v == -12.5);
  SND_CHECK(ui::ParseNumber("1e3", &v) && v == 1000.0);
  SND_CHECK(!ui::ParseNumber("", &v));
  SND_CHECK(!ui::ParseNumber("1x", &v));
  SND_CHECK(!ui::ParseNumber("inf", &v));
  SND_CHECK(!ui::ParseNumber("1e999", &v));
  SND_CHECK_EQ(ui::FormatNumberText(1.5000, 4), std::string("1.5"));
  SND_CHECK_EQ(ui::FormatNumberText(-0.0, 2), std::string("0"));
  SND_CHECK_EQ(ui::FormatNumberText(2.0, 3), std::string("2"));

  ui::TextEntry te;
  std::string committed;
  bool accept = false;
  te.Begin("f", "ab", [&](const std::string& t) {
    committed = t;
    return accept;
  }, false);
  ui::KeyEvent k;
  k.text = 'c';
  SND_CHECK(te.Key(k));
  k = ui::KeyEvent{};
  k.key = ui::kKeyBackspace;
  te.Key(k);
  te.Key(k);
  SND_CHECK_EQ(te.text(), std::string("a"));
  k.key = ui::kKeyEnter;
  bool refused = false;
  te.Key(k, &refused);
  SND_CHECK(refused && te.active() && committed == "a");  // refused: still open
  accept = true;
  te.Key(k, &refused);
  SND_CHECK(!refused && !te.active());
  // Numbers take no letters; Ctrl passes through; Escape cancels.
  te.Begin("n", "", [](const std::string&) { return true; }, true);
  k = ui::KeyEvent{};
  k.text = 'x';
  te.Key(k);
  k.text = '7';
  te.Key(k);
  SND_CHECK_EQ(te.text(), std::string("7"));
  k = ui::KeyEvent{};
  k.key = 'S';
  k.modifiers = ui::kModControl;
  SND_CHECK(te.Key(k));  // D-186: the entry takes every key, shortcuts do nothing
  SND_CHECK_EQ(te.text(), std::string("7"));
  k = ui::KeyEvent{};
  k.key = ui::kKeyEscape;
  te.Key(k);
  SND_CHECK(!te.active());
  // At most kMaxBytes.
  te.Begin("l", std::string(300, 'z'), [](const std::string&) { return true; }, false);
  SND_CHECK_EQ(te.text().size(), ui::TextEntry::kMaxBytes);
}

SND_TEST(ToolUiInput, ScreenKeysUndoAndSave) {
  Editor t(ui::Screen::Mixer);
  t.Key(ui::kKeyF1 + 4u);  // F5
  SND_CHECK(t.app.view().screen == ui::Screen::Event);
  t.Key(ui::kKeyF1 + 1u);  // F2
  SND_CHECK(t.app.view().screen == ui::Screen::Mixer);
  t.Key(ui::kKeyPageDown, ui::kModControl);
  SND_CHECK(t.app.view().screen == ui::Screen::Buses);
  t.Key(ui::kKeyPageUp, ui::kModControl);
  t.Key(ui::kKeyPageUp, ui::kModControl);
  SND_CHECK(t.app.view().screen == ui::Screen::Problems);
  // Ctrl+Z on the mixer undoes the live values; elsewhere the document.
  t.app.OnScreen(ui::Screen::Mixer);
  const std::int32_t sfx = t.s.project().FindBus(snd::HashId("Sfx"));
  t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -20.0f);
  EventEditor(t.doc, doctest::G(8)).SetNumber(EventNumber::Priority, 7);
  t.Key('Z', ui::kModControl);
  SND_CHECK(t.s.mixer().buses()[static_cast<std::size_t>(sfx)].faderDb != -20.0f);
  SND_CHECK_EQ(EventEditor(t.doc, doctest::G(8)).NumberOf(EventNumber::Priority), 7.0);
  t.app.OnScreen(ui::Screen::Event);
  t.Key('Z', ui::kModControl);
  SND_CHECK(EventEditor(t.doc, doctest::G(8)).NumberOf(EventNumber::Priority) != 7.0);
  t.Key('Z', ui::kModControl | ui::kModShift);  // redo
  SND_CHECK_EQ(EventEditor(t.doc, doctest::G(8)).NumberOf(EventNumber::Priority), 7.0);
  // The footer's undo / redo / save and the unsaved mark.
  SND_CHECK(t.app.dirty());
  SND_CHECK(t.Label("hdr.project").find("unsaved") != std::string::npos);
  SND_REQUIRE(t.Click("hdr.undo"));
  SND_CHECK(EventEditor(t.doc, doctest::G(8)).NumberOf(EventNumber::Priority) != 7.0);
  SND_REQUIRE(t.Click("hdr.redo"));
  SND_CHECK_EQ(EventEditor(t.doc, doctest::G(8)).NumberOf(EventNumber::Priority), 7.0);
}

SND_TEST(ToolUiInput, SaveWritesTheDocument) {
  doctest::TempDir dir("ui-save");
  doctest::WriteAll(dir, doctest::BaseFiles());
  Session s;
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  ui::AppDesc d;
  d.workspace.document = &doc;
  ui::App app(s, d);
  app.view().screen = ui::Screen::Event;
  app.Declare(1);
  EventEditor(doc, doctest::G(8)).SetNumber(EventNumber::Priority, 9);
  app.Declare(2);
  SND_CHECK(app.dirty());
  ui::KeyEvent k;
  k.key = 'S';
  k.modifiers = ui::kModControl;
  SND_CHECK(app.Key(k));
  app.Declare(3);
  SND_CHECK(!app.dirty());
  SND_CHECK(dir.Read("events/Shot.json").find("\"priority\": 9") != std::string::npos);
}

SND_TEST(ToolUiInput, CustomGesturesAcrossRedeclarations) {
  Editor t(ui::Screen::Event);
  t.app.view().event.msPerPx = 1.0;
  t.Frame();
  // A drag keeps its handler while the clip's hit moves under it every frame.
  const tela::PlacedElement* clip = t.El("ev.t0.c0");
  SND_REQUIRE(clip != nullptr);
  const tela::Point c = Center(clip);
  t.ptr.Down(c.x, c.y);
  for (int i = 1; i <= 10; ++i) {
    t.ptr.Move(c.x + 3.0f * static_cast<float>(i), c.y);
    t.Frame();
  }
  SND_CHECK(t.app.router().dragging());
  // Shift does not change a position drag's mapping (absolute from the press).
  t.ptr.Move(c.x + 40.0f, c.y, ui::kModShift);
  t.Frame();
  t.ptr.Up(c.x + 40.0f, c.y);
  t.ptr.Advance(1000);
  t.Frame();
  SND_CHECK_EQ(EventEditor(t.doc, doctest::G(8)).ClipNumberOf(0, 0, ClipNumber::StartMs), 40.0);
  // The element of a gesture going away ends it (release is called, the step closes).
  const std::size_t undo0 = t.doc.UndoCount();
  clip = t.El("ev.t0.c0");
  const tela::Point c2 = Center(clip);
  t.ptr.Down(c2.x, c2.y);
  t.ptr.Move(c2.x + 10.0f, c2.y);
  t.app.OnScreen(ui::Screen::Mixer);  // the clip's hit is no longer declared
  t.Frame();
  SND_CHECK(!t.app.router().dragging());
  SND_CHECK(!t.doc.InGroup());
  SND_CHECK_EQ(t.doc.UndoCount(), undo0 + 1u);
  t.ptr.Up(c2.x + 10.0f, c2.y);
  t.ptr.Advance(1000);
}

SND_TEST(ToolUiInput, NumberFieldDragShiftAndWheel) {
  Editor t(ui::Screen::Event);
  EventEditor ed(t.doc, doctest::G(8));
  const double r0 = ed.NumberOf(EventNumber::RetriggerMs);
  const std::size_t undo0 = t.doc.UndoCount();
  // 40 px up = 10 steps of 10 ms, one undo step.
  t.Drag("ev.f.n2", 0.0f, -40.0f, 4);
  SND_CHECK_NEAR(ed.NumberOf(EventNumber::RetriggerMs), r0 + 100.0, 1e-9);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0 + 1u);
  // With Shift: 1/10.
  t.Drag("ev.f.n2", 0.0f, -40.0f, 4, ui::kModShift);
  SND_CHECK_NEAR(ed.NumberOf(EventNumber::RetriggerMs), r0 + 110.0, 1e-9);
  // Down past the minimum: clamped at 0.
  t.Drag("ev.f.n2", 0.0f, 400.0f, 4);
  SND_CHECK_EQ(ed.NumberOf(EventNumber::RetriggerMs), 0.0);
  // Three wheel notches: one undo step.
  const std::size_t undo1 = t.doc.UndoCount();
  const tela::Point c = Center(t.El("ev.f.n2"));
  for (int i = 0; i < 3; ++i) {
    t.ptr.Wheel(c.x, c.y, 1.0f);
    t.Frame();
  }
  SND_CHECK_NEAR(ed.NumberOf(EventNumber::RetriggerMs), 30.0, 1e-9);
  SND_CHECK_EQ(t.doc.UndoCount(), undo1 + 1u);
  SND_CHECK(t.app.DocUndo());
  SND_CHECK_EQ(ed.NumberOf(EventNumber::RetriggerMs), 0.0);
}

SND_TEST(ToolUiInput, PickerOpensClosesAndEscapes) {
  Editor t(ui::Screen::Event);
  SND_REQUIRE(t.Click("ev.f.seq"));
  SND_REQUIRE(t.app.picker() != nullptr);
  SND_CHECK(t.Has("picker.title") && t.Has("picker.item.0") && t.Has("picker.cancel"));
  t.Key(ui::kKeyEscape);
  SND_CHECK(t.app.picker() == nullptr);
  SND_REQUIRE(t.Click("ev.f.seq"));
  SND_REQUIRE(t.Click("picker.cancel"));
  SND_CHECK(t.app.picker() == nullptr);
  // Starting a text entry closes an open list and the other way round.
  SND_REQUIRE(t.Click("ev.f.seq"));
  t.app.BeginText("x", "", [](const std::string&) { return true; }, false);
  SND_CHECK(t.app.picker() == nullptr);
  t.Frame();
  SND_REQUIRE(t.Click("ev.f.seq"));
  SND_CHECK(!t.app.text().active());
}
