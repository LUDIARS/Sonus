// Mixer screen (design.md 11.1): bus strips (effect knobs, mute / solo, fader,
// meter), category strips, the overrides list with undo / redo / revert, and
// the parameter overrides.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "snd/tool/names.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"
#include "snd/tool/ui/widgets.hpp"

namespace snd::tool::ui {
namespace {

// Yellow: waiting for the Ack; orange: no answer for 2 s; red: refused.
tela::Color SyncColor(const CellSync& sync, std::int32_t cell, const Palette& p, std::uint64_t nowNs, bool* lit) {
  *lit = false;
  if (cell < 0 || static_cast<std::size_t>(cell) >= sync.cells().size()) return p.track;
  const CellStatus& s = sync.cells()[static_cast<std::size_t>(cell)];
  *lit = s.state != CellState::Synced;
  if (s.state == CellState::Failed) return p.failed;
  return CellSync::IsNoAnswer(s, nowNs) ? p.mute : p.pending;
}

std::string Short(const std::string& s, std::size_t n) { return s.size() <= n ? s : s.substr(0, n); }

std::string FormatValue(float v, const std::string& unit) {
  if (!std::isfinite(v)) return "-";  // D-189: a shown value is finite
  char b[32];
  if (unit == "Hz" && v >= 1000.0f) {
    std::snprintf(b, sizeof b, "%.1fk", static_cast<double>(v) / 1000.0);
  } else {
    std::snprintf(b, sizeof b, "%.3g%s", static_cast<double>(v), unit.empty() ? "" : unit.c_str());
  }
  return b;
}

void DeclareFader(tela::Document& doc, ViewContext& ctx, const std::string& base, const Target& target, float db,
                  float bankDb, bool silenced, const StripMetrics& m) {
  const std::string hit = base + ".fader.hit";
  FaderLook look;
  look.dpi = ctx.dpi;
  look.width = m.faderWidth;
  look.height = m.faderHeight;
  look.capHeight = m.capHeight;
  look.pos = FaderDbToPos(db);
  look.bankPos = FaderDbToPos(bankDb);
  look.pressed = IsPressed(ctx.runtime, hit);
  look.silenced = silenced;
  Button(doc, hit, std::string(), {}, 0, m.faderTop, m.faderWidth, m.faderHeight);
  Canvas(doc, base + ".fader", DrawFader(look, ctx.palette), 0, m.faderTop, m.faderWidth, m.faderHeight);
  ControlBinding b;
  b.hitId = hit;
  b.kind = ControlKind::Fader;
  b.target = target;
  b.travel = FaderGeometry{m.faderHeight, m.capHeight}.travel();
  ctx.controls.push_back(b);
}

void DeclareBusStrip(tela::Document& doc, ViewContext& ctx, std::int32_t bus, float x, float y,
                     const StripMetrics& m) {
  const Project& project = ctx.session.project();
  const MixerModel& mixer = ctx.session.mixer();
  const BusDesc& desc = project.buses()[static_cast<std::size_t>(bus)];
  const MixerModel::BusStrip& strip = mixer.buses()[static_cast<std::size_t>(bus)];
  const std::string base = "bus." + desc.key;
  Actions* a = &ctx.actions;
  Panel(doc, base, x, y, m.width, m.height, [&] {
    Text(doc, base + ".name", Short(desc.name, 14), 0, 0, m.inner - 12, m.nameHeight, 0.7f);
    bool lit = false;
    const CellSync& sync = ctx.session.sync();
    const tela::Color sc = SyncColor(sync, sync.Find(mixer.Cells()[static_cast<std::size_t>(bus)].address), ctx.palette, ctx.nowNs, &lit);
    Canvas(doc, base + ".sync", DrawLight(8, 8, lit, sc, ctx.palette, ctx.dpi), m.inner - 9, 6, 8, 8);
    // Effect parameter knobs (first knobSlots parameters over all effects).
    std::uint32_t slot = 0, hidden = 0;
    const float cellW = m.inner / static_cast<float>(m.knobColumns);
    for (std::size_t e = 0; e < desc.effects.size(); ++e) {
      const EffectDesc& fx = desc.effects[e];
      for (std::size_t pi = 0; pi < fx.params.size(); ++pi) {
        if (slot >= m.knobSlots()) {
          ++hidden;
          continue;
        }
        const EffectParamDesc& pd = fx.params[pi];
        const Target t{Control::EffectParam, bus, static_cast<std::int32_t>(e), static_cast<std::int32_t>(pi)};
        const float cx = static_cast<float>(slot % m.knobColumns) * cellW;
        const float cy = m.knobTop + static_cast<float>(slot / m.knobColumns) * m.knobRow;
        const float kx = cx + (cellW - m.knobSize) * 0.5f;
        const std::string kid = base + ".fx" + std::to_string(e) + "." + std::to_string(pi);
        const KnobRange range{pd.minValue, pd.maxValue, pd.logScale, pd.integer};
        KnobLook look;
        look.dpi = ctx.dpi;
        look.size = m.knobSize;
        look.norm = KnobToNorm(strip.params[e][pi], range);
        look.bankNorm = KnobToNorm(pd.projectValue, range);
        look.editable = !pd.initOnly;
        look.pressed = IsPressed(ctx.runtime, kid + ".hit");
        if (look.editable) {
          Button(doc, kid + ".hit", std::string(), {}, kx, cy, m.knobSize, m.knobSize);
          ControlBinding b;
          b.hitId = kid + ".hit";
          b.kind = ControlKind::Knob;
          b.target = t;
          b.travel = kKnobDragPixels;
          b.range = range;
          ctx.controls.push_back(b);
        }
        Canvas(doc, kid, DrawKnob(look, ctx.palette), kx, cy, m.knobSize, m.knobSize);
        Text(doc, kid + ".label", Short(pd.name, 9) + " " + FormatValue(strip.params[e][pi], pd.unit), cx,
             cy + m.knobSize, cellW, 14, 0.45f);
        ++slot;
      }
    }
    if (desc.effects.empty()) Text(doc, base + ".fx.none", "no effects", 0, m.knobTop, m.inner, 18, 0.55f);
    if (hidden != 0u) {
      Text(doc, base + ".fx.more", "+" + std::to_string(hidden) + " params", 0, m.buttonsTop - 16, m.inner, 14, 0.45f);
    }
    // Mute / solo with their lights.
    const Target mute{Control::BusMute, bus, 0, 0}, solo{Control::BusSolo, bus, 0, 0};
    Button(doc, base + ".mute", "M", [a, mute] { a->OnToggle(mute); }, 0, m.buttonsTop, m.buttonWidth, m.buttonHeight);
    Canvas(doc, base + ".mute.led", DrawLight(m.buttonWidth - 8, 3, strip.muted, ctx.palette.mute, ctx.palette, ctx.dpi), 4,
           m.buttonsTop + m.buttonHeight - 4, m.buttonWidth - 8, 3);
    Button(doc, base + ".solo", "S", [a, solo] { a->OnToggle(solo); }, m.buttonWidth + 4, m.buttonsTop,
           m.buttonWidth, m.buttonHeight);
    Canvas(doc, base + ".solo.led", DrawLight(m.buttonWidth - 8, 3, strip.soloed, ctx.palette.solo, ctx.palette, ctx.dpi),
           m.buttonWidth + 8, m.buttonsTop + m.buttonHeight - 4, m.buttonWidth - 8, 3);
    // Fader and meter.
    const bool soloMuted = mixer.SoloMuted(bus);
    DeclareFader(doc, ctx, base, Target{Control::BusFader, bus, 0, 0}, strip.faderDb,
                 mixer.BankValue(Target{Control::BusFader, bus, 0, 0}), strip.muted || soloMuted, m);
    MeterLook ml;
    ml.dpi = ctx.dpi;
    ml.width = m.meterWidth;
    ml.height = m.faderHeight;
    // The game reports the master bus with id 0 (SND_ID_NONE): the output.
    const BusMeter* meter = ctx.session.profiler().FindMeter(desc.master ? 0u : desc.id);
    ml.live = meter != nullptr && ctx.session.connection().state() == LinkState::Online;
    if (meter != nullptr) {
      ml.channels = std::max<std::uint32_t>(1u, meter->channels);
      for (std::uint32_t c = 0; c < 8u; ++c) {
        ml.peakDb[c] = LinearToDb(meter->peak[c]);
        ml.rmsDb[c] = LinearToDb(meter->rms[c]);
        ml.holdDb[c] = meter->holdDb[c];
      }
      ml.clipped = meter->clipped;
    }
    Canvas(doc, base + ".meter", DrawMeter(ml, ctx.palette), m.meterLeft, m.faderTop, m.meterWidth, m.faderHeight);
    std::string value = FormatDb(strip.faderDb);
    if (strip.muted) value += " M";
    if (soloMuted) value += " s";
    Text(doc, base + ".db", value, 0, m.faderTop + m.faderHeight + 2, m.inner, m.valueHeight, 0.6f);
  }, m.padding);
}

void DeclareCategoryStrip(tela::Document& doc, ViewContext& ctx, std::int32_t cat, float x, float y,
                          const StripMetrics& m) {
  const Project& project = ctx.session.project();
  const MixerModel& mixer = ctx.session.mixer();
  const CategoryDesc& desc = project.categories()[static_cast<std::size_t>(cat)];
  const MixerModel::CategoryStrip& strip = mixer.categories()[static_cast<std::size_t>(cat)];
  const std::string base = "cat." + desc.key;
  Actions* a = &ctx.actions;
  Panel(doc, base, x, y, m.width, m.height, [&] {
    Text(doc, base + ".name", "cat " + Short(desc.name, 10), 0, 0, m.inner - 12, m.nameHeight, 0.7f);
    bool lit = false;
    const CellSync& sync = ctx.session.sync();
    CellAddress addr;
    for (const Cell& c : mixer.Cells()) {
      if (c.owner.control == Control::CategoryFader && c.owner.a == cat) addr = c.address;
    }
    const tela::Color sc = SyncColor(sync, sync.Find(addr), ctx.palette, ctx.nowNs, &lit);
    Canvas(doc, base + ".sync", DrawLight(8, 8, lit, sc, ctx.palette, ctx.dpi), m.inner - 9, 6, 8, 8);
    Text(doc, base + ".kind", "category", 0, m.knobTop, m.inner, 18, 0.55f);
    const Target mute{Control::CategoryMute, cat, 0, 0};
    Button(doc, base + ".mute", "M", [a, mute] { a->OnToggle(mute); }, 0, m.buttonsTop, m.buttonWidth, m.buttonHeight);
    Canvas(doc, base + ".mute.led", DrawLight(m.buttonWidth - 8, 3, strip.muted, ctx.palette.mute, ctx.palette, ctx.dpi), 4,
           m.buttonsTop + m.buttonHeight - 4, m.buttonWidth - 8, 3);
    DeclareFader(doc, ctx, base, Target{Control::CategoryFader, cat, 0, 0}, strip.faderDb,
                 mixer.BankValue(Target{Control::CategoryFader, cat, 0, 0}), strip.muted, m);
    Text(doc, base + ".db", FormatDb(strip.faderDb) + (strip.muted ? " M" : ""), 0, m.faderTop + m.faderHeight + 2,
         m.inner, m.valueHeight, 0.6f);
  }, m.padding);
}

void DeclareSide(tela::Document& doc, ViewContext& ctx, float x, float height) {
  const MixerModel& mixer = ctx.session.mixer();
  Actions* a = &ctx.actions;
  const float w = kSidePanelWidth;
  Panel(doc, "side", x, 0, w, height, [&] {
    const std::vector<OverrideItem> items = mixer.Overrides();
    Text(doc, "ovr.title", "Overrides (" + std::to_string(items.size()) + ")", 0, 0, w - 12, 22, 0.75f);
    Button(doc, "ovr.undo", "Undo", [a] { a->OnUndo(); }, 0, 26, 70, 24);
    Button(doc, "ovr.redo", "Redo", [a] { a->OnRedo(); }, 76, 26, 70, 24);
    Button(doc, "ovr.revertall", "Revert all", [a] { a->OnRevertAll(); }, 152, 26, 140, 24);
    // 1.1 read-back of every mixer cell, and (with a document) the live values into the project.
    if (ctx.app != nullptr) {
      App* app = ctx.app;
      Button(doc, "ovr.readback", "Read back", [app] {
        if (!app->session().ReadBack()) app->Message("read back: needs a 1.1 game");
      }, 0, 54, 100, 22, 0.6f);
      if (app->document() != nullptr) {
        Button(doc, "ovr.writeback", "Write back to project", [app] { app->WriteBackMixer(); }, 106, 54, 186, 22, 0.6f);
      }
    }
    float y = 82.0f;
    const float paramTop = std::max(160.0f, height * 0.55f);
    std::size_t shown = 0;
    for (const OverrideItem& it : items) {
      if (y + 22.0f > paramTop - 20.0f) break;
      std::string label = it.label + " " + (it.target.control == Control::EffectParam || it.target.control ==
                                                    Control::ParamOverrideOn
                                                ? FormatValue(it.value, "")
                                                : FormatDb(it.value));
      if (it.muted) label += " (muted)";
      const std::string key = it.key;
      Text(doc, "ovr." + key, label, 0, y, w - 84, 22, 0.55f);
      Button(doc, "ovr." + key + ".revert", "Revert", [a, key] { a->OnRevert(key); }, w - 80, y, 68, 20, 0.55f);
      y += 24.0f;
      ++shown;
    }
    if (shown < items.size()) Text(doc, "ovr.more", "+" + std::to_string(items.size() - shown) + " more", 0, y, w - 12, 18, 0.55f);
    // Parameter overrides (global parameters).
    Text(doc, "par.title", "Parameters", 0, paramTop, w - 12, 22, 0.75f);
    float py = paramTop + 26.0f;
    const Project& project = ctx.session.project();
    for (std::size_t i = 0; i < project.parameters().size(); ++i) {
      const ParameterDesc& pd = project.parameters()[i];
      if (!pd.global) continue;
      if (py + 32.0f > height - 8.0f) break;
      const MixerModel::ParamState& ps = mixer.parameters()[i];
      const std::string base = "par." + pd.key;
      const KnobRange range{std::min(pd.minValue, pd.maxValue), std::max(pd.minValue, pd.maxValue), false, pd.isSwitch};
      char b[96];
      std::snprintf(b, sizeof b, "%s %s%s", Short(pd.name, 16).c_str(), FormatValue(ps.value, std::string()).c_str(),
                    ps.enabled ? " (override)" : "");
      Text(doc, base + ".name", b, 0, py + 4, w - 150, 22, 0.55f);
      KnobLook look;
      look.dpi = ctx.dpi;
      look.size = 28;
      look.norm = KnobToNorm(ps.value, range);
      look.bankNorm = KnobToNorm(pd.defaultValue, range);
      look.pressed = IsPressed(ctx.runtime, base + ".hit");
      Button(doc, base + ".hit", std::string(), {}, w - 146, py, 28, 28);
      Canvas(doc, base + ".knob", DrawKnob(look, ctx.palette), w - 146, py, 28, 28);
      ControlBinding cb;
      cb.hitId = base + ".hit";
      cb.kind = ControlKind::Knob;
      cb.target = Target{Control::ParamOverride, static_cast<std::int32_t>(i), 0, 0};
      cb.travel = kKnobDragPixels;
      cb.range = range;
      ctx.controls.push_back(cb);
      const Target on{Control::ParamOverrideOn, static_cast<std::int32_t>(i), 0, 0};
      Button(doc, base + ".on", ps.enabled ? "Release" : "Override", [a, on] { a->OnToggle(on); }, w - 110, py + 2, 98,
             24, 0.55f);
      py += 34.0f;
    }
  }, 6.0f);
}

}  // namespace

void DeclareMixer(tela::Document& doc, ViewContext& ctx, float width, float height) {
  const Project& project = ctx.session.project();
  Actions* a = &ctx.actions;
  const float areaW = std::max(kStripWidth, width - kSidePanelWidth - kStripGap);
  const std::uint32_t perPage = StripsPerPage(areaW);
  const std::uint32_t total = static_cast<std::uint32_t>(project.buses().size() + project.categories().size());
  const std::uint32_t pages = std::max<std::uint32_t>(1u, (total + perPage - 1u) / perPage);
  const std::uint32_t page = std::min(ctx.state.mixerPage, pages - 1u);
  Panel(doc, "mixer", 0, 0, areaW, height, [&] {
    char b[64];
    std::snprintf(b, sizeof b, "page %u / %u   %u buses  %u categories", page + 1u, pages,
                  static_cast<unsigned>(project.buses().size()), static_cast<unsigned>(project.categories().size()));
    Button(doc, "mixer.prev", "<", [a] { a->OnPage(-1); }, 0, 2, 32, 24);
    Button(doc, "mixer.next", ">", [a] { a->OnPage(1); }, 36, 2, 32, 24);
    Text(doc, "mixer.page", b, 76, 2, areaW - 80, 24, 0.6f);
    const StripMetrics m = ComputeStripMetrics(height - kPagerHeight);
    for (std::uint32_t k = 0; k < perPage; ++k) {
      const std::uint32_t i = page * perPage + k;
      if (i >= total) break;
      const float x = static_cast<float>(k) * (kStripWidth + kStripGap);
      if (i < project.buses().size()) {
        DeclareBusStrip(doc, ctx, static_cast<std::int32_t>(i), x, kPagerHeight, m);
      } else {
        DeclareCategoryStrip(doc, ctx, static_cast<std::int32_t>(i - project.buses().size()), x, kPagerHeight, m);
      }
    }
  });
  DeclareSide(doc, ctx, areaW + kStripGap, height);
}

}  // namespace snd::tool::ui
