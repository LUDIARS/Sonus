#include "windows/wide_string.hpp"

#include "common/owned_string.hpp"
#include "windows/windows_headers.hpp"

namespace snd::platform::detail::wasapi {

bool Utf8ToWide(const char* in, wchar_t* out, std::size_t outChars) noexcept {
  if (outChars == 0u) return false;
  out[0] = L'\0';
  if (in == nullptr) return true;
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, out, static_cast<int>(outChars));
  if (n <= 0) {
    out[0] = L'\0';
    return false;
  }
  return true;
}

void WideToUtf8(const wchar_t* in, char* out, std::size_t outBytes) noexcept {
  if (outBytes == 0u) return;
  out[0] = '\0';
  if (in == nullptr) return;
  // Convert into a large enough temporary, then truncate at a UTF-8 boundary.
  char tmp[1024];
  const int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, tmp, static_cast<int>(sizeof(tmp)), nullptr, nullptr);
  if (n <= 0) {
    tmp[sizeof(tmp) - 1u] = '\0';  // too long: WideCharToMultiByte wrote nothing usable
    tmp[0] = '\0';
  }
  CopyTruncatedUtf8(out, outBytes, tmp);
}

}  // namespace snd::platform::detail::wasapi
