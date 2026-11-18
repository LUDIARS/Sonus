// The mappings between values and pixels of the tool's controls (decided here,
// tested analytically in tool/test/ui_*).
//
// Fader (dB <-> travel position p in [0, 1], piecewise linear through):
//   p    0      0+     0.05   0.25   0.50   0.75   1.00
//   dB   -inf   -90    -60    -30    -12    0      +12
//   p = 0 is -inf (kFaderOffDb, silence). Positions are clamped into [0, 1]
//   ("saturation at the ends"): dragging past the top stays at +12 dB, past the
//   bottom at -inf. The cap's centre moves over the canvas height minus the cap
//   height: y(p) = capHalf + (1 - p) * travel.
// Fader drag: relative to where the drag started; moving the pointer up by the
//   whole travel adds 1 to p. With Shift (modifier bit 0) the motion counts 1/10.
//   Double click: the bank value. Wheel: +-1 dB per notch (0.1 with Shift); from
//   -inf a positive notch starts at -90 dB, a value below -90 dB becomes -inf.
// Knob: normalized n in [0, 1] over the parameter range, linear, or logarithmic
//   for "Hz" parameters with a positive minimum; integer parameters round.
//   Drag: 150 px of vertical motion = the whole range (1/10 with Shift).
//   Double click: the bank value. Wheel: 1/100 of the range per notch.
//   Arc: from 225 deg (n = 0) clockwise through 270 deg to -45 deg (n = 1),
//   angles measured counter-clockwise from +x, y down on screen.
// Meter: linear in dB over [-60, 0] dBFS: fraction f = (dB + 60) / 60 clamped to
//   [0, 1]; a bar of a canvas of height H fills H * f from the bottom.
// Timing graph: x of sample i of n = left + i * width / (n - 1); y of v =
//   bottom - v / yMax * height with yMax = max(1.25 * budget, 1.1 * max sample, 1).
#ifndef SND_TOOL_UI_CONTROL_MATH_HPP
#define SND_TOOL_UI_CONTROL_MATH_HPP

#include <cstdint>

namespace snd::tool::ui {

inline constexpr float kFaderLowestDb = -90.0f;  // just above p = 0
inline constexpr float kWheelDbStep = 1.0f;
inline constexpr float kFineFactor = 0.1f;
inline constexpr float kKnobDragPixels = 150.0f;
inline constexpr float kKnobWheelStep = 0.01f;
inline constexpr float kMeterMinDb = -60.0f;
inline constexpr std::uint32_t kModShift = 1u << 0;
inline constexpr std::uint32_t kModControl = 1u << 1;

float FaderDbToPos(float db);
float FaderPosToDb(float pos);
float FaderWheel(float db, float notches, bool fine);

struct FaderGeometry {
  float height = 0.0f;  // canvas height
  float capHeight = 0.0f;
  float travel() const { return height > capHeight ? height - capHeight : 0.0f; }
  float CapCenterY(float pos) const;  // local y of the cap's centre
};

struct KnobRange {
  float minValue = 0.0f;
  float maxValue = 1.0f;
  bool logScale = false;
  bool integer = false;
};
float KnobToNorm(float value, const KnobRange& r);
float KnobFromNorm(float norm, const KnobRange& r);
float KnobAngle(float norm);  // radians, see above

float MeterFraction(float db);

struct GraphScale {
  float yMax = 1.0f;
  static GraphScale For(float budget, float maxSample);
  float Y(float value, float top, float height) const;  // bottom - value / yMax * height
};
float GraphX(std::uint32_t i, std::uint32_t n, float left, float width);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_CONTROL_MATH_HPP
