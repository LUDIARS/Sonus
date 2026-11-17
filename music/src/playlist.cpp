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
  if (mode == PlaylistMode::Shuffle && n > kMaxShuffle) n = kMaxShuffle;
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
      if (c.picks == 0u) {
        for (std::uint32_t k = 0; k < n; ++k) c.order[k] = static_cast<std::uint8_t>(k);
        for (std::uint32_t k = n; k > 1u; --k) {
          const std::uint32_t j = NextRandom(rng) % k;
          const std::uint8_t tmp = c.order[k - 1u];
          c.order[k - 1u] = c.order[j];
          c.order[j] = tmp;
        }
        if (n > 1u && c.current != bf::kNone && c.order[0] == c.current) {
          const std::uint8_t tmp = c.order[0];
          c.order[0] = c.order[1];
          c.order[1] = tmp;
        }
      }
      idx = c.order[c.picks];
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
