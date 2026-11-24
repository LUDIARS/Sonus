// Drawings of the editor screens.
#include "snd/tool/ui/editor_draw.hpp"

#include <algorithm>
#include <cmath>

namespace snd::tool::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;
tela::Stroke Line(tela::Color c, float w) { return tela::Stroke{c, w}; }
float ClampX(float x, float w) { return std::clamp(x, -2.0f, w + 2.0f); }

}  // namespace

// --- lanes ------------------------------------------------------------------------------------
tela::Rect LaneClipRect(const LaneLook& l, const LaneClip& c) {
  const float x0 = l.axis.X(c.startMs);
  const float x1 = c.infinite ? l.width : l.axis.X(std::max(c.endMs, c.startMs));
  if (x1 < 0.0f || x0 > l.width) return tela::Rect{};
  const float a = ClampX(x0, l.width), b = ClampX(std::max(x1, x0 + 2.0f), l.width);
  return tela::Rect{a, 4.0f, std::max(2.0f, b - a), std::max(2.0f, l.height - 8.0f)};
}

tela::Drawing DrawLane(const LaneLook& l, const Palette& p, LaneShape* shape) {
  SafeDrawing d(l.dpi);
  d.Rectangle(tela::Rect{0, 0, l.width, l.height}, l.selected ? Mix(p.canvas, p.accent, 0.12f) : p.canvas);
  d.Line(tela::Point{0.0f, l.height - 0.5f}, tela::Point{l.width, l.height - 0.5f}, Line(p.track, 1.0f));
  LaneShape s;
  // Boxes first (every clip gets one while the shapes last), the details after.
  for (const LaneClip& c : l.clips) {
    const tela::Rect r = LaneClipRect(l, c);
    s.clips.push_back(r);
    if (r.width <= 0.0f) continue;
    tela::Color fill = c.known ? Mix(p.accent, p.canvas, 0.45f) : Mix(p.failed, p.canvas, 0.5f);
    if (c.selected) fill = Mix(p.accent, p.text, 0.2f);
    d.Rectangle(r, fill, Line(c.selected ? p.text : Mix(p.accent, p.text, 0.3f), c.selected ? 2.0f : 1.0f), 3.0f);
  }
  for (std::size_t i = 0; i < l.clips.size(); ++i) {
    const LaneClip& c = l.clips[i];
    const tela::Rect r = s.clips[i];
    if (r.width <= 0.0f) continue;
    // Loop passes (at most 16 lines).
    if (c.passMs > 0.0 && (c.infinite || c.endMs > c.startMs + c.passMs)) {
      const double end = c.infinite ? l.axis.Ms(l.width) : c.endMs;
      int n = 0;
      for (double t = c.startMs + c.passMs; t < end - 1e-9 && n < 16; t += c.passMs, ++n) {
        const float x = l.axis.X(t);
        if (x < 0.0f || x > l.width) continue;
        d.Line(tela::Point{x, r.y + 2.0f}, tela::Point{x, r.y + r.height - 2.0f}, Line(p.textDim, 1.0f));
      }
    }
    if (c.fadeInMs > 0.0) {
      const float x = std::min(r.x + r.width, l.axis.X(c.startMs + c.fadeInMs));
      d.Polyline({tela::Point{r.x, r.y + r.height}, tela::Point{x, r.y}}, Line(p.text, 1.0f));
    }
    if (c.fadeOutMs > 0.0 && !c.infinite) {
      const float x = std::max(r.x, l.axis.X(c.endMs - c.fadeOutMs));
      d.Polyline({tela::Point{x, r.y}, tela::Point{r.x + r.width, r.y + r.height}}, Line(p.text, 1.0f));
    }
    if (c.infinite) d.Line(tela::Point{l.width - 6.0f, l.height * 0.5f}, tela::Point{l.width - 1.0f, l.height * 0.5f},
                           Line(p.text, 2.0f));
  }
  for (const auto& [from, to] : l.overlapsMs) {
    const float a = ClampX(l.axis.X(from), l.width);
    const float b = std::isfinite(to) ? ClampX(l.axis.X(to), l.width) : l.width;
    if (b > a) d.Rectangle(tela::Rect{a, 0.0f, b - a, 3.0f}, p.failed);
  }
  for (double t : l.actionsMs) {
    const float x = l.axis.X(t);
    if (x < 0.0f || x > l.width) continue;
    d.Line(tela::Point{x, 0.0f}, tela::Point{x, l.height}, Line(p.solo, 1.5f));
  }
  if (shape != nullptr) *shape = std::move(s);
  return d.Take();
}

