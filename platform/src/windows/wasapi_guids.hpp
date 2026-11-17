// COM class / interface ids and keys used by the WASAPI backend. Defined
// here with their documented values instead of __uuidof / the uuid
// libraries, so MSVC, clang-cl and MinGW build the same way (no initguid.h
// ordering, no ksuser.lib / uuid.lib). Values: mmdeviceapi.h, audioclient.h,
// ksmedia.h, functiondiscoverykeys_devpkey.h (Windows SDK 10.0).
#ifndef SND_PLATFORM_SRC_WINDOWS_WASAPI_GUIDS_HPP
#define SND_PLATFORM_SRC_WINDOWS_WASAPI_GUIDS_HPP

#include "windows/windows_headers.hpp"

namespace snd::platform::detail::wasapi {

// BCDE0395-E52F-467C-8E3D-C4579291692E
inline const CLSID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
// A95664D2-9614-4F35-A746-DE8DB63617E6
inline const IID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
// 7991EEC9-7E89-4D85-8390-6C703CEC60C0
inline const IID kIidIMMNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
// 00000000-0000-0000-C000-000000000046
inline const IID kIidIUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// 1CB9AD4C-DBFA-4C32-B178-C2F568A703B2
inline const IID kIidIAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
// 7ED4EE07-8E67-4CD4-8C1A-2B7A5987AD42
inline const IID kIidIAudioClient3 = {0x7ED4EE07, 0x8E67, 0x4CD4, {0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42}};
// F294ACFC-3146-4483-A7BF-ADDCA7C260E2
inline const IID kIidIAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
// CD63314F-3FBA-4A1B-812C-EF96358728E7
inline const IID kIidIAudioClock = {0xCD63314F, 0x3FBA, 0x4A1B, {0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7}};
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT 00000003-0000-0010-8000-00AA00389B71
inline const GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
// KSDATAFORMAT_SUBTYPE_PCM 00000001-0000-0010-8000-00AA00389B71
inline const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
// PKEY_Device_FriendlyName {A45C254E-DF1C-4EFD-8020-67D146A850E0}, 14
inline const PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

}  // namespace snd::platform::detail::wasapi

#endif  // SND_PLATFORM_SRC_WINDOWS_WASAPI_GUIDS_HPP
