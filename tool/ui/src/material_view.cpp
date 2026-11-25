// Material table screen (design.md 9.1, 9.3, 11.1 マテリアル表エディタ): tables, materials with
// their numbers (insert / move / remove renumber: the screen says how), single events per contact
// kind, footsteps, the global defaults, the symmetric pair table (a window of 8 x 8 cells, scrolled),
// the resolution of a contact with the fallbacks list, and the strength curves with a test input.
// Edits through MaterialEditor.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "editor_util.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/editor_draw.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {
namespace {

constexpr float kListW = 200.0f;
constexpr float kRow = 26.0f;
constexpr std::uint32_t kWindow = 8;
const char* const kKinds[] = {"impact", "slide", "roll", "break"};

std::string RenumberText(const Renumbering& r) {
  std::string s;
  for (std::size_t i = 0; i < r.oldToNew.size(); ++i) {
    if (r.oldToNew[i] == static_cast<int>(i)) continue;
    s += (s.empty() ? "" : ", ") + std::to_string(i) + "->" + (r.oldToNew[i] < 0 ? std::string("x") : std::to_string(r.oldToNew[i]));
  }
  return s.empty() ? std::string() : "renumbered (game code using numbers must follow): " + s;
}

void MaterialsColumn(Kit& kit, const std::string& g, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  MaterialView* mv = &app->view().material;
  MaterialEditor me(*d, g);
  const std::vector<std::string> mats = me.Materials();
  if (std::find(mats.begin(), mats.end(), mv->material) == mats.end()) mv->material = mats.empty() ? std::string() : mats.front();
  kit.Title("ma.mat.title", "materials (" + std::to_string(mats.size()) + ")", x, y, w);
  y += 24.0f;
  kit.Action("ma.mat.add", "+", [d, g, mv] {
    MaterialEditor e(*d, g);
    std::string name;
    for (std::uint32_t n = 1; n < 10000u; ++n) {
      name = "material" + std::to_string(n);
      if (e.IndexOf(name) < 0) break;
    }
    Renumbering r;
    if (e.AddMaterial(name, static_cast<std::uint32_t>(e.Materials().size()), &r)) mv->material = name;
  }, x, y, 30);
  const int sel = me.IndexOf(mv->material);
  kit.Action("ma.mat.remove", "Remove", [d, g, mv, app] {
    Renumbering r;
    if (!MaterialEditor(*d, g).RemoveMaterial(mv->material, &r)) return app->Message("refused: the last material stays");
    mv->renumbered = RenumberText(r);
  }, x + 34, y, 70, 22, sel >= 0);
  kit.Action("ma.mat.up", "Up", [d, g, mv, sel] {
    Renumbering r;
    if (MaterialEditor(*d, g).MoveMaterial(static_cast<std::uint32_t>(sel), static_cast<std::uint32_t>(sel - 1), &r)) {
      mv->renumbered = RenumberText(r);
    }
  }, x + 108, y, 40, 22, sel > 0);
  kit.Action("ma.mat.down", "Down", [d, g, mv, sel] {
    Renumbering r;
    if (MaterialEditor(*d, g).MoveMaterial(static_cast<std::uint32_t>(sel), static_cast<std::uint32_t>(sel + 1), &r)) {
      mv->renumbered = RenumberText(r);
    }
  }, x + 152, y, 50, 22, sel >= 0 && static_cast<std::size_t>(sel + 1) < mats.size());
  y += 26.0f;
  if (sel >= 0) {
    const std::string cur = mv->material;
    kit.NameField("ma.mat.name", "Name", cur, [d, g, mv, cur](const std::string& n) {
      if (!MaterialEditor(*d, g).RenameMaterial(cur, n)) return false;
      mv->material = n;
      return true;
    }, x, y, w, 50);
  }
  y += 26.0f;
  const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (h - y - 40.0f) / 24.0f));
  const std::uint32_t first = kit.Pager("ma.mat.pager", static_cast<std::uint32_t>(mats.size()), per, &mv->listPage, x, y, w);
  if (mats.size() > per) y += 22.0f;
  const std::vector<std::string> keys = NameKeys::Of(mats);
  for (std::uint32_t i = first; i < mats.size() && i < first + per; ++i) {
    const std::string m = mats[i];
    Button(kit.doc(), "ma.mat." + keys[i], (m == mv->material ? "> " : "  ") + std::to_string(i) + ": " + Clip(m, 24),
           [mv, m] { mv->material = m; }, x, y, w, 22, 0.55f);
    y += 24.0f;
  }
}