tela::Drawing DrawRuler(const TimeAxis& axis, float width, float height, const Palette& p, float dpi,
                        std::vector<double>* ticks) {
  SafeDrawing d(dpi);
  d.Rectangle(tela::Rect{0, 0, width, height}, p.canvas);
  const double step = RulerStepMs(axis.msPerPx, 60.0f);
  const double first = std::ceil(axis.scrollMs / step) * step;
  std::vector<double> t;
  for (double ms = first; t.size() < 200u; ms += step) {
    const float x = axis.X(ms);
    if (x > width) break;
    d.Line(tela::Point{x, height * 0.4f}, tela::Point{x, height}, Line(p.textDim, 1.0f));
    t.push_back(ms);
  }
  if (ticks != nullptr) *ticks = std::move(t);
  return d.Take();
}

// --- curves -----------------------------------------------------------------------------------
tela::Drawing DrawCurve(const CurveLook& l, const Palette& p, CurveShape* shape) {
  SafeDrawing d(l.dpi);
  d.Rectangle(tela::Rect{0, 0, l.width, l.height}, p.canvas);
  const PlotAxes& a = l.axes;
  // Grid: quarters, and the zero lines when inside.
  for (float f : {0.25f, 0.5f, 0.75f}) {
    d.Line(tela::Point{0.0f, l.height * f}, tela::Point{l.width, l.height * f}, Line(p.track, 1.0f));
    d.Line(tela::Point{l.width * f, 0.0f}, tela::Point{l.width * f, l.height}, Line(p.track, 1.0f));
  }
  if (a.y0 < 0.0 && a.y1 > 0.0) d.Line(tela::Point{0, a.Y(0.0)}, tela::Point{l.width, a.Y(0.0)}, Line(p.textDim, 1.0f));
  CurveShape s;
  if (l.points.size() >= 2u) {
    std::vector<tela::Point> pts;
    const std::uint32_t n = 160;
    for (std::uint32_t i = 0; i <= n; ++i) {
      const float x = l.width * static_cast<float>(i) / static_cast<float>(n);
      const float v = EvaluateCurve(l.points, static_cast<float>(a.ValueX(x)));
      pts.push_back(tela::Point{x, a.Y(v)});
    }
    d.Polyline(pts, Line(p.accent, 2.0f), 64);
  }
  for (std::size_t i = 0; i < l.points.size(); ++i) {
    const tela::Point c{a.X(l.points[i].x), a.Y(l.points[i].y)};
    s.points.push_back(c);
    const float r = l.pointRadius;
    const bool sel = static_cast<std::int32_t>(i) == l.selected;
    d.Ellipse(tela::Rect{c.x - r, c.y - r, 2 * r, 2 * r}, sel ? p.text : p.accent, Line(p.text, sel ? 2.0f : 1.0f));
  }
  if (l.hasMarker) {
    const float x = a.X(l.markerX);
    const float y = a.Y(EvaluateCurve(l.points, static_cast<float>(l.markerX)));
    s.marker = tela::Point{x, y};
    d.Line(tela::Point{x, 0.0f}, tela::Point{x, l.height}, Line(p.solo, 1.0f));
    d.Ellipse(tela::Rect{x - 3.0f, y - 3.0f, 6.0f, 6.0f}, p.solo);
  }
  if (shape != nullptr) *shape = std::move(s);
  return d.Take();
}

// --- 3D -------------------------------------------------------------------------------------------
tela::Point YawHandle(tela::Point c, float yaw, float dist) {
  return tela::Point{c.x + dist * std::sin(yaw), c.y - dist * std::cos(yaw)};
}

