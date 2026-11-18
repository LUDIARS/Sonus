// WASAPI device enumeration (D-64): active render endpoints in the order of
// IMMDeviceEnumerator::EnumAudioEndpoints; id = IMMDevice::GetId (UTF-8).
#include <cstring>

#include "backends.hpp"
#include "common/owned_string.hpp"
#include "windows/com_ptr.hpp"
#include "windows/com_scope.hpp"
#include "windows/wasapi_guids.hpp"
#include "windows/wide_string.hpp"

namespace snd::platform::detail {

using wasapi::ComPtr;

std::uint32_t EnumerateWasapiDevices(OutputDeviceInfo* out, std::uint32_t capacity) noexcept {
  wasapi::ComScope com;
  if (!com.Usable()) return 0;
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(wasapi::kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, wasapi::kIidIMMDeviceEnumerator,
                              enumerator.PutVoid()))) {
    return 0;
  }
  wchar_t defaultId[512] = {};
  {
    ComPtr<IMMDevice> def;
    LPWSTR id = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, def.Put())) && SUCCEEDED(def->GetId(&id))) {
      wcsncpy(defaultId, id, 511);
      CoTaskMemFree(id);
    }
  }
  ComPtr<IMMDeviceCollection> list;
  if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, list.Put()))) return 0;
  UINT count = 0;
  if (FAILED(list->GetCount(&count))) return 0;
  for (UINT i = 0; i < count && i < capacity; ++i) {
    OutputDeviceInfo& info = out[i];
    info = OutputDeviceInfo{};
    ComPtr<IMMDevice> dev;
    if (FAILED(list->Item(i, dev.Put()))) continue;
    LPWSTR id = nullptr;
    if (SUCCEEDED(dev->GetId(&id))) {
      wasapi::WideToUtf8(id, info.id, sizeof(info.id));
      info.isDefault = defaultId[0] != L'\0' && wcscmp(id, defaultId) == 0;
      CoTaskMemFree(id);
    }
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.Put()))) {
      PROPVARIANT v;
      PropVariantInit(&v);
      if (SUCCEEDED(props->GetValue(wasapi::kPkeyDeviceFriendlyName, &v)) && v.vt == VT_LPWSTR) {
        wasapi::WideToUtf8(v.pwszVal, info.name, sizeof(info.name));
      }
      PropVariantClear(&v);
    }
    if (info.name[0] == '\0') CopyTruncatedUtf8(info.name, sizeof(info.name), info.id);
  }
  return static_cast<std::uint32_t>(count);
}

}  // namespace snd::platform::detail
