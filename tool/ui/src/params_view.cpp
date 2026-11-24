// Parameters and curves screen (design.md 4.3, 11.1 パラメータとカーブのエディタ): the parameter list
// and members (kind, scope, range, default, follow speed with its ramp), an event's curves (the plot
// with draggable points: double click on the plot adds a point, Delete or "Remove" removes the
// selected one; shape, target, compose, input), and the composition check (parameter values at a
// scope and the distance -> the result per target and each curve's part). Edits through
// ParameterEditor / CurveEditor.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "editor_util.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/editor_draw.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {
namespace {

constexpr float kRow = 26.0f;
constexpr float kListW = 220.0f;
const char* const kDistance[] = {"volume", "lowpass", "spread", "reverbSend", "blend3D"};
const CurveTarget kDistanceTarget[] = {CurveTarget::Volume, CurveTarget::Lowpass, CurveTarget::Spread,
                                       CurveTarget::ReverbSend, CurveTarget::Blend3D};

int ToInt(const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); }

// The curves of an event: member paths and labels.
std::vector<std::pair<std::string, std::string>> CurvesOf(const ProjectDocument& doc, const std::string& ev) {
  std::vector<std::pair<std::string, std::string>> v;
  const DocObject* o = doc.Find(ev);
  if (o == nullptr) return v;
  if (const json::Value* c = o->doc.Find("curves"); c != nullptr && c->IsArray()) {
    for (std::size_t i = 0; i < c->items.size(); ++i) {
      const std::string p = StringAt(c->items[i], "parameter");
      const std::string label = (p == "distance" ? std::string("distance") : RefName(doc, "parameter", p)) + " -> " +
                                StringAt(c->items[i], "target", "volume");
      v.emplace_back("curves[" + std::to_string(i) + "]", label);
    }
  }
  for (int k = 0; k < 5; ++k) {
    const std::string path = std::string("distanceCurves.") + kDistance[k];
    if (FindPath(o->doc, path) != nullptr) v.emplace_back(path, std::string("distance ") + kDistance[k]);
  }
  return v;
}

// The x range a curve is shown over.
void XRange(const ProjectDocument& doc, const std::string& ev, const std::string& path,
            const std::vector<CurvePoint>& pts, double* x0, double* x1) {
  *x0 = pts.empty() ? 0.0 : pts.front().x;
  *x1 = pts.empty() ? 1.0 : pts.back().x;
  const DocObject* o = doc.Find(ev);
  if (o == nullptr) return;
  if (path.rfind("distanceCurves.", 0) == 0) {
    *x0 = std::min(*x0, 0.0);
    *x1 = std::max(*x1, NumberAt(o->doc, "maxDistance", *x1));
    return;
  }
  const std::string p = StringAt(o->doc, path + ".parameter");
  const DocObject* par = p == "distance" ? nullptr : doc.Resolve("parameter", p);
  if (par != nullptr) {
    *x0 = std::min(*x0, NumberAt(par->doc, "min", *x0));
    *x1 = std::max(*x1, NumberAt(par->doc, "max", *x1));
  }
}

