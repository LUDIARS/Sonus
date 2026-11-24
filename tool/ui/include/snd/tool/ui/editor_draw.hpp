// Drawings of the editor screens (stage 5). Like widgets.hpp: each function returns a drawing in
// the canvas's local logical coordinates, built with SafeDrawing (never beyond Tela's limits: what
// does not fit is dropped and counted), and fills a shape struct with the geometry it used where
// a test needs it. Positions come from control_math.hpp (TimeAxis, PlotAxes) or the model's own
// view transform (3D: SpatialPreview::WorldToScreen).
#ifndef SND_TOOL_UI_EDITOR_DRAW_HPP
#define SND_TOOL_UI_EDITOR_DRAW_HPP

#include <cstdint>
#include <vector>

#include <tela/drawing.hpp>

#include "snd/tool/curve_editor.hpp"
#include "snd/tool/ui/control_math.hpp"
#include "snd/tool/ui/palette.hpp"
#include "snd/tool/ui/safe_drawing.hpp"

namespace snd::tool::ui {

// --- Event time line: one track lane --------------------------------------------------------
struct LaneClip {
  double startMs = 0.0, endMs = 0.0, passMs = 0.0, fadeInMs = 0.0, fadeOutMs = 0.0;
  bool infinite = false, known = true, selected = false;
};
struct LaneLook {
  float dpi = kMaxDpiScale;
  float width = 600.0f, height = 40.0f;
  TimeAxis axis;
  std::vector<LaneClip> clips;
  std::vector<double> actionsMs;
  std::vector<std::pair<double, double>> overlapsMs;  // to = +inf for an infinite overlap
  bool selected = false;  // the track
};
struct LaneShape {
  std::vector<tela::Rect> clips;  // box of each clip (local), empty rect when not visible
};
// Clip box: x from axis.X(start) to axis.X(end) (an infinite clip runs to the right edge), the
// lane's height minus 4 px top and bottom. Loop passes, fades, overlaps and actions on top.
tela::Rect LaneClipRect(const LaneLook& look, const LaneClip& clip);
tela::Drawing DrawLane(const LaneLook& look, const Palette& p, LaneShape* shape = nullptr);
// Ruler ticks every RulerStepMs(msPerPx, 60) ms; `ticks` gets the ms of each tick drawn.
tela::Drawing DrawRuler(const TimeAxis& axis, float width, float height, const Palette& p, float dpi,
                        std::vector<double>* ticks = nullptr);

// --- value plots (curves) ------------------------------------------------------------------------
struct CurveLook {
  float dpi = kMaxDpiScale;
  float width = 300.0f, height = 160.0f;
  PlotAxes axes;
  std::vector<CurvePoint> points;
  std::int32_t selected = -1;
  bool hasMarker = false;  // a vertical line at markerX (the current input) and its value
  double markerX = 0.0;
  float pointRadius = 4.0f;
};
struct CurveShape {
  std::vector<tela::Point> points;  // centres of the points
  tela::Point marker{};             // (x of the input, y of EvaluateCurve there)
};
// The curve is drawn by sampling EvaluateCurve over the visible x range (what the runtime reads).
tela::Drawing DrawCurve(const CurveLook& look, const Palette& p, CurveShape* shape = nullptr);

// --- 3D top-down view -----------------------------------------------------------------------------
struct TopDownLook {
  float dpi = kMaxDpiScale;
  float width = 500.0f, height = 400.0f;
  float gridPx = 50.0f;          // a grid line every gridPx (0 = none), through gridX / gridY
  float gridX = 0.0f, gridY = 0.0f;
  tela::Point listener{};        // screen positions
  tela::Point emitter{};
  float listenerYaw = 0.0f;      // radians, 0 = up the screen, positive = toward +x (right)
  float emitterYaw = 0.0f;
  float maxDistancePx = 0.0f;    // circle around the emitter (0 = none)
  float innerDeg = 360.0f, outerDeg = 360.0f;  // cone (full angles); 360 = none
  float coneLengthPx = 60.0f;
  bool emitter3D = true;
  float handlePx = 28.0f;        // distance of the facing handles from their centre
};
struct TopDownShape {
  tela::Point listenerHandle{}, emitterHandle{};
  tela::Rect circle{};           // the maxDistance circle's bounds
};
tela::Point YawHandle(tela::Point centre, float yaw, float distancePx);
tela::Drawing DrawTopDown(const TopDownLook& look, const Palette& p, TopDownShape* shape = nullptr);

// --- marks on a time line (music) ---------------------------------------------------------------
enum class MarkKind : std::uint8_t { Beat = 0, Bar = 1, Marker = 2, Request = 3, Fire = 4, Target = 5, Tempo = 6 };
enum class SpanKind : std::uint8_t { Segment = 0, FadeOut = 1, FadeIn = 2, Bridge = 3, Lookahead = 4 };
struct Mark {
  float x = 0.0f;
  MarkKind kind = MarkKind::Beat;
  bool selected = false;
};
struct Span {
  float x0 = 0.0f, x1 = 0.0f;
  SpanKind kind = SpanKind::Segment;
  float top = 0.0f, height = 0.0f;  // 0 height = the whole lane
};
struct MarkLook {
  float dpi = kMaxDpiScale;
  float width = 600.0f, height = 60.0f;
  std::vector<Span> spans;
  std::vector<Mark> marks;
};
tela::Drawing DrawMarks(const MarkLook& look, const Palette& p);

// --- a patch graph -------------------------------------------------------------------------------
struct PatchNodeLook {
  tela::Rect box{};
  std::uint32_t inputs = 0;
  bool output = true;
  bool selected = false;
  bool pending = false;  // the "connect from" node
};
struct PatchWire {
  tela::Point from{}, to{};
  bool control = false;  // control signal (dashed look: thinner)
};
struct PatchLook {
  float dpi = kMaxDpiScale;
  float width = 600.0f, height = 400.0f;
  std::vector<PatchNodeLook> nodes;
  std::vector<PatchWire> wires;
};
// Ports: input i of a node at (box.x, box.y + 20 + i * 16), the output at (box.right, box.y + 20).
tela::Point PatchInputPort(const tela::Rect& box, std::uint32_t input);
tela::Point PatchOutputPort(const tela::Rect& box);
tela::Drawing DrawPatch(const PatchLook& look, const Palette& p);

// --- a value series (CPU load, LOD history) --------------------------------------------------------
struct SeriesLook {
  float dpi = kMaxDpiScale;
  float width = 400.0f, height = 120.0f;
  std::vector<float> values;  // oldest first (at most 240 drawn: the newest)
  float yMax = 1.0f;          // top of the plot
  float reference = -1.0f;    // a dashed line (e.g. load 1 = the budget), < 0 none
  bool steps = false;         // a step line (levels)
};
tela::Drawing DrawSeries(const SeriesLook& look, const Palette& p, std::vector<tela::Point>* points = nullptr);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_EDITOR_DRAW_HPP
