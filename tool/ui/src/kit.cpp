// Building blocks of the editor screens.
#include "snd/tool/ui/kit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/control_math.hpp"
#include "snd/tool/ui/layout_metrics.hpp"

namespace snd::tool::ui {

bool WholeU16(double v, std::uint16_t* out) {
  if (!std::isfinite(v) || v != std::floor(v) || v < 0.0 || v > 65535.0) return false;
  *out = static_cast<std::uint16_t>(v);
  return true;
}
bool WholeU32(double v, std::uint32_t* out) {
  if (!std::isfinite(v) || v != std::floor(v) || v < 0.0 || v > 4294967295.0) return false;
  *out = static_cast<std::uint32_t>(v);
  return true;
}
bool WholeU64(double v, std::uint64_t* out) {
  if (!std::isfinite(v) || v != std::floor(v) || v < 0.0 || v >= 18446744073709551616.0) return false;
  *out = static_cast<std::uint64_t>(v);
  return true;
}
bool ToF32(double v, float* out) {
  if (!std::isfinite(v) || std::fabs(v) > static_cast<double>(std::numeric_limits<float>::max())) return false;
  *out = static_cast<float>(v);
  return true;
}
std::uint32_t SatU32(double v, std::uint32_t def) {
  if (!std::isfinite(v)) return def;
  if (v <= 0.0) return 0u;
  if (v >= 4294967295.0) return 4294967295u;
  return static_cast<std::uint32_t>(v);
}
std::uint64_t SatU64(double v, std::uint64_t def) {
  if (!std::isfinite(v)) return def;
  if (v <= 0.0) return 0u;
  if (v >= 18446744073709549568.0) return 18446744073709549568ull;
  return static_cast<std::uint64_t>(v);
}
int SatInt(double v, int def) {
  if (!std::isfinite(v)) return def;
  if (v <= -2147483648.0) return std::numeric_limits<int>::min();
  if (v >= 2147483647.0) return std::numeric_limits<int>::max();
  return static_cast<int>(v);
}
std::string ShownNumber(double v, int decimals, const std::string& unit) {
  if (!std::isfinite(v)) return "-";
  return NumberText(v, decimals, unit);
}

std::string NumberText(double v, int decimals, const std::string& unit) {
  if (!std::isfinite(v)) return "-";  // D-189: no "inf" / "nan" on the screen
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

int VsnprintfFinite(char* out, std::size_t size, const char* fmt, std::va_list args) {
  std::string r;
  for (const char* p = fmt; *p != '\0';) {
    if (*p != '%') {
      r += *p++;
      continue;
    }
    // One conversion: % flags width .precision length conversion.
    const char* start = p++;
    if (*p == '%') {
      r += '%';
      ++p;
      continue;
    }
    int stars[2] = {0, 0};
    int nstars = 0;
    while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') ++p;
    if (*p == '*') {
      stars[nstars++] = va_arg(args, int);
      ++p;
    }
    while (*p >= '0' && *p <= '9') ++p;
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        stars[nstars++] = va_arg(args, int);
        ++p;
      }
      while (*p >= '0' && *p <= '9') ++p;
    }
    std::string len;
    while (*p == 'h' || *p == 'l' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') len += *p++;
    const char conv = *p;
    if (conv == '\0') break;
    ++p;
    const std::string spec(start, static_cast<std::size_t>(p - start));
    char b[512];
    b[0] = '\0';
    auto put = [&](auto v) {
      if (nstars == 2) std::snprintf(b, sizeof b, spec.c_str(), stars[0], stars[1], v);
      else if (nstars == 1) std::snprintf(b, sizeof b, spec.c_str(), stars[0], v);
      else std::snprintf(b, sizeof b, spec.c_str(), v);
    };
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
#endif
    switch (conv) {
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        const double v = len == "L" ? static_cast<double>(va_arg(args, long double)) : va_arg(args, double);
        if (!std::isfinite(v)) {
          r += '-';
          continue;
        }
        if (len == "L") std::snprintf(b, sizeof b, "%g", v);
        else put(v);
        break;
      }
      case 's': put(va_arg(args, const char*)); break;
      case 'c': put(va_arg(args, int)); break;
      case 'p': put(va_arg(args, void*)); break;
      case 'd': case 'i':
        if (len == "ll") put(va_arg(args, long long));
        else if (len == "l") put(va_arg(args, long));
        else if (len == "z" || len == "t") put(va_arg(args, std::ptrdiff_t));
        else if (len == "j") put(va_arg(args, std::intmax_t));
        else put(va_arg(args, int));
        break;
      case 'u': case 'o': case 'x': case 'X':
        if (len == "ll") put(va_arg(args, unsigned long long));
        else if (len == "l") put(va_arg(args, unsigned long));
        else if (len == "z" || len == "t") put(va_arg(args, std::size_t));
        else if (len == "j") put(va_arg(args, std::uintmax_t));
        else put(va_arg(args, unsigned));
        break;
      default: r += spec; break;  // not a conversion the tool uses: as written
    }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    r += b;
  }
  if (size > 0u) {
    const std::size_t n = std::min(r.size(), size - 1u);
    std::memcpy(out, r.data(), n);
    out[n] = '\0';
  }
  return static_cast<int>(r.size());
}