void ParameterPanel(Kit& kit, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  ParamsView& pv = app->view().params;
  if (pv.parameter.empty() || d->Find(pv.parameter) == nullptr) {
    kit.Label("pc.par.none", "no parameter selected", x, y, w);
    return;
  }
  const std::string g = pv.parameter;
  ParameterEditor pe(*d, g);
  kit.Title("pc.par.title", d->Find(g)->name(), x, y, w);
  y += 24.0f;
  const bool sw = pe.isSwitch();
  kit.Toggle("pc.par.switch", sw ? "switch" : "continuous", sw, [d, g, sw] { ParameterEditor(*d, g).SetSwitch(!sw); }, x,
             y, 110);
  const std::uint32_t scope = pe.scope();
  const char* names[] = {"global", "emitter", "playback"};
  for (std::uint32_t i = 0; i < 3; ++i) {
    const std::uint32_t bit = 1u << i;
    const bool on = (scope & bit) != 0u;
    kit.Toggle("pc.par.scope" + std::to_string(i), names[i], on, [d, g, scope, bit, app] {
      if (!ParameterEditor(*d, g).SetScope(scope ^ bit)) app->Message("refused: a parameter needs one scope");
    }, x + 114 + static_cast<float>(i) * ((w - 114) / 3.0f), y, (w - 114) / 3.0f - 4);
  }
  y += kRow;
  const double mn = pe.minValue(), mx = pe.maxValue();
  const double span = std::max(1e-6, mx - mn);
  NumberSpec sp;
  sp.step = span / 100.0;
  sp.decimals = 3;
  sp.enabled = !sw;
  sp.value = mn;
  // One field per row (D-187: the column is narrow; every value keeps its room and its label).
  kit.NumberField("pc.par.min", "Min", sp, [d, g, mx](double v, std::uint32_t) { return ParameterEditor(*d, g).SetRange(v, mx); },
                  x, y, w, 60);
  y += kRow;
  sp.value = mx;
  kit.NumberField("pc.par.max", "Max", sp, [d, g, mn](double v, std::uint32_t) { return ParameterEditor(*d, g).SetRange(mn, v); },
                  x, y, w, 60);
  y += kRow;
  sp.enabled = true;
  sp.value = pe.defaultValue();
  sp.integer = sw;
  kit.NumberField("pc.par.def", "Default", sp, [d, g](double v, std::uint32_t) { return ParameterEditor(*d, g).SetDefault(v); },
                  x, y, w, 60);
  y += kRow;
  NumberSpec rp;
  rp.value = pe.rampRate();
  rp.step = std::max(0.01, span / 20.0);
  rp.minValue = 0;
  rp.decimals = 3;
  rp.unit = "/s";
  kit.NumberField("pc.par.ramp", "Ramp", rp, [d, g](double v, std::uint32_t) { return ParameterEditor(*d, g).SetRampRate(v); },
                  x, y, w, 60);
  y += kRow;
  // The follow speed: the runtime's trajectory from min to max (256-frame blocks at 48 kHz).
  const std::vector<float> traj = RampTrajectory(static_cast<float>(mn), static_cast<float>(mx),
                                                 static_cast<float>(pe.rampRate()), sw, 256, 48000, 200);
  SeriesLook sl;
  sl.dpi = kit.ctx().dpi;
  sl.width = w;
  sl.height = std::max(40.0f, std::min(90.0f, h - (y + 20.0f)));
  for (float v : traj) sl.values.push_back(static_cast<float>(v - mn));
  sl.yMax = static_cast<float>(span);
  sl.reference = static_cast<float>(span);
  Canvas(kit.doc(), "pc.par.ramp.graph", DrawSeries(sl, kit.palette()), x, y, w, sl.height);
  std::uint32_t reach = 0;
  while (reach < traj.size() && traj[reach] < static_cast<float>(mx)) ++reach;
  kit.Label("pc.par.ramp.text",
            "min -> max: " + (reach >= traj.size() ? std::string("> 200 blocks") : std::to_string(reach) + " blocks (" +
                                                                                     FormatNumberText(reach * 256.0 / 48.0, 1) + " ms)"),
            x, y + sl.height, w, 18, 0.5f);
}

