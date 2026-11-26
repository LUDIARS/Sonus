// Parameters and curves (curve_editor.hpp).
#include "snd/tool/curve_editor.hpp"

#include <algorithm>
#include <cmath>

namespace snd::tool {
namespace {

using json::Value;
const char* const kTargets[] = {"volume", "pitch", "lowpass", "highpass", "pan", "send", "effectParam",
                                "spread", "reverbSend", "blend3D"};
const char* const kComposes[] = {"addDb", "addCents", "min", "max", "multiply"};

Value PointsJson(const std::vector<CurvePoint>& pts) {
  Value a = json::MakeArray();
  for (const CurvePoint& p : pts) {
    Value v = json::MakeArray();
    v.items.push_back(Number(p.x));
    v.items.push_back(Number(p.y));
    if (p.shape != 0.0) v.items.push_back(Number(p.shape));
    a.items.push_back(v);
  }
  return a;
}

bool ValidPoints(const std::vector<CurvePoint>& pts) {
  if (pts.size() < 2) return false;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    if (!std::isfinite(pts[i].x) || !std::isfinite(pts[i].y) || !std::isfinite(pts[i].shape) || pts[i].shape < 0) return false;
    if (i > 0 && pts[i].x < pts[i - 1].x) return false;
  }
  return true;
}

double Combine(CurveCompose c, double acc, double y, bool first) {
  switch (c) {
    case CurveCompose::AddDb:
    case CurveCompose::AddCents: return acc + y;
    case CurveCompose::Min: return first ? y : std::min(acc, y);
    case CurveCompose::Max: return first ? y : std::max(acc, y);
    case CurveCompose::Multiply: return first ? y : acc * y;
  }
  return acc;
}

}  // namespace

float EvaluateCurve(const std::vector<CurvePoint>& p, float x) {
  // Same operations, in float, as core/src/core_util.cpp EvaluateCurve.
  const std::size_t count = p.size();
  if (count == 0) return 0.0f;
  const float x0f = static_cast<float>(p[0].x);
  if (x <= x0f || count == 1) return static_cast<float>(p[0].y);
  for (std::size_t i = 1; i < count; ++i) {
    const float xi = static_cast<float>(p[i].x);
    if (x <= xi) {
      const float xa = static_cast<float>(p[i - 1].x), xb = xi;
      const float ya = static_cast<float>(p[i - 1].y), yb = static_cast<float>(p[i].y);
      float tt = xb > xa ? (x - xa) / (xb - xa) : 1.0f;
      const float shape = static_cast<float>(p[i - 1].shape);
      if (shape > 0.0f && shape != 1.0f) tt = std::pow(tt, shape);
      return ya + (yb - ya) * tt;
    }
  }
  return static_cast<float>(p[count - 1].y);
}

const char* CurveTargetName(CurveTarget t) { return kTargets[static_cast<int>(t)]; }
const char* CurveComposeName(CurveCompose c) { return kComposes[static_cast<int>(c)]; }
bool ParseCurveTarget(const std::string& s, CurveTarget* out) {
  for (int i = 0; i < 10; ++i) {
    if (s == kTargets[i]) {
      *out = static_cast<CurveTarget>(i);
      return true;
    }
  }
  return false;
}
bool ParseCurveCompose(const std::string& s, CurveCompose* out) {
  for (int i = 0; i < 5; ++i) {
    if (s == kComposes[i]) {
      *out = static_cast<CurveCompose>(i);
      return true;
    }
  }
  return false;
}
CurveCompose DefaultCompose(CurveTarget t) {
  switch (t) {
    case CurveTarget::Volume: return CurveCompose::AddDb;
    case CurveTarget::Pitch: return CurveCompose::AddCents;
    case CurveTarget::Lowpass: return CurveCompose::Min;
    case CurveTarget::Highpass: return CurveCompose::Max;
    default: return CurveCompose::Multiply;
  }
}

bool ComposeIgnored(CurveTarget t) {
  return t == CurveTarget::Volume || t == CurveTarget::Pitch || t == CurveTarget::Lowpass || t == CurveTarget::Highpass ||
         t == CurveTarget::Pan;
}