int SnprintfFinite(char* out, std::size_t size, const char* fmt, ...) {
  std::va_list args;
  va_start(args, fmt);
  const int n = VsnprintfFinite(out, size, fmt, args);
  va_end(args);
  return n;
}

void Kit::Label(const std::string& id, const std::string& text, float x, float y, float w, float h, float scale) {
  Text(doc_, id, text, x, y, w, h, scale);  // names as they are; numbers are made finite where they become text
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
  // Room (D-187): the value keeps at least kMinNumberValue px. The label gives way first; when it
  // would get less than 24 px it goes into the value's text. A field narrower than the buttons need
  // has no buttons (the value still drags, wheels and types).
  constexpr float kButtons = 44.0f;
  const float minField = kButtons + kMinNumberValue;
  float lw = label.empty() ? 0.0f : std::min(labelW, std::max(0.0f, w - minField));
  const bool inlineLabel = !label.empty() && lw < 24.0f;
  if (inlineLabel) lw = 0.0f;
  if (lw > 0.0f) Text(doc_, id + ".label", label, x, y, lw, 22.0f, 0.55f);
  const float fx = x + lw;
  const float fw = std::max(kMinNumberValue, w - lw);
  const bool buttons = fw >= minField;
  App* app = ctx_.app;
  const TextEntry& te = app->text();
  const bool editing = te.active() && te.field() == id;
  std::string text = editing ? te.text() + "_" : NumberText(spec.value, spec.decimals, spec.unit);
  if (inlineLabel && !editing) text = label + " " + text;
  if (!spec.enabled) {
    Text(doc_, id + ".value", "(" + text + ")", fx, y, fw, 22.0f, 0.55f);
    return;
  }
  const NumberSpec sp = spec;
  // A typed value goes to the model as typed: the model refuses what breaks its rules and the entry
  // then stays open (D-187; the field's own range only bounds the buttons, the drag and the wheel).
  auto commitText = [set, integer = sp.integer](const std::string& t) {
    double v = 0.0;
    if (!ParseNumber(t, &v)) return false;
    if (integer && v != std::floor(v)) return false;  // D-187: an integer field takes whole numbers only
    return set(v, 0u);
  };
  // Steps (buttons, wheel) start from the model's latest value: several of them between two
  // declarations add up (D-187).
  auto step = [set, sp, app, id](double delta, std::uint32_t key) {
    const double base = app->FieldBase(id, sp.value);
    const double v = ClampNumber(base + delta, sp.minValue, sp.maxValue, sp.integer);
    if (!set(v, key)) return;
    app->FieldWrote(id, v);
    app->Edited();
  };
  const float vx = buttons ? fx + 22.0f : fx;
  const float vw = buttons ? fw - kButtons : fw;
  if (buttons) Button(doc_, id + ".dec", "-", [step, sp] { step(-sp.step, 0u); }, fx, y, 20.0f, 22.0f, 0.6f);
  // The value: a drag hit with the text above it.
  struct State {
    double anchor = 0.0;
    float up = 0.0f;
    bool grouped = false;
  };
  auto st = std::make_shared<State>();
  auto h = std::make_shared<DragHandler>();
  h->press = [st, sp, app, id](float, float, std::uint32_t) {
    st->anchor = app->FieldBase(id, sp.value);
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
  h->cancel = [st, sp, set, app] {  // Esc during the drag: as if it never happened (D-186)
    if (!st->grouped) return;
    st->grouped = false;
    if (sp.document && app->document() != nullptr) {
      app->document()->AbortGroup();
    } else {
      set(st->anchor, 0u);  // a value outside the document: back to where the drag started
    }
    app->Edited();
  };
  h->wheel = [step, sp](float notches, std::uint32_t mods, std::uint32_t key) {
    step(sp.step * ((mods & kModShift) != 0u ? kFineFactor : 1.0) * notches, key);
  };
  Hit(id, h, vx, y, vw, 22.0f);
  Text(doc_, id + ".value", text, vx, y, vw, 22.0f, 0.55f);
  if (buttons) Button(doc_, id + ".inc", "+", [step, sp] { step(sp.step, 0u); }, fx + fw - 20.0f, y, 20.0f, 22.0f, 0.6f);
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
  h->cancel = [a, grouped] {  // Esc during the drag: discarded, no step, the history as before (D-186)
    if (!*grouped) return;
    *grouped = false;
    if (a->document() != nullptr) a->document()->AbortGroup();
    a->Edited();
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
