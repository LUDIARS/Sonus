// Value <-> pixel mappings of the tool's controls.
#include "snd/tool/ui/control_math.hpp"

#include <algorithm>
#include <cmath>

#include "snd/tool/mixer.hpp"

namespace snd::tool::ui {
namespace {

struct Knot {
  float pos;
  float db;
};
constexpr Knot kKnots[] = {{0.0f, kFaderLowestDb}, {0.05f, -60.0f}, {0.25f, -30.0f},
                           {0.50f, -12.0f},         {0.75f, 0.0f},   {1.00f, kFaderMaxDb}};
constexpr std::size_t kKnotCount = sizeof(kKnots) / sizeof(kKnots[0]);

}  // namespace

float FaderDbToPos(float db) {
  if (std::isnan(db) || db < kFaderLowestDb) return 0.0f;
  if (db >= kFaderMaxDb) return 1.0f;
  for (std::size_t i = 1; i < kKnotCount; ++i) {
    if (db <= kKnots[i].db) {
      const Knot a = kKnots[i - 1], b = kKnots[i];
      return a.pos + (db - a.db) / (b.db - a.db) * (b.pos - a.pos);
    }
  }
  return 1.0f;
}

float FaderPosToDb(float pos) {
  if (std::isnan(pos) || pos <= 0.0f) return kFaderOffDb;
  if (pos >= 1.0f) return kFaderMaxDb;
  for (std::size_t i = 1; i < kKnotCount; ++i) {
    if (pos <= kKnots[i].pos) {
      const Knot a = kKnots[i - 1], b = kKnots[i];
      return a.db + (pos - a.pos) / (b.pos - a.pos) * (b.db - a.db);
    }
  }
  return kFaderMaxDb;
}

float FaderWheel(float db, float notches, bool fine) {
  if (!std::isfinite(notches) || notches == 0.0f) return db;
  const float step = kWheelDbStep * (fine ? kFineFactor : 1.0f);
  float base = db;
  if (db < kFaderLowestDb) {
    if (notches < 0.0f) return kFaderOffDb;
    base = kFaderLowestDb;
    notches -= 1.0f;  // the first notch reaches -90 dB
  }
  const float next = base + notches * step;
  if (next < kFaderLowestDb) return kFaderOffDb;
  return std::min(next, kFaderMaxDb);
}

float FaderGeometry::CapCenterY(float pos) const {
  return capHeight * 0.5f + (1.0f - std::clamp(pos, 0.0f, 1.0f)) * travel();
}

float KnobToNorm(float v, const KnobRange& r) {
  if (!(r.maxValue > r.minValue) || std::isnan(v)) return 0.0f;
  v = std::clamp(v, r.minValue, r.maxValue);
  if (r.logScale && r.minValue > 0.0f) {
    return std::log(v / r.minValue) / std::log(r.maxValue / r.minValue);
  }
  return (v - r.minValue) / (r.maxValue - r.minValue);
}

float KnobFromNorm(float n, const KnobRange& r) {
  if (std::isnan(n)) n = 0.0f;
  n = std::clamp(n, 0.0f, 1.0f);
  if (!(r.maxValue > r.minValue)) return r.minValue;
  float v = 0.0f;
  if (r.logScale && r.minValue > 0.0f) {
    v = r.minValue * std::pow(r.maxValue / r.minValue, n);
  } else {
    v = r.minValue + n * (r.maxValue - r.minValue);
  }
  if (r.integer) v = std::round(v);
  return std::clamp(v, r.minValue, r.maxValue);
}

float KnobAngle(float n) {
  constexpr float kPi = 3.14159265358979f;
  n = std::clamp(n, 0.0f, 1.0f);
  return (225.0f - 270.0f * n) * kPi / 180.0f;
}

float MeterFraction(float db) {
  if (std::isnan(db)) return 0.0f;
  return std::clamp((db - kMeterMinDb) / (0.0f - kMeterMinDb), 0.0f, 1.0f);
}

GraphScale GraphScale::For(float budget, float maxSample) {
  GraphScale s;
  s.yMax = std::max({1.25f * std::max(0.0f, budget), 1.1f * std::max(0.0f, maxSample), 1.0f});
  return s;
}

float GraphScale::Y(float value, float top, float height) const {
  const float v = std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, yMax);
  return top + height - v / yMax * height;
}

