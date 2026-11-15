// FNV-1a 64 for content hashes (differential build cache, contentHash).
// detail: a generic hash that belongs in base.
#ifndef SND_BUILDTOOL_SRC_DETAIL_FNV_HPP
#define SND_BUILDTOOL_SRC_DETAIL_FNV_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace snd::buildtool::detail {

class Fnv64 {
 public:
  Fnv64& Bytes(const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
      h_ ^= b[i];
      h_ *= 1099511628211ull;
    }
    return *this;
  }
  Fnv64& U64(std::uint64_t v) {
    std::uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(v >> (8 * i));
    return Bytes(b, 8);
  }
  Fnv64& Str(std::string_view s) {
    U64(s.size());
    return Bytes(s.data(), s.size());
  }
  std::uint64_t value() const { return h_; }

 private:
  std::uint64_t h_ = 14695981039346656037ull;
};

}  // namespace snd::buildtool::detail

#endif  // SND_BUILDTOOL_SRC_DETAIL_FNV_HPP
