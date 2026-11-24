// Pointer gestures on drawn controls.
#include "snd/tool/ui/pointer_router.hpp"

#include <algorithm>
#include <cmath>

#include "snd/id.hpp"

namespace snd::tool::ui {

void PointerRouter::SetControls(std::vector<ControlBinding> controls) {
  controls_ = std::move(controls);
  // Indexes change with the declaration; the ids of a drag in progress do not.
  drag_ = dragId_.empty() ? -1 : Find(dragId_);
  if (drag_ < 0) dragId_.clear();
  if (drag_ < 0 && custom_ != nullptr) EndDrag();  // the element of a custom gesture went away
  armed_ = armedId_.empty() ? -1 : Find(armedId_);
  if (armed_ < 0) armedId_.clear();
}

const ControlBinding* PointerRouter::active() const {
  return drag_ >= 0 ? &controls_[static_cast<std::size_t>(drag_)] : nullptr;
}

std::int32_t PointerRouter::Find(const std::string& hitId) const {
  for (std::size_t i = 0; i < controls_.size(); ++i) {
    if (controls_[i].hitId == hitId) return static_cast<std::int32_t>(i);
  }
  return -1;
}

std::int32_t PointerRouter::Under(const tela::Runtime& runtime, float x, float y) const {
  if (runtime.hit(x, y) != tela::InputPolicy::exclusive) return -1;
  // Topmost exclusive element under the point, as Tela's own hit test.
  const auto& placed = runtime.elements();
  for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
    if (it->element.input == tela::InputPolicy::passthrough || !it->clip.contains(x, y)) continue;
    return Find(it->element.id);
  }
  return -1;
}

bool PointerRouter::Pressed(const tela::Runtime& runtime, std::int32_t control) const {
  if (control < 0) return false;
  const tela::ElementState* s = runtime.state(controls_[static_cast<std::size_t>(control)].hitId);
  return s != nullptr && s->pressed;
}

float PointerRouter::ToNorm(const ControlBinding& b, float value) const {
  return b.kind == ControlKind::Fader ? FaderDbToPos(value) : KnobToNorm(value, b.range);
}

float PointerRouter::FromNorm(const ControlBinding& b, float norm) const {
  return b.kind == ControlKind::Fader ? FaderPosToDb(norm) : KnobFromNorm(norm, b.range);
}

bool PointerRouter::Local(const tela::Runtime& runtime, const std::string& id, float* x, float* y) const {
  for (const tela::PlacedElement& p : runtime.elements()) {
    if (p.element.id == id) {
      *x = p.bounds.x;
      *y = p.bounds.y;
      return true;
    }
  }
  return false;
}

void PointerRouter::StartDrag(const tela::Runtime& runtime, std::int32_t control, float x, float y, bool fine,
                              std::uint32_t modifiers) {
  const ControlBinding& b = controls_[static_cast<std::size_t>(control)];
  drag_ = control;
  dragId_ = b.hitId;
  anchorY_ = y;
  lastY_ = y;
  if (b.kind == ControlKind::Custom) {
    custom_ = b.handler;
    pressX_ = lastX_ = x;
    pressY_ = y;
    moved_ = false;
    originX_ = originY_ = 0.0f;
    Local(runtime, b.hitId, &originX_, &originY_);
    wheelId_.clear();
    if (custom_ != nullptr && custom_->press) custom_->press(x - originX_, y - originY_, modifiers);
    return;
  }
  anchorValue_ = sink_.ControlValue(b.target);
  anchorNorm_ = ToNorm(b, anchorValue_);
  anchorFine_ = fine;
  wheelId_.clear();
  sink_.ControlBegin(b.target);
}