void MaterialProps(Kit& kit, const std::string& g, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  MaterialView* mv = &app->view().material;
  MaterialEditor me(*d, g);
  if (mv->material.empty()) return;
  const std::string m = mv->material;
  kit.Title("ma.p.title", m + " (number " + std::to_string(me.IndexOf(m)) + ")", x, y, w);
  y += 24.0f;
  for (int k = 0; k < 4; ++k) {
    const ContactKind kind = static_cast<ContactKind>(k);
    kit.PickField("ma.p.single" + std::to_string(k), kKinds[k], RefName(*d, "event", me.Single(m, kind), "(default)"), "Single event",
                  [d] { return DocItems(*d, "event", "(default)"); },
                  [d, g, m, kind](const std::string& v) { MaterialEditor(*d, g).SetSingle(m, kind, v); }, x, y, w, 64);
    y += kRow;
  }
  kit.PickField("ma.p.foot", "footstep", RefName(*d, "event", me.Footstep(m), "(none)"), "Footstep event",
                [d] { return DocItems(*d, "event", "(none)"); },
                [d, g, m](const std::string& v) { MaterialEditor(*d, g).SetFootstep(m, v); }, x, y, w, 64);
  y += kRow + 6.0f;
  // Strength curves.
  const StrengthCurves c = me.Curves(m);
  kit.Label("ma.p.curves", "strength curves", x, y, w, 20, 0.6f);
  y += 22.0f;
  const float cw = (w - 8.0f) * 0.5f;
  static const char* const kThr[] = {"weak", "medium", "strong", "full"};
  for (int i = 0; i < 4; ++i) {
    NumberSpec sp;
    sp.value = c.impulseToVelocity[i];
    sp.step = std::max(0.01, static_cast<double>(c.impulseToVelocity[i]) * 0.1);
    sp.minValue = 0;
    sp.decimals = 3;
    kit.NumberField("ma.p.imp" + std::to_string(i), kThr[i], sp, [d, g, m, c, i](double v, std::uint32_t) {
      StrengthCurves n = c;
      if (!ToF32(v, &n.impulseToVelocity[i])) return false;
      return MaterialEditor(*d, g).SetCurves(m, n);
    }, x + static_cast<float>(i % 2) * (cw + 8), y, cw, 50);
    if (i % 2 == 1) y += kRow;
  }
  static const char* const kMs[] = {"1 kg ct", "100 kg ct", "0.1 m/s dB", "10 m/s dB"};
  for (int i = 0; i < 4; ++i) {
    NumberSpec sp;
    sp.value = i < 2 ? c.massToPitchCents[i] : c.speedToVolumeDb[i - 2];
    sp.step = i < 2 ? 10.0 : 1.0;
    sp.decimals = 2;
    kit.NumberField("ma.p.ms" + std::to_string(i), kMs[i], sp, [d, g, m, c, i](double v, std::uint32_t) {
      StrengthCurves n = c;
      if (!ToF32(v, i < 2 ? &n.massToPitchCents[i] : &n.speedToVolumeDb[i - 2])) return false;
      return MaterialEditor(*d, g).SetCurves(m, n);
    }, x + static_cast<float>(i % 2) * (cw + 8), y, cw, 64);
    if (i % 2 == 1) y += kRow;
  }
  // Test input -> result, and the impulse -> velocity plot with the layer thresholds.
  NumberSpec in;
  in.document = false;
  in.decimals = 3;
  in.minValue = 0;
  in.value = mv->impulse;
  in.step = std::max(0.01, mv->impulse * 0.1);
  kit.NumberField("ma.t.impulse", "impulse", in, [mv](double v, std::uint32_t) {
    float f = 0.0f;
    return ToF32(v, &f) && (mv->impulse = v, true);
  }, x, y, w / 3 - 4, 48);
  in.value = mv->massKg;
  in.step = std::max(0.01, mv->massKg * 0.1);
  kit.NumberField("ma.t.mass", "kg", in, [mv](double v, std::uint32_t) {
    float f = 0.0f;
    return ToF32(v, &f) && (mv->massKg = v, true);
  }, x + w / 3, y, w / 3 - 4, 20);
  in.value = mv->speed;
  in.step = 0.1;
  kit.NumberField("ma.t.speed", "m/s", in, [mv](double v, std::uint32_t) {
    float f = 0.0f;
    return ToF32(v, &f) && (mv->speed = v, true);
  }, x + 2 * w / 3, y, w / 3, 26);
  y += kRow;
  const StrengthResult r = EvaluateStrength(c, static_cast<float>(mv->impulse), static_cast<float>(mv->massKg),
                                            static_cast<float>(mv->speed));
  static const char* const kLayer[] = {"dropped", "weak", "medium", "strong"};
  char b[160];
  SnprintfFinite(b, sizeof b, "-> %s  velocity %.3f  pitch %+.1f ct  volume %+.2f dB", kLayer[std::clamp(r.layer + 1, 0, 3)], r.velocity,
                r.pitchCents, r.volumeDb);
  kit.Label("ma.t.result", b, x, y, w, 20, 0.5f);
  y += 22.0f;
  const float plotH = std::max(50.0f, std::min(110.0f, h - y - 4.0f));
  CurveLook cl;
  cl.dpi = kit.ctx().dpi;
  cl.width = w;
  cl.height = plotH;
  const double full = std::max(1e-6, static_cast<double>(c.impulseToVelocity[3]));
  cl.axes = PlotAxes::Fit(0.0, full * 1.2, 0.0, 1.0, w, plotH, 0.05, kCurveXMargin);
  cl.points = {CurvePoint{0.0, 0.0, 0.0}, CurvePoint{full, 1.0, 0.0}, CurvePoint{full * 1.2, 1.0, 0.0}};
  cl.hasMarker = true;
  cl.markerX = mv->impulse;
  Canvas(kit.doc(), "ma.t.plot", DrawCurve(cl, kit.palette()), x, y, w, plotH);
}

