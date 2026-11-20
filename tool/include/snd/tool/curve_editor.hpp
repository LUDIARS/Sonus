// Parameters and curves (design.md 4.3, 11.1 パラメータとカーブのエディタ).
//
// CurveEditor edits any curve object of the project with one component: an event's
// "curves[i]" (parameter curves), "distanceCurves.<volume|lowpass|spread|reverbSend|blend3D>"
// (input = distance), a segment's "stems[i].volumeCurve". Points stay in ascending x
// (a move cannot pass a neighbour; equal x is allowed as the build allows it); a curve
// keeps at least two points; shape >= 0 (0 linear, > 0 exponent of the segment, CPNT.shape).
// Evaluate is the runtime's formula (core EvaluateCurve, float): x at or before the first
// point -> first y; between points -> y0 + (y1 - y0) * t^shape (shape of the left point,
// t = (x - x0) / (x1 - x0), 1 when x1 == x0); after the last -> last y.
//
// ParameterEditor: kind (continuous / switch), scope (a non-empty subset of global /
// emitter / playback), range (continuous: min < max; the default inside), rampRate >= 0.
//
// Composition (4.3), ComposeEvent: the value each curve of an event reads is resolved as
// the runtime does (core params.hpp ParamValueOf): playback, then emitter, then global,
// then the parameter's default (a switch without a value: 0). A value set at a scope the
// parameter does not declare is still used by the runtime (it does not check the scope)
// and is reported in `outsideScope`. The curve results are combined per target as
// core EvaluateDspParams does: volume dB added, pitch cents added, lowpass the minimum,
// highpass the maximum, pan added (the runtime adds pan, 4.3 says "other: multiply";
// see the notes); send / effectParam / spread / reverbSend / blend3D follow the curve's
// "compose" (default multiply) since the runtime does not apply them yet (stage 3).
// RampTrajectory reproduces EvaluateLogicParams: per block the current value moves toward
// the target by rampRate x block seconds (0 = at once; switches at once; a value seen for
// the first time starts from the parameter's default).
#ifndef SND_TOOL_CURVE_EDITOR_HPP
#define SND_TOOL_CURVE_EDITOR_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "snd/tool/document.hpp"

namespace snd::tool {

struct CurvePoint {
  double x = 0.0;
  double y = 0.0;
  double shape = 0.0;
};

// The runtime's curve evaluation (float, as core EvaluateCurve).
float EvaluateCurve(const std::vector<CurvePoint>& points, float x);

enum class CurveTarget : std::uint8_t {
  Volume = 0, Pitch = 1, Lowpass = 2, Highpass = 3, Pan = 4, Send = 5, EffectParam = 6, Spread = 7,
  ReverbSend = 8, Blend3D = 9,
};
enum class CurveCompose : std::uint8_t { AddDb = 0, AddCents = 1, Min = 2, Max = 3, Multiply = 4 };
const char* CurveTargetName(CurveTarget t);
const char* CurveComposeName(CurveCompose c);
bool ParseCurveTarget(const std::string& s, CurveTarget* out);
bool ParseCurveCompose(const std::string& s, CurveCompose* out);
CurveCompose DefaultCompose(CurveTarget t);  // 4.3
// D-179b: the runtime does not read the "compose" column; for these targets it applies its fixed
// rule (volume / pitch / pan added, lowpass minimum, highpass maximum) whatever is written.
bool ComposeIgnored(CurveTarget t);

class CurveEditor {
 public:
  CurveEditor(ProjectDocument& doc, std::string guid, std::string path);
  bool valid() const;
  std::vector<CurvePoint> Points() const;
  float Evaluate(float x) const { return EvaluateCurve(Points(), x); }
  // Index of the new point (after points with the same x), -1 if refused.
  int AddPoint(double x, double y, double shape = 0.0);
  bool MovePoint(std::uint32_t i, double x, double y, std::uint32_t mergeKey = 0);
  bool SetShape(std::uint32_t i, double shape);
  bool RemovePoint(std::uint32_t i);
  bool SetPoints(const std::vector<CurvePoint>& points);  // ascending x, >= 2
  // Parameter curves only (not distanceCurves): input, target, compose.
  bool SetParameter(const std::string& parameterGuidOrDistance);
  bool SetTarget(CurveTarget t);
  bool SetCompose(CurveCompose c);
  CurveTarget target() const;
  CurveCompose compose() const;  // the written one, else DefaultCompose(target)
  // A "compose" is written and the runtime does not apply it (ComposeIgnored(target)): the
  // screen warns. The editor still accepts it (the project format allows it).
  bool ComposeIgnored() const;

