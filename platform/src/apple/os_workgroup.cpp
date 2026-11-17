// Apple audio workgroups (D-65): the os_workgroup_t of the output device's
// IO thread (macOS 11+ / iOS 14+). Joining makes the calling thread part of
// the device's real-time workgroup, so the scheduler sees the audio thread
// and the RT workers as one piece of work with the IO deadline.
#include <os/object.h>
#include <os/workgroup.h>

#include <cstring>
#include <new>

#include "snd/platform/workgroup.hpp"

namespace snd::platform {

static_assert(sizeof(os_workgroup_join_token_s) <= 64, "WorkgroupMembership::token_ too small");

WorkgroupMembership::~WorkgroupMembership() { Leave(); }

WorkgroupJoinResult WorkgroupMembership::Join(const Workgroup& group) noexcept {
  Leave();
  if (group.kind != WorkgroupKind::AppleOsWorkgroup || group.native == nullptr) return WorkgroupJoinResult::NoWorkgroup;
  if (__builtin_available(macOS 11.0, iOS 14.0, tvOS 14.0, *)) {
    auto wg = static_cast<os_workgroup_t>(group.native);
    os_retain(wg);  // keep it alive while joined, whatever the device does
    auto* token = ::new (static_cast<void*>(token_)) os_workgroup_join_token_s();
    std::memset(token, 0, sizeof(*token));
    if (os_workgroup_join(wg, token) != 0) {  // EINVAL: cancelled, EALREADY: already joined
      os_release(wg);
      return WorkgroupJoinResult::Failed;
    }
    native_ = wg;
    kind_ = WorkgroupKind::AppleOsWorkgroup;
    joined_ = true;
    return WorkgroupJoinResult::Joined;
  }
  return WorkgroupJoinResult::Unsupported;
}

void WorkgroupMembership::Leave() noexcept {
  if (joined_ && native_ != nullptr) {
    if (__builtin_available(macOS 11.0, iOS 14.0, tvOS 14.0, *)) {
      auto wg = static_cast<os_workgroup_t>(native_);
      os_workgroup_leave(wg, reinterpret_cast<os_workgroup_join_token_s*>(token_));
      os_release(wg);
    }
  }
  native_ = nullptr;
  kind_ = WorkgroupKind::None;
  joined_ = false;
}

}  // namespace snd::platform