void PairsAndResolve(Kit& kit, const std::string& g, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  MaterialView* mv = &app->view().material;
  MaterialEditor me(*d, g);
  const std::vector<std::string> mats = me.Materials();
  const std::uint32_t n = static_cast<std::uint32_t>(mats.size());
  if (mv->pairRow + kWindow > n) mv->pairRow = n > kWindow ? n - kWindow : 0u;
  if (mv->pairCol + kWindow > n) mv->pairCol = n > kWindow ? n - kWindow : 0u;
  kit.Title("ma.pair.title", "pairs (symmetric; a pair overrides every kind)", x, y, w - 120);
  if (n > kWindow) {
    kit.Action("ma.pair.up", "^", [mv] { mv->pairRow = mv->pairRow > 0u ? mv->pairRow - 1u : 0u; }, x + w - 112, y, 24);
    kit.Action("ma.pair.down", "v", [mv] { ++mv->pairRow; }, x + w - 84, y, 24);
    kit.Action("ma.pair.left", "<", [mv] { mv->pairCol = mv->pairCol > 0u ? mv->pairCol - 1u : 0u; }, x + w - 56, y, 24);
    kit.Action("ma.pair.right", ">", [mv] { ++mv->pairCol; }, x + w - 28, y, 24);
  }
  y += 24.0f;
  const std::vector<std::string> keys = NameKeys::Of(mats);
  const float headW = 70.0f;
  const float cellW = std::max(30.0f, (w - headW) / static_cast<float>(kWindow));
  for (std::uint32_t c = 0; c < kWindow && mv->pairCol + c < n; ++c) {
    kit.Label("ma.pair.col." + std::to_string(c), std::to_string(mv->pairCol + c) + " " + Clip(mats[mv->pairCol + c], 6),
              x + headW + static_cast<float>(c) * cellW, y, cellW, 18, 0.42f);
  }
  y += 18.0f;
  for (std::uint32_t r = 0; r < kWindow && mv->pairRow + r < n; ++r) {
    const std::string a = mats[mv->pairRow + r];
    kit.Label("ma.pair.row." + std::to_string(r), std::to_string(mv->pairRow + r) + " " + Clip(a, 8), x, y, headW, 22, 0.45f);
    for (std::uint32_t c = 0; c < kWindow && mv->pairCol + c < n; ++c) {
      const std::string b = mats[mv->pairCol + c];
      const std::string ev = me.Pair(a, b);
      kit.PickField("ma.pair." + keys[mv->pairRow + r] + "." + keys[mv->pairCol + c], "", ev.empty() ? std::string(".") : Clip(RefName(*d, "event", ev), 7),
                    a + " x " + b, [d] { return DocItems(*d, "event", "(none)"); },
                    [d, g, a, b](const std::string& v) { MaterialEditor(*d, g).SetPair(a, b, v); },
                    x + headW + static_cast<float>(c) * cellW, y, cellW - 2);
    }
    y += 24.0f;
  }
  y += 8.0f;
  // The global defaults.
  for (int k = 0; k < 4; ++k) {
    const ContactKind kind = static_cast<ContactKind>(k);
    kit.PickField("ma.def" + std::to_string(k), std::string("default ") + kKinds[k], RefName(*d, "event", me.Default(kind), "(none)"),
                  "Default event", [d] { return DocItems(*d, "event", "(none)"); },
                  [d, g, kind](const std::string& v) { MaterialEditor(*d, g).SetDefault(kind, v); },
                  x + static_cast<float>(k % 2) * (w * 0.5f), y, w * 0.5f - 4, 100);
    if (k % 2 == 1) y += kRow;
  }
  y += 6.0f;
  // Resolution of one contact.
  if (n == 0u) return;
  if (mv->a >= n) mv->a = 0;
  if (mv->b >= n) mv->b = 0;
  auto matItems = [mats] {
    std::vector<PickerItem> v;
    for (std::size_t i = 0; i < mats.size(); ++i) v.push_back(PickerItem{std::to_string(i) + ": " + mats[i], std::to_string(i)});
    return v;
  };
  kit.PickField("ma.res.a", "contact", mats[mv->a], "Material a", matItems,
                [mv](const std::string& v) { mv->a = static_cast<std::uint32_t>(std::atoi(v.c_str())); }, x, y, w * 0.38f, 60);
  kit.PickField("ma.res.b", "x", mats[mv->b], "Material b", matItems,
                [mv](const std::string& v) { mv->b = static_cast<std::uint32_t>(std::atoi(v.c_str())); }, x + w * 0.38f + 4, y,
                w * 0.3f, 14);
  kit.PickField("ma.res.kind", "", kKinds[std::min<std::uint32_t>(mv->kind, 3u)], "Contact kind", [] {
    std::vector<PickerItem> v;
    for (int k = 0; k < 4; ++k) v.push_back(PickerItem{kKinds[k], std::to_string(k)});
    return v;
  }, [mv](const std::string& v) { mv->kind = static_cast<std::uint32_t>(std::atoi(v.c_str())); }, x + w * 0.68f + 8, y,
                w * 0.32f - 8);
  y += kRow;
  const MaterialResolution res = me.Resolve(mv->a, mv->b, static_cast<ContactKind>(std::min<std::uint32_t>(mv->kind, 3u)));
  std::string text = "-> " + res.source + ":";
  for (const std::string& e : res.events) text += " " + RefName(*d, "event", e);
  if (res.events.empty()) text += " no sound";
  kit.Label("ma.res.text", text, x, y, w, 20, 0.55f);
  y += 24.0f;
  // Fallbacks (contacts that do not sound the materials' own events).
  const std::vector<MaterialResolution> fb = me.Fallbacks();
  kit.Label("ma.fb.title", "fallbacks (" + std::to_string(fb.size()) + ")", x, y, w - 210, 20, 0.55f);
  const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (h - y - 26.0f) / 18.0f));
  const std::uint32_t first = kit.Pager("ma.fb.pager", static_cast<std::uint32_t>(fb.size()), per, &mv->fallbackPage, x + w - 200, y, 200);
  y += 22.0f;
  for (std::uint32_t i = first; i < fb.size() && i < first + per; ++i) {
    const MaterialResolution& f = fb[i];
    std::string t2 = mats[f.a] + " x " + mats[f.b] + " " + ContactName(f.kind) + ": " + f.source;
    for (const std::string& e : f.events) t2 += " " + RefName(*d, "event", e);
    kit.Label("ma.fb." + std::to_string(i - first), t2, x, y, w, 18, 0.48f);
    y += 18.0f;
  }
}

}  // namespace

