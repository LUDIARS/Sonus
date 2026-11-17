// Drawings of the tool's controls.
#include "snd/tool/ui/widgets.hpp"

#include <algorithm>
#include <cmath>

#include "snd/tool/ui/safe_drawing.hpp"

namespace snd::tool::ui {
namespace {

tela::Stroke NoStroke() { return tela::Stroke{{0, 0, 0, 0}, 0.0f}; }
tela::Stroke Line(tela::Color c, float w) { return tela::Stroke{c, w}; }

}  // namespace

tela::Drawing DrawFader(const FaderLook& l, const Palette& p, FaderShape* shape) {
  SafeDrawing d(l.dpi);
  d.Rectangle(tela::Rect{0, 0, l.width, l.height}, p.canvas);
  const FaderGeometry g{l.height, l.capHeight};
  const float cx = l.width * 0.5f;
  const float top = g.CapCenterY(1.0f), bottom = g.CapCenterY(0.0f);
  // scale ticks (0, -12, -30, -60 dB) and the track
  for (float db : {0.0f, -12.0f, -30.0f, -60.0f}) {
    const float y = g.CapCenterY(FaderDbToPos(db));
    const float half = db == 0.0f ? l.width * 0.5f - 2.0f : l.width * 0.25f;
    d.Line(tela::Point{cx - half, y}, tela::Point{cx + half, y}, Line(p.textDim, 1.0f));
  }
  d.Rectangle(tela::Rect{cx - 2.0f, top, 4.0f, bottom - top}, p.track, NoStroke(), 2.0f);
  const float capY = g.CapCenterY(l.pos);
  d.Rectangle(tela::Rect{cx - 2.0f, capY, 4.0f, bottom - capY}, WithAlpha(p.accent, 160), NoStroke(), 2.0f);
  // bank value marker on the right edge
  const float by = g.CapCenterY(l.bankPos);
  d.Line(tela::Point{l.width - 5.0f, by}, tela::Point{l.width - 1.0f, by}, Line(p.text, 2.0f));
  tela::Color cap = l.pressed ? Mix(p.accent, p.text, 0.35f) : p.accent;
  if (l.silenced) cap = Mix(cap, p.canvas, 0.6f);
  const tela::Rect capRect{2.0f, capY - l.capHeight * 0.5f, l.width - 4.0f, l.capHeight};
  d.Rectangle(capRect, cap, Line(Mix(cap, p.text, 0.5f), 1.0f), 3.0f);
  d.Line(tela::Point{4.0f, capY}, tela::Point{l.width - 4.0f, capY}, Line(p.text, 1.0f));
  if (shape != nullptr) *shape = FaderShape{capRect.y, capRect.y + capRect.height, capY};
  return d.Take();
}

tela::Drawing DrawKnob(const KnobLook& l, const Palette& p, KnobShape* shape) {
  SafeDrawing d(l.dpi);
  d.Rectangle(tela::Rect{0, 0, l.size, l.size}, p.canvas);
  const float r = l.size * 0.5f - 2.0f;
  const tela::Point c{l.size * 0.5f, l.size * 0.5f};
  d.Ellipse(tela::Rect{c.x - r, c.y - r, 2 * r, 2 * r}, p.track);
  auto at = [&](float n, float radius) {
    const float a = KnobAngle(n);
    return tela::Point{c.x + radius * std::cos(a), c.y - radius * std::sin(a)};
  };
  const tela::Color arc = l.editable ? p.accent : p.textDim;
  const float n = std::clamp(l.norm, 0.0f, 1.0f);
  if (n > 0.0f) {
    std::vector<tela::Point> pts;
    const std::uint32_t steps = 2u + static_cast<std::uint32_t>(std::ceil(n * 24.0f));
    for (std::uint32_t i = 0; i <= steps; ++i) pts.push_back(at(n * static_cast<float>(i) / static_cast<float>(steps), r - 1.5f));
    d.Polyline(pts, Line(arc, 3.0f));
  }
  const float ir = r * 0.62f;
  d.Ellipse(tela::Rect{c.x - ir, c.y - ir, 2 * ir, 2 * ir}, l.pressed ? Mix(p.canvas, p.accent, 0.3f) : p.canvas);
  // bank value tick on the rim
  d.Line(at(l.bankNorm, r - 0.5f), at(l.bankNorm, r + 1.5f), Line(p.text, 1.5f));
  const tela::Point tip = at(n, r - 3.0f);
  d.Line(c, tip, Line(l.editable ? p.text : p.textDim, 2.0f));
  if (shape != nullptr) *shape = KnobShape{c, tip, r};
  return d.Take();
}

tela::Color MeterZoneColor(float db, const Palette& p) {
  if (db >= -6.0f) return p.meterHigh;
  if (db >= -18.0f) return p.meterMid;
  return p.meterLow;
}

tela::Drawing DrawMeter(const MeterLook& l, const Palette& p, MeterShape* shape) {
  SafeDrawing d(l.dpi);
  d.Rectangle(tela::Rect{0, 0, l.width, l.height}, p.canvas);
  const std::uint32_t n = std::clamp<std::uint32_t>(l.channels, 1u, 8u);
  const float gap = 2.0f;
  const float cw = std::max(1.0f, (l.width - 4.0f - gap * static_cast<float>(n - 1u)) / static_cast<float>(n));
  MeterShape s;
  for (std::uint32_t c = 0; c < n; ++c) {
    MeterShape::Bar b;
    b.x = 2.0f + static_cast<float>(c) * (cw + gap);
    b.width = cw;
    d.Rectangle(tela::Rect{b.x, 0.0f, cw, l.height}, p.track);
    const float fp = l.live ? MeterFraction(l.peakDb[c]) : 0.0f;
    const float fr = l.live ? MeterFraction(l.rmsDb[c]) : 0.0f;
    const float fh = l.live ? MeterFraction(l.holdDb[c]) : 0.0f;
    b.peakTop = l.height * (1.0f - fp);
    b.rmsTop = l.height * (1.0f - fr);
    b.holdY = l.height * (1.0f - fh);
    if (fp > 0.0f) d.Rectangle(tela::Rect{b.x, b.peakTop, cw, l.height - b.peakTop}, MeterZoneColor(l.peakDb[c], p));
    if (fr > 0.0f) {
      const float inset = std::min(2.0f, cw * 0.25f);
      d.Rectangle(tela::Rect{b.x + inset, b.rmsTop, cw - 2 * inset, l.height - b.rmsTop}, p.rmsFill);
    }
    if (fh > 0.0f) d.Rectangle(tela::Rect{b.x, std::max(0.0f, b.holdY - 1.0f), cw, 2.0f}, p.text);
    if (l.clipped) d.Rectangle(tela::Rect{b.x, 0.0f, cw, 3.0f}, p.clip);
    s.bars.push_back(b);
  }
  // scale ticks: 0, -6, -18, -40 dBFS
  for (float db : {-6.0f, -18.0f, -40.0f}) {
    const float y = l.height * (1.0f - MeterFraction(db));
    d.Line(tela::Point{0.0f, y}, tela::Point{2.0f, y}, Line(p.textDim, 1.0f));
  }
  if (shape != nullptr) *shape = std::move(s);
  return d.Take();
}

tela::Drawing DrawTimingGraph(const GraphLook& l, const Palette& p, GraphShape* shape) {
  SafeDrawing d(l.dpi);
  d.Rectangle(tela::Rect{0, 0, l.width, l.height}, p.canvas);
  const std::size_t n = std::min<std::size_t>(l.total.size(), kGraphMaxPoints);
  const std::size_t first = l.total.size() - n;
  float maxSample = 0.0f;
  for (std::size_t i = first; i < l.total.size(); ++i) maxSample = std::max(maxSample, l.total[i]);
  GraphShape s;
  s.scale = GraphScale::For(l.budget, maxSample);
  for (float f : {0.25f, 0.5f, 0.75f}) {
    const float y = s.scale.Y(s.scale.yMax * f, 0.0f, l.height);
    d.Line(tela::Point{0.0f, y}, tela::Point{l.width, y}, Line(p.track, 1.0f));
  }
  s.budgetY = s.scale.Y(l.budget, 0.0f, l.height);
  if (l.budget > 0.0f) {
    tela::Stroke dash{p.budget, 1.5f, 6.0f, 4.0f};
    d.Polyline({tela::Point{0.0f, s.budgetY}, tela::Point{l.width, s.budgetY}}, dash);
  }
  if (n >= 2u) {
    std::vector<tela::Point> render;
    for (std::size_t i = 0; i < n; ++i) {
      const float x = GraphX(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(n), 0.0f, l.width);
      s.totalPoints.push_back(tela::Point{x, s.scale.Y(l.total[first + i], 0.0f, l.height)});
      const std::size_t ri = first + i;
      render.push_back(tela::Point{x, s.scale.Y(ri < l.render.size() ? l.render[ri] : 0.0f, 0.0f, l.height)});
    }
    const std::uint32_t share = (SafeDrawing::kMaxShapes - d.shapes()) / 2u;
    d.Polyline(s.totalPoints, Line(p.accent, 1.5f), share);  // the main series first
    d.Polyline(render, Line(p.accentSoft, 1.0f), share);
  }
  if (shape != nullptr) *shape = std::move(s);
  return d.Take();
}

tela::Drawing DrawLight(float width, float height, bool lit, tela::Color on, const Palette& p, float dpi) {
  SafeDrawing d(dpi);
  d.Rectangle(tela::Rect{0, 0, width, height}, lit ? on : p.track, NoStroke(), std::min(width, height) * 0.5f);
  return d.Take();
}

tela::Drawing DrawUsageBar(float width, float height, float fraction, const Palette& p, float dpi) {
  SafeDrawing d(dpi);
  d.Rectangle(tela::Rect{0, 0, width, height}, p.track);
  const float f = std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 1.0f) : 0.0f;
  if (f > 0.0f) d.Rectangle(tela::Rect{0, 0, width * f, height}, fraction > 1.0f ? p.budget : p.accent);
  return d.Take();
}

}  // namespace snd::tool::ui