void PointerRouter::EndDrag() {
  if (custom_ != nullptr || (drag_ >= 0 && controls_[static_cast<std::size_t>(drag_)].kind == ControlKind::Custom)) {
    const std::shared_ptr<DragHandler> h = std::move(custom_);
    custom_.reset();
    drag_ = -1;
    dragId_.clear();
    if (h != nullptr && h->release) h->release(moved_, lastX_ - originX_, lastY_ - originY_);
    return;
  }
  if (drag_ >= 0) sink_.ControlEnd(controls_[static_cast<std::size_t>(drag_)].target);
  drag_ = -1;
  dragId_.clear();
}

bool PointerRouter::Cancel() {
  if (drag_ < 0 && custom_ == nullptr) return false;
  if (custom_ != nullptr) {
    const std::shared_ptr<DragHandler> h = std::move(custom_);
    custom_.reset();
    drag_ = -1;
    dragId_.clear();
    if (h->cancel) h->cancel();
  } else {
    const ControlBinding& b = controls_[static_cast<std::size_t>(drag_)];
    const Target t = b.target;
    drag_ = -1;
    dragId_.clear();
    sink_.ControlCancel(t, anchorValue_);
  }
  armed_ = -1;
  armedId_.clear();
  return true;
}

void PointerRouter::Tick(const tela::Runtime& runtime) {
  if (drag_ >= 0 && !Pressed(runtime, drag_)) EndDrag();
  if (armed_ >= 0 && drag_ < 0 && !Pressed(runtime, armed_) && !runtime.captured()) {
    armed_ = -1;
    armedId_.clear();
  }
}