// --- CurveEditor ----------------------------------------------------------------------------
CurveEditor::CurveEditor(ProjectDocument& doc, std::string guid, std::string path)
    : doc_(doc), guid_(NormalizeGuid(guid)), path_(std::move(path)) {}

bool CurveEditor::valid() const {
  const DocObject* o = doc_.Find(guid_);
  const Value* c = o != nullptr ? FindPath(o->doc, path_) : nullptr;
  return c != nullptr && c->IsObject();
}
bool CurveEditor::IsDistanceCurve() const { return path_.rfind("distanceCurves.", 0) == 0; }

std::vector<CurvePoint> CurveEditor::Points() const {
  std::vector<CurvePoint> out;
  const DocObject* o = doc_.Find(guid_);
  const Value* pts = o != nullptr ? FindPath(o->doc, path_ + ".points") : nullptr;
  if (pts == nullptr || !pts->IsArray()) return out;
  for (const Value& p : pts->items) {
    CurvePoint c;
    c.x = NumberAt(p, "[0]", 0);
    c.y = NumberAt(p, "[1]", 0);
    c.shape = NumberAt(p, "[2]", 0);
    out.push_back(c);
  }
  return out;
}

// D-177b: the same points (a shape 0 written or not) leave "points" as written.
static bool SamePoints(const std::vector<CurvePoint>& a, const std::vector<CurvePoint>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].shape != b[i].shape) return false;
  }
  return true;
}

bool CurveEditor::SetPoints(const std::vector<CurvePoint>& pts) {
  if (!valid() || !ValidPoints(pts)) return false;
  if (SamePoints(Points(), pts)) return true;
  return doc_.Set(guid_, path_ + ".points", PointsJson(pts));
}

int CurveEditor::AddPoint(double x, double y, double shape) {
  std::vector<CurvePoint> p = Points();
  std::size_t at = 0;
  while (at < p.size() && p[at].x <= x) ++at;
  p.insert(p.begin() + static_cast<std::ptrdiff_t>(at), CurvePoint{x, y, shape});
  if (!ValidPoints(p) || !doc_.Set(guid_, path_ + ".points", PointsJson(p))) return -1;
  return static_cast<int>(at);
}

bool CurveEditor::MovePoint(std::uint32_t i, double x, double y, std::uint32_t mergeKey) {
  std::vector<CurvePoint> p = Points();
  if (i >= p.size()) return false;
  if ((i > 0 && x < p[i - 1].x) || (i + 1 < p.size() && x > p[i + 1].x)) return false;  // keeps the order
  if (p[i].x == x && p[i].y == y) return std::isfinite(x) && std::isfinite(y);
  p[i].x = x;
  p[i].y = y;
  if (!ValidPoints(p)) return false;
  return doc_.Set(guid_, path_ + ".points", PointsJson(p), mergeKey);
}

bool CurveEditor::SetShape(std::uint32_t i, double shape) {
  std::vector<CurvePoint> p = Points();
  if (i >= p.size()) return false;
  if (p[i].shape == shape) return true;
  p[i].shape = shape;
  return ValidPoints(p) && doc_.Set(guid_, path_ + ".points", PointsJson(p));
}

bool CurveEditor::RemovePoint(std::uint32_t i) {
  std::vector<CurvePoint> p = Points();
  if (i >= p.size() || p.size() <= 2) return false;
  p.erase(p.begin() + static_cast<std::ptrdiff_t>(i));
  return doc_.Set(guid_, path_ + ".points", PointsJson(p));
}

