// Playlist cursor (design 8.3): sequential, weighted random and shuffle over
// the MPLE entries of an MPLS row, each entry played `repeat` times in a row,
// `loopCount` passes (0 = endless). A pass is entryCount picks for random.
// Shuffle draws a new permutation every pass and avoids repeating the last
// entry of the previous pass first. The permutation is not stored: entry k of
// a pass is a keyed bijection of [0, n) (cycle walking over a power-of-two
// domain), so playlists of any length shuffle without memory per entry. Random numbers come from the playback's
// own generator (2.5: decisions are reproducible from the seed).
#ifndef SND_MUSIC_SRC_PLAYLIST_HPP
#define SND_MUSIC_SRC_PLAYLIST_HPP

#include <cstdint>

#include "bank_tables.hpp"

namespace snd::music::detail {

enum class PlaylistMode : std::uint32_t { Sequential = 0, Random = 1, Shuffle = 2 };

struct Cursor {
  std::uint32_t playlist = bf::kNone;  // MPLS row
  std::uint32_t current = bf::kNone;   // entry index inside the playlist of the last pick
  std::uint32_t repeatLeft = 0;        // further plays of `current`
  std::uint32_t picks = 0;             // picks in this pass
  std::uint32_t pass = 0;
  std::uint32_t finished = 0;
  std::uint32_t passKey = 0;             // shuffle: key of this pass's permutation
  std::uint32_t swapFirst = 0;           // shuffle: first two of the pass exchanged (no repeat)
};

std::uint32_t NextRandom(std::uint32_t& state) noexcept;  // xorshift32

void CursorReset(Cursor& c, std::uint32_t playlist) noexcept;
// Next MSEG row, or kNone when the playlist is finished (or empty / invalid).
// `entryOut` (optional) receives the entry index inside the playlist.
std::uint32_t CursorNext(Cursor& c, const BankTables& tb, std::uint32_t& rng, std::uint32_t* entryOut = nullptr) noexcept;
// The segment the playlist starts with, without consuming randomness (lead-time estimates).
// Shuffle permutation: position `k` of a pass of `n` entries under `key` (a bijection of [0, n)).
std::uint32_t ShuffleAt(std::uint32_t k, std::uint32_t n, std::uint32_t key) noexcept;
std::uint32_t FirstSegment(const BankTables& tb, std::uint32_t playlist) noexcept;

}  // namespace snd::music::detail

#endif  // SND_MUSIC_SRC_PLAYLIST_HPP
