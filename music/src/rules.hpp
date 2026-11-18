// States and the transition rule table (design 8.5). Rules are matched top to
// bottom; the first row whose From / To match (0 = wildcard) wins. When no row
// matches, the built-in default rule applies (8.7: the bottom row of the
// design's table): next bar, 1 beat out / 1 beat in, no bridge, from the head,
// interruptible, late = next grid.
#ifndef SND_MUSIC_SRC_RULES_HPP
#define SND_MUSIC_SRC_RULES_HPP

#include <cstdint>

#include "bank_tables.hpp"

namespace snd::music::detail {

enum class StartPosition : std::uint32_t { Head = 0, SameBar = 1, Marker = 2, LastPosition = 3 };
enum class Interrupt : std::uint32_t { Allowed = 0, AfterBridge = 1 };
enum RuleFlags : std::uint32_t { kRulePrepare = 1u << 0 };

struct Rule {
  std::uint32_t row = bf::kNone;  // MTRN row; kNone = built-in default
  std::uint32_t timing = SND_WHEN_NEXT_BAR;
  std::uint32_t timingCount = 0;  // BARS_AFTER: N; GRID: division; NEXT_MARKER: marker id (0 = any)
  float fadeOutBeats = 1.0f;
  float fadeInBeats = 1.0f;
  std::uint32_t bridge = bf::kNone;  // MSEG row
  StartPosition start = StartPosition::Head;
  Id startMarker = 0;
  Interrupt interrupt = Interrupt::Allowed;
  std::uint8_t lateRule = 0;  // ReservationLateRule
  std::uint32_t flags = 0;
};

Rule DefaultRule() noexcept;
Rule FindRule(const BankTables& tb, std::uint32_t eventRow, Id from, Id to) noexcept;
// MSTA row of `state` for the music event, or kNone.
std::uint32_t FindStateRow(const BankTables& tb, std::uint32_t eventRow, Id state) noexcept;
// Initial state: EVNT.musicState, else the event's first MSTA row (8.7), else kNone.
std::uint32_t InitialStateRow(const BankTables& tb, std::uint32_t eventRow) noexcept;
// MSTG row of stinger `id`, or kNone.
std::uint32_t FindStinger(const BankTables& tb, Id id) noexcept;

}  // namespace snd::music::detail

#endif  // SND_MUSIC_SRC_RULES_HPP
