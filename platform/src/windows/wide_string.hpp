// UTF-8 <-> UTF-16 for device ids and names (WideCharToMultiByte /
// MultiByteToWideChar). Non-RT. Output is always terminated (truncated if needed).
#ifndef SND_PLATFORM_SRC_WINDOWS_WIDE_STRING_HPP
#define SND_PLATFORM_SRC_WINDOWS_WIDE_STRING_HPP

#include <cstddef>

namespace snd::platform::detail::wasapi {

// false if the input is not valid UTF-8 or does not fit.
bool Utf8ToWide(const char* in, wchar_t* out, std::size_t outChars) noexcept;
// Converts and truncates at a character boundary (never fails for valid UTF-16).
void WideToUtf8(const wchar_t* in, char* out, std::size_t outBytes) noexcept;

}  // namespace snd::platform::detail::wasapi

#endif  // SND_PLATFORM_SRC_WINDOWS_WIDE_STRING_HPP
