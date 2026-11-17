// Pointer gestures on the tool's drawn controls (faders, knobs).
//
// Tela gives a declaration press / release / activation for buttons only, and
// no pointer positions. A control is therefore declared as an exclusive Tela
// button (its "hit" element: Tela owns the gesture, keeps it across
// redeclarations, captures the mouse and cancels it on focus loss) with the
// control drawn on a passthrough canvas above it. The router follows the
// pointer itself: a drag runs while Tela reports the hit element pressed
// (Runtime::state(id)->pressed), so Tela stays the owner of every gesture.
//
// Hosts pass every pointer event (desktop physical pixels, as Tela's
// HostPointerEvent) with a millisecond time. `forward` = true also hands
// down / up / move / cancel to Runtime::pointer (headless and tests); hosts
// whose Tela adapter already feeds the runtime pass false and call the router
// after the adapter (Windows) or before it (macOS: the drag then starts on the
// first move after Tela granted the press).
//
// Rules (control_math.hpp): drag relative to the start; Shift (kModShift) =
// 1/10, re-anchored when Shift changes mid-drag; a second press on the same
// control within 400 ms and 4 px sets the bank value (one undo step, no drag);
// wheel notches step the value and merge into one undo step per control while
// they follow each other. A drag is one undo step (ControlBegin / ControlEnd).
#ifndef SND_TOOL_UI_POINTER_ROUTER_HPP
#define SND_TOOL_UI_POINTER_ROUTER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <tela/runtime.hpp>

#include "snd/tool/edit_history.hpp"
#include "snd/tool/ui/control_math.hpp"

namespace snd::tool::ui {

class ControlSink {
 public:
  virtual ~ControlSink() = default;
  virtual float ControlValue(const Target& t) const = 0;
  virtual float ControlDefault(const Target& t) const = 0;
  virtual void ControlBegin(const Target& t) = 0;
  virtual void ControlSet(const Target& t, float value, std::uint32_t mergeKey) = 0;
  virtual void ControlEnd(const Target& t) = 0;
};

enum class ControlKind : std::uint8_t { Fader = 0, Knob = 1 };

struct ControlBinding {
  std::string hitId;  // the exclusive button element under the drawing
  ControlKind kind = ControlKind::Fader;
  Target target;
  float travel = 1.0f;  // pixels for the whole range (fader travel, kKnobDragPixels)
  KnobRange range;      // knobs
};

struct RouterParams {
  std::uint64_t doubleClickMs = 400;
  float doubleClickPx = 4.0f;
};

class PointerRouter {
 public:
  explicit PointerRouter(ControlSink& sink, RouterParams params = RouterParams{}) : sink_(sink), params_(params) {}

  void SetControls(std::vector<ControlBinding> controls);
  const std::vector<ControlBinding>& controls() const { return controls_; }
  // true if the event was used by a control (or by Tela when forwarded).
  bool Pointer(tela::Runtime& runtime, const tela::HostPointerEvent& event, std::uint64_t timeMs, bool forward);
  // Once per frame: ends a drag whose Tela gesture was cancelled.
  void Tick(const tela::Runtime& runtime);

  bool dragging() const { return drag_ >= 0; }
  const ControlBinding* active() const;

 private:
  std::int32_t Find(const std::string& hitId) const;
  std::int32_t Under(const tela::Runtime& runtime, float x, float y) const;
  bool Pressed(const tela::Runtime& runtime, std::int32_t control) const;
  float ToNorm(const ControlBinding& b, float value) const;
  float FromNorm(const ControlBinding& b, float norm) const;
  void StartDrag(std::int32_t control, float y, bool fine);
  void EndDrag();

  ControlSink& sink_;
  RouterParams params_;
  std::vector<ControlBinding> controls_;
  // drag in progress (-1: none) and the press that may start one
  std::int32_t drag_ = -1;
  std::int32_t armed_ = -1;
  std::string dragId_, armedId_;
  float anchorY_ = 0.0f, anchorNorm_ = 0.0f, lastY_ = 0.0f;
  bool anchorFine_ = false;
  float armedY_ = 0.0f;
  // last press, for double clicks
  std::string lastPressId_;
  std::uint64_t lastPressMs_ = 0;
  float lastPressX_ = 0.0f, lastPressY_ = 0.0f;
  bool havePress_ = false;
  // wheel merge
  std::string wheelId_;
  std::uint32_t wheelKey_ = 0;
};

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_POINTER_ROUTER_HPP
