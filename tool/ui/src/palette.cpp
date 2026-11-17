// Colours derived from the Tela theme.
#include "snd/tool/ui/palette.hpp"

#include <algorithm>
#include <cmath>

namespace snd::tool::ui {

tela::Color Mix(tela::Color a, tela::Color b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  auto m = [t](unsigned char x, unsigned char y) {
    return static_cast<unsigned char>(std::lround(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t));
  };
  return tela::Color{m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

tela::Color WithAlpha(tela::Color c, unsigned char a) {
  c.a = a;
  return c;
}

Palette MakePalette(const tela::Theme& theme) {
  Palette p;
  p.theme = theme;
  const tela::Color panel = WithAlpha(theme.panel, 255);
  p.canvas = Mix(panel, tela::Color{0, 0, 0, 255}, 0.35f);
  p.track = Mix(panel, WithAlpha(theme.button, 255), 0.6f);
  p.accent = WithAlpha(theme.pressed, 255);
  p.accentSoft = Mix(WithAlpha(theme.text, 255), p.canvas, 0.45f);
  p.text = WithAlpha(theme.text, 255);
  p.textDim = Mix(p.text, p.canvas, 0.55f);
  auto hue = [&](tela::Color c) { return Mix(c, panel, 0.12f); };
  p.meterLow = hue(tela::Color{64, 200, 110, 255});
  p.meterMid = hue(tela::Color{230, 200, 60, 255});
  p.meterHigh = hue(tela::Color{235, 70, 60, 255});
  p.rmsFill = Mix(p.meterLow, p.canvas, 0.25f);
  p.mute = hue(tela::Color{240, 170, 40, 255});
  p.solo = hue(tela::Color{70, 210, 120, 255});
  p.clip = hue(tela::Color{250, 40, 40, 255});
  p.pending = hue(tela::Color{240, 200, 70, 255});
  p.failed = hue(tela::Color{240, 60, 60, 255});
  p.budget = hue(tela::Color{235, 90, 70, 255});
  return p;
}

}  // namespace snd::tool::ui