float GraphX(std::uint32_t i, std::uint32_t n, float left, float width) {
  if (n < 2u) return left + width;
  return left + static_cast<float>(i) * width / static_cast<float>(n - 1u);
}

// --- editors ------------------------------------------------------------------------------
float TimeAxis::X(double ms) const {
  const double per = msPerPx > 0.0 ? msPerPx : 1.0;
  const double x = (ms - scrollMs) / per;
  return static_cast<float>(std::clamp(x, -90000.0, 90000.0));
}

double TimeAxis::Ms(float x) const { return scrollMs + static_cast<double>(x) * (msPerPx > 0.0 ? msPerPx : 1.0); }

TimeAxis TimeAxis::Zoomed(double factor, float aboutX) const {
  TimeAxis t = *this;
  if (!(factor > 0.0) || !std::isfinite(factor)) return t;
  const double at = Ms(aboutX);
  t.msPerPx = std::clamp(msPerPx * factor, kMinMsPerPx, kMaxMsPerPx);
  t.scrollMs = at - static_cast<double>(aboutX) * t.msPerPx;
  return t;
}

double RulerStepMs(double msPerPx, float minPx) {
  const double want = std::max(1e-6, msPerPx * static_cast<double>(std::max(1.0f, minPx)));
  double base = std::pow(10.0, std::floor(std::log10(want)));
  for (int i = 0; i < 4; ++i) {
    for (double m : {1.0, 2.0, 5.0}) {
      if (base * m >= want) return base * m;
    }
    base *= 10.0;
  }
  return base;
}

float PlotAxes::X(double x) const {
  const double span = x1 - x0;
  const double v = span != 0.0 ? (x - x0) / span * width : 0.0;
  return static_cast<float>(std::clamp(std::isfinite(v) ? v : 0.0, -90000.0, 90000.0));
}

float PlotAxes::Y(double y) const {
  const double span = y1 - y0;
  const double v = span != 0.0 ? height - (y - y0) / span * height : height;
  return static_cast<float>(std::clamp(std::isfinite(v) ? v : 0.0, -90000.0, 90000.0));
}

double PlotAxes::ValueX(float px) const { return width > 0.0f ? x0 + static_cast<double>(px) / width * (x1 - x0) : x0; }

double PlotAxes::ValueY(float py) const {
  return height > 0.0f ? y0 + static_cast<double>(height - py) / height * (y1 - y0) : y0;
}

PlotAxes PlotAxes::Fit(double x0, double x1, double y0, double y1, float width, float height, double yMargin) {
  PlotAxes a;
  if (!std::isfinite(x0) || !std::isfinite(x1)) x0 = 0.0, x1 = 1.0;
  if (!std::isfinite(y0) || !std::isfinite(y1)) y0 = 0.0, y1 = 1.0;
  if (x1 < x0) std::swap(x0, x1);
  if (y1 < y0) std::swap(y0, y1);
  if (x1 - x0 <= 0.0) x0 -= 0.5, x1 += 0.5;
  if (y1 - y0 <= 0.0) y0 -= 0.5, y1 += 0.5;
  const double m = (y1 - y0) * std::max(0.0, yMargin);
  a.x0 = x0;
  a.x1 = x1;
  a.y0 = y0 - m;
  a.y1 = y1 + m;
  a.width = std::max(1.0f, width);
  a.height = std::max(1.0f, height);
  return a;
}

double ClampNumber(double v, double minValue, double maxValue, bool integer) {
  if (!std::isfinite(v)) v = 0.0;
  if (integer) v = std::round(v);
  if (v < minValue) v = integer ? std::ceil(minValue) : minValue;
  if (v > maxValue) v = integer ? std::floor(maxValue) : maxValue;
  return v;
}

double NumberDrag(double anchor, float pixelsUp, double step, double minValue, double maxValue, bool integer) {
  const double v = anchor + static_cast<double>(pixelsUp) / static_cast<double>(kNumberDragPixels) * step;
  return ClampNumber(v, minValue, maxValue, integer);
}

}  // namespace snd::tool::ui