void DeclareMaterial(tela::Document& doc, ViewContext& ctx, float width, float height) {
  Kit kit(doc, ctx);
  if (!NeedDocument(kit, "ma.nodoc", width)) return;
  App* app = ctx.app;
  ProjectDocument* d = app->document();
  MaterialView& mv = app->view().material;
  ObjectListSpec ls;
  ls.id = "ma.list";
  ls.kind = "materialTable";
  ls.title = "Material tables";
  ls.selected = &mv.table;
  ls.page = &mv.listPage;
  ls.create = [d] {
    json::Value init = json::MakeObject();
    const std::string bank = FirstBank(*d);
    if (!bank.empty()) json::Add(init, "bank", String(bank));
    json::Value mats = json::MakeArray();
    mats.items.push_back(String("default"));
    json::Add(init, "materials", mats);
    return d->Create("materialTable", NewName(*d, "materialTable", "Materials"), &init);
  };
  const std::string before = mv.table;
  Panel(doc, "ma.listpanel", 0, 0, kListW, height, [&] { ObjectList(kit, ls, 0, 0, kListW - 12, height - 12); }, 6.0f);
  if (mv.table != before) mv.material.clear();
  if (mv.table.empty()) return;
  const std::string g = mv.table;
  const float c2 = kListW + 8.0f, w2 = 220.0f;
  Panel(doc, "ma.mats", c2, 0, w2, height, [&] {
    MaterialsColumn(kit, g, 0, 0, w2 - 12, height - 12);
    if (!mv.renumbered.empty()) kit.Label("ma.renumbered", Clip(mv.renumbered, 80), 0, height - 40, w2 - 12, 26, 0.42f);
  }, 6.0f);
  const float c3 = c2 + w2 + 8.0f, w3 = 330.0f;
  Panel(doc, "ma.props", c3, 0, w3, height, [&] { MaterialProps(kit, g, 0, 0, w3 - 12, height - 12); }, 6.0f);
  const float c4 = c3 + w3 + 8.0f;
  Panel(doc, "ma.pairs", c4, 0, std::max(120.0f, width - c4), height,
        [&] { PairsAndResolve(kit, g, 0, 0, std::max(100.0f, width - c4 - 12), height - 12); }, 6.0f);
}

}  // namespace snd::tool::ui
