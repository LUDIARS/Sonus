// The tool's UI over a Session.
#include "snd/tool/ui/app.hpp"

#include <algorithm>
#include <cmath>

#include "snd/tool/live_sync.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/safe_drawing.hpp"

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
  if (document() != nullptr) {
    seenDocRevision_ = document()->revision();
    mixerShape_ = MixerSignature(false);
    mixerValues_ = MixerSignature(true);
    dirty_ = document()->IsDirty();
  }
}

void App::SetViewport(int width, int height, float dpiScale) {
  if (desc_.hostOwnsViewport) return;
  // Tela's limits (Runtime::viewport throws outside them).
  // The DPI scales the tool accepts (as --dpi): kMinDpiScale .. kMaxDpiScale, clamped.
  if (!std::isfinite(dpiScale)) dpiScale = 1.0f;
  dpiScale = std::clamp(dpiScale, kMinDpiScale, kMaxDpiScale);
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
  if (desc_.workspace.preview != nullptr) desc_.workspace.preview->Pump();
  session_.Tick(nowNs);
  router_.Tick(runtime_);
  Declare(nowNs);
}

// --- the document --------------------------------------------------------------------------
std::string App::MixerSignature(bool withValues) const {
  const ProjectDocument* d = document();
  if (d == nullptr) return std::string();
  std::string s;
  for (const char* kind : {"bus", "category", "parameter"}) {
    for (const DocObject* o : d->OfKind(kind)) {
      s += o->guid;
      s += '|';
      s += o->name();
      s += '|';
      s += StringAt(o->doc, "parent");
      s += '|';
      s += StringAt(o->doc, "bank");
      if (const json::Value* fx = o->doc.Find("effects"); fx != nullptr && fx->IsArray()) {
        for (const json::Value& e : fx->items) {
          s += ',';
          s += StringAt(e, "type");
          if (withValues) {
            if (const json::Value* p = e.Find("params"); p != nullptr) s += json::Write(*p);
          }
        }
      }
      if (std::string(kind) == "parameter") {
        s += BoolAt(o->doc, "switch", false) ? "s" : "c";
        s += json::Write(o->doc.Find("scope") != nullptr ? *o->doc.Find("scope") : json::Value{});
      }
      if (withValues) {
        s += '|' + FormatNumber(NumberAt(o->doc, "volumeDb", 0.0));
        s += '|' + FormatNumber(NumberAt(o->doc, "min", 0.0)) + '|' + FormatNumber(NumberAt(o->doc, "max", 1.0)) + '|' +
             FormatNumber(NumberAt(o->doc, "default", 0.0));
      }
      s += '\n';
    }
  }
  return s;
}

void App::RebindProject() {
  const ProjectDocument* d = document();
  if (d == nullptr) return;
  Project p;
  p.Load(*d);
  p.SetBuildHash(session_.project().buildHash());
  session_.UseProject(p);
  mixerShape_ = MixerSignature(false);
  mixerValues_ = MixerSignature(true);
  staleMixer_ = false;
  view_.mixerPage = 0;
}

void App::TrackDocument() {
  ProjectDocument* d = document();
  if (d == nullptr) return;
  if (d->revision() != seenDocRevision_) {
    seenDocRevision_ = d->revision();
    dirty_ = d->IsDirty();
    const std::string shape = MixerSignature(false);
    const std::string values = MixerSignature(true);
    if (shape != mixerShape_) {
      RebindProject();  // new / removed buses, categories, parameters: the mixer must follow
    } else if (values != mixerValues_) {
      if (session_.mixer().Overrides().empty()) {
        RebindProject();
      } else if (!staleMixer_) {
        staleMixer_ = true;
        Message("mixer: project values changed under live overrides (write back or revert all to refresh)");
      }
    }
  }
  if (staleMixer_ && session_.mixer().Overrides().empty()) RebindProject();
  // Problems: the build's check after an edit, at most every 250 ms.
  if (problemsRevision_ != d->revision() && (nowNs_ - problemsAtNs_ >= 250000000ull || problemsRevision_ == ~0ull)) {
    RefreshProblems();
  }
}

void App::RefreshProblems() {
  const ProjectDocument* d = document();
  if (d == nullptr) return;
  problems_ = CheckDocument(*d);
  std::stable_sort(problems_.begin(), problems_.end(), [](const DocIssue& a, const DocIssue& b) {
    return a.error && !b.error;
  });
  problemsRevision_ = d->revision();
  problemsAtNs_ = nowNs_;
}

void App::Edited() {
  if (session_.live() != nullptr) session_.live()->FlushNow();
}

bool App::DocUndo() {
  ProjectDocument* d = document();
  if (d == nullptr || !d->Undo()) return false;
  Edited();
  return true;
}

bool App::DocRedo() {
  ProjectDocument* d = document();
  if (d == nullptr || !d->Redo()) return false;
  Edited();
  return true;
}