bool CurveEditor::SetParameter(const std::string& ref) {
  if (!valid() || IsDistanceCurve()) return false;
  if (ref == "distance") return doc_.Set(guid_, path_ + ".parameter", String("distance"));
  const DocObject* p = doc_.Find(ref);
  if (p == nullptr || p->kind() != "parameter") return false;
  const DocObject* cur = doc_.Resolve("parameter", StringAt(doc_.Find(guid_)->doc, path_ + ".parameter"));
  if (cur != nullptr && cur->guid == p->guid) return true;  // D-177b
  return doc_.Set(guid_, path_ + ".parameter", String(p->guid));
}
bool CurveEditor::SetTarget(CurveTarget t) {
  return valid() && !IsDistanceCurve() && doc_.SetDefaulted(guid_, path_ + ".target", String(CurveTargetName(t)), String("volume"));
}
bool CurveEditor::SetCompose(CurveCompose c) {
  if (!valid() || IsDistanceCurve()) return false;
  if (FindPath(doc_.Find(guid_)->doc, path_ + ".compose") == nullptr && c == DefaultCompose(target())) return true;  // D-177b
  return doc_.Set(guid_, path_ + ".compose", String(CurveComposeName(c)));
}
CurveTarget CurveEditor::target() const {
  CurveTarget t = CurveTarget::Volume;
  const DocObject* o = doc_.Find(guid_);
  if (o == nullptr) return t;
  if (IsDistanceCurve()) {
    ParseCurveTarget(path_.substr(std::string("distanceCurves.").size()), &t);
    return t;
  }
  ParseCurveTarget(StringAt(o->doc, path_ + ".target", "volume"), &t);
  return t;
}
bool CurveEditor::ComposeIgnored() const {
  const DocObject* o = doc_.Find(guid_);
  return o != nullptr && FindPath(o->doc, path_ + ".compose") != nullptr && snd::tool::ComposeIgnored(target());
}
CurveCompose CurveEditor::compose() const {
  CurveCompose c = DefaultCompose(target());
  const DocObject* o = doc_.Find(guid_);
  if (o != nullptr) ParseCurveCompose(StringAt(o->doc, path_ + ".compose"), &c);
  return c;
}

int AddEventCurve(ProjectDocument& doc, const std::string& eventGuid, const std::string& ref, CurveTarget target,
                  const std::vector<CurvePoint>& points) {
  const DocObject* e = doc.Find(eventGuid);
  if (e == nullptr || e->kind() != "event" || !ValidPoints(points)) return -1;
  std::string input = "distance";
  if (ref != "distance") {
    const DocObject* p = doc.Find(ref);
    if (p == nullptr || p->kind() != "parameter") return -1;
    input = p->guid;
  }
  Value c = json::MakeObject();
  json::Add(c, "parameter", String(input));
  json::Add(c, "target", String(CurveTargetName(target)));
  json::Add(c, "points", PointsJson(points));
  const Value* curves = e->doc.Find("curves");
  const std::size_t n = curves != nullptr && curves->IsArray() ? curves->items.size() : 0u;
  if (!doc.Insert(e->guid, "curves", n, c)) return -1;
  return static_cast<int>(n);
}

bool SetDistanceCurve(ProjectDocument& doc, const std::string& eventGuid, CurveTarget target,
                      const std::vector<CurvePoint>& points) {
  const DocObject* e = doc.Find(eventGuid);
  if (e == nullptr || e->kind() != "event" || !ValidPoints(points)) return false;
  if (target != CurveTarget::Volume && target != CurveTarget::Lowpass && target != CurveTarget::Spread &&
      target != CurveTarget::ReverbSend && target != CurveTarget::Blend3D) {
    return false;
  }
  Value c = json::MakeObject();
  json::Add(c, "points", PointsJson(points));
  return doc.Set(e->guid, std::string("distanceCurves.") + CurveTargetName(target), c);
}

// --- ParameterEditor --------------------------------------------------------------------------
ParameterEditor::ParameterEditor(ProjectDocument& doc, std::string guid) : doc_(doc), guid_(NormalizeGuid(guid)) {}
bool ParameterEditor::valid() const {
  const DocObject* o = doc_.Find(guid_);
  return o != nullptr && o->kind() == "parameter";
}
bool ParameterEditor::isSwitch() const { return valid() && StringAt(doc_.Find(guid_)->doc, "kind") == "switch"; }
double ParameterEditor::minValue() const { return valid() ? NumberAt(doc_.Find(guid_)->doc, "min", 0) : 0; }
double ParameterEditor::maxValue() const {
  return valid() ? NumberAt(doc_.Find(guid_)->doc, "max", isSwitch() ? 0.0 : 1.0) : 0;
}
double ParameterEditor::defaultValue() const { return valid() ? NumberAt(doc_.Find(guid_)->doc, "default", minValue()) : 0; }
double ParameterEditor::rampRate() const { return valid() ? NumberAt(doc_.Find(guid_)->doc, "rampRate", 0) : 0; }

