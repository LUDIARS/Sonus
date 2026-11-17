// The tool's UI: Tela runtime, screens and pointer routing over a Session.
//
// Frame(nowNs) runs one UI frame: Session::Tick (connection, value sync,
// profiler), then the declaration of the current screen into the Tela runtime
// (redeclared every frame: meters and graphs move). Pointer() takes host
// pointer events. Everything runs on the UI thread; the live link client's
// network thread is only reached through the client's thread-safe calls.
#ifndef SND_TOOL_UI_APP_HPP
#define SND_TOOL_UI_APP_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <tela/runtime.hpp>

#include "snd/tool/session.hpp"
#include "snd/tool/ui/palette.hpp"
#include "snd/tool/ui/pointer_router.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {

struct AppDesc {
  int width = 1280;  // physical pixels (headless viewport)
  int height = 800;
  float dpiScale = 1.0f;
  tela::Theme theme{};
  // The host's Tela adapter (WindowsView / MacOSOverlay) reports the viewport and feeds
  // pointer events to the runtime itself.
  bool hostOwnsViewport = false;
  ConnectionDesc connection;          // used by the Connect button
  std::string recordingPath = "sonus-recording.snrec";
  float insetTop = 0.0f;  // logical pixels left free at the top (macOS: the title bar under Tela's overlay)
};

class App final : public ControlSink, public Actions {
 public:
  App(Session& session, const AppDesc& desc);
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  void Frame(std::uint64_t nowNs);
  // Re-declares without ticking the session (tests, after an input event).
  void Declare(std::uint64_t nowNs);
  // Headless viewport (physical pixels). Ignored when the host owns the viewport.
  void SetViewport(int width, int height, float dpiScale);
  void SetInsetTop(float logicalPixels);
  // `forward`: also hand the event to the Tela runtime (headless, tests).
  bool Pointer(const tela::HostPointerEvent& event, std::uint64_t timeMs, bool forward);

  tela::Runtime& runtime() { return runtime_; }
  const tela::Runtime& runtime() const { return runtime_; }
  PointerRouter& router() { return router_; }
  Session& session() { return session_; }
  const Palette& palette() const { return palette_; }
  ViewState& view() { return view_; }
  const AppDesc& desc() const { return desc_; }
  std::uint64_t declarations() const { return declarations_; }

  // ControlSink
  float ControlValue(const Target& t) const override;
  float ControlDefault(const Target& t) const override;
  void ControlBegin(const Target& t) override;
  void ControlSet(const Target& t, float value, std::uint32_t mergeKey) override;
  void ControlEnd(const Target& t) override;
  // Actions
  void OnScreen(Screen s) override;
  void OnConnectToggle() override;
  void OnUndo() override;
  void OnRedo() override;
  void OnToggle(const Target& t) override;
  void OnRevert(const std::string& key) override;
  void OnRevertAll() override;
  void OnPage(std::int32_t delta) override;
  void OnRecordToggle() override;
  void OnSaveRecording() override;
  void OnLoadRecording() override;
  void OnResetClips() override;

 private:
  Session& session_;
  AppDesc desc_;
  Palette palette_;
  tela::Runtime runtime_;
  PointerRouter router_;
  ViewState view_;
  std::uint64_t revision_ = 0;
  std::uint64_t declarations_ = 0;
  std::uint64_t nowNs_ = 0;
};

// A tela::Theme is valid for Runtime::theme (finite font metrics in range).
bool ValidTheme(const tela::Theme& t);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_APP_HPP