tela::Drawing DrawTopDown(const TopDownLook& l, const Palette& p, TopDownShape* shape) {
  SafeDrawing d(l.dpi);
  d.Rectangle(tela::Rect{0, 0, l.width, l.height}, p.canvas);
  if (l.gridPx >= 8.0f) {
    int n = 0;
    for (float x = std::fmod(l.gridX, l.gridPx); x < l.width && n < 60; x += l.gridPx, ++n) {
      if (x >= 0.0f) d.Line(tela::Point{x, 0.0f}, tela::Point{x, l.height}, Line(p.track, 1.0f));
    }
    n = 0;
    for (float y = std::fmod(l.gridY, l.gridPx); y < l.height && n < 60; y += l.gridPx, ++n) {
      if (y >= 0.0f) d.Line(tela::Point{0.0f, y}, tela::Point{l.width, y}, Line(p.track, 1.0f));
    }
  }
  TopDownShape s;
  // maxDistance circle around the emitter.
  if (l.maxDistancePx > 0.0f) {
    const float r = std::min(l.maxDistancePx, 50000.0f);
    s.circle = tela::Rect{l.emitter.x - r, l.emitter.y - r, 2 * r, 2 * r};
    if (r < 4000.0f) d.Ellipse(s.circle, tela::Color{0, 0, 0, 0}, Line(p.textDim, 1.5f));
  }
  // Cone: inner and outer half angles around the emitter's facing.
  if (l.emitter3D && l.outerDeg < 360.0f) {
    for (float deg : {l.innerDeg, l.outerDeg}) {
      const float half = deg * 0.5f * kPi / 180.0f;
      const tela::Color c = deg == l.innerDeg ? p.solo : p.mute;
      d.Polyline({YawHandle(l.emitter, l.emitterYaw - half, l.coneLengthPx), l.emitter,
                  YawHandle(l.emitter, l.emitterYaw + half, l.coneLengthPx)},
                 Line(c, 1.5f));
    }
  }
  // Listener: a ring with its facing; emitter: a filled dot with its facing.
  s.listenerHandle = YawHandle(l.listener, l.listenerYaw, l.handlePx);
  s.emitterHandle = YawHandle(l.emitter, l.emitterYaw, l.handlePx);
  d.Line(l.listener, s.listenerHandle, Line(p.text, 2.0f));
  d.Ellipse(tela::Rect{s.listenerHandle.x - 4, s.listenerHandle.y - 4, 8, 8}, p.text);
  d.Ellipse(tela::Rect{l.listener.x - 9, l.listener.y - 9, 18, 18}, p.canvas, Line(p.text, 2.0f));
  d.Line(l.emitter, s.emitterHandle, Line(p.accent, 2.0f));
  d.Ellipse(tela::Rect{s.emitterHandle.x - 4, s.emitterHandle.y - 4, 8, 8}, p.accent);
  d.Ellipse(tela::Rect{l.emitter.x - 8, l.emitter.y - 8, 16, 16}, p.accent, Line(p.text, 1.0f));
  if (shape != nullptr) *shape = s;
  return d.Take();
}

// --- marks ---------------------------------------------------------------------------------------
tela::Drawing DrawMarks(const MarkLook& l, const Palette& p) {
  SafeDrawing d(l.dpi);
  d.Rectangle(tela::Rect{0, 0, l.width, l.height}, p.canvas);
  for (const Span& s : l.spans) {
    const float a = ClampX(std::min(s.x0, s.x1), l.width), b = ClampX(std::max(s.x0, s.x1), l.width);
    if (b <= a) continue;
    const float top = s.height > 0.0f ? s.top : 0.0f, h = s.height > 0.0f ? s.height : l.height;
    tela::Color c = Mix(p.accent, p.canvas, 0.55f);
    if (s.kind == SpanKind::FadeOut) c = Mix(p.mute, p.canvas, 0.4f);
    if (s.kind == SpanKind::FadeIn) c = Mix(p.solo, p.canvas, 0.4f);
    if (s.kind == SpanKind::Bridge) c = Mix(p.pending, p.canvas, 0.4f);
    if (s.kind == SpanKind::Lookahead) c = Mix(p.failed, p.canvas, 0.6f);
    d.Rectangle(tela::Rect{a, top, b - a, h}, c);
  }
  // Beats before bars before the rest (bars over beats, markers over bars).
  for (int pass = 0; pass < 3; ++pass) {
    for (const Mark& m : l.marks) {
      const int order = m.kind == MarkKind::Beat ? 0 : m.kind == MarkKind::Bar ? 1 : 2;
      if (order != pass || m.x < -1.0f || m.x > l.width + 1.0f) continue;
      tela::Color c = p.track;
      float w = 1.0f, top = 0.0f;
      switch (m.kind) {
        case MarkKind::Beat: c = p.track; top = l.height * 0.6f; break;
        case MarkKind::Bar: c = p.textDim; w = 1.5f; break;
        case MarkKind::Marker: c = p.solo; w = 2.0f; break;
        case MarkKind::Request: c = p.pending; w = 2.0f; break;
        case MarkKind::Fire: c = p.failed; w = 2.0f; break;
        case MarkKind::Target: c = p.accent; w = 2.0f; break;
        case MarkKind::Tempo: c = p.text; w = 1.5f; top = l.height * 0.5f; break;
      }
      if (m.selected) w += 1.5f;
      d.Line(tela::Point{m.x, top}, tela::Point{m.x, l.height}, Line(c, w));
    }
  }
  return d.Take();
}