bool ParameterEditor::SetSwitch(bool s) {
  if (!valid()) return false;
  if (isSwitch() == s) return true;  // D-177b (an explicit "continuous" stays)
  if (!s) return doc_.Erase(guid_, "kind");
  return doc_.Set(guid_, "kind", String("switch"));
}
std::uint32_t ParameterEditor::scope() const {
  if (!valid()) return 0;
  const Value* s = doc_.Find(guid_)->doc.Find("scope");
  if (s == nullptr || !s->IsArray()) return 7u;
  std::uint32_t m = 0;
  for (const Value& v : s->items) {
    if (!v.IsString()) continue;
    m |= v.text == "global" ? 1u : v.text == "emitter" ? 2u : v.text == "playback" ? 4u : 0u;
  }
  return m;
}
bool ParameterEditor::SetScope(std::uint32_t mask) {
  if (!valid() || mask == 0u || mask > 7u) return false;
  if (scope() == mask) return true;  // D-177b (absent = all three)
  Value a = json::MakeArray();
  if ((mask & 1u) != 0u) a.items.push_back(String("global"));
  if ((mask & 2u) != 0u) a.items.push_back(String("emitter"));
  if ((mask & 4u) != 0u) a.items.push_back(String("playback"));
  return doc_.Set(guid_, "scope", a);
}
bool ParameterEditor::SetRange(double lo, double hi) {
  if (!valid() || !std::isfinite(lo) || !std::isfinite(hi) || lo > hi || (lo == hi && !isSwitch())) return false;
  const double def = std::clamp(defaultValue(), lo, hi);
  if (minValue() == lo && maxValue() == hi && defaultValue() == def) return true;  // D-177b
  return doc_.Modify(guid_, [&](Value& v) {
    const char* const* order = SpecMemberOrder("parameter", "");
    SetMember(v, "min", Number(lo), order);
    SetMember(v, "max", Number(hi), order);
    if (v.Find("default") != nullptr || def != lo) SetMember(v, "default", Number(def), order);
    return true;
  });
}
bool ParameterEditor::SetDefault(double v) {
  if (!valid() || !std::isfinite(v) || v < minValue() || v > maxValue()) return false;
  return doc_.SetDefaulted(guid_, "default", Number(v), Number(minValue()));
}
bool ParameterEditor::SetRampRate(double r) {
  return valid() && std::isfinite(r) && r >= 0 && doc_.SetDefaulted(guid_, "rampRate", Number(r), Number(0));
}

