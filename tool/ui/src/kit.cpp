// Building blocks of the editor screens.
#include "snd/tool/ui/kit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/control_math.hpp"
#include "snd/tool/ui/layout_metrics.hpp"

namespace snd::tool::ui {

std::string NumberText(double v, int decimals, const std::string& unit) {
  std::string s = FormatNumberText(v, decimals);
  if (!unit.empty()) s += " " + unit;
  return s;
}

std::string Clip(const std::string& s, std::size_t n) {
  if (s.size() <= n) return s;
  std::size_t cut = n;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0u) == 0x80u) --cut;  // UTF-8 boundary
  return s.substr(0, cut) + "~";
}

Kit::Kit(tela::Document& doc, ViewContext& ctx) : doc_(doc), ctx_(ctx) {}

void Kit::Label(const std::string& id, const std::string& text, float x, float y, float w, float h, float scale) {
  Text(doc_, id, text, x, y, w, h, scale);
}

void Kit::Title(const std::string& id, const std::string& text, float x, float y, float w) {
  Text(doc_, id, text, x, y, w, 22.0f, 0.72f);
}

void Kit::Action(const std::string& id, const std::string& label, std::function<void()> fn, float x, float y, float w,
                 float h, bool enabled) {
  if (!enabled) {
    Text(doc_, id, "(" + label + ")", x, y, w, h, 0.55f);
    return;
  }
  Button(doc_, id, label, std::move(fn), x, y, w, h, 0.55f);
}

void Kit::NameField(const std::string& id, const std::string& label, const std::string& value,
                    std::function<bool(const std::string&)> commit, float x, float y, float w, float labelW) {
  if (!label.empty()) Text(doc_, id + ".label", label, x, y, labelW, 22.0f, 0.55f);
  const float fx = label.empty() ? x : x + labelW;
  App* app = ctx_.app;
  const TextEntry& te = app->text();
  const bool editing = te.active() && te.field() == id;
  const std::string shown = editing ? te.text() + "_" : (value.empty() ? std::string("-") : value);
  Button(doc_, id, Clip(shown, 64), [app, id, value, commit] { app->BeginText(id, value, commit, false); }, fx, y,
         std::max(10.0f, w - (fx - x)), 22.0f, 0.55f);
}

void Kit::NumberField(const std::string& id, const std::string& label, const NumberSpec& spec, NumberSetter set,
                      float x, float y, float w, float labelW) {
  if (!label.empty()) Text(doc_, id + ".label", label, x, y, labelW, 22.0f, 0.55f);
  const float fx = label.empty() ? x : x + labelW;
  const float fw = std::max(40.0f, w - (fx - x));
  App* app = ctx_.app;
  const TextEntry& te = app->text();
  const bool editing = te.active() && te.field() == id;
  const std::string text = editing ? te.text() + "_" : NumberText(spec.value, spec.decimals, spec.unit);
  if (!spec.enabled) {
    Text(doc_, id + ".value", "(" + text + ")", fx, y, fw, 22.0f, 0.55f);
    return;
  }
  const NumberSpec sp = spec;
  auto commitText = [set, sp](const std::string& t) {
    double v = 0.0;
    if (!ParseNumber(t, &v)) return false;
    return set(ClampNumber(v, sp.minValue, sp.maxValue, sp.integer), 0u);
  };
  Button(doc_, id + ".dec", "-",
         [set, sp, app] {
           if (set(ClampNumber(sp.value - sp.step, sp.minValue, sp.maxValue, sp.integer), 0u)) app->Edited();
         },
         fx, y, 20.0f, 22.0f, 0.6f);
  // The value: a drag hit with the text above it.
  struct State {
    double anchor = 0.0;
    float up = 0.0f;
    bool grouped = false;
  };
  auto st = std::make_shared<State>();
  auto h = std::make_shared<DragHandler>();
  h->press = [st, sp](float, float, std::uint32_t) {
    st->anchor = sp.value;
    st->up = 0.0f;
  };
  h->drag = [st, sp, set, app](const DragMotion& m) {
    if (!st->grouped && sp.document && app->document() != nullptr) app->document()->BeginGroup();
    st->grouped = true;
    st->up += -m.ddy * ((m.modifiers & kModShift) != 0u ? kFineFactor : 1.0f);
    set(NumberDrag(st->anchor, st->up, sp.step, sp.minValue, sp.maxValue, sp.integer), 0u);
  };
  h->release = [st, sp, app, id, commitText](bool moved, float, float) {
    if (st->grouped) {
      if (sp.document && app->document() != nullptr) app->document()->EndGroup();
      st->grouped = false;
      app->Edited();
    }
    if (!moved) app->BeginText(id, FormatNumberText(sp.value, std::max(sp.decimals, 6)), commitText, true);
  };
  h->wheel = [sp, set, app](float notches, std::uint32_t mods, std::uint32_t key) {
    const double step = sp.step * ((mods & kModShift) != 0u ? kFineFactor : 1.0);
    if (set(ClampNumber(sp.value + step * notches, sp.minValue, sp.maxValue, sp.integer), key)) app->Edited();
  };
  Hit(id, h, fx + 22.0f, y, fw - 44.0f, 22.0f);
  Text(doc_, id + ".value", text, fx + 22.0f, y, fw - 44.0f, 22.0f, 0.55f);
  Button(doc_, id + ".inc", "+",
         [set, sp, app] {
           if (set(ClampNumber(sp.value + sp.step, sp.minValue, sp.maxValue, sp.integer), 0u)) app->Edited();
         },
         fx + fw - 20.0f, y, 20.0f, 22.0f, 0.6f);
}

