// The Windows headers used by the WASAPI backend, in one place, with the
// macros they need. Internal to src/windows/.
#ifndef SND_PLATFORM_SRC_WINDOWS_WINDOWS_HEADERS_HPP
#define SND_PLATFORM_SRC_WINDOWS_WINDOWS_HEADERS_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
// windows.h first: the audio headers rely on its types.
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <propidl.h>
#include <propsys.h>

// Stream flags (audiosessiontypes.h, Windows 7+). Older MinGW headers lack them.
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
#ifndef AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
#define AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED static_cast<HRESULT>(0x88890019L)
#endif
#ifndef AUDCLNT_E_DEVICE_INVALIDATED
#define AUDCLNT_E_DEVICE_INVALIDATED static_cast<HRESULT>(0x88890004L)
#endif

// IAudioClient3 (Windows 10 SDK). Without it the low-latency shared path is
// compiled out and IAudioClient::Initialize is used (D-63).
#if defined(__IAudioClient3_INTERFACE_DEFINED__)
#define SND_WASAPI_HAVE_CLIENT3 1
#else
#define SND_WASAPI_HAVE_CLIENT3 0
#endif

#endif  // SND_PLATFORM_SRC_WINDOWS_WINDOWS_HEADERS_HPP
