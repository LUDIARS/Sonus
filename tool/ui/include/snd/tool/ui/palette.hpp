// Colours of the tool's controls, derived from the Tela theme (the theme is
// the single source: panel, text, button, pressed). Semantic colours (meter
// zones, mute, solo, clip, sync state) are fixed hues blended toward the
// theme's panel so they sit in any theme.
#ifndef SND_TOOL_UI_PALETTE_HPP
#define SND_TOOL_UI_PALETTE_HPP

#include <tela/geometry.hpp>

namespace snd::tool::ui {

struct Palette {
  tela::Theme theme;
  tela::Color canvas;      // control background (opaque)
  tela::Color track;       // fader track / knob ring / graph grid
  tela::Color accent;      // fader cap, knob value arc, graph series (theme.pressed)
  tela::Color accentSoft;  // second graph series
  tela::Color text;        // markers, peak hold (theme.text)
  tela::Color textDim;
  tela::Color meterLow, meterMid, meterHigh;  // below -18 dBFS, -18..-6, above -6
  tela::Color rmsFill;     // the RMS bar (meterLow tint)
  tela::Color mute, solo, clip, pending, failed, budget;
};

Palette MakePalette(const tela::Theme& theme);
tela::Color Mix(tela::Color a, tela::Color b, float t);  // a..b, alpha too
tela::Color WithAlpha(tela::Color c, unsigned char a);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_PALETTE_HPP
