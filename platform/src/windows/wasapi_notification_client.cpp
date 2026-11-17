#include "windows/wasapi_notification_client.hpp"

#include "windows/wasapi_guids.hpp"

namespace snd::platform::detail::wasapi {

HRESULT STDMETHODCALLTYPE NotificationClient::QueryInterface(REFIID riid, void** out) {
  if (out == nullptr) return E_POINTER;
  if (IsEqualIID(riid, kIidIUnknown) || IsEqualIID(riid, kIidIMMNotificationClient)) {
    *out = static_cast<IMMNotificationClient*>(this);
    AddRef();
    return S_OK;
  }
  *out = nullptr;
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE NotificationClient::AddRef() { return refs_.fetch_add(1, std::memory_order_relaxed) + 1u; }

ULONG STDMETHODCALLTYPE NotificationClient::Release() {
  // Owned by the device object: reaching 0 does not delete.
  return refs_.fetch_sub(1, std::memory_order_acq_rel) - 1u;
}

void NotificationClient::Signal() noexcept {
  HANDLE e = wake_.load(std::memory_order_acquire);
  if (e != nullptr) SetEvent(e);
}

HRESULT STDMETHODCALLTYPE NotificationClient::OnDeviceStateChanged(LPCWSTR, DWORD) {
  stateChanged_.store(true, std::memory_order_release);
  Signal();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE NotificationClient::OnDeviceAdded(LPCWSTR) {
  stateChanged_.store(true, std::memory_order_release);  // a lost device may be back
  Signal();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE NotificationClient::OnDeviceRemoved(LPCWSTR) {
  stateChanged_.store(true, std::memory_order_release);
  Signal();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE NotificationClient::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) {
  // Games follow the console role (eConsole), like GetDefaultAudioEndpoint(eRender, eConsole).
  if (flow == eRender && role == eConsole) {
    defaultChanged_.store(true, std::memory_order_release);
    Signal();
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE NotificationClient::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) {
  // Format changes in the control panel invalidate the stream
  // (AUDCLNT_E_DEVICE_INVALIDATED), which the device thread handles.
  return S_OK;
}

}  // namespace snd::platform::detail::wasapi
