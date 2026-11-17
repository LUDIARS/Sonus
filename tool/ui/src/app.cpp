// The tool's UI over a Session.
#include "snd/tool/ui/app.hpp"

#include <algorithm>
#include <cmath>

#include "snd/tool/ui/layout_metrics.hpp"

namespace snd::tool::ui {

bool ValidTheme(const tela::Theme& t) {
  return std::isfinite(t.font_size) && std::isfinite(t.line_height) && t.font_size > 0.0f && t.font_size <= 256.0f &&
         t.line_height > 0.0f && t.line_height <= 512.0f;
}

App::App(Session& session, const AppDesc& desc)
    : session_(session), desc_(desc), palette_(MakePalette(ValidTheme(desc.theme) ? desc.theme : tela::Theme{})),
      router_(*this) {
  runtime_.theme(palette_.theme);  // validated above: Tela would throw on an invalid theme
  if (!desc_.hostOwnsViewport) SetViewport(desc_.width, desc_.height, desc_.dpiScale);
}

void App::SetViewport(int width, int height, float dpiScale) {
  if (desc_.hostOwnsViewport) return;
  // Tela's limits (Runtime::viewport throws outside them).
  if (!(std::isfinite(dpiScale) && dpiScale >= 0.25f && dpiScale <= 8.0f)) dpiScale = 1.0f;
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  if (width > 16384) width = 16384;
  if (height > 16384) height = 16384;
  if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > 16777216u) height = 16777216 / width;
  tela::Viewport v;
  v.host_id = "sonus-tool";
  v.view_id = "main";
  v.revision = ++revision_;
  v.width = width;
  v.height = height;
  v.dpi_scale = dpiScale;
  v.visible = true;
  v.focused = true;
  runtime_.viewport(v);
}

void App::SetInsetTop(float logicalPixels) {
  desc_.insetTop = std::isfinite(logicalPixels) ? std::clamp(logicalPixels, 0.0f, 200.0f) : 0.0f;
}

void App::Frame(std::uint64_t nowNs) {
  session_.Tick(nowNs);
  router_.Tick(runtime_);
  Declare(nowNs);
}

void App::Declare(std::uint64_t nowNs) {
  nowNs_ = nowNs;
  const tela::Viewport& vp = runtime_.viewport();
  const float scale = vp.dpi_scale > 0.0f ? vp.dpi_scale : 1.0f;
  const float w = std::max(320.0f, static_cast<float>(vp.width) / scale);
  const float inset = std::isfinite(desc_.insetTop) ? std::clamp(desc_.insetTop, 0.0f, 200.0f) : 0.0f;
  const float h = std::max(240.0f, static_cast<float>(vp.height) / scale - inset);
  {  // keep the mixer page in range (DeclareMixer's paging)
    const float areaW = std::max(kStripWidth, (w - 12.0f) - kSidePanelWidth - kStripGap);
    const std::uint32_t per = StripsPerPage(areaW);
    const std::uint32_t total = static_cast<std::uint32_t>(session_.project().buses().size() +
                                                           session_.project().categories().size());
    const std::uint32_t pages = std::max<std::uint32_t>(1u, (total + per - 1u) / per);
    if (view_.mixerPage >= pages) view_.mixerPage = pages - 1u;
  }
  std::vector<ControlBinding> controls;
  ViewContext ctx{session_, palette_, view_, *this, runtime_, controls, nowNs, desc_.recordingPath};
  tela::Document doc;
  Panel(doc, "root", 0, inset, w, h, [&] {
    Panel(doc, "header", 0, 0, w, kHeaderHeight, [&] { DeclareHeader(doc, ctx, w); });
    const float bodyH = h - kHeaderHeight - kFooterHeight;
    Panel(doc, "body", 0, kHeaderHeight, w, bodyH, [&] {
      if (view_.screen == Screen::Mixer) {
        DeclareMixer(doc, ctx, w - 12.0f, bodyH - 12.0f);
      } else {
        DeclareProfiler(doc, ctx, w - 12.0f, bodyH - 12.0f);
      }
    }, 6.0f);
    Panel(doc, "footer", 0, h - kFooterHeight, w, kFooterHeight, [&] { DeclareFooter(doc, ctx, w); });
  });
  runtime_.document(std::move(doc));
  router_.SetControls(std::move(controls));
  ++declarations_;
}

bool App::Pointer(const tela::HostPointerEvent& event, std::uint64_t timeMs, bool forward) {
  const bool used = router_.Pointer(runtime_, event, timeMs, forward);
  return used;
}

// --- ControlSink ---------------------------------------------------------------------------
float App::ControlValue(const Target& t) const { return session_.mixer().Get(t); }
float App::ControlDefault(const Target& t) const { return session_.mixer().BankValue(t); }
void App::ControlBegin(const Target&) { session_.mixer().BeginGroup(); }
void App::ControlSet(const Target& t, float value, std::uint32_t mergeKey) {
  MixerModel& m = session_.mixer();
  // A parameter knob turns its override on (one edit with the value).
  if (t.control == Control::ParamOverride && m.Get(Target{Control::ParamOverrideOn, t.a, 0, 0}) == 0.0f) {
    m.BeginGroup();
    m.Set(t, value);
    m.Set(Target{Control::ParamOverrideOn, t.a, 0, 0}, 1.0f);
    m.EndGroup();
    return;
  }
  m.Set(t, value, mergeKey);
}
void App::ControlEnd(const Target&) { session_.mixer().EndGroup(); }

// --- Actions ---------------------------------------------------------------------------------
void App::OnScreen(Screen s) { view_.screen = s; }
void App::OnConnectToggle() {
  if (session_.connection().IsOpen()) {
    session_.Disconnect();
    session_.Message("disconnected");
  } else {
    session_.Connect(desc_.connection);
  }
}
void App::OnUndo() { session_.mixer().Undo(); }
void App::OnRedo() { session_.mixer().Redo(); }
void App::OnToggle(const Target& t) {
  MixerModel& m = session_.mixer();
  m.Set(t, m.Get(t) != 0.0f ? 0.0f : 1.0f);
}
void App::OnRevert(const std::string& key) {
  for (const OverrideItem& it : session_.mixer().Overrides()) {
    if (it.key == key) {
      session_.mixer().Revert(it);
      return;
    }
  }
}
void App::OnRevertAll() { session_.mixer().RevertAll(); }
void App::OnPage(std::int32_t delta) {
  const std::int64_t p = static_cast<std::int64_t>(view_.mixerPage) + delta;
  view_.mixerPage = p < 0 ? 0u : static_cast<std::uint32_t>(p);  // clamped when declared
}
void App::OnRecordToggle() {
  Profiler& p = session_.profiler();
  const RecordingState s = p.recordingState();
  if (s == RecordingState::Recording || s == RecordingState::Starting) {
    p.StopRecording(session_.connection().client());
  } else if (!p.StartRecording(session_.connection().client())) {
    session_.Message("cannot start recording (not connected)");
  }
}
void App::OnSaveRecording() { session_.SaveRecording(desc_.recordingPath); }
void App::OnLoadRecording() { session_.LoadRecording(desc_.recordingPath); }
void App::OnResetClips() { session_.profiler().ResetClips(); }

}  // namespace snd::tool::ui
