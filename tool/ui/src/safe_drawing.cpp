// tela::Drawing builder with Tela's limits checked up front.
#include "snd/tool/ui/safe_drawing.hpp"

#include <algorithm>
#include <cmath>

namespace snd::tool::ui {
namespace {

constexpr float kMaxCoord = 100000.0f;

bool Finite(float v) { return std::isfinite(v) && std::fabs(v) <= kMaxCoord; }
bool FiniteRect(const tela::Rect& r) {
  return Finite(r.x) && Finite(r.y) && Finite(r.width) && Finite(r.height) && r.width >= 0.0f && r.height >= 0.0f;
}
bool ValidStroke(const tela::Stroke& s) {
  return std::isfinite(s.width) && s.width >= 0.0f && s.width <= 64.0f && std::isfinite(s.dash) &&
         std::isfinite(s.gap) && s.dash >= 0.0f && s.gap >= 0.0f && ((s.dash == 0.0f) == (s.gap == 0.0f)) &&
         std::isfinite(s.glow.radius) && s.glow.radius >= 0.0f && s.glow.radius <= 64.0f;
}

}  // namespace

bool SafeDrawing::Admit(tela::Rect extent, float strokeWidth, std::uint32_t points) {
  if (shapes_ + 1 > kMaxShapes || points_ + points > kMaxPoints) {
    ++dropped_;
    return false;
  }
  const double pad = strokeWidth / 2.0 + 2.0;
  const double w = (extent.width + 2.0 * pad) * kMaxDpi + 1.0;
  const double h = (extent.height + 2.0 * pad) * kMaxDpi + 1.0;
  const double work = w * h * std::max<std::uint32_t>(1u, points);
  if (work_ + work > kWorkBudget) {
    ++dropped_;
    return false;
  }
  work_ += work;
  ++shapes_;
  points_ += points;
  return true;
}

bool SafeDrawing::Rectangle(tela::Rect r, tela::Color fill, tela::Stroke stroke, float radius) {
  if (!FiniteRect(r) || !ValidStroke(stroke) || stroke.dash != 0.0f || !std::isfinite(radius) || radius < 0.0f) {
    ++dropped_;
    return false;
  }
  if (!Admit(r, stroke.width, 0)) return false;
  drawing_.rectangle(r, fill, stroke, radius);
  return true;
}

bool SafeDrawing::Ellipse(tela::Rect r, tela::Color fill, tela::Stroke stroke) {
  if (!FiniteRect(r) || !ValidStroke(stroke) || stroke.dash != 0.0f) {
    ++dropped_;
    return false;
  }
  if (!Admit(r, stroke.width, 0)) return false;
  drawing_.ellipse(r, fill, stroke);
  return true;
}

bool SafeDrawing::Line(tela::Point a, tela::Point b, tela::Stroke stroke) {
  return Polyline({a, b}, stroke);
}

bool SafeDrawing::Polyline(const std::vector<tela::Point>& points, tela::Stroke stroke) {
  if (points.size() < 2 || !ValidStroke(stroke)) {
    ++dropped_;
    return false;
  }
  for (const tela::Point& p : points) {
    if (!Finite(p.x) || !Finite(p.y)) {
      ++dropped_;
      return false;
    }
  }
  bool all = true;
  for (std::size_t start = 0; start + 1 < points.size(); start += kPolylinePiece - 1) {
    const std::size_t end = std::min(points.size(), start + kPolylinePiece);
    std::vector<tela::Point> piece(points.begin() + static_cast<std::ptrdiff_t>(start),
                                   points.begin() + static_cast<std::ptrdiff_t>(end));
    float x0 = piece[0].x, x1 = piece[0].x, y0 = piece[0].y, y1 = piece[0].y;
    for (const tela::Point& p : piece) {
      x0 = std::min(x0, p.x);
      x1 = std::max(x1, p.x);
      y0 = std::min(y0, p.y);
      y1 = std::max(y1, p.y);
    }
    if (!Admit(tela::Rect{x0, y0, x1 - x0, y1 - y0}, stroke.width, static_cast<std::uint32_t>(piece.size()))) {
      all = false;
      break;
    }
    drawing_.polyline(std::move(piece), stroke);
  }
  return all;
}

}  // namespace snd::tool::ui
