// Building blocks of the editor screens: the one way every screen takes input (tool/README.md
// "画面と操作の仕様 / 段階5 の画面").
//
//   Action      a button that runs a function (a model edit or a selection).
//   NameField   a name: a click starts the tool's text entry (text_entry.hpp) with the current
//               text; Enter commits through the model's setter (refused: the entry stays open).
//   NumberField [-] value [+]: the buttons step, a vertical drag on the value changes it
//               (kNumberDragPixels per step, Shift 1/10; one undo step per drag; Esc takes it
//               back), the wheel steps (one undo step per series), a click without motion types the
//               value (as a name). Steps add to the model's latest value (several between two
//               declarations count each); the buttons, the drag and the wheel stop at the field's
//               range; a typed value goes to the model unchanged (refused: the entry stays open).
//               The value is at least kMinNumberValue wide (the label shrinks, then moves into
//               the value's text; a field too narrow for the buttons has none).
//   PickField   a reference or a choice: a click opens the candidate list (App::OpenPicker) over
//               the screen; picking one calls the setter; outside the list closes it.
//   Toggle      an on / off button ("[x] label").
//   Pager       "<" ">" and "page a / b" of a list that does not fit; returns the rows of the page.
//   Hit         the exclusive button under a drawn item (clip, point, node ...) with its
//               DragHandler (pointer_router.hpp); the drawing is a passthrough canvas above it.
// Every element is positioned (relative to the current panel) and its id is the caller's (stable:
// built from GUIDs and member keys). Nothing is declared once the document holds kMaxElements.
#ifndef SND_TOOL_UI_KIT_HPP
#define SND_TOOL_UI_KIT_HPP

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <tela/document.hpp>

#include "snd/tool/ui/pointer_router.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {

struct PickerItem {
  std::string label;
  std::string value;
};
using PickerItems = std::function<std::vector<PickerItem>()>;
using PickFn = std::function<void(const std::string& value)>;

// An open candidate list (App::OpenPicker).
struct Picker {
  std::string field;  // the element that opened it
  std::string title;
  std::vector<PickerItem> items;
  PickFn pick;
  std::uint32_t page = 0;
};

// The narrowest a number field's value gets (D-187: readable and pressable).
inline constexpr float kMinNumberValue = 48.0f;

struct NumberSpec {
  double value = 0.0;
  double step = 1.0;  // a button click / wheel notch / kNumberDragPixels of drag
  double minValue = -std::numeric_limits<double>::max();
  double maxValue = std::numeric_limits<double>::max();
  int decimals = 3;
  std::string unit;
  bool integer = false;
  bool enabled = true;
  bool document = true;  // a drag is one document step (BeginGroup / EndGroup)
};
// Sets the value: false if the model refused it. `mergeKey` != 0 for wheel series.
using NumberSetter = std::function<bool(double value, std::uint32_t mergeKey)>;

class Kit {
 public:
  Kit(tela::Document& doc, ViewContext& ctx);

  App& app() { return *ctx_.app; }
  ViewContext& ctx() { return ctx_; }
  tela::Document& doc() { return doc_; }
  const Palette& palette() const { return ctx_.palette; }

  void Label(const std::string& id, const std::string& text, float x, float y, float w, float h = 20.0f,
             float scale = 0.6f);
  void Title(const std::string& id, const std::string& text, float x, float y, float w);
  // enabled = false: the label as text (greyed by its "(off)" suffix when `why` is given).
  void Action(const std::string& id, const std::string& label, std::function<void()> fn, float x, float y, float w,
              float h = 22.0f, bool enabled = true);
  void NameField(const std::string& id, const std::string& label, const std::string& value,
                 std::function<bool(const std::string&)> commit, float x, float y, float w, float labelW = 80.0f);
  void NumberField(const std::string& id, const std::string& label, const NumberSpec& spec, NumberSetter set, float x,
                   float y, float w, float labelW = 80.0f);
  void PickField(const std::string& id, const std::string& label, const std::string& current, const std::string& title,
                 PickerItems items, PickFn pick, float x, float y, float w, float labelW = 80.0f);
  void Toggle(const std::string& id, const std::string& label, bool on, std::function<void()> fn, float x, float y,
              float w, bool enabled = true);
  // Rows per page `perPage` (>= 1) of `total`: clamps *page and returns the first row.
  std::uint32_t Pager(const std::string& id, std::uint32_t total, std::uint32_t perPage, std::uint32_t* page, float x,
                      float y, float w);
  void Hit(const std::string& id, std::shared_ptr<DragHandler> handler, float x, float y, float w, float h);
  // A row of tabs (selected one in brackets); returns nothing, calls `select` with the index.
  void Tabs(const std::string& id, const std::vector<std::string>& labels, std::uint32_t selected,
            std::function<void(std::uint32_t)> select, float x, float y, float tabW);

 private:
  tela::Document& doc_;
  ViewContext& ctx_;
};

// --- numbers to the model's types without undefined behaviour (D-187) ------------------------
// A typed or dragged value as a whole number of the type: false (refused, nothing converted) when it
// is not finite, not whole, negative or beyond the type.
bool WholeU16(double v, std::uint16_t* out);
bool WholeU32(double v, std::uint32_t* out);
bool WholeU64(double v, std::uint64_t* out);
// A value as a float: false when it is not finite or beyond the float's range.
bool ToF32(double v, float* out);
// A number read from a file (any double) shown or used as an integer: clamped into the type, the
// default when not finite.
std::uint32_t SatU32(double v, std::uint32_t def = 0);
std::uint64_t SatU64(double v, std::uint64_t def = 0);
int SatInt(double v, int def = 0);
// A number for display: "-" when not finite (D-189: every shown value is finite).
std::string ShownNumber(double v, int decimals, const std::string& unit = {});

// A label's text with every non-finite number ("inf", "-nan" as printf writes them) shown as "-".
// Kit::Label passes every label through it.
std::string FiniteText(const std::string& text);

// Display text of a number with its unit.
std::string NumberText(double v, int decimals, const std::string& unit);
// A short label (at most n bytes, cut at a UTF-8 boundary, "~" appended when cut).
std::string Clip(const std::string& s, std::size_t n);

// A drag handler that changes a document value by a 2D motion as one undo step: the first motion
// opens a group on the App's document, release closes it and flushes the live link.
// `apply(motion)` performs the model edit for the motion (from the press).
std::shared_ptr<DragHandler> DocumentDrag(App& app, std::function<void(float x, float y, std::uint32_t mods)> press,
                                          std::function<void(const DragMotion&)> apply,
                                          std::function<void(bool moved, float x, float y)> release = {});

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_KIT_HPP