// --- patch ----------------------------------------------------------------------------------------
tela::Point PatchInputPort(const tela::Rect& b, std::uint32_t i) {
  return tela::Point{b.x, b.y + 20.0f + static_cast<float>(i) * 16.0f};
}
tela::Point PatchOutputPort(const tela::Rect& b) { return tela::Point{b.x + b.width, b.y + 20.0f}; }

tela::Drawing DrawPatch(const PatchLook& l, const Palette& p) {
  SafeDrawing d(l.dpi);
  d.Rectangle(tela::Rect{0, 0, l.width, l.height}, p.canvas);
  for (const PatchWire& w : l.wires) {
    // A cubic from the output to the input (horizontal tangents), 16 segments.
    std::vector<tela::Point> pts;
    const float dx = std::max(30.0f, std::fabs(w.to.x - w.from.x) * 0.5f);
    for (int i = 0; i <= 16; ++i) {
      const float t = static_cast<float>(i) / 16.0f, u = 1.0f - t;
      const float x = u * u * u * w.from.x + 3 * u * u * t * (w.from.x + dx) + 3 * u * t * t * (w.to.x - dx) +
                      t * t * t * w.to.x;
      const float y = u * u * u * w.from.y + 3 * u * u * t * w.from.y + 3 * u * t * t * w.to.y + t * t * t * w.to.y;
      pts.push_back(tela::Point{x, y});
    }
    d.Polyline(pts, Line(w.control ? p.solo : p.accent, w.control ? 1.5f : 2.5f), 4);
  }
  for (const PatchNodeLook& n : l.nodes) {
    tela::Color fill = Mix(p.track, p.canvas, 0.3f);
    if (n.selected) fill = Mix(p.accent, p.canvas, 0.5f);
    d.Rectangle(n.box, fill, Line(n.pending ? p.pending : p.text, n.pending || n.selected ? 2.0f : 1.0f), 4.0f);
    for (std::uint32_t i = 0; i < n.inputs; ++i) {
      const tela::Point c = PatchInputPort(n.box, i);
      d.Ellipse(tela::Rect{c.x - 4, c.y - 4, 8, 8}, p.canvas, Line(p.text, 1.0f));
    }
    if (n.output) {
      const tela::Point c = PatchOutputPort(n.box);
      d.Ellipse(tela::Rect{c.x - 4, c.y - 4, 8, 8}, p.accent, Line(p.text, 1.0f));
    }
  }
  return d.Take();
}

// --- series ---------------------------------------------------------------------------------------
tela::Drawing DrawSeries(const SeriesLook& l, const Palette& p, std::vector<tela::Point>* out) {
  SafeDrawing d(l.dpi);
  d.Rectangle(tela::Rect{0, 0, l.width, l.height}, p.canvas);
  const float yMax = l.yMax > 0.0f && std::isfinite(l.yMax) ? l.yMax : 1.0f;
  auto Y = [&](float v) { return l.height - std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, yMax) / yMax * l.height; };
  for (float f : {0.25f, 0.5f, 0.75f}) {
    d.Line(tela::Point{0.0f, l.height * f}, tela::Point{l.width, l.height * f}, Line(p.track, 1.0f));
  }
  if (l.reference >= 0.0f) {
    tela::Stroke dash{p.budget, 1.5f, 6.0f, 4.0f};
    d.Polyline({tela::Point{0.0f, Y(l.reference)}, tela::Point{l.width, Y(l.reference)}}, dash);
  }
  const std::size_t n = std::min<std::size_t>(l.values.size(), 240u);
  const std::size_t first = l.values.size() - n;
  std::vector<tela::Point> pts;
  for (std::size_t i = 0; i < n; ++i) {
    const float x = GraphX(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(n), 0.0f, l.width);
    const float y = Y(l.values[first + i]);
    if (l.steps && !pts.empty()) pts.push_back(tela::Point{x, pts.back().y});
    pts.push_back(tela::Point{x, y});
  }
  if (pts.size() >= 2u) d.Polyline(pts, Line(p.accent, 1.5f));
  if (out != nullptr) *out = std::move(pts);
  return d.Take();
}

}  // namespace snd::tool::ui