bool PointerRouter::Pointer(tela::Runtime& runtime, const tela::HostPointerEvent& e, std::uint64_t timeMs,
                            bool forward) {
  bool used = false;
  if (forward && e.phase != tela::PointerPhase::wheel) used = runtime.pointer(e, tela::InputSource::native);
  const tela::Viewport& vp = runtime.viewport();
  const float scale = vp.dpi_scale > 0.0f ? vp.dpi_scale : 1.0f;
  const float x = static_cast<float>(e.desktop_x - vp.desktop_x) / scale;
  const float y = static_cast<float>(e.desktop_y - vp.desktop_y) / scale;
  const bool fine = (e.modifiers & kModShift) != 0u;

  switch (e.phase) {
    case tela::PointerPhase::down: {
      if (e.button != tela::PointerButton::primary) break;
      if (drag_ >= 0) EndDrag();
      const std::int32_t c = Under(runtime, x, y);
      if (c < 0) {
        havePress_ = false;
        break;
      }
      const ControlBinding& b = controls_[static_cast<std::size_t>(c)];
      const bool doubleClick = havePress_ && lastPressId_ == b.hitId && timeMs >= lastPressMs_ &&
                               timeMs - lastPressMs_ <= params_.doubleClickMs &&
                               std::fabs(x - lastPressX_) <= params_.doubleClickPx &&
                               std::fabs(y - lastPressY_) <= params_.doubleClickPx;
      const bool customDouble = b.kind == ControlKind::Custom && b.handler != nullptr && b.handler->doubleClick;
      if (doubleClick && (b.kind != ControlKind::Custom || customDouble)) {
        havePress_ = false;
        armed_ = -1;
        armedId_.clear();
        if (customDouble) {
          float ox = 0.0f, oy = 0.0f;
          Local(runtime, b.hitId, &ox, &oy);
          const std::shared_ptr<DragHandler> h = b.handler;  // the call may replace the controls
          h->doubleClick(x - ox, y - oy);
        } else {
          sink_.ControlReset(b.target);
        }
        return true;
      }
      havePress_ = true;
      lastPressId_ = b.hitId;
      lastPressMs_ = timeMs;
      lastPressX_ = x;
      lastPressY_ = y;
      armed_ = c;
      armedId_ = b.hitId;
      armedX_ = x;
      armedY_ = y;
      armedMods_ = e.modifiers;
      if (Pressed(runtime, c)) StartDrag(runtime, c, x, y, fine, e.modifiers);
      used = true;
      break;
    }
    case tela::PointerPhase::move: {
      if (drag_ < 0 && armed_ >= 0 && Pressed(runtime, armed_)) {
        StartDrag(runtime, armed_, armedX_, armedY_, (armedMods_ & kModShift) != 0u, armedMods_);
      }
      if (drag_ < 0) break;
      if (!Pressed(runtime, drag_)) {  // Tela cancelled the gesture
        EndDrag();
        break;
      }
      const ControlBinding& b = controls_[static_cast<std::size_t>(drag_)];
      if (b.kind == ControlKind::Custom) {
        DragMotion m;
        m.dx = x - pressX_;
        m.dy = y - pressY_;
        m.ddx = x - lastX_;
        m.ddy = y - lastY_;
        m.x = x - originX_;
        m.y = y - originY_;
        m.modifiers = e.modifiers;
        lastX_ = x;
        lastY_ = y;
        if (m.dx != 0.0f || m.dy != 0.0f) moved_ = true;
        const std::shared_ptr<DragHandler> h = custom_;
        if (h != nullptr && h->drag && moved_) h->drag(m);
        used = true;
        break;
      }
      if (fine != anchorFine_) {  // re-anchor at the previous position so that the value does not jump
        anchorValue_ = sink_.ControlValue(b.target);
        anchorNorm_ = ToNorm(b, anchorValue_);
        anchorY_ = lastY_;
        anchorFine_ = fine;
      }
      lastY_ = y;
      const float travel = b.travel > 0.0f ? b.travel : 1.0f;
      if (y == anchorY_) {  // no vertical motion: the exact value, never a round trip through the law
        sink_.ControlSet(b.target, anchorValue_, 0);
        used = true;
        break;
      }
      const float norm = std::clamp(anchorNorm_ + (anchorY_ - y) / travel * (fine ? kFineFactor : 1.0f), 0.0f, 1.0f);
      sink_.ControlSet(b.target, FromNorm(b, norm), 0);
      used = true;
      break;
    }
    case tela::PointerPhase::up:
    case tela::PointerPhase::cancel: {
      if (drag_ >= 0 || custom_ != nullptr) {
        EndDrag();
        used = true;
      }
      armed_ = -1;
      armedId_.clear();
      if (e.phase == tela::PointerPhase::cancel) havePress_ = false;
      break;
    }
    case tela::PointerPhase::wheel: {
      if (drag_ >= 0) break;
      const std::int32_t c = Under(runtime, x, y);
      if (c < 0 || !std::isfinite(e.wheel_y) || e.wheel_y == 0.0f) {
        wheelId_.clear();
        break;
      }
      const ControlBinding& b = controls_[static_cast<std::size_t>(c)];
      if (wheelId_ != b.hitId) {
        wheelId_ = b.hitId;
        ++wheelKey_;
        if (wheelKey_ == 0u) wheelKey_ = 1u;
      }
      if (b.kind == ControlKind::Custom) {
        const std::shared_ptr<DragHandler> h = b.handler;
        if (h == nullptr || !h->wheel) break;
        h->wheel(e.wheel_y, e.modifiers, HashId(b.hitId) ^ wheelKey_);
        used = true;
        break;
      }
      const float v = sink_.ControlValue(b.target);
      float next = v;
      if (b.kind == ControlKind::Fader) {
        next = FaderWheel(v, e.wheel_y, fine);
      } else {
        const float step = kKnobWheelStep * (fine ? kFineFactor : 1.0f);
        next = KnobFromNorm(KnobToNorm(v, b.range) + e.wheel_y * step, b.range);
        if (b.range.integer && next == v) next = KnobFromNorm(KnobToNorm(v, b.range), b.range) + (e.wheel_y > 0 ? 1.0f : -1.0f);
        next = std::clamp(next, b.range.minValue, b.range.maxValue);
      }
      sink_.ControlSet(b.target, next, HashId(b.hitId) ^ wheelKey_);
      used = true;
      break;
    }
  }
  return used;
}

}  // namespace snd::tool::ui
