// Declarations of the tool's screens (design.md 11.1). Each function adds positioned elements
// to the current Tela panel; actions call back into Actions or the App (editors: through the
// model's functions, so every edit is one undoable document step), drawn controls are
// registered in `controls`.
#ifndef SND_TOOL_UI_VIEWS_HPP
#define SND_TOOL_UI_VIEWS_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <tela/document.hpp>
#include <tela/runtime.hpp>

#include "snd/tool/curve_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/session.hpp"
#include "snd/tool/ui/palette.hpp"
#include "snd/tool/ui/pointer_router.hpp"

namespace snd::tool {
struct ReplayRun;  // recording_report.hpp
}

namespace snd::tool::ui {

class App;

// Screens. Mixer and Profiler keep their stage-2 numbers.
enum class Screen : std::uint8_t {
  Mixer = 0,
  Profiler = 1,
  Buses = 2,     // mixer structure: buses, sends, effects, snapshots, ducking, categories
  Event = 3,
  Params = 4,    // parameters and curves
  Spatial = 5,   // 3D attenuation preview
  Music = 6,
  Material = 7,
  Patch = 8,
  Replay = 9,    // recording and replay
  Link = 10,     // connection, matching, live reflection
  Problems = 11,
};
inline constexpr std::uint32_t kScreenCount = 12;
// The header's tab order.
inline constexpr Screen kTabOrder[kScreenCount] = {Screen::Mixer,   Screen::Buses,    Screen::Event,    Screen::Params,
                                                   Screen::Spatial, Screen::Music,    Screen::Material, Screen::Patch,
                                                   Screen::Profiler, Screen::Replay,  Screen::Link,     Screen::Problems};
const char* ScreenName(Screen s);   // "mixer", "buses", ... (--screen)
const char* ScreenLabel(Screen s);  // tab text
bool ParseScreen(const std::string& name, Screen* out);

// --- per screen state (selection, paging, view) -------------------------------------------
struct EventView {
  std::string event;          // GUID
  std::uint32_t track = 0;
  std::int32_t clip = -1;     // in `track`
  std::int32_t action = -1;   // in `track`
  double scrollMs = 0.0;      // timeline: left edge
  double msPerPx = 4.0;       // timeline: zoom
  std::uint32_t trackTop = 0; // first track shown
  std::uint32_t listPage = 0;
  std::uint32_t actionPage = 0;
  std::string addAsset;       // asset GUID the "+ clip" button adds
};
struct BusesView {
  std::uint32_t tab = 0;      // 0 buses, 1 snapshots, 2 ducking, 3 categories
  std::string bus, snapshot, ducking, category, group;
  std::uint32_t listPage = 0, effectPage = 0, valuePage = 0, resultPage = 0;
  std::map<std::string, double> weights;  // snapshot GUID -> preview weight
};
struct ParamsView {
  std::string parameter;      // GUID
  std::string event;          // curves of this event
  std::string curve;          // member path ("curves[0]", "distanceCurves.volume")
  std::int32_t point = -1;
  std::uint32_t listPage = 0, curvePage = 0, resultPage = 0;
  std::map<std::string, double> values;     // parameter GUID -> value (composition check)
  std::map<std::string, std::uint32_t> scopes;  // parameter GUID -> ParamScope bit the value is set at
  double distance = 10.0;
};
struct SpatialView {
  std::string event;
  std::uint32_t listPage = 0;
  std::uint32_t outputs = 2;
  bool playing = false;
  std::uint32_t curve = 0;    // distance curve shown (0 volume .. 4 blend3D)
};
struct MusicView {
  std::uint32_t tab = 0;      // 0 segments, 1 playlists, 2 states and rules, 3 stingers
  std::string segment, playlist, event;
  std::int32_t tempo = -1, marker = -1, stem = -1, entry = -1, rule = -1, stinger = -1;
  std::uint32_t listPage = 0, rulePage = 0, rowPage = 0, lookPage = 0;
  std::string from, to;       // transition preview states
  double requestSeconds = 1.0;
  MusicSettings settings;
  TapTempo tap;
  double tapBpm = 0.0;
  double gridOffsetMs = 0.0;
};
struct MaterialView {
  std::string table;
  std::string material;
  std::uint32_t listPage = 0, pairRow = 0, pairCol = 0, fallbackPage = 0;
  std::uint32_t kind = 0;     // ContactKind of the resolution shown
  std::uint32_t a = 0, b = 0; // materials of the resolution shown
  double impulse = 5.0, massKg = 1.0, speed = 1.0;
  std::string renumbered;     // the last renumbering, as text
};
struct PatchView {
  std::string patch;
  std::string node;
  std::string pendingFrom;    // "connect from" chosen, waiting for an input
  float scrollX = 0.0f, scrollY = 0.0f;
  std::uint32_t listPage = 0, paramPage = 0;
};
struct ProfilerViewState {
  std::uint32_t tab = 0;      // 0 overview (stage 2), 1 CPU, 2 memory, 3 streams, 4 LOD, 5 log
  std::uint32_t page = 0;
  bool logCommands = true, logEvents = true, logEnds = true;
  std::uint32_t logId = 0;
};
struct ReplayViewState {
  std::uint32_t page = 0;
  std::string title = "bug report";
  std::string notes;
  std::string reportDir = "sonus-report";
  std::string wavPath = "sonus-replay.wav";
  std::vector<std::string> result;  // the last replay / comparison / report, line by line
  std::shared_ptr<ReplayRun> lastRun;  // the first replay of the last comparison (WAV, listening)
};
struct LinkViewState {
  std::uint32_t tab = 0;      // 0 matching, 1 items, 2 problems
  std::uint32_t page = 0;
};

struct ViewState {
  Screen screen = Screen::Mixer;
  std::uint32_t mixerPage = 0;
  EventView event;
  BusesView buses;
  ParamsView params;
  SpatialView spatial;
  MusicView music;
  MaterialView material;
  PatchView patch;
  ProfilerViewState profiler;
  ReplayViewState replay;
  LinkViewState link;
  std::uint32_t problemsPage = 0;
};

class Actions {
 public:
  virtual ~Actions() = default;
  virtual void OnScreen(Screen s) = 0;
  virtual void OnConnectToggle() = 0;
  virtual void OnUndo() = 0;
  virtual void OnRedo() = 0;
  virtual void OnToggle(const Target& t) = 0;  // mute / solo / category mute / parameter override
  virtual void OnRevert(const std::string& overrideKey) = 0;
  virtual void OnRevertAll() = 0;
  virtual void OnPage(std::int32_t delta) = 0;
  virtual void OnRecordToggle() = 0;
  virtual void OnSaveRecording() = 0;
  virtual void OnLoadRecording() = 0;
  virtual void OnResetClips() = 0;
};

struct ViewContext {
  const Session& session;
  const Palette& palette;
  const ViewState& state;
  Actions& actions;
  const tela::Runtime& runtime;  // pressed states of the previous frame
  std::vector<ControlBinding>& controls;
  std::uint64_t nowNs = 0;
  std::string recordingPath;     // where Save writes / Load reads
  float dpi = 1.0f;              // the viewport's DPI scale (drawing budgets)
  App* app = nullptr;            // the editors' access to the document and the UI state
};

bool IsPressed(const tela::Runtime& runtime, const std::string& id);

void DeclareHeader(tela::Document& doc, ViewContext& ctx, float width);
void DeclareFooter(tela::Document& doc, ViewContext& ctx, float width);
void DeclareMixer(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclareProfiler(tela::Document& doc, ViewContext& ctx, float width, float height);
// Stage 5 screens (each needs ctx.app; without a document they say so).
void DeclareBuses(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclareEvent(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclareParams(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclareSpatial(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclareMusic(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclareMaterial(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclarePatch(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclareReplay(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclareLink(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclareProblems(tela::Document& doc, ViewContext& ctx, float width, float height);
// The overlay of an open candidate list (App::OpenPicker), above everything.
void DeclarePicker(tela::Document& doc, ViewContext& ctx, float width, float height);

// Stable element ids (tests and hosts): "bus." / "cat." / "par." + the object's key
// (BusDesc::key). These helpers give the id of an object whose runtime id is unique in
// its project (every project that builds); objects that share a name get GUID keys.
std::string BusId(std::uint32_t busRuntimeId);       // "bus.0a1b2c3d"
std::string CategoryId(std::uint32_t categoryId);    // "cat.0a1b2c3d"
std::string ParameterId(std::uint32_t parameterId);  // "par.0a1b2c3d"
// Element id part of a document object: its GUID without dashes (32 hex digits).
std::string ObjectKey(const std::string& guid);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_VIEWS_HPP