bool App::SaveDocument() {
  ProjectDocument* d = document();
  if (d == nullptr) {
    Message("save: no project document");
    return false;
  }
  if (text_.active()) text_.CommitNow();
  std::string error;
  if (!d->Save(&error)) {
    Message("save failed: " + error);
    dirty_ = d->IsDirty();
    return false;
  }
  dirty_ = d->IsDirty();
  Message("saved " + d->directory());
  return true;
}

std::uint32_t App::WriteBackMixer() {
  ProjectDocument* d = document();
  if (d == nullptr) return 0;
  const std::uint32_t n = snd::tool::WriteBackMixer(*d, session_.mixer());
  RebindProject();
  Edited();
  Message("wrote " + std::to_string(n) + " live values into the project");
  return n;
}

SpatialPreview* App::spatial() {
  const ProjectDocument* d = document();
  if (d == nullptr || view_.spatial.event.empty()) return nullptr;
  if (spatial_ == nullptr || spatialEvent_ != view_.spatial.event) {
    spatial_ = std::make_unique<SpatialPreview>(*d, view_.spatial.event);
    spatialEvent_ = view_.spatial.event;
    if (!spatial_->valid()) {
      spatial_.reset();
      spatialEvent_.clear();
      return nullptr;
    }
    spatial_->FitView(560.0f, 420.0f);
  }
  return spatial_.get();
}

void App::Navigate(const std::string& guid) {
  const ProjectDocument* d = document();
  const DocObject* o = d != nullptr ? d->Find(guid) : nullptr;
  if (o == nullptr) return;
  const std::string kind = o->kind();
  const std::string g = o->guid;
  if (kind == "event") {
    view_.event.event = g;
    view_.event.clip = -1;
    view_.event.track = 0;
    OnScreen(o->doc.Find("music") != nullptr ? Screen::Music : Screen::Event);
    if (o->doc.Find("music") != nullptr) {
      view_.music.event = g;
      view_.music.tab = 2;
    }
  } else if (kind == "bus") {
    view_.buses.bus = g;
    view_.buses.tab = 0;
    OnScreen(Screen::Buses);
  } else if (kind == "snapshot") {
    view_.buses.snapshot = g;
    view_.buses.tab = 1;
    OnScreen(Screen::Buses);
  } else if (kind == "ducking") {
    view_.buses.ducking = g;
    view_.buses.tab = 2;
    OnScreen(Screen::Buses);
  } else if (kind == "category" || kind == "voiceLimitGroup") {
    if (kind == "category") view_.buses.category = g;
    else view_.buses.group = g;
    view_.buses.tab = 3;
    OnScreen(Screen::Buses);
  } else if (kind == "parameter") {
    view_.params.parameter = g;
    OnScreen(Screen::Params);
  } else if (kind == "segment") {
    view_.music.segment = g;
    view_.music.tab = 0;
    OnScreen(Screen::Music);
  } else if (kind == "playlist") {
    view_.music.playlist = g;
    view_.music.tab = 1;
    OnScreen(Screen::Music);
  } else if (kind == "materialTable") {
    view_.material.table = g;
    OnScreen(Screen::Material);
  } else if (kind == "patch") {
    view_.patch.patch = g;
    OnScreen(Screen::Patch);
  } else {
    Message(kind + " " + o->name() + ": no editor screen (edit the file)");
  }
}

// --- text entry and pickers -------------------------------------------------------------------
void App::BeginText(const std::string& field, const std::string& initial, TextEntry::Commit commit, bool numeric) {
  picker_.reset();
  text_.Begin(field, initial, std::move(commit), numeric);
}

void App::OpenPicker(Picker picker) {
  text_.Cancel();
  picker_ = std::move(picker);
}

void App::Pick(const std::string& value) {
  if (!picker_) return;
  // Copy the value first: it may be an item of the list that closes below (D-187).
  const std::string chosen = value;
  PickFn fn = std::move(picker_->pick);
  picker_.reset();
  if (fn) fn(chosen);
  Edited();
}

void App::PickerPage(std::int32_t delta) {
  if (!picker_) return;
  const std::int64_t p = static_cast<std::int64_t>(picker_->page) + delta;
  picker_->page = p < 0 ? 0u : static_cast<std::uint32_t>(p);  // clamped when declared
}

