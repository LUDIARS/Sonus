// 3D attenuation preview screen (design.md 4.5, 11.1 3D減衰プレビュー): the top-down view of the
// listener and the emitter (drag them, drag their facing handles; drag the background to pan, wheel
// to zoom), the maxDistance circle and the cone, the values the runtime computes for the
// arrangement (SpatialPreview::Compute), the distance curve with the current distance, and the
// audition (the model's request through the AuditionRouter: the emitter and the listener follow
// the drags while it plays; disabled with the reason when no destination can take it).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "editor_util.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/spatial_preview.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/editor_draw.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {
namespace {

constexpr float kListW = 200.0f;
constexpr float kSideW = 330.0f;
constexpr float kRow = 26.0f;
const CurveTarget kCurves[] = {CurveTarget::Volume, CurveTarget::Lowpass, CurveTarget::Spread, CurveTarget::ReverbSend,
                               CurveTarget::Blend3D};

float Yaw(const Vec3f& f) { return std::atan2(f.x, f.z); }

// While a 3D audition plays, the moving emitter and listener follow the view.
void Follow(App* app) {
  if (!app->view().spatial.playing || app->workspace().audition == nullptr || app->spatial() == nullptr) return;
  SpatialPreview* sp = app->spatial();
  app->workspace().audition->Update3D(sp->listener(), sp->emitter());
}

void ReportAudition(App* app, const AuditionResult& r, const char* what) {
  if (!r.ok) {
    app->Message(std::string(what) + ": " + (r.error.empty() ? "refused" : r.error));
  } else if (!r.unsupported.empty()) {
    app->Message(std::string(what) + ": " + r.unsupported.front());
  }
}

void TopDown(Kit& kit, SpatialPreview* sp, float x, float y, float w, float h) {
  App* app = &kit.app();
  // Background: pan and zoom.
  auto bg = std::make_shared<DragHandler>();
  struct Pan {
    float cx = 0.0f, cz = 0.0f;
  };
  auto pan = std::make_shared<Pan>();
  bg->press = [sp, pan, w, h](float, float, std::uint32_t) {
    sp->ScreenToWorld(w * 0.5f, h * 0.5f, w, h, &pan->cx, &pan->cz);
  };
  bg->drag = [sp, pan](const DragMotion& m) {
    const float mpp = sp->metersPerPixel();
    sp->SetView(pan->cx - m.dx * mpp, pan->cz + m.dy * mpp, mpp);
  };
  bg->wheel = [sp, w, h](float notches, std::uint32_t, std::uint32_t) {
    float cx = 0.0f, cz = 0.0f;
    sp->ScreenToWorld(w * 0.5f, h * 0.5f, w, h, &cx, &cz);
    sp->Zoom(std::pow(1.25f, notches), cx, cz);
  };
  kit.Hit("sp.view.hit", bg, x, y, w, h);
  TopDownLook look;
  look.dpi = kit.ctx().dpi;
  look.width = w;
  look.height = h;
  const float mpp = sp->metersPerPixel();
  // A grid of 1, 2, 5 x 10^n metres about 40 px apart.
  const double step = RulerStepMs(mpp, 40.0f);
  look.gridPx = static_cast<float>(step / mpp);
  float ox = 0.0f, oy = 0.0f;
  sp->WorldToScreen(0.0f, 0.0f, w, h, &ox, &oy);
  look.gridX = ox;
  look.gridY = oy;
  sp->WorldToScreen(sp->listener().position.x, sp->listener().position.z, w, h, &look.listener.x, &look.listener.y);
  sp->WorldToScreen(sp->emitter().position.x, sp->emitter().position.z, w, h, &look.emitter.x, &look.emitter.y);
  look.listenerYaw = Yaw(sp->listener().forward);
  look.emitterYaw = Yaw(sp->emitter().forward);
  look.maxDistancePx = sp->MaxDistance() > 0.0f ? sp->MaxDistance() / mpp : 0.0f;
  float outerDb = 0.0f;
  sp->Cone(&look.innerDeg, &look.outerDeg, &outerDb);
  TopDownShape shape;
  const tela::Drawing drawing = DrawTopDown(look, kit.palette(), &shape);
  // Movable things (positions in world units from the press).
  struct Grab {
    float wx = 0.0f, wz = 0.0f;
  };
  auto mover = [sp, app, w, h](bool listener) {
    auto g = std::make_shared<Grab>();
    auto hd = std::make_shared<DragHandler>();
    hd->press = [sp, g, listener](float, float, std::uint32_t) {
      const Vec3f p = listener ? sp->listener().position : sp->emitter().position;
      g->wx = p.x;
      g->wz = p.z;
    };
    hd->drag = [sp, g, listener, app](const DragMotion& m) {
      const float k = sp->metersPerPixel();
      const float nx = g->wx + m.dx * k, nz = g->wz - m.dy * k;
      if (listener) sp->MoveListener(nx, nz);
      else sp->MoveEmitter(nx, nz);
      Follow(app);
    };
    (void)w;
    (void)h;
    return hd;
  };
  auto turner = [sp, app](bool listener, tela::Point centre, float ox2, float oy2) {
    auto hd = std::make_shared<DragHandler>();
    hd->drag = [sp, app, listener, centre, ox2, oy2](const DragMotion& m) {
      // The pointer in view coordinates: the hit's origin + its local position.
      const float px = ox2 + m.x, py = oy2 + m.y;
      const float yaw = std::atan2(px - centre.x, centre.y - py);
      if (listener) sp->SetListenerYaw(yaw);
      else sp->SetEmitterYaw(yaw);
      Follow(app);
    };
    return hd;
  };
  kit.Hit("sp.listener", mover(true), x + look.listener.x - 12.0f, y + look.listener.y - 12.0f, 24.0f, 24.0f);
  kit.Hit("sp.emitter", mover(false), x + look.emitter.x - 12.0f, y + look.emitter.y - 12.0f, 24.0f, 24.0f);
  kit.Hit("sp.listener.yaw", turner(true, look.listener, shape.listenerHandle.x - 7.0f, shape.listenerHandle.y - 7.0f),
          x + shape.listenerHandle.x - 7.0f, y + shape.listenerHandle.y - 7.0f, 14.0f, 14.0f);
  kit.Hit("sp.emitter.yaw", turner(false, look.emitter, shape.emitterHandle.x - 7.0f, shape.emitterHandle.y - 7.0f),
          x + shape.emitterHandle.x - 7.0f, y + shape.emitterHandle.y - 7.0f, 14.0f, 14.0f);
  Canvas(kit.doc(), "sp.view", drawing, x, y, w, h);
  kit.Label("sp.view.scale", "grid " + FormatNumberText(step, 3) + " m", x + 4, y + h - 18, 120, 16, 0.45f);
}

void Values(Kit& kit, SpatialPreview* sp, float x, float y, float w, float h) {
  App* app = &kit.app();
  SpatialView& sv = app->view().spatial;
  SpatialView* svp = &sv;
  const SpatialResult r = sp->Compute(sv.outputs, 1, 0.0f);
  char b[200];
  std::vector<std::string> lines;
  std::snprintf(b, sizeof b, "%s  distance %.3f m%s", r.is3D ? "3D" : "2D (not attenuated)", r.distance,
                r.beyondMaxDistance ? "  beyond maxDistance" : "");
  lines.push_back(b);
  std::snprintf(b, sizeof b, "azimuth %.1f deg  cone %.1f deg -> %+.2f dB", r.azimuthRad * 180.0f / 3.14159265f,
                r.coneAngleDeg, r.coneDb);
  lines.push_back(b);
  std::snprintf(b, sizeof b, "%s %+.2f dB  x%.4f  curves %+.2f dB", r.volumeCurve ? "distance curve" : "inverse distance",
                r.distanceDb, r.distanceGain, r.curveDb);
  lines.push_back(b);
  std::snprintf(b, sizeof b, "gain %+.2f dB = %.5f  pitch %+.1f ct", r.gainDb, r.linearGain, r.pitchCents);
  lines.push_back(b);
  std::snprintf(b, sizeof b, "lowpass %s  highpass %s", r.lowpassHz > 0 ? (FormatNumberText(r.lowpassHz, 1) + " Hz").c_str() : "open",
                r.highpassHz > 0 ? (FormatNumberText(r.highpassHz, 1) + " Hz").c_str() : "none");
  lines.push_back(b);
  std::snprintf(b, sizeof b, "pan %+.3f  spread %.3f  3D blend %.3f  reverb %.3f", r.pan, r.spread, r.blend3D, r.reverbSend);
  lines.push_back(b);
  std::string out = "out";
  for (std::uint32_t c = 0; c < r.outputChannels && c < 8u; ++c) out += " " + FormatNumberText(r.outputGain[c], 4);
  lines.push_back(out);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    kit.Label("sp.v." + std::to_string(i), lines[i], x, y + static_cast<float>(i) * 18.0f, w, 18, 0.5f);
  }
  y += static_cast<float>(lines.size()) * 18.0f + 4.0f;
  kit.PickField("sp.outputs", "Outputs", std::to_string(sv.outputs), "Output channels", [] {
    std::vector<PickerItem> v;
    for (const char* n : {"1", "2", "4", "6", "8"}) v.push_back(PickerItem{n, n});
    return v;
  }, [svp](const std::string& v) { svp->outputs = static_cast<std::uint32_t>(std::atoi(v.c_str())); }, x, y, w * 0.5f, 60);
  y += kRow;
  // Positions as numbers (metres) and the facings (degrees).
  const float cw = (w - 8.0f) * 0.5f;
  NumberSpec sp1;
  sp1.step = 0.5;
  sp1.decimals = 2;
  sp1.document = false;
  sp1.unit = "m";
  const Vec3f lp = sp->listener().position, ep = sp->emitter().position;
  sp1.value = lp.x;
  kit.NumberField("sp.l.x", "L x", sp1, [sp, app, lp](double v, std::uint32_t) {
    sp->MoveListener(static_cast<float>(v), lp.z);
    Follow(app);
    return true;
  }, x, y, cw, 30);
  sp1.value = lp.z;
  kit.NumberField("sp.l.z", "L z", sp1, [sp, app, lp](double v, std::uint32_t) {
    sp->MoveListener(lp.x, static_cast<float>(v));
    Follow(app);
    return true;
  }, x + cw + 8, y, cw, 30);
  y += kRow;
  sp1.value = ep.x;
  kit.NumberField("sp.e.x", "E x", sp1, [sp, app, ep](double v, std::uint32_t) {
    sp->MoveEmitter(static_cast<float>(v), ep.z);
    Follow(app);
    return true;
  }, x, y, cw, 30);
  sp1.value = ep.z;
  kit.NumberField("sp.e.z", "E z", sp1, [sp, app, ep](double v, std::uint32_t) {
    sp->MoveEmitter(ep.x, static_cast<float>(v));
    Follow(app);
    return true;
  }, x + cw + 8, y, cw, 30);
  y += kRow;
  NumberSpec yaw;
  yaw.step = 5;
  yaw.decimals = 1;
  yaw.document = false;
  yaw.unit = "deg";
  yaw.value = Yaw(sp->listener().forward) * 180.0 / 3.14159265358979;
  kit.NumberField("sp.l.yaw", "L yaw", yaw, [sp, app](double v, std::uint32_t) {
    sp->SetListenerYaw(static_cast<float>(v * 3.14159265358979 / 180.0));
    Follow(app);
    return true;
  }, x, y, cw, 40);
  yaw.value = Yaw(sp->emitter().forward) * 180.0 / 3.14159265358979;
  kit.NumberField("sp.e.yaw", "E yaw", yaw, [sp, app](double v, std::uint32_t) {
    sp->SetEmitterYaw(static_cast<float>(v * 3.14159265358979 / 180.0));
    Follow(app);
    return true;
  }, x + cw + 8, y, cw, 40);
  y += kRow + 4.0f;
  // The distance curve with the current distance.
  static const char* const kNames[] = {"volume", "lowpass", "spread", "reverbSend", "blend3D"};
  kit.Tabs("sp.curve", {"vol", "lp", "spread", "rev", "3D"}, sv.curve, [svp](std::uint32_t i) { svp->curve = i; }, x, y,
           (w - 16.0f) / 5.0f);
  y += 28.0f;
  const std::vector<CurvePoint> pts = sp->DistanceCurve(kCurves[std::min<std::uint32_t>(sv.curve, 4u)]);
  const float plotH = std::max(60.0f, std::min(160.0f, h - y - 70.0f));
  if (pts.size() >= 2u) {
    double y0 = pts.front().y, y1 = pts.front().y;
    for (const CurvePoint& p : pts) y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    CurveLook cl;
    cl.dpi = kit.ctx().dpi;
    cl.width = w;
    cl.height = plotH;
    cl.axes = PlotAxes::Fit(0.0, std::max<double>(pts.back().x, std::max(1.0f, sp->MaxDistance())), y0, y1, w, plotH, 0.1);
    cl.points = pts;
    cl.hasMarker = true;
    cl.markerX = r.distance;
    Canvas(kit.doc(), "sp.curve.plot", DrawCurve(cl, kit.palette()), x, y, w, plotH);
  } else {
    kit.Label("sp.curve.none", std::string("no distance ") + kNames[std::min<std::uint32_t>(sv.curve, 4u)] +
                                   " curve (Params screen: + Distance)", x, y, w, 20, 0.5f);
  }
  y += plotH + 6.0f;
  // Audition.
  AuditionRouter* router = app->workspace().audition;
  std::string why = "no audition router";
  const bool ready = router != nullptr && router->Ready(&why);
  if (router != nullptr) {
    const bool preview = router->target() == AuditionTarget::Preview;
    kit.Toggle("sp.target", preview ? "to preview engine" : "to the game", preview, [router, app] {
      router->SetTarget(router->target() == AuditionTarget::Preview ? AuditionTarget::Game : AuditionTarget::Preview);
      app->view().spatial.playing = false;
    }, x, y, w * 0.5f);
  }
  kit.Action("sp.play", sv.playing ? "Stop" : "Play", [router, sp, app, svp] {
    if (svp->playing) {
      ReportAudition(app, router->StopAll(), "stop");
      svp->playing = false;
      return;
    }
    const AuditionResult started = router->Start3D(sp->Audition());
    ReportAudition(app, started, "play");
    svp->playing = started.ok;
  }, x + w * 0.5f + 4, y, w * 0.5f - 4, 22, ready);
  y += kRow;
  if (!ready) kit.Label("sp.why", "audition: " + why, x, y, w, 20, 0.5f);
}

}  // namespace

