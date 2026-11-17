// Declarations of the tool's screens (header and status, mixer, profiler).
// Each function adds positioned elements to the current Tela panel; actions
// call back into Actions, drawn controls are registered in `controls`.
#ifndef SND_TOOL_UI_VIEWS_HPP
#define SND_TOOL_UI_VIEWS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <tela/document.hpp>
#include <tela/runtime.hpp>

#include "snd/tool/session.hpp"
#include "snd/tool/ui/palette.hpp"
#include "snd/tool/ui/pointer_router.hpp"

namespace snd::tool::ui {

enum class Screen : std::uint8_t { Mixer = 0, Profiler = 1 };

struct ViewState {
  Screen screen = Screen::Mixer;
  std::uint32_t mixerPage = 0;
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
};

bool IsPressed(const tela::Runtime& runtime, const std::string& id);

void DeclareHeader(tela::Document& doc, ViewContext& ctx, float width);
void DeclareFooter(tela::Document& doc, ViewContext& ctx, float width);
void DeclareMixer(tela::Document& doc, ViewContext& ctx, float width, float height);
void DeclareProfiler(tela::Document& doc, ViewContext& ctx, float width, float height);

// Stable element ids (tests and hosts).
std::string BusId(std::uint32_t busRuntimeId);       // "bus.0a1b2c3d"
std::string CategoryId(std::uint32_t categoryId);    // "cat.0a1b2c3d"
std::string ParameterId(std::uint32_t parameterId);  // "par.0a1b2c3d"

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_VIEWS_HPP
