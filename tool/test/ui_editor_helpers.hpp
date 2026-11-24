// Helpers for the stage 5 screen tests: an App over an editable document (doc_fixture.hpp's
// in-memory project "Doc"), clicks by element id, candidate list picks, typing.
#ifndef SND_TOOL_TEST_UI_EDITOR_HELPERS_HPP
#define SND_TOOL_TEST_UI_EDITOR_HELPERS_HPP

#include <set>
#include <string>

#include "doc_fixture.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/editor_draw.hpp"
#include "snd/tool/ui/kit.hpp"
#include "ui_helpers.hpp"

namespace edtest {

using namespace snd::tool;
using uitest::Center;
using uitest::Find;
using uitest::Ids;

struct DocHolder {
  ProjectDocument doc;
  explicit DocHolder(const std::map<std::string, std::string>& files = doctest::BaseFiles()) {
    doctest::LoadBase(doc, files);
  }
};

struct Editor : DocHolder {
  Session s;
  ui::App app;
  uitest::Pointer ptr;
  std::uint64_t now = 1000000000ull;
  explicit Editor(ui::Screen screen, int width = 1280, int height = 800, float dpi = 1.0f,
                  const std::map<std::string, std::string>& files = doctest::BaseFiles(),
                  ui::Workspace extra = ui::Workspace{})
      : DocHolder(files), app(s, Desc(&doc, width, height, dpi, extra)), ptr(app) {
    Project p;
    p.Load(doc);
    s.UseProject(p);
    app.view().screen = screen;
    Frame();
  }
  static ui::AppDesc Desc(ProjectDocument* d, int w, int h, float dpi, ui::Workspace extra) {
    ui::AppDesc a;
    a.width = w;
    a.height = h;
    a.dpiScale = dpi;
    a.workspace = extra;
    a.workspace.document = d;
    return a;
  }
  void Frame() {
    now += 20000000ull;
    app.Declare(now);
  }
  const tela::PlacedElement* El(const std::string& id) const { return Find(app.runtime(), id); }
  bool Has(const std::string& id) const { return El(id) != nullptr; }
  // Clicks an element by id (DPI 1: logical = desktop pixels) and redeclares.
  bool Click(const std::string& id) {
    const tela::PlacedElement* e = El(id);
    if (e == nullptr) return false;
    const tela::Point c = Center(e);
    const float k = app.runtime().viewport().dpi_scale;
    ptr.Click(c.x * k, c.y * k);
    Frame();
    return true;
  }
  // Opens a PickField and picks the item whose label is `label`.
  bool Pick(const std::string& field, const std::string& label) {
    if (!Click(field) || app.picker() == nullptr) return false;
    for (int page = 0; page < 50; ++page) {
      for (int i = 0; i < 64; ++i) {
        const tela::PlacedElement* e = El("picker.item." + std::to_string(i));
        if (e == nullptr) break;
        if (e->element.label == label) return Click("picker.item." + std::to_string(i));
      }
      if (!Click("picker.next")) break;
    }
    return false;
  }
  // Types text into the entry that is open and presses Enter.
  void Type(const std::string& text, bool enter = true) {
    for (unsigned char c : text) {
      ui::KeyEvent k;
      k.text = c;
      app.Key(k);
    }
    if (enter) {
      ui::KeyEvent k;
      k.key = ui::kKeyEnter;
      app.Key(k);
    }
    Frame();
  }
  void Key(std::uint32_t key, std::uint32_t mods = 0) {
    ui::KeyEvent k;
    k.key = key;
    k.modifiers = mods;
    app.Key(k);
    Frame();
  }
  // Clears the open entry (Backspace until empty).
  void ClearEntry() {
    while (!app.text().text().empty()) {
      ui::KeyEvent k;
      k.key = ui::kKeyBackspace;
      app.Key(k);
    }
  }
  // Drags from the centre of an element by (dx, dy) logical pixels in `steps` moves.
  void Drag(const std::string& id, float dx, float dy, int steps = 4, std::uint32_t mods = 0) {
    const tela::PlacedElement* e = El(id);
    SND_CHECK_MSG(e != nullptr, "no element %s", id.c_str());
    if (e == nullptr) return;
    DragFrom(Center(e), dx, dy, steps, mods);
  }
  void DragFrom(tela::Point c, float dx, float dy, int steps = 4, std::uint32_t mods = 0) {
    const float k = app.runtime().viewport().dpi_scale;
    ptr.Down(c.x * k, c.y * k, mods);
    for (int i = 1; i <= steps; ++i) {
      const float f = static_cast<float>(i) / static_cast<float>(steps);
      ptr.Move((c.x + dx * f) * k, (c.y + dy * f) * k, mods);
      Frame();  // the declaration is replaced during the drag
    }
    ptr.Up((c.x + dx) * k, (c.y + dy) * k);
    ptr.Advance(1000);
    Frame();
  }
  std::string Label(const std::string& id) const {
    const tela::PlacedElement* e = El(id);
    return e != nullptr ? e->element.label : std::string();
  }
};

inline std::string Key(const std::string& guid) { return snd::tool::ui::ObjectKey(guid); }

}  // namespace edtest

#endif  // SND_TOOL_TEST_UI_EDITOR_HELPERS_HPP