void DeclareSpatial(tela::Document& doc, ViewContext& ctx, float width, float height) {
  Kit kit(doc, ctx);
  if (!NeedDocument(kit, "sp.nodoc", width)) return;
  App* app = ctx.app;
  SpatialView& sv = app->view().spatial;
  ObjectListSpec ls;
  ls.id = "sp.list";
  ls.kind = "event";
  ls.title = "Events";
  ls.selected = &sv.event;
  ls.page = &sv.listPage;
  ls.manage = false;
  ls.label = [](const DocObject& o) { return o.name() + (BoolAt(o.doc, "is3D", false) ? " (3D)" : ""); };
  Panel(doc, "sp.listpanel", 0, 0, kListW, height, [&] { ObjectList(kit, ls, 0, 0, kListW - 12, height - 12); }, 6.0f);
  SpatialPreview* sp = app->spatial();
  if (sp == nullptr) {
    kit.Label("sp.none", "no event", kListW + 8, 0, 200);
    return;
  }
  const float vx = kListW + 8.0f, vw = std::max(200.0f, width - kListW - kSideW - 16.0f);
  Panel(doc, "sp.viewpanel", vx, 0, vw, height, [&] {
    kit.Action("sp.fit", "Fit view", [sp, vw, height] { sp->FitView(vw - 12.0f, height - 46.0f); }, 0, 0, 80);
    float inner = 0.0f, outer = 0.0f, odb = 0.0f;
    sp->Cone(&inner, &outer, &odb);
    kit.Label("sp.info", "maxDistance " + FormatNumberText(sp->MaxDistance(), 2) + " m   cone " +
                             FormatNumberText(inner, 1) + " / " + FormatNumberText(outer, 1) + " deg " +
                             FormatNumberText(odb, 1) + " dB   (drag the listener, the emitter, their handles)",
              86, 0, vw - 98, 22, 0.5f);
    TopDown(kit, sp, 0, 28, vw - 12, height - 46);
  }, 6.0f);
  Panel(doc, "sp.side", width - kSideW, 0, kSideW, height, [&] { Values(kit, sp, 0, 0, kSideW - 12, height - 12); }, 6.0f);
}

}  // namespace snd::tool::ui
