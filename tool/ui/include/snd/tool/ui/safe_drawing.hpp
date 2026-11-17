// A tela::Drawing builder that never trips Tela's limits.
//
// Tela validates drawings by throwing (invalid coordinates, more than 256
// shapes or 4096 points, a CPU work budget of 64M per drawing, dashes on
// non-polylines). Sonus is built without exceptions, so every shape is
// checked here first and refused (counted in dropped()) instead: non-finite
// values, coordinates beyond +-100000, negative sizes, and the estimated
// raster work (Tela's formula: pixel bounding box x points, summed over the
// drawing) for a DPI scale up to kMaxDpi, against Tela's budget (64M) less a margin. Long
// polylines are split into short pieces so that the work stays small. Above
// kMaxDpi a heavy canvas could still exceed Tela's budget; the hosts catch
// Tela's exception at the boundary.
#ifndef SND_TOOL_UI_SAFE_DRAWING_HPP
#define SND_TOOL_UI_SAFE_DRAWING_HPP

#include <cstdint>
#include <vector>

#include <tela/drawing.hpp>

namespace snd::tool::ui {

class SafeDrawing {
 public:
  static constexpr std::uint32_t kMaxShapes = 256;
  static constexpr std::uint32_t kMaxPoints = 4096;
  static constexpr float kMaxDpi = 2.0f;
  static constexpr double kWorkBudget = 60.0e6;  // Tela: 64M
  static constexpr std::uint32_t kPolylinePiece = 8;  // points per polyline piece

  bool Rectangle(tela::Rect r, tela::Color fill, tela::Stroke stroke = {{0, 0, 0, 0}, 0}, float radius = 0.0f);
  bool Ellipse(tela::Rect r, tela::Color fill, tela::Stroke stroke = {{0, 0, 0, 0}, 0});
  bool Line(tela::Point a, tela::Point b, tela::Stroke stroke);
  // Splits into pieces of kPolylinePiece points (sharing end points). Dashes are kept per piece.
  bool Polyline(const std::vector<tela::Point>& points, tela::Stroke stroke);

  const tela::Drawing& drawing() const { return drawing_; }
  tela::Drawing Take() { return std::move(drawing_); }
  std::uint32_t shapes() const { return shapes_; }
  std::uint32_t dropped() const { return dropped_; }

 private:
  bool Admit(tela::Rect extent, float strokeWidth, std::uint32_t points);
  tela::Drawing drawing_;
  std::uint32_t shapes_ = 0;
  std::uint32_t points_ = 0;
  std::uint32_t dropped_ = 0;
  double work_ = 0.0;
};

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_SAFE_DRAWING_HPP