// --- keys -----------------------------------------------------------------------------------
bool App::Key(const KeyEvent& k) {
  const bool ctrl = (k.modifiers & kModControl) != 0u;
  const bool shift = (k.modifiers & kModShift) != 0u;
  if (text_.active()) {
    bool refused = false;
    if (text_.Key(k, &refused)) {
      if (refused) Message("refused: " + text_.text());
      else Edited();
      return true;
    }
  }
  if (picker_ && k.key == kKeyEscape) {
    picker_.reset();
    return true;
  }
  const bool mixerLive = view_.screen == Screen::Mixer;
  if (ctrl && k.key == 'Z') {
    if (shift) {
      mixerLive ? OnRedo() : (void)DocRedo();
    } else {
      mixerLive ? OnUndo() : (void)DocUndo();
    }
    return true;
  }
  if (ctrl && k.key == 'Y') {
    mixerLive ? OnRedo() : (void)DocRedo();
    return true;
  }
  if (ctrl && k.key == 'S') {
    SaveDocument();
    return true;
  }
  if (ctrl && (k.key == kKeyPageDown || k.key == kKeyPageUp || k.key == kKeyTab)) {
    std::uint32_t at = 0;
    for (std::uint32_t i = 0; i < kScreenCount; ++i) {
      if (kTabOrder[i] == view_.screen) at = i;
    }
    const bool back = k.key == kKeyPageUp || (k.key == kKeyTab && shift);
    at = back ? (at + kScreenCount - 1u) % kScreenCount : (at + 1u) % kScreenCount;
    OnScreen(kTabOrder[at]);
    return true;
  }
  if (!ctrl && k.key >= kKeyF1 + 1u && k.key <= kKeyF1 + 9u) {  // F2 .. F10
    static constexpr Screen kF[] = {Screen::Mixer, Screen::Profiler, Screen::Buses, Screen::Event, Screen::Params,
                                    Screen::Spatial, Screen::Music, Screen::Material, Screen::Patch};
    OnScreen(kF[k.key - kKeyF1 - 1u]);
    return true;
  }
  if ((k.key == kKeyDelete || k.key == kKeyBackspace) && !ctrl && deleteAction_) {
    const std::function<void()> fn = deleteAction_;
    fn();
    Edited();
    return true;
  }
  if (k.key == kKeyEscape) {
    view_.patch.pendingFrom.clear();
    return false;
  }
  return false;
}

// --- declaration -----------------------------------------------------------------------------
void App::Declare(std::uint64_t nowNs) {
  nowNs_ = nowNs;
  deleteAction_ = nullptr;
  TrackDocument();
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
  ViewContext ctx{session_, palette_, view_, *this, runtime_, controls, nowNs, desc_.recordingPath, scale, this};
  tela::Document doc;
  Panel(doc, "root", 0, inset, w, h, [&] {
    Panel(doc, "header", 0, 0, w, kHeaderHeight, [&] { DeclareHeader(doc, ctx, w); });
    const float bodyH = h - kHeaderHeight - kFooterHeight;
    Panel(doc, "body", 0, kHeaderHeight, w, bodyH, [&] {
      const float bw = w - 12.0f, bh = bodyH - 12.0f;
      switch (view_.screen) {
        case Screen::Mixer: DeclareMixer(doc, ctx, bw, bh); break;
        case Screen::Profiler: DeclareProfiler(doc, ctx, bw, bh); break;
        case Screen::Buses: DeclareBuses(doc, ctx, bw, bh); break;
        case Screen::Event: DeclareEvent(doc, ctx, bw, bh); break;
        case Screen::Params: DeclareParams(doc, ctx, bw, bh); break;
        case Screen::Spatial: DeclareSpatial(doc, ctx, bw, bh); break;
        case Screen::Music: DeclareMusic(doc, ctx, bw, bh); break;
        case Screen::Material: DeclareMaterial(doc, ctx, bw, bh); break;
        case Screen::Patch: DeclarePatch(doc, ctx, bw, bh); break;
        case Screen::Replay: DeclareReplay(doc, ctx, bw, bh); break;
        case Screen::Link: DeclareLink(doc, ctx, bw, bh); break;
        case Screen::Problems: DeclareProblems(doc, ctx, bw, bh); break;
      }
    }, 6.0f);
    Panel(doc, "footer", 0, h - kFooterHeight, w, kFooterHeight, [&] { DeclareFooter(doc, ctx, w); });
    if (picker_) DeclarePicker(doc, ctx, w, h);
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
  // With a merge key (wheel) both changes carry it, so the series stays one step.
  if (t.control == Control::ParamOverride && m.Get(Target{Control::ParamOverrideOn, t.a, 0, 0}) == 0.0f) {
    if (mergeKey == 0u) m.BeginGroup();
    m.Set(t, value, mergeKey);
    m.Set(Target{Control::ParamOverrideOn, t.a, 0, 0}, 1.0f, mergeKey);
    if (mergeKey == 0u) m.EndGroup();
    return;
  }
  m.Set(t, value, mergeKey);
}
void App::ControlEnd(const Target&) { session_.mixer().EndGroup(); }
void App::ControlReset(const Target& t) {
  MixerModel& m = session_.mixer();
  if (t.control == Control::ParamOverride) {  // release the override (as its Revert)
    OverrideItem it;
    it.target = Target{Control::ParamOverrideOn, t.a, 0, 0};
    m.Revert(it);
    return;
  }
  m.BeginGroup();
  m.Set(t, m.BankValue(t));
  m.EndGroup();
}

// --- Actions ---------------------------------------------------------------------------------
void App::OnScreen(Screen s) {
  if (s != view_.screen) {
    text_.Cancel();
    picker_.reset();
  }
  view_.screen = s;
}
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
