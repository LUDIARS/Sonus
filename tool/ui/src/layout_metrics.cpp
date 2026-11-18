// Screen positions and positioned-element helpers.
#include "snd/tool/ui/layout_metrics.hpp"

#include <algorithm>
#include <cmath>

namespace snd::tool::ui {
namespace {

float Dim(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 32000.0f) : 0.0f; }
float Pos(float v) { return std::isfinite(v) ? std::clamp(v, -32000.0f, 32000.0f) : 0.0f; }

}  // namespace

StripMetrics ComputeStripMetrics(float height) {
  StripMetrics m;
  m.height = std::max(height, 260.0f);
  m.buttonsTop = m.knobTop + static_cast<float>(m.knobRows) * m.knobRow + 4.0f;
  m.faderTop = m.buttonsTop + m.buttonHeight + 6.0f;
  const float content = m.height - 2.0f * m.padding;
  m.faderHeight = std::max(60.0f, content - m.faderTop - m.valueHeight - 4.0f);
  return m;
}

std::uint32_t StripsPerPage(float width) {
  const float n = std::floor((width + kStripGap) / (kStripWidth + kStripGap));
  return n < 1.0f ? 1u : static_cast<std::uint32_t>(n);
}

tela::Layout At(float x, float y, float w, float h, float padding, float textScale) {
  tela::Layout l;
  l.positioned = true;
  l.x = Pos(x);
  l.y = Pos(y);
  l.width = Dim(w);
  l.height = Dim(h);
  l.padding = Dim(padding);
  l.gap = 0.0f;
  l.text_scale = std::isfinite(textScale) && textScale > 0.0f ? std::min(textScale, 8.0f) : 1.0f;
  return l;
}

bool Room(const tela::Document& doc, std::size_t more) { return doc.elements().size() + more <= kMaxElements; }

void Text(tela::Document& doc, const std::string& id, const std::string& text, float x, float y, float w, float h,
          float textScale) {
  if (!Room(doc)) return;
  doc.text(id, text.size() > 1024u ? text.substr(0, 1024) : text, At(x, y, w, h, 2.0f, textScale));
}

void Button(tela::Document& doc, const std::string& id, const std::string& label, std::function<void()> action,
            float x, float y, float w, float h, float textScale) {
  if (!Room(doc)) return;
  doc.button(id, label, std::move(action), At(x, y, w, h, 3.0f, textScale));
}

void Canvas(tela::Document& doc, const std::string& id, tela::Drawing drawing, float x, float y, float w, float h) {
  if (!Room(doc)) return;
  doc.canvas(id, std::move(drawing), At(x, y, w, h));
}

void Panel(tela::Document& doc, const std::string& id, float x, float y, float w, float h,
           const std::function<void()>& children, float padding) {
  if (!Room(doc)) return;
  tela::Layout l = At(x, y, w, h, padding);
  doc.panel(id, children, l);
}

}  // namespace snd::tool::ui