void CurvePanel(Kit& kit, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  ParamsView& pv = app->view().params;
  ParamsView* pvp = &pv;
  KeepSelection(*d, "event", &pv.event);
  kit.PickField("pc.ev", "Event", RefName(*d, "event", pv.event), "Event of the curves", [d] { return DocItems(*d, "event"); },
                [pvp](const std::string& v) {
                  pvp->event = v;
                  pvp->curve.clear();
                  pvp->point = -1;
                }, x, y, w * 0.5f, 50);
  if (pv.event.empty()) return;
  const std::string ev = pv.event;
  const auto curves = CurvesOf(*d, ev);
  bool found = false;
  for (const auto& c : curves) found = found || c.first == pv.curve;
  if (!found) {
    pv.curve = curves.empty() ? std::string() : curves.front().first;
    pv.point = -1;
  }
  const std::string par = pv.parameter;
  kit.PickField("pc.curve.add", "", "+ Curve", "New curve (input: the selected parameter)", [] {
    std::vector<PickerItem> v;
    for (int t = 0; t <= 9; ++t) v.push_back(PickerItem{CurveTargetName(static_cast<CurveTarget>(t)), std::to_string(t)});
    return v;
  }, [d, ev, par, pvp, app](const std::string& v) {
    const DocObject* p = d->Find(par);
    const double mn = p != nullptr ? NumberAt(p->doc, "min", 0.0) : 0.0, mx = p != nullptr ? NumberAt(p->doc, "max", 1.0) : 1.0;
    const int i = AddEventCurve(*d, ev, par.empty() ? std::string("distance") : par, static_cast<CurveTarget>(ToInt(v)),
                                {CurvePoint{mn, 0.0, 0.0}, CurvePoint{mx, 1.0, 0.0}});
    if (i < 0) return app->Message("refused: new curve");
    pvp->curve = "curves[" + std::to_string(i) + "]";
    pvp->point = -1;
  }, x + w * 0.5f + 4, y, w * 0.25f - 4);
  kit.PickField("pc.curve.adddist", "", "+ Distance", "Distance curve", [] {
    std::vector<PickerItem> v;
    for (int k = 0; k < 5; ++k) v.push_back(PickerItem{kDistance[k], std::to_string(k)});
    return v;
  }, [d, ev, pvp, app](const std::string& v) {
    const int k = std::clamp(ToInt(v), 0, 4);
    const double md = std::max(1.0, NumberAt(d->Find(ev)->doc, "maxDistance", 50.0));
    const bool vol = k == 0;
    if (!SetDistanceCurve(*d, ev, kDistanceTarget[k], {CurvePoint{1.0, vol ? 0.0 : 1.0, 0.0}, CurvePoint{md, vol ? -40.0 : 0.0, 0.0}})) {
      return app->Message("refused: distance curve");
    }
    pvp->curve = std::string("distanceCurves.") + kDistance[k];
    pvp->point = -1;
  }, x + w * 0.75f + 4, y, w * 0.25f - 4);
  y += kRow;
  // The curves of the event (paged, one row each).
  const std::uint32_t per = 4;
  const std::uint32_t first = kit.Pager("pc.curves", static_cast<std::uint32_t>(curves.size()), per, &pv.curvePage, x, y, w);
  if (curves.size() > per) y += 22.0f;
  for (std::uint32_t i = first; i < curves.size() && i < first + per; ++i) {
    const std::string path = curves[i].first;
    Button(kit.doc(), "pc.curve." + std::to_string(i), (path == pv.curve ? "> " : "  ") + curves[i].second,
           [pvp, path] {
             pvp->curve = path;
             pvp->point = -1;
           },
           x + static_cast<float>((i - first) % 2u) * (w * 0.5f), y + static_cast<float>((i - first) / 2u) * 24.0f,
           w * 0.5f - 4, 22, 0.5f);
  }
  y += 50.0f;
  if (pv.curve.empty()) {
    kit.Label("pc.curve.none", "no curves: + Curve / + Distance", x, y, w);
    return;
  }
  const std::string path = pv.curve;
  CurveEditor ce(*d, ev, path);
  if (!ce.valid()) return;
  const std::vector<CurvePoint> pts = ce.Points();
  if (pv.point >= static_cast<std::int32_t>(pts.size())) pv.point = -1;
  double x0 = 0.0, x1 = 1.0;
  XRange(*d, ev, path, pts, &x0, &x1);
  double y0 = 0.0, y1 = 1.0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    if (i == 0) y0 = y1 = pts[i].y;
    y0 = std::min(y0, pts[i].y);
    y1 = std::max(y1, pts[i].y);
  }
  const float plotH = std::max(100.0f, std::min(260.0f, h - (y + 4.0f * kRow + 10.0f)));
  const PlotAxes axes = PlotAxes::Fit(x0, x1, y0, y1, w, plotH, 0.12, kCurveXMargin);
  // Background: press deselects, double click adds a point there.
  auto bg = std::make_shared<DragHandler>();
  bg->press = [pvp](float, float, std::uint32_t) { pvp->point = -1; };
  bg->doubleClick = [d, ev, path, axes, pvp, app](float px, float py) {
    const int i = CurveEditor(*d, ev, path).AddPoint(axes.ValueX(px), axes.ValueY(py));
    if (i >= 0) {
      pvp->point = i;
      app->Edited();
    }
  };
  kit.Hit("pc.plot.hit", bg, x, y, w, plotH);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const std::uint32_t pi = static_cast<std::uint32_t>(i);
    const float px = axes.X(pts[i].x), py = axes.Y(pts[i].y);
    // The axes stay those of the press for the whole drag (the plot would rescale under it).
    auto h2 = DocumentDrag(
        *app, [pvp, pi](float, float, std::uint32_t) { pvp->point = static_cast<std::int32_t>(pi); },
        [d, ev, path, pi, axes, px, py, p = pts[i]](const DragMotion& m) {
          // An axis without motion keeps its exact value (no round trip through the pixels).
          CurveEditor(*d, ev, path).MovePoint(pi, m.dx == 0.0f ? p.x : axes.ValueX(px + m.dx),
                                              m.dy == 0.0f ? p.y : axes.ValueY(py + m.dy));
        });
    kit.Hit("pc.pt." + std::to_string(i), h2, x + px - 6.0f, y + py - 6.0f, 12.0f, 12.0f);
  }
  CurveLook cl;
  cl.dpi = kit.ctx().dpi;
  cl.width = w;
  cl.height = plotH;
  cl.axes = axes;
  cl.points = pts;
  cl.selected = pv.point;
  // The composition check's input value of this curve.
  const std::string inParam = path.rfind("distanceCurves.", 0) == 0 ? std::string("distance")
                                                                     : StringAt(d->Find(ev)->doc, path + ".parameter");
  if (inParam == "distance") {
    cl.hasMarker = true;
    cl.markerX = pv.distance;
  } else if (const DocObject* po = d->Resolve("parameter", inParam); po != nullptr && pv.values.count(po->guid) != 0u) {
    cl.hasMarker = true;
    cl.markerX = pv.values[po->guid];
  }
  Canvas(kit.doc(), "pc.plot", DrawCurve(cl, kit.palette()), x, y, w, plotH);
  kit.Label("pc.plot.x", "x " + FormatNumberText(axes.x0, 2) + " .. " + FormatNumberText(axes.x1, 2) + "   y " +
                             FormatNumberText(y0, 2) + " .. " + FormatNumberText(y1, 2),
            x, y + plotH, w, 18, 0.45f);
  y += plotH + 20.0f;
  // The selected point.
  if (pv.point >= 0) {
    const std::uint32_t pi = static_cast<std::uint32_t>(pv.point);
    const CurvePoint p = pts[pi];
    const float cw = (w - 16.0f) / 3.0f;
    NumberSpec sp;
    sp.step = (axes.x1 - axes.x0) / 100.0;
    sp.decimals = 3;
    sp.value = p.x;
    kit.NumberField("pc.pt.x", "x", sp, [d, ev, path, pi, p](double v, std::uint32_t) {
      return CurveEditor(*d, ev, path).MovePoint(pi, v, p.y);
    }, x, y, cw, 20);
    sp.step = std::max(1e-3, (axes.y1 - axes.y0) / 100.0);
    sp.value = p.y;
    kit.NumberField("pc.pt.y", "y", sp, [d, ev, path, pi, p](double v, std::uint32_t) {
      return CurveEditor(*d, ev, path).MovePoint(pi, p.x, v);
    }, x + cw + 8, y, cw, 20);
    NumberSpec sh;
    sh.value = p.shape;
    sh.step = 0.1;
    sh.minValue = 0;
    sh.decimals = 2;
    kit.NumberField("pc.pt.shape", "shape", sh, [d, ev, path, pi](double v, std::uint32_t) {
      return CurveEditor(*d, ev, path).SetShape(pi, v);
    }, x + 2 * (cw + 8), y, cw, 44);
    y += kRow;
    auto remove = [d, ev, path, pi, pvp, app] {
      if (CurveEditor(*d, ev, path).RemovePoint(pi)) {
        pvp->point = -1;
      } else {
        app->Message("refused: a curve keeps two points");
      }
    };
    kit.Action("pc.pt.remove", "Remove point", remove, x, y, 110);
    app->SetDeleteAction(remove);
  } else {
    kit.Label("pc.pt.hint", "double click the plot to add a point; drag points to move", x, y, w, 22, 0.5f);
  }
  y += kRow;
  // Target, compose, input (parameter curves only).
  if (path.rfind("curves[", 0) == 0) {
    const float cw = (w - 8.0f) / 3.0f;
    kit.PickField("pc.c.target", "", CurveTargetName(ce.target()), "Target", [] {
      std::vector<PickerItem> v;
      for (int t = 0; t <= 9; ++t) v.push_back(PickerItem{CurveTargetName(static_cast<CurveTarget>(t)), std::to_string(t)});
      return v;
    }, [d, ev, path](const std::string& v) { CurveEditor(*d, ev, path).SetTarget(static_cast<CurveTarget>(ToInt(v))); },
                  x, y, cw);
    kit.PickField("pc.c.compose", "", std::string(CurveComposeName(ce.compose())) + (ce.ComposeIgnored() ? " (ignored)" : ""),
                  "Compose", [] {
                    std::vector<PickerItem> v;
                    for (int c = 0; c <= 4; ++c) v.push_back(PickerItem{CurveComposeName(static_cast<CurveCompose>(c)), std::to_string(c)});
                    return v;
                  },
                  [d, ev, path](const std::string& v) { CurveEditor(*d, ev, path).SetCompose(static_cast<CurveCompose>(ToInt(v))); },
                  x + cw + 4, y, cw);
    kit.PickField("pc.c.input", "", inParam == "distance" ? std::string("distance") : RefName(*d, "parameter", inParam),
                  "Input", [d] {
                    std::vector<PickerItem> v = DocItems(*d, "parameter");
                    v.push_back(PickerItem{"distance", "distance"});
                    return v;
                  },
                  [d, ev, path](const std::string& v) { CurveEditor(*d, ev, path).SetParameter(v); }, x + 2 * (cw + 4), y,
                  cw);
    y += kRow;
    if (ce.ComposeIgnored()) {
      kit.Label("pc.c.warn", "the runtime ignores this compose for the target (fixed rule)", x, y, w, 20, 0.5f);
    }
  }
}

