// Workgroups on OSes without an audio workgroup API (Linux and others): the
// RT threads share SCHED_FIFO at the same priority instead (thread.hpp).
#include "snd/platform/workgroup.hpp"

namespace snd::platform {

WorkgroupMembership::~WorkgroupMembership() { Leave(); }

WorkgroupJoinResult WorkgroupMembership::Join(const Workgroup& group) noexcept {
  Leave();
  return group.Valid() ? WorkgroupJoinResult::Unsupported : WorkgroupJoinResult::NoWorkgroup;
}

void WorkgroupMembership::Leave() noexcept {
  joined_ = false;
  kind_ = WorkgroupKind::None;
  native_ = nullptr;
}

}  // namespace snd::platform
