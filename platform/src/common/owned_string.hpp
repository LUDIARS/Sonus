// Copy of an option string in allocator memory (OutputDeviceOptions strings
// are copied at CreateOutputDevice, D-64). Non-RT.
#ifndef SND_PLATFORM_SRC_COMMON_OWNED_STRING_HPP
#define SND_PLATFORM_SRC_COMMON_OWNED_STRING_HPP

#include <cstddef>
#include <cstring>

#include "snd/ext/allocator.hpp"

namespace snd::platform::detail {

class OwnedString {
 public:
  OwnedString() noexcept = default;
  ~OwnedString() { Reset(); }
  OwnedString(const OwnedString&) = delete;
  OwnedString& operator=(const OwnedString&) = delete;

  // Copies `s` (null -> empty). false on allocation failure.
  bool Assign(IAllocator& a, const char* s) noexcept {
    Reset();
    if (s == nullptr) return true;
    const std::size_t n = std::strlen(s);
    char* p = static_cast<char*>(a.Allocate(n + 1u, 1, MemTag::Misc));
    if (p == nullptr) return false;
    std::memcpy(p, s, n + 1u);
    alloc_ = &a;
    data_ = p;
    return true;
  }
  void Reset() noexcept {
    if (data_ != nullptr && alloc_ != nullptr) alloc_->Free(data_, MemTag::Misc);
    data_ = nullptr;
    alloc_ = nullptr;
  }
  const char* Get() const noexcept { return data_; }  // null when unset
  bool Empty() const noexcept { return data_ == nullptr || data_[0] == '\0'; }

 private:
  IAllocator* alloc_ = nullptr;
  char* data_ = nullptr;
};

// Copies at most cap-1 bytes and terminates (cap > 0). Never splits a UTF-8 sequence.
inline void CopyTruncatedUtf8(char* dst, std::size_t cap, const char* src) noexcept {
  if (cap == 0u) return;
  std::size_t n = src == nullptr ? 0u : std::strlen(src);
  if (n >= cap) {
    n = cap - 1u;
    while (n > 0u && (static_cast<unsigned char>(src[n]) & 0xC0u) == 0x80u) --n;  // continuation byte
  }
  if (n > 0u) std::memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace snd::platform::detail

#endif  // SND_PLATFORM_SRC_COMMON_OWNED_STRING_HPP