 private:
  bool IsDistanceCurve() const;
  ProjectDocument& doc_;
  std::string guid_;
  std::string path_;
};

// A new parameter curve on an event (appended to "curves"); returns its index or -1.
int AddEventCurve(ProjectDocument& doc, const std::string& eventGuid, const std::string& parameterGuidOrDistance,
                  CurveTarget target, const std::vector<CurvePoint>& points);
// A distance curve (volume, lowpass, spread, reverbSend, blend3D) of an event.
bool SetDistanceCurve(ProjectDocument& doc, const std::string& eventGuid, CurveTarget target,
                      const std::vector<CurvePoint>& points);

enum class ParamScope : std::uint8_t { Global = 1, Emitter = 2, Playback = 4 };

class ParameterEditor {
 public:
  ParameterEditor(ProjectDocument& doc, std::string guid);
  bool valid() const;
  bool SetSwitch(bool isSwitch);
  bool SetScope(std::uint32_t mask);  // ParamScope bits, non-empty
  std::uint32_t scope() const;
  bool SetRange(double minValue, double maxValue);
  bool SetDefault(double v);
  bool SetRampRate(double unitsPerSecond);
  double minValue() const;
  double maxValue() const;
  double defaultValue() const;
  double rampRate() const;
  bool isSwitch() const;

 private:
  ProjectDocument& doc_;
  std::string guid_;
};

// Values set for one parameter at each scope (absent = not set).
struct ParamScopeValues {
  bool hasGlobal = false, hasEmitter = false, hasPlayback = false;
  double global = 0.0, emitter = 0.0, playback = 0.0;
};

struct CurveContribution {
  std::uint32_t curve = 0;    // index in "curves"
  std::string parameter;      // GUID, "distance"
  double input = 0.0;         // the value the curve read
  const char* inputFrom = "";  // "playback", "emitter", "global", "default", "distance"
  CurveTarget target = CurveTarget::Volume;
  CurveCompose compose = CurveCompose::AddDb;
  double output = 0.0;
};

struct ComposeResult {
  double volumeDb = 0.0;
  double pitchCents = 0.0;
  double lowpassHz = 0.0;   // 0 = no lowpass curve
  double highpassHz = 0.0;  // 0 = none
  double pan = 0.0;
  // Other targets: product (or per compose) of their curves; 1 = no curve.
  double send = 1.0, effectParam = 1.0, spread = 1.0, reverbSend = 1.0, blend3D = 1.0;
  std::vector<CurveContribution> curves;
  std::vector<std::string> outsideScope;  // parameter GUIDs read at an undeclared scope
  std::vector<std::uint32_t> composeIgnored;  // curves whose written "compose" the runtime ignores
};

// Event curves (not distance curves) with the given parameter values (by parameter GUID)
// and distance.
ComposeResult ComposeEvent(const ProjectDocument& doc, const std::string& eventGuid,
                           const std::map<std::string, ParamScopeValues>& values, double distance);

// Values of a parameter's current value block by block from `from` toward `target`
// (EvaluateLogicParams). from < 0-sized? `firstSeen`: the runtime starts at the default.
std::vector<float> RampTrajectory(float from, float target, float rampRate, bool isSwitch, std::uint32_t blockFrames,
                                  std::uint32_t sampleRate, std::uint32_t blocks);

}  // namespace snd::tool

#endif  // SND_TOOL_CURVE_EDITOR_HPP
