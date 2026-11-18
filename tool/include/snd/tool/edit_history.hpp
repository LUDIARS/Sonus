// Undo / redo of mixer edits (design.md 11.1 ミキサー).
//
// An edit is a list of changes (target, value before, value after). Changes
// pushed between BeginGroup and EndGroup form one edit (a fader drag, "revert
// all"); inside a group, changes of the same target merge (first `before`,
// last `after`). Outside a group, a change with a non-zero merge key merges
// into the previous edit when that edit has the same key and nothing else came between
// (wheel notches on one control, with what the first notch turned on); changes of the
// same target merge, and a series that ends where it started leaves no edit. Undo returns the changes to apply in
// reverse order with before/after swapped; a new edit clears the redo list.
#ifndef SND_TOOL_EDIT_HISTORY_HPP
#define SND_TOOL_EDIT_HISTORY_HPP

#include <cstdint>
#include <vector>

namespace snd::tool {

enum class Control : std::uint8_t {
  BusFader = 0,         // a = bus index; value dB (kFaderOffDb = -inf)
  BusMute = 1,          // value 0 / 1
  BusSolo = 2,          // value 0 / 1
  EffectParam = 3,      // a = bus, b = effect, c = parameter
  CategoryFader = 4,    // a = category index
  CategoryMute = 5,
  ParamOverride = 6,    // a = parameter index; value of the override
  ParamOverrideOn = 7,  // a = parameter index; 0 / 1
};

struct Target {
  Control control = Control::BusFader;
  std::int32_t a = 0, b = 0, c = 0;
  bool operator==(const Target&) const = default;
};

struct Change {
  Target target;
  float before = 0.0f;
  float after = 0.0f;
};

class EditHistory {
 public:
  explicit EditHistory(std::uint32_t maxEdits = 512) : max_(maxEdits) {}

  void BeginGroup();
  void EndGroup();
  bool InGroup() const { return depth_ != 0u; }
  void Push(const Change& change, std::uint32_t mergeKey = 0);

  // Changes to apply, in order (already reversed / swapped for undo).
  bool Undo(std::vector<Change>* apply);
  bool Redo(std::vector<Change>* apply);
  bool CanUndo() const { return !done_.empty(); }
  bool CanRedo() const { return !undone_.empty(); }
  std::size_t UndoCount() const { return done_.size(); }
  std::size_t RedoCount() const { return undone_.size(); }
  void Clear();

 private:
  struct Edit {
    std::vector<Change> changes;
    std::uint32_t mergeKey = 0;
  };
  void Commit(Edit e);
  std::uint32_t max_;
  std::uint32_t depth_ = 0;
  Edit open_;
  bool closedLast_ = true;  // the last edit may no longer merge (a group or another key ended it)
  std::vector<Edit> done_;
  std::vector<Edit> undone_;
};

}  // namespace snd::tool

#endif  // SND_TOOL_EDIT_HISTORY_HPP
