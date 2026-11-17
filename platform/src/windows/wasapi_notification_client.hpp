// IMMNotificationClient for the WASAPI device (D-63). The OS calls it on its
// own notification thread; it only sets flags and signals the device
// thread's event (it must not block or call back into the enumerator). The
// device thread does all COM work. The object is embedded in the device and
// never deleted through Release (reference count kept for COM's sake).
#ifndef SND_PLATFORM_SRC_WINDOWS_WASAPI_NOTIFICATION_CLIENT_HPP
#define SND_PLATFORM_SRC_WINDOWS_WASAPI_NOTIFICATION_CLIENT_HPP

#include <atomic>

#include "windows/windows_headers.hpp"

namespace snd::platform::detail::wasapi {

class NotificationClient final : public IMMNotificationClient {
 public:
  NotificationClient() noexcept = default;
  virtual ~NotificationClient() = default;
  NotificationClient(const NotificationClient&) = delete;
  NotificationClient& operator=(const NotificationClient&) = delete;

  void SetWakeEvent(HANDLE event) noexcept { wake_.store(event, std::memory_order_release); }
  // Device thread: true once per change since the last call.
  bool TakeDefaultChanged() noexcept { return defaultChanged_.exchange(false, std::memory_order_acq_rel); }
  bool TakeStateChanged() noexcept { return stateChanged_.exchange(false, std::memory_order_acq_rel); }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  // IMMNotificationClient
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override;
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR deviceId) override;
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override;
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR defaultDeviceId) override;
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key) override;

 private:
  void Signal() noexcept;
  std::atomic<ULONG> refs_{1};
  std::atomic<bool> defaultChanged_{false};
  std::atomic<bool> stateChanged_{false};
  std::atomic<HANDLE> wake_{nullptr};
};

}  // namespace snd::platform::detail::wasapi

#endif  // SND_PLATFORM_SRC_WINDOWS_WASAPI_NOTIFICATION_CLIENT_HPP