void Kit::PickField(const std::string& id, const std::string& label, const std::string& current,
                    const std::string& title, PickerItems items, PickFn pick, float x, float y, float w,
                    float labelW) {
  if (!label.empty()) Text(doc_, id + ".label", label, x, y, labelW, 22.0f, 0.55f);
  const float fx = label.empty() ? x : x + labelW;
  App* app = ctx_.app;
  Button(doc_, id, Clip(current.empty() ? std::string("-") : current, 48) + " v",
         [app, id, title, items, pick] {
           Picker p;
           p.field = id;
           p.title = title;
           if (items) p.items = items();
           p.pick = pick;
           app->OpenPicker(std::move(p));
         },
         fx, y, std::max(10.0f, w - (fx - x)), 22.0f, 0.55f);
}

void Kit::Toggle(const std::string& id, const std::string& label, bool on, std::function<void()> fn, float x, float y,
                 float w, bool enabled) {
  const std::string text = (on ? "[x] " : "[ ] ") + label;
  if (!enabled) {
    Text(doc_, id, "(" + text + ")", x, y, w, 22.0f, 0.55f);
    return;
  }
  App* app = ctx_.app;
  Button(doc_, id, text, [fn, app] {
    fn();
    app->Edited();
  }, x, y, w, 22.0f, 0.55f);
}

std::uint32_t Kit::Pager(const std::string& id, std::uint32_t total, std::uint32_t perPage, std::uint32_t* page,
                         float x, float y, float w) {
  perPage = std::max(1u, perPage);
  const std::uint32_t pages = std::max(1u, (total + perPage - 1u) / perPage);
  if (*page >= pages) *page = pages - 1u;
  if (pages <= 1u) return 0;
  std::uint32_t* p = page;
  Button(doc_, id + ".prev", "<", [p] {
    if (*p > 0u) --*p;
  }, x, y, 24.0f, 20.0f, 0.6f);
  Button(doc_, id + ".next", ">", [p, pages] {
    if (*p + 1u < pages) ++*p;
  }, x + 28.0f, y, 24.0f, 20.0f, 0.6f);
  char b[48];
  std::snprintf(b, sizeof b, "%u / %u", *page + 1u, pages);
  Text(doc_, id + ".page", b, x + 56.0f, y, std::max(10.0f, w - 56.0f), 20.0f, 0.5f);
  return *page * perPage;
}

void Kit::Hit(const std::string& id, std::shared_ptr<DragHandler> handler, float x, float y, float w, float h) {
  if (!Room(doc_)) return;
  Button(doc_, id, std::string(), {}, x, y, w, h);
  ControlBinding b;
  b.hitId = id;
  b.kind = ControlKind::Custom;
  b.handler = std::move(handler);
  ctx_.controls.push_back(std::move(b));
}

void Kit::Tabs(const std::string& id, const std::vector<std::string>& labels, std::uint32_t selected,
               std::function<void(std::uint32_t)> select, float x, float y, float tabW) {
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const std::uint32_t k = static_cast<std::uint32_t>(i);
    Button(doc_, id + "." + std::to_string(i), k == selected ? "[" + labels[i] + "]" : labels[i],
           [select, k] { select(k); }, x + static_cast<float>(i) * (tabW + 4.0f), y, tabW, 24.0f, 0.6f);
  }
}

std::shared_ptr<DragHandler> DocumentDrag(App& app, std::function<void(float, float, std::uint32_t)> press,
                                          std::function<void(const DragMotion&)> apply,
                                          std::function<void(bool, float, float)> release) {
  auto grouped = std::make_shared<bool>(false);
  App* a = &app;
  auto h = std::make_shared<DragHandler>();
  h->press = std::move(press);
  h->drag = [a, grouped, apply](const DragMotion& m) {
    if (!*grouped && a->document() != nullptr) a->document()->BeginGroup();
    *grouped = true;
    if (apply) apply(m);
  };
  h->release = [a, grouped, release](bool moved, float x, float y) {
    if (release) release(moved, x, y);  // inside the group: a release edit is part of the step
    if (*grouped) {
      if (a->document() != nullptr) a->document()->EndGroup();
      *grouped = false;
      a->Edited();
    }
  };
  return h;
}

}  // namespace snd::tool::ui
