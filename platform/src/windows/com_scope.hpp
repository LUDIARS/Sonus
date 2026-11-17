// CoInitializeEx(MTA) for the calling thread, undone at scope exit when it
// succeeded (S_OK or S_FALSE). A thread already in an STA (RPC_E_CHANGED_MODE)
// keeps its apartment; COM still works for the calls made here. Non-RT.
#ifndef SND_PLATFORM_SRC_WINDOWS_COM_SCOPE_HPP
#define SND_PLATFORM_SRC_WINDOWS_COM_SCOPE_HPP

#include "windows/windows_headers.hpp"

namespace snd::platform::detail::wasapi {

class ComScope {
 public:
  ComScope() noexcept : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  bool Usable() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

 private:
  HRESULT hr_;
};

}  // namespace snd::platform::detail::wasapi

#endif  // SND_PLATFORM_SRC_WINDOWS_COM_SCOPE_HPP