void ComposePanel(Kit& kit, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  ParamsView& pv = app->view().params;
  ParamsView* pvp = &pv;
  if (pv.event.empty() || d->Find(pv.event) == nullptr) return;
  const std::string ev = pv.event;
  kit.Title("pc.cmp.title", "composition: " + d->Find(ev)->name(), x, y, w);
  y += 24.0f;
  // The parameters the event's curves read.
  std::vector<std::string> params;
  if (const json::Value* c = d->Find(ev)->doc.Find("curves"); c != nullptr && c->IsArray()) {
    for (const json::Value& cv : c->items) {
      const DocObject* p = d->Resolve("parameter", StringAt(cv, "parameter"));
      if (p != nullptr && std::find(params.begin(), params.end(), p->guid) == params.end()) params.push_back(p->guid);
    }
  }
  std::map<std::string, ParamScopeValues> values;
  const char* const scopes[] = {"global", "emitter", "playback"};
  for (std::size_t i = 0; i < params.size() && i < 8u; ++i) {
    const std::string g = params[i];
    const DocObject* p = d->Find(g);
    const double def = NumberAt(p->doc, "default", 0.0);
    const double v = pv.values.count(g) != 0u ? pv.values[g] : def;
    const std::uint32_t scope = pv.scopes.count(g) != 0u ? pv.scopes[g] : 1u;
    NumberSpec sp;
    sp.value = v;
    sp.minValue = NumberAt(p->doc, "min", 0.0);
    sp.maxValue = NumberAt(p->doc, "max", 1.0);
    sp.step = std::max(1e-3, (sp.maxValue - sp.minValue) / 50.0);
    sp.decimals = 3;
    sp.document = false;
    std::map<std::string, double>* vm = &pv.values;
    kit.NumberField("pc.cmp.v." + ObjectKey(g), Clip(p->name(), 12), sp, [vm, g](double nv, std::uint32_t) {
      (*vm)[g] = nv;
      return true;
    }, x, y, w * 0.68f, 90);
    std::map<std::string, std::uint32_t>* sm = &pv.scopes;
    kit.PickField("pc.cmp.s." + ObjectKey(g), "", scope == 4u ? "playback" : scope == 2u ? "emitter" : "global", "Set at",
                  [scopes] {
                    std::vector<PickerItem> it;
                    for (std::uint32_t k = 0; k < 3; ++k) it.push_back(PickerItem{scopes[k], std::to_string(1u << k)});
                    return it;
                  },
                  [sm, g](const std::string& s) { (*sm)[g] = static_cast<std::uint32_t>(ToInt(s)); }, x + w * 0.7f, y,
                  w * 0.3f);
    if (pv.values.count(g) != 0u) {
      ParamScopeValues& sv = values[g];
      if (scope == 4u) sv.hasPlayback = true, sv.playback = v;
      else if (scope == 2u) sv.hasEmitter = true, sv.emitter = v;
      else sv.hasGlobal = true, sv.global = v;
    }
    y += kRow;
  }
  NumberSpec ds;
  ds.value = pv.distance;
  ds.step = 1;
  ds.minValue = 0;
  ds.unit = "m";
  ds.decimals = 2;
  ds.document = false;
  kit.NumberField("pc.cmp.dist", "distance", ds, [pvp](double v, std::uint32_t) {
    pvp->distance = v;
    return true;
  }, x, y, w * 0.68f, 90);
  y += kRow + 4.0f;
  const ComposeResult r = ComposeEvent(*d, ev, values, pv.distance);
  char b[256];
  std::snprintf(b, sizeof b, "volume %+.2f dB  pitch %+.1f ct  pan %+.3f", r.volumeDb, r.pitchCents, r.pan);
  kit.Label("pc.cmp.r0", b, x, y, w, 20, 0.55f);
  y += 20.0f;
  std::snprintf(b, sizeof b, "lowpass %s  highpass %s", r.lowpassHz > 0 ? (FormatNumberText(r.lowpassHz, 1) + " Hz").c_str() : "open",
                r.highpassHz > 0 ? (FormatNumberText(r.highpassHz, 1) + " Hz").c_str() : "none");
  kit.Label("pc.cmp.r1", b, x, y, w, 20, 0.55f);
  y += 20.0f;
  std::snprintf(b, sizeof b, "send %.3f  fx %.3f  spread %.3f  reverb %.3f  3D %.3f", r.send, r.effectParam, r.spread,
                r.reverbSend, r.blend3D);
  kit.Label("pc.cmp.r2", b, x, y, w, 20, 0.5f);
  y += 24.0f;
  const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (h - y - 60.0f) / 20.0f));
  const std::uint32_t first = kit.Pager("pc.cmp.pager", static_cast<std::uint32_t>(r.curves.size()), per, &pv.resultPage, x, y, 200);
  if (r.curves.size() > per) y += 22.0f;
  for (std::uint32_t i = first; i < r.curves.size() && i < first + per; ++i) {
    const CurveContribution& c = r.curves[i];
    const std::string pname = c.parameter == "distance" ? std::string("distance") : RefName(*d, "parameter", c.parameter);
    std::snprintf(b, sizeof b, "curve %u: %s = %.3f (%s) -> %s %s %.3f", c.curve, pname.c_str(), c.input, c.inputFrom,
                  CurveTargetName(c.target), CurveComposeName(c.compose), c.output);
    kit.Label("pc.cmp.c." + std::to_string(i - first), b, x, y, w, 20, 0.5f);
    y += 20.0f;
  }
  for (std::size_t i = 0; i < r.outsideScope.size() && i < 3u; ++i) {
    kit.Label("pc.cmp.scope." + std::to_string(i),
              "warning: " + RefName(*d, "parameter", r.outsideScope[i]) + " is read at a scope it does not declare", x, y, w,
              20, 0.5f);
    y += 20.0f;
  }
  if (!r.composeIgnored.empty()) {
    kit.Label("pc.cmp.ignored", "warning: " + std::to_string(r.composeIgnored.size()) + " curves write a compose the runtime ignores",
              x, y, w, 20, 0.5f);
  }
}

}  // namespace