// --- composition ------------------------------------------------------------------------------
ComposeResult ComposeEvent(const ProjectDocument& doc, const std::string& eventGuid,
                           const std::map<std::string, ParamScopeValues>& values, double distance) {
  ComposeResult r;
  const DocObject* e = doc.Find(eventGuid);
  if (e == nullptr) return r;
  const Value* curves = e->doc.Find("curves");
  if (curves == nullptr || !curves->IsArray()) return r;
  bool firstOther[10] = {true, true, true, true, true, true, true, true, true, true};
  double acc[10] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
  float gainDb = 0.0f, cents = 0.0f, pan = 0.0f;
  float lowpass = 1.0e9f, highpass = 0.0f;
  for (std::size_t k = 0; k < curves->items.size(); ++k) {
    const Value& cv = curves->items[k];
    CurveContribution c;
    c.curve = static_cast<std::uint32_t>(k);
    ParseCurveTarget(StringAt(cv, "target", "volume"), &c.target);
    c.compose = DefaultCompose(c.target);
    ParseCurveCompose(StringAt(cv, "compose"), &c.compose);
    if (cv.Find("compose") != nullptr && ComposeIgnored(c.target)) r.composeIgnored.push_back(static_cast<std::uint32_t>(k));
    const std::string pref = StringAt(cv, "parameter");
    float x = 0.0f;
    if (pref == "distance") {
      c.parameter = "distance";
      c.inputFrom = "distance";
      x = static_cast<float>(distance);
    } else {
      const DocObject* p = doc.Resolve("parameter", pref);
      if (p == nullptr) continue;  // the build reports it
      c.parameter = p->guid;
      const bool isSwitch = StringAt(p->doc, "kind") == "switch";
      std::uint32_t mask = 7u;
      if (const Value* s = p->doc.Find("scope"); s != nullptr && s->IsArray()) {
        mask = 0u;
        for (const Value& sv : s->items) {
          mask |= sv.text == "global" ? 1u : sv.text == "emitter" ? 2u : sv.text == "playback" ? 4u : 0u;
        }
      }
      auto it = values.find(p->guid);
      const ParamScopeValues sv = it != values.end() ? it->second : ParamScopeValues{};
      std::uint32_t used = 0;
      if (sv.hasPlayback) {
        x = static_cast<float>(sv.playback);
        c.inputFrom = "playback";
        used = 4u;
      } else if (sv.hasEmitter) {
        x = static_cast<float>(sv.emitter);
        c.inputFrom = "emitter";
        used = 2u;
      } else if (sv.hasGlobal) {
        x = static_cast<float>(sv.global);
        c.inputFrom = "global";
        used = 1u;
      } else {
        x = isSwitch ? 0.0f : static_cast<float>(NumberAt(p->doc, "default", NumberAt(p->doc, "min", 0)));
        c.inputFrom = "default";
      }
      if (used != 0u && (mask & used) == 0u &&
          std::find(r.outsideScope.begin(), r.outsideScope.end(), p->guid) == r.outsideScope.end()) {
        r.outsideScope.push_back(p->guid);
      }
    }
    std::vector<CurvePoint> pts;
    if (const Value* pa = cv.Find("points"); pa != nullptr && pa->IsArray()) {
      for (const Value& pt : pa->items) pts.push_back(CurvePoint{NumberAt(pt, "[0]", 0), NumberAt(pt, "[1]", 0), NumberAt(pt, "[2]", 0)});
    }
    c.input = x;
    const float y = EvaluateCurve(pts, x);
    c.output = y;
    const int ti = static_cast<int>(c.target);
    switch (c.target) {  // core EvaluateDspParams
      case CurveTarget::Volume: gainDb += y; break;
      case CurveTarget::Pitch: cents += y; break;
      case CurveTarget::Lowpass: lowpass = y < lowpass ? y : lowpass; break;
      case CurveTarget::Highpass: highpass = y > highpass ? y : highpass; break;
      case CurveTarget::Pan: pan += y; break;
      default:
        acc[ti] = Combine(c.compose, acc[ti], y, firstOther[ti]);
        firstOther[ti] = false;
        break;
    }
    r.curves.push_back(c);
  }
  r.volumeDb = gainDb;
  r.pitchCents = cents;
  r.lowpassHz = lowpass < 1.0e9f ? lowpass : 0.0;
  r.highpassHz = highpass;
  r.pan = pan;
  r.send = acc[5];
  r.effectParam = acc[6];
  r.spread = acc[7];
  r.reverbSend = acc[8];
  r.blend3D = acc[9];
  return r;
}

std::vector<float> RampTrajectory(float from, float target, float rampRate, bool isSwitch, std::uint32_t blockFrames,
                                  std::uint32_t sampleRate, std::uint32_t blocks) {
  std::vector<float> out;
  const float seconds = static_cast<float>(blockFrames) / static_cast<float>(sampleRate == 0 ? 48000u : sampleRate);
  float current = from;
  for (std::uint32_t b = 0; b < blocks; ++b) {
    if (isSwitch || rampRate <= 0.0f) {
      current = target;
    } else {
      const float step = rampRate * seconds;
      const float c = current;
      current = c < target ? (target - c <= step ? target : c + step) : (c - target <= step ? target : c - step);
    }
    out.push_back(current);
  }
  return out;
}

}  // namespace snd::tool
