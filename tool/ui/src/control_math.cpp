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

}  // namespace snd::tool::ui
