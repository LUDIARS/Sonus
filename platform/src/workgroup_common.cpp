#include "snd/platform/workgroup.hpp"

namespace snd::platform {

const char* WorkgroupJoinResultName(WorkgroupJoinResult r) noexcept {
  switch (r) {
    case WorkgroupJoinResult::Joined: return "joined";
    case WorkgroupJoinResult::NoWorkgroup: return "no-workgroup";
    case WorkgroupJoinResult::Unsupported: return "unsupported";
    case WorkgroupJoinResult::Failed: return "failed";
  }
  return "?";
}

}  // namespace snd::platform
