// Undo / redo of mixer edits.
#include "snd/tool/edit_history.hpp"

namespace snd::tool {
namespace {

void MergeInto(std::vector<Change>& changes, const Change& c) {
  for (Change& e : changes) {
    if (e.target == c.target) {
      e.after = c.after;
      return;
    }
  }
  changes.push_back(c);
}

}  // namespace

void EditHistory::BeginGroup() {
  if (depth_++ == 0u) open_ = Edit{};
}

void EditHistory::EndGroup() {
  if (depth_ == 0u) return;
  if (--depth_ != 0u) return;
  Edit e = std::move(open_);
  open_ = Edit{};
  // Changes that ended where they started are not an edit.
  std::vector<Change> kept;
  for (const Change& c : e.changes) {
    if (c.before != c.after) kept.push_back(c);
  }
  e.changes = std::move(kept);
  if (!e.changes.empty()) Commit(std::move(e));
  closedLast_ = true;
}

void EditHistory::Push(const Change& change, std::uint32_t mergeKey) {
  if (depth_ != 0u) {
    MergeInto(open_.changes, change);
    return;
  }
  if (mergeKey != 0u && !closedLast_ && !done_.empty() && done_.back().mergeKey == mergeKey) {
    Edit& last = done_.back();
    MergeInto(last.changes, change);
    // A series that came back to where it started is no edit (like a drag).
    bool any = false;
    for (const Change& c : last.changes) any = any || c.before != c.after;
    if (!any) done_.pop_back();
    undone_.clear();
    return;
  }
  Edit e;
  e.changes.push_back(change);
  e.mergeKey = mergeKey;
  Commit(std::move(e));
  closedLast_ = mergeKey == 0u;
}

void EditHistory::Commit(Edit e) {
  done_.push_back(std::move(e));
  undone_.clear();
  if (done_.size() > max_) done_.erase(done_.begin());
}

bool EditHistory::Undo(std::vector<Change>* apply) {
  apply->clear();
  if (depth_ != 0u || done_.empty()) return false;
  Edit e = std::move(done_.back());
  done_.pop_back();
  for (auto it = e.changes.rbegin(); it != e.changes.rend(); ++it) {
    apply->push_back(Change{it->target, it->after, it->before});
  }
  undone_.push_back(std::move(e));
  closedLast_ = true;
  return true;
}

bool EditHistory::Redo(std::vector<Change>* apply) {
  apply->clear();
  if (depth_ != 0u || undone_.empty()) return false;
  Edit e = std::move(undone_.back());
  undone_.pop_back();
  for (const Change& c : e.changes) apply->push_back(c);
  done_.push_back(std::move(e));
  closedLast_ = true;
  return true;
}

void EditHistory::Clear() {
  done_.clear();
  undone_.clear();
  open_ = Edit{};
  depth_ = 0;
  closedLast_ = true;
}

}  // namespace snd::tool
