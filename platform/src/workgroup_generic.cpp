// Workgroups on OSes without an audio workgroup API (Linux and others): the
// RT threads share SCHED_FIFO at the same priority instead (thread.hpp).
#include <cstring>

#include "snd/platform/workgroup.hpp"

namespace snd::platform {

WorkgroupMembership::~WorkgroupMembership() { Leave(); }

WorkgroupJoinResult WorkgroupMembership::Join(const Workgroup& group) noexcept {
  Leave();
  return group.Valid() ? WorkgroupJoinResult::Unsupported : WorkgroupJoinResult::NoWorkgroup;
}

void WorkgroupMembership::Leave() noexcept {
  std::memset(token_, 0, sizeof(token_));  // only Apple keeps a join token
  joined_ = false;
  kind_ = WorkgroupKind::None;
  native_ = nullptr;
}

}  // namespace snd::platform