void DeclareParams(tela::Document& doc, ViewContext& ctx, float width, float height) {
  Kit kit(doc, ctx);
  if (!NeedDocument(kit, "pc.nodoc", width)) return;
  App* app = ctx.app;
  ProjectDocument* d = app->document();
  ParamsView& pv = app->view().params;
  ObjectListSpec ls;
  ls.id = "pc.list";
  ls.kind = "parameter";
  ls.title = "Parameters";
  ls.selected = &pv.parameter;
  ls.page = &pv.listPage;
  ls.create = [d] {
    json::Value init = json::MakeObject();
    json::Add(init, "min", Number(0.0));
    json::Add(init, "max", Number(1.0));
    json::Add(init, "default", Number(0.0));
    const std::string g = d->Create("parameter", NewName(*d, "parameter", "Parameter"), &init);
    const std::string bank = FirstBank(*d);
    if (!g.empty() && !bank.empty()) d->Set(g, "bank", String(bank));
    return g;
  };
  const float listH = std::max(200.0f, height * 0.45f);
  Panel(doc, "pc.listpanel", 0, 0, kListW, height, [&] {
    ObjectList(kit, ls, 0, 0, kListW - 12, listH);
    ParameterPanel(kit, 0, listH + 8.0f, kListW - 12, height - listH - 20.0f);
  }, 6.0f);
  const float cx = kListW + 8.0f, cw = std::max(300.0f, (width - cx) * 0.58f);
  Panel(doc, "pc.curvepanel", cx, 0, cw, height, [&] { CurvePanel(kit, 0, 0, cw - 12, height - 12); }, 6.0f);
  const float rx = cx + cw + 8.0f;
  Panel(doc, "pc.compose", rx, 0, std::max(100.0f, width - rx), height,
        [&] { ComposePanel(kit, 0, 0, std::max(80.0f, width - rx - 12), height - 12); }, 6.0f);
}

}  // namespace snd::tool::ui
