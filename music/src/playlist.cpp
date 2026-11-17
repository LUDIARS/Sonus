#include "playlist.hpp"

namespace snd::music::detail {

std::uint32_t NextRandom(std::uint32_t& state) noexcept {
  std::uint32_t x = state != 0u ? state : 0x9E3779B9u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return x;
}

namespace {
std::uint32_t Mix(std::uint32_t x) noexcept {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}
}  // namespace

std::uint32_t ShuffleAt(std::uint32_t k, std::uint32_t n, std::uint32_t key) noexcept {
  if (n <= 1u) return 0;
  std::uint32_t bits = 1;
  while ((1u << bits) < n) ++bits;
  const std::uint32_t mask = (1u << bits) - 1u;
  const std::uint32_t shift = bits / 2u + 1u;
  std::uint32_t mul[3], add[3];
  for (std::uint32_t r = 0; r < 3; ++r) {
    mul[r] = Mix(key + 0x9E3779B9u * (2u * r + 1u)) | 1u;  // odd: a bijection mod 2^bits
    add[r] = Mix(key ^ (0x85EBCA6Bu * (2u * r + 2u)));
  }
  auto h = [&](std::uint32_t x) {
    for (std::uint32_t r = 0; r < 3; ++r) {
      x = (x * mul[r]) & mask;
      x ^= x >> shift;  // bijective (shift >= 1)
      x = (x + add[r]) & mask;
    }
    return x;
  };
  std::uint32_t y = h(k);
  for (std::uint32_t guard = 0; y >= n && guard <= mask; ++guard) y = h(y);  // cycle walking
  return y < n ? y : k;
}

void CursorReset(Cursor& c, std::uint32_t playlist) noexcept {
  c = Cursor{};
  c.playlist = playlist;
}

namespace {
bool Entries(const BankTables& tb, std::uint32_t playlist, std::uint32_t* first, std::uint32_t* n) noexcept {
  if (!tb.mpls.Has(playlist)) return false;
  *first = tb.mpls.U32(bf::col::MPLS_entryFirst, playlist);
  *n = tb.mpls.U32(bf::col::MPLS_entryCount, playlist);
  if (*first >= tb.mple.Rows() || *n > tb.mple.Rows() - *first) return false;
  return *n != 0u;
}
}  // namespace

std::uint32_t FirstSegment(const BankTables& tb, std::uint32_t playlist) noexcept {
  std::uint32_t first = 0, n = 0;
  if (!Entries(tb, playlist, &first, &n)) return bf::kNone;
  return tb.mple.U32(bf::col::MPLE_segment, first);
}

std::uint32_t CursorNext(Cursor& c, const BankTables& tb, std::uint32_t& rng, std::uint32_t* entryOut) noexcept {
  std::uint32_t first = 0, n = 0;
  if (c.finished != 0u || !Entries(tb, c.playlist, &first, &n)) return bf::kNone;
  const PlaylistMode mode = static_cast<PlaylistMode>(tb.mpls.U32(bf::col::MPLS_mode, c.playlist));
  if (c.current != bf::kNone && c.current < n && c.repeatLeft > 0u) {
    --c.repeatLeft;
    if (entryOut != nullptr) *entryOut = c.current;
    return tb.mple.U32(bf::col::MPLE_segment, first + c.current);
  }
  if (c.picks >= n) {
    ++c.pass;
    c.picks = 0;
    const std::uint32_t loops = tb.mpls.U32(bf::col::MPLS_loopCount, c.playlist);
    if (loops != 0u && c.pass >= loops) {
      c.finished = 1;
      return bf::kNone;
    }
  }
  std::uint32_t idx = 0;
  switch (mode) {
    case PlaylistMode::Random: {
      double total = 0.0;
      for (std::uint32_t k = 0; k < n; ++k) {
        const float w = tb.mple.F32(bf::col::MPLE_weight, first + k);
        total += w > 0.0f ? w : 0.0f;
      }
      const double u = static_cast<double>(NextRandom(rng)) / 4294967296.0;
      if (total <= 0.0) {
        idx = static_cast<std::uint32_t>(u * n);
      } else {
        double x = u * total;
        idx = n - 1u;
        for (std::uint32_t k = 0; k < n; ++k) {
          const float w = tb.mple.F32(bf::col::MPLE_weight, first + k);
          const double wk = w > 0.0f ? w : 0.0;
          if (x < wk) {
            idx = k;
            break;
          }
          x -= wk;
        }
      }
      break;
    }
    case PlaylistMode::Shuffle: {
      if (c.picks == 0u) {  // a new pass: new permutation, no repeat across the boundary
        c.passKey = NextRandom(rng);
        c.swapFirst = (n > 1u && c.current != bf::kNone && ShuffleAt(0, n, c.passKey) == c.current) ? 1u : 0u;
      }
      std::uint32_t k = c.picks;
      if (c.swapFirst != 0u && k < 2u) k = 1u - k;
      idx = ShuffleAt(k, n, c.passKey);
      break;
    }
    case PlaylistMode::Sequential:
    default:
      idx = c.picks;
      break;
  }
  if (idx >= n) idx = n - 1u;
  ++c.picks;
  c.current = idx;
  const std::uint32_t rep = tb.mple.U32(bf::col::MPLE_repeat, first + idx);
  c.repeatLeft = rep == 0u ? 0u : rep - 1u;
  if (entryOut != nullptr) *entryOut = idx;
  return tb.mple.U32(bf::col::MPLE_segment, first + idx);
}

}  // namespace snd::music::detail
