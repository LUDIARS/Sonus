// A tela::Drawing builder that never trips Tela's limits.
//
// Tela validates drawings by throwing (invalid coordinates, more than 256
// shapes or 4096 points, a CPU work budget of 64M per drawing, dashes on
// non-polylines). Sonus is built without exceptions, so every shape is
// checked here first and refused (counted in dropped()) instead: non-finite
// values, coordinates beyond +-100000, negative sizes, and the raster work
// with Tela's own formula (paint_drawing: for each shape the pixel box of its
// bounds padded by stroke / 2 + glow + 1 / scale, plus one, times its points)
// at the DPI scale the drawing will be painted at, against kWorkBudget.
//
// Polylines: one shape when that fits; otherwise split into shorter pieces
// (less work, more shapes) and, when the shapes run out, thinned (every other
// point, first and last kept) until the work fits. A polyline is added
// completely or not at all.
#ifndef SND_TOOL_UI_SAFE_DRAWING_HPP
#define SND_TOOL_UI_SAFE_DRAWING_HPP

#include <cstdint>
#include <vector>

#include <tela/drawing.hpp>

namespace snd::tool::ui {

// DPI scales the tool accepts (App::SetViewport and --dpi). 8 (Tela's limit) does not
// fit a worst-case timing graph into Tela's budget, 4 does.
inline constexpr float kMinDpiScale = 0.25f;
inline constexpr float kMaxDpiScale = 4.0f;

class SafeDrawing {
 public:
  static constexpr std::uint32_t kMaxShapes = 256;
  static constexpr std::uint32_t kMaxPoints = 4096;
  static constexpr double kWorkBudget = 60.0e6;  // Tela: 64M
  static constexpr std::uint32_t kPolylinePiece = 8;  // points per piece when a polyline is split

  // `dpi`: the scale the drawing will be painted at (default: the largest the tool accepts).
  explicit SafeDrawing(float dpi = kMaxDpiScale);

  bool Rectangle(tela::Rect r, tela::Color fill, tela::Stroke stroke = {{0, 0, 0, 0}, 0}, float radius = 0.0f);
  bool Ellipse(tela::Rect r, tela::Color fill, tela::Stroke stroke = {{0, 0, 0, 0}, 0});
  bool Line(tela::Point a, tela::Point b, tela::Stroke stroke);
  // At most `maxShapes` shapes for this polyline (a share of the drawing's 256).
  bool Polyline(const std::vector<tela::Point>& points, tela::Stroke stroke, std::uint32_t maxShapes = kMaxShapes);

  const tela::Drawing& drawing() const { return drawing_; }
  tela::Drawing Take() { return std::move(drawing_); }
  std::uint32_t shapes() const { return shapes_; }
  std::uint32_t dropped() const { return dropped_; }
  std::uint32_t thinned() const { return thinned_; }  // polylines drawn with fewer points
  double work() const { return work_; }
  float dpi() const { return dpi_; }

 private:
  double Work(tela::Rect extent, const tela::Stroke& stroke, std::size_t points) const;
  bool Admit(double work, std::uint32_t points);
  tela::Drawing drawing_;
  float dpi_;
  std::uint32_t shapes_ = 0;
  std::uint32_t points_ = 0;
  std::uint32_t dropped_ = 0;
  std::uint32_t thinned_ = 0;
  double work_ = 0.0;
};

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_SAFE_DRAWING_HPP
