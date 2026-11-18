// Windows "workgroup" = the MMCSS task of the WASAPI device thread (D-65).
// Joining = registering the calling thread with the same task index.
#include <cstring>

#include "snd/platform/workgroup.hpp"
#include "windows/windows_headers.hpp"

namespace snd::platform {

WorkgroupMembership::~WorkgroupMembership() { Leave(); }

WorkgroupJoinResult WorkgroupMembership::Join(const Workgroup& group) noexcept {
  Leave();
  if (group.kind != WorkgroupKind::WindowsMmcss || group.taskIndex == 0u) return WorkgroupJoinResult::NoWorkgroup;
  DWORD task = group.taskIndex;
  HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  if (h == nullptr) return WorkgroupJoinResult::Failed;
  native_ = h;
  kind_ = WorkgroupKind::WindowsMmcss;
  joined_ = true;
  return WorkgroupJoinResult::Joined;
}

void WorkgroupMembership::Leave() noexcept {
  std::memset(token_, 0, sizeof(token_));  // only Apple keeps a join token
  if (joined_ && native_ != nullptr) AvRevertMmThreadCharacteristics(static_cast<HANDLE>(native_));
  native_ = nullptr;
  kind_ = WorkgroupKind::None;
  joined_ = false;
}

}  // namespace snd::platform
