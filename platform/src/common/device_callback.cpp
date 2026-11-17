#include "common/device_callback.hpp"

namespace snd::platform::detail {

const void*& CurrentCallbackDevice() noexcept {
  thread_local const void* current = nullptr;
  return current;
}

}  // namespace snd::platform::detail
