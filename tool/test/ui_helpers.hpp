// Helpers for the UI tests: an App over an offline Session, element lookup and
// simulated pointer gestures (Tela HostPointerEvent, desktop = logical at DPI 1).
#ifndef SND_TOOL_TEST_UI_HELPERS_HPP
#define SND_TOOL_TEST_UI_HELPERS_HPP

#include <set>
#include <string>

#include <tela/runtime.hpp>

#include "snd/tool/names.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/control_math.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "tool_fixture.hpp"

namespace uitest {

using namespace snd::tool;

inline const tela::PlacedElement* Find(const tela::Runtime& rt, const std::string& id) {
  for (const tela::PlacedElement& p : rt.elements()) {
    if (p.element.id == id) return &p;
  }
  return nullptr;
}

inline std::set<std::string> Ids(const tela::Runtime& rt) {
  std::set<std::string> s;
  for (const tela::PlacedElement& p : rt.elements()) s.insert(p.element.id);
  return s;
}

// Drives pointer events into an App (forwarded to Tela, as a headless host does).
class Pointer {
 public:
  explicit Pointer(ui::App& app) : app_(app) {}
  bool Event(tela::PointerPhase phase, float x, float y, std::uint32_t mods = 0, float wheel = 0.0f) {
    tela::HostPointerEvent e;
    e.sequence = ++seq_;
    e.viewport_revision = app_.runtime().viewport().revision;
    if (phase == tela::PointerPhase::down) ++gesture_;
    e.gesture_id = gesture_;
    e.phase = phase;
    e.button = phase == tela::PointerPhase::move || phase == tela::PointerPhase::wheel ? tela::PointerButton::none
                                                                                       : tela::PointerButton::primary;
    e.desktop_x = static_cast<int>(std::lround(x));
    e.desktop_y = static_cast<int>(std::lround(y));
    e.wheel_y = wheel;
    e.modifiers = mods;
    return app_.Pointer(e, timeMs_, true);
  }
  bool Down(float x, float y, std::uint32_t mods = 0) { return Event(tela::PointerPhase::down, x, y, mods); }
  bool Move(float x, float y, std::uint32_t mods = 0) { return Event(tela::PointerPhase::move, x, y, mods); }
  bool Up(float x, float y) { return Event(tela::PointerPhase::up, x, y); }
  bool Wheel(float x, float y, float notches, std::uint32_t mods = 0) {
    return Event(tela::PointerPhase::wheel, x, y, mods, notches);
  }
  bool Cancel() { return Event(tela::PointerPhase::cancel, 0, 0); }
  void Click(float x, float y) {
    Down(x, y);
    Up(x, y);
    timeMs_ += 1000;  // never a double click
  }
  void Advance(std::uint64_t ms) { timeMs_ += ms; }

 private:
  ui::App& app_;
  std::uint64_t seq_ = 0, gesture_ = 0, timeMs_ = 100000;
};

inline tela::Point Center(const tela::PlacedElement* p) {
  return tela::Point{p->bounds.x + p->bounds.width * 0.5f, p->bounds.y + p->bounds.height * 0.5f};
}

}  // namespace uitest

#endif  // SND_TOOL_TEST_UI_HELPERS_HPP
