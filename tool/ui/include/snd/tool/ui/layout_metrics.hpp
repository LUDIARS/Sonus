// Positions of the tool's screens (logical pixels) and positioned-element
// helpers. Every element is declared with an explicit position and size so the
// layout is exact and testable; ids are stable (built from runtime ids, never
// from list positions, except for the rows of the profiler's lists).
#ifndef SND_TOOL_UI_LAYOUT_METRICS_HPP
#define SND_TOOL_UI_LAYOUT_METRICS_HPP

#include <cstdint>
#include <functional>
#include <string>

#include <tela/document.hpp>

namespace snd::tool::ui {

inline constexpr float kHeaderHeight = 40.0f;
inline constexpr float kFooterHeight = 26.0f;
inline constexpr float kSidePanelWidth = 310.0f;
inline constexpr float kStripWidth = 92.0f;
inline constexpr float kStripGap = 6.0f;
inline constexpr float kPagerHeight = 30.0f;
inline constexpr std::size_t kMaxElements = 3800;  // Tela refuses more than 4096

struct StripMetrics {
  float width = kStripWidth;
  float height = 0.0f;
  float padding = 4.0f;
  float inner = kStripWidth - 8.0f;  // content width
  float nameHeight = 20.0f;
  float knobTop = 24.0f;
  float knobSize = 32.0f;
  float knobRow = 48.0f;
  std::uint32_t knobColumns = 2, knobRows = 3;
  float buttonsTop = 0.0f;
  float buttonWidth = 40.0f, buttonHeight = 22.0f;
  float faderTop = 0.0f;
  float faderWidth = 40.0f, faderHeight = 0.0f, capHeight = 18.0f;
  float meterLeft = 46.0f, meterWidth = 38.0f;
  float valueHeight = 18.0f;
  std::uint32_t knobSlots() const { return knobColumns * knobRows; }
};
// For a strip of `height` (the body height minus the pager).
StripMetrics ComputeStripMetrics(float height);
// Strips that fit in `width` (at least 1).
std::uint32_t StripsPerPage(float width);

// Positioned element helpers (relative to the parent's inner area).
tela::Layout At(float x, float y, float w, float h, float padding = 0.0f, float textScale = 1.0f);
void Text(tela::Document& doc, const std::string& id, const std::string& text, float x, float y, float w, float h,
          float textScale = 0.75f);
void Button(tela::Document& doc, const std::string& id, const std::string& label, std::function<void()> action,
            float x, float y, float w, float h, float textScale = 0.75f);
void Canvas(tela::Document& doc, const std::string& id, tela::Drawing drawing, float x, float y, float w, float h);
void Panel(tela::Document& doc, const std::string& id, float x, float y, float w, float h,
           const std::function<void()>& children, float padding = 0.0f);
bool Room(const tela::Document& doc, std::size_t more = 1);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_LAYOUT_METRICS_HPP
