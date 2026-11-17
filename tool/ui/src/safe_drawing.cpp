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

tela::Rect Extent(const std::vector<tela::Point>& p, std::size_t a, std::size_t b) {
  float x0 = p[a].x, x1 = p[a].x, y0 = p[a].y, y1 = p[a].y;
  for (std::size_t i = a; i < b; ++i) {
    x0 = std::min(x0, p[i].x);
    x1 = std::max(x1, p[i].x);
    y0 = std::min(y0, p[i].y);
    y1 = std::max(y1, p[i].y);
  }
  return tela::Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

SafeDrawing::SafeDrawing(float dpi)
    : dpi_(std::isfinite(dpi) ? std::clamp(dpi, kMinDpiScale, 8.0f) : kMaxDpiScale) {}

double SafeDrawing::Work(tela::Rect e, const tela::Stroke& stroke, std::size_t points) const {
  // Tela's pixel_extent (without the clip, which only makes it smaller).
  const double pad = stroke.width / 2.0 + stroke.glow.radius + 1.0 / dpi_;
  const double w = (e.width + 2.0 * pad) * dpi_;
  const double h = (e.height + 2.0 * pad) * dpi_;
  return (std::ceil(w) + 2.0) * (std::ceil(h) + 2.0) * static_cast<double>(std::max<std::size_t>(1u, points));
}

bool SafeDrawing::Admit(double work, std::uint32_t points) {
  if (shapes_ + 1 > kMaxShapes || points_ + points > kMaxPoints || work_ + work > kWorkBudget) {
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
  if (!Admit(Work(r, stroke, 0), 0)) return false;
  drawing_.rectangle(r, fill, stroke, radius);
  return true;
}

bool SafeDrawing::Ellipse(tela::Rect r, tela::Color fill, tela::Stroke stroke) {
  if (!FiniteRect(r) || !ValidStroke(stroke) || stroke.dash != 0.0f) {
    ++dropped_;
    return false;
  }
  if (!Admit(Work(r, stroke, 0), 0)) return false;
  drawing_.ellipse(r, fill, stroke);
  return true;
}

bool SafeDrawing::Line(tela::Point a, tela::Point b, tela::Stroke stroke) { return Polyline({a, b}, stroke); }

bool SafeDrawing::Polyline(const std::vector<tela::Point>& input, tela::Stroke stroke, std::uint32_t maxShapes) {
  if (input.size() < 2 || !ValidStroke(stroke)) {
    ++dropped_;
    return false;
  }
  for (const tela::Point& p : input) {
    if (!Finite(p.x) || !Finite(p.y)) {
      ++dropped_;
      return false;
    }
  }
  const std::uint32_t shapeRoom = std::min(maxShapes, kMaxShapes - shapes_);
  std::vector<tela::Point> pts = input;
  for (;;) {
    const std::size_t m = pts.size();
    // Piece sizes to try: the whole polyline, then kPolylinePiece, then smaller.
    std::vector<std::size_t> sizes{m};
    for (std::size_t k = std::min<std::size_t>(kPolylinePiece, m - 1); k >= 2; --k) sizes.push_back(k);
    for (std::size_t k : sizes) {
      const std::size_t pieces = (m - 1 + (k - 2)) / (k - 1);
      const std::size_t points = m + pieces - 1;
      if (pieces > shapeRoom || points_ + points > kMaxPoints) continue;
      double work = 0.0;
      for (std::size_t start = 0; start + 1 < m; start += k - 1) {
        const std::size_t end = std::min(m, start + k);
        work += Work(Extent(pts, start, end), stroke, end - start);
      }
      if (work_ + work > kWorkBudget) continue;
      for (std::size_t start = 0; start + 1 < m; start += k - 1) {
        const std::size_t end = std::min(m, start + k);
        drawing_.polyline(std::vector<tela::Point>(pts.begin() + static_cast<std::ptrdiff_t>(start),
                                                   pts.begin() + static_cast<std::ptrdiff_t>(end)),
                          stroke);
      }
      work_ += work;
      shapes_ += static_cast<std::uint32_t>(pieces);
      points_ += static_cast<std::uint32_t>(points);
      if (m != input.size()) ++thinned_;
      return true;
    }
    if (m <= 2) break;
    // Thin: every other point, the first and the last kept.
    std::vector<tela::Point> next;
    for (std::size_t i = 0; i < m; i += 2) next.push_back(pts[i]);
    if ((m - 1) % 2 != 0) next.push_back(pts.back());
    pts = std::move(next);
  }
  ++dropped_;
  return false;
}

}  // namespace snd::tool::ui
