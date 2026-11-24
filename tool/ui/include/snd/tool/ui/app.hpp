// The tool's UI: Tela runtime, screens and pointer routing over a Session.
//
// Frame(nowNs) runs one UI frame: Session::Tick (connection, value sync,
// profiler), then the declaration of the current screen into the Tela runtime
// (redeclared every frame: meters and graphs move). Pointer() takes host
// pointer events, Key() host keys. Everything runs on the UI thread; the live link
// client's network thread is only reached through the client's thread-safe calls.
//
// Stage 5 (the editors): with a document (Workspace::document) every editor screen edits it
// through the model's functions (one undo history across screens). Ctrl+Z / Ctrl+Y undo / redo
// the document on every screen but the mixer (there: the live values' own history, as before);
// Ctrl+S saves the document. After an edit that changes the mixer's buses, categories or
// parameters (or their project values while the mixer holds no override), the session's project
// is rebuilt from the document and the mixer is bound to it again (MixerModel::Bind).
#ifndef SND_TOOL_UI_APP_HPP
#define SND_TOOL_UI_APP_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <tela/runtime.hpp>

#include "snd/tool/doc_build.hpp"
#include "snd/tool/document.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/session.hpp"
#include "snd/tool/spatial_preview.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/palette.hpp"
#include "snd/tool/ui/pointer_router.hpp"
#include "snd/tool/ui/services.hpp"
#include "snd/tool/ui/text_entry.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {

// What the editor screens work on (all optional; a screen without what it needs says so).
struct Workspace {
  ProjectDocument* document = nullptr;  // the editors; should be attached to the session (AttachDocument)
  IPreviewEngine* preview = nullptr;    // the preview engine (Frame pumps it)
  AuditionRouter* audition = nullptr;   // auditions (3D, music transitions, events)
  ReplayService* replay = nullptr;      // replays and bug reports (services.hpp)
};

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
  Workspace workspace;
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
  // A key from the host (text_entry.hpp). true if used.
  bool Key(const KeyEvent& key);

  tela::Runtime& runtime() { return runtime_; }
  const tela::Runtime& runtime() const { return runtime_; }
  PointerRouter& router() { return router_; }
  Session& session() { return session_; }
  const Palette& palette() const { return palette_; }
  ViewState& view() { return view_; }
  const AppDesc& desc() const { return desc_; }
  std::uint64_t declarations() const { return declarations_; }

  // --- stage 5 ---------------------------------------------------------------------------
  const Workspace& workspace() const { return desc_.workspace; }
  ProjectDocument* document() { return desc_.workspace.document; }
  const ProjectDocument* document() const { return desc_.workspace.document; }
  // Text entry and candidate lists (kit.hpp).
  TextEntry& text() { return text_; }
  void BeginText(const std::string& field, const std::string& initial, TextEntry::Commit commit, bool numeric);
  void OpenPicker(Picker picker);
  const Picker* picker() const { return picker_ ? &*picker_ : nullptr; }
  void ClosePicker() { picker_.reset(); }
  void Pick(const std::string& value);
  void PickerPage(std::int32_t delta);
  // The document's history (every screen but the mixer's live values).
  bool DocUndo();
  bool DocRedo();
  bool SaveDocument();
  bool dirty() const { return dirty_; }  // unsaved document changes (refreshed each frame)
  // After a document edit made outside a drag (buttons, text): flushes the live link at once.
  void Edited();
  // The build's problems of the document (CheckDocument), refreshed at most every 250 ms after an
  // edit (RefreshProblems forces it).
  const std::vector<DocIssue>& problems() const { return problems_; }
  void RefreshProblems();
  // Selects an object in its editor and shows that screen (problems list, references).
  void Navigate(const std::string& guid);
  // The 3D preview of SpatialView::event (recreated when the event changes; null without one).
  SpatialPreview* spatial();
  // Writes the mixer's live values into the document (one step) and rebinds the mixer.
  std::uint32_t WriteBackMixer();
  // Rebuilds the session's project from the document now and rebinds the mixer, carrying its live
  // values over by object (MixerModel::Rebind). With values that have no place in the new project
  // (D-188) it does not rebind: it asks (rebindPending; ResolveRebind) unless `discardUnmatched`.
  void RebindProject();
  bool RebindProject(bool discardUnmatched);
  bool rebindPending() const { return rebindPending_; }
  const std::vector<LiveValue>& rebindDropped() const { return rebindDropped_; }
  // The answer: write the live values into the document, or discard the ones that cannot go over.
  void ResolveRebind(bool writeBack);
  void Message(const std::string& text) { session_.Message(text); }
  // The file the recording's Save / Load use, and the connection the Connect button opens.
  void SetRecordingPath(const std::string& path) { desc_.recordingPath = path; }
  ConnectionDesc& connectionDesc() { return desc_.connection; }
  // What Delete (and Backspace outside a text entry) does on the current screen: set by the
  // screen's declaration (the selected point, node, clip ...), cleared at each declaration.
  void SetDeleteAction(std::function<void()> fn) { deleteAction_ = std::move(fn); }
  // Number fields (kit.hpp): the latest value a field wrote since the last declaration (else the
  // declared one), so that steps between two declarations add up.
  double FieldBase(const std::string& field, double declared) const;
  void FieldWrote(const std::string& field, double value);
  void OnDocUndo() { DocUndo(); }
  void OnDocRedo() { DocRedo(); }

  // ControlSink
  float ControlValue(const Target& t) const override;
  float ControlDefault(const Target& t) const override;
  void ControlBegin(const Target& t) override;
  void ControlSet(const Target& t, float value, std::uint32_t mergeKey) override;
  void ControlEnd(const Target& t) override;
  void ControlReset(const Target& t) override;
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
  void TrackDocument();
  void ClearPositionSelections();
  std::string MixerSignature(bool withValues) const;
  Session& session_;
  AppDesc desc_;
  Palette palette_;
  tela::Runtime runtime_;
  PointerRouter router_;
  ViewState view_;
  std::uint64_t revision_ = 0;
  std::uint64_t declarations_ = 0;
  std::uint64_t nowNs_ = 0;
  // stage 5
  TextEntry text_;
  std::optional<Picker> picker_;
  std::uint64_t seenDocRevision_ = ~0ull;
  std::uint64_t problemsRevision_ = ~0ull;
  std::uint64_t problemsAtNs_ = 0;
  std::vector<DocIssue> problems_;
  bool dirty_ = false;
  std::string mixerShape_, mixerValues_;
  bool staleMixer_ = false;
  bool rebindPending_ = false;
  std::vector<LiveValue> rebindDropped_;
  std::uint64_t rebindMixerRevision_ = 0;
  std::function<void()> deleteAction_;
  std::map<std::string, std::pair<std::uint64_t, double>> fieldLatest_;  // field -> (declaration, value)
  std::unique_ptr<SpatialPreview> spatial_;
  std::string spatialEvent_;
};

// A tela::Theme is valid for Runtime::theme (finite font metrics in range).
bool ValidTheme(const tela::Theme& t);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_APP_HPP
