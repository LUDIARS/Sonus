// Drawings of the tool's controls (Tela has panel / text / button / canvas
// only; faders, knobs, meters and graphs are drawn here on canvases). Every
// function returns the drawing in the canvas's local logical coordinates and,
// where a test needs it, the geometry it used (control_math.hpp rules).
#ifndef SND_TOOL_UI_WIDGETS_HPP
#define SND_TOOL_UI_WIDGETS_HPP

#include <cstdint>
#include <vector>

#include <tela/drawing.hpp>

#include "snd/tool/ui/control_math.hpp"
#include "snd/tool/ui/palette.hpp"

namespace snd::tool::ui {

struct FaderLook {
  float width = 40.0f;
  float height = 300.0f;
  float capHeight = 18.0f;
  float pos = 0.75f;        // FaderDbToPos(value)
  float bankPos = 0.75f;    // marker of the bank value
  bool pressed = false;
  bool silenced = false;    // muted or solo-muted: the cap is drawn dimmed
};
struct FaderShape {
  float capTop = 0.0f, capBottom = 0.0f, capCenter = 0.0f;
};
tela::Drawing DrawFader(const FaderLook& look, const Palette& p, FaderShape* shape = nullptr);

struct KnobLook {
  float size = 36.0f;
  float norm = 0.0f;
  float bankNorm = 0.0f;
  bool editable = true;
  bool pressed = false;
};
struct KnobShape {
  tela::Point center{};
  tela::Point pointerEnd{};  // tip of the value pointer
  float radius = 0.0f;
};
tela::Drawing DrawKnob(const KnobLook& look, const Palette& p, KnobShape* shape = nullptr);

struct MeterLook {
  float width = 30.0f;
  float height = 300.0f;
  std::uint32_t channels = 2;
  float peakDb[8] = {};  // dBFS
  float rmsDb[8] = {};
  float holdDb[8] = {};
  bool clipped = false;
  bool live = true;      // false: no meter data (drawn empty)
};
struct MeterShape {
  struct Bar {
    float x = 0.0f, width = 0.0f;
    float peakTop = 0.0f, rmsTop = 0.0f, holdY = 0.0f;  // local y (0 = top)
  };
  std::vector<Bar> bars;
};
tela::Drawing DrawMeter(const MeterLook& look, const Palette& p, MeterShape* shape = nullptr);
tela::Color MeterZoneColor(float db, const Palette& p);

struct GraphLook {
  float width = 600.0f;
  float height = 200.0f;
  std::vector<float> total;   // logic + render per block (us), oldest first
  std::vector<float> render;  // render only
  float budget = 0.0f;        // us
};
struct GraphShape {
  GraphScale scale;
  float budgetY = 0.0f;
  std::vector<tela::Point> totalPoints;
};
inline constexpr std::uint32_t kGraphMaxPoints = 240;
tela::Drawing DrawTimingGraph(const GraphLook& look, const Palette& p, GraphShape* shape = nullptr);

// A small state light (mute / solo / sync state): filled with `on` or the track colour.
tela::Drawing DrawLight(float width, float height, bool lit, tela::Color on, const Palette& p);
// A horizontal usage bar: fraction of the width filled (accent, or budget colour above 1).
tela::Drawing DrawUsageBar(float width, float height, float fraction, const Palette& p);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_WIDGETS_HPP
