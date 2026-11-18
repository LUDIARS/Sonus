#include "rules.hpp"

namespace snd::music::detail {

Rule DefaultRule() noexcept { return Rule{}; }

Rule FindRule(const BankTables& tb, std::uint32_t eventRow, Id from, Id to) noexcept {
  for (std::uint32_t r = 0; r < tb.mtrn.Rows(); ++r) {
    if (tb.mtrn.U32(bf::col::MTRN_musicEvent, r) != eventRow) continue;
    const Id f = tb.mtrn.U32(bf::col::MTRN_fromState, r);
    const Id t = tb.mtrn.U32(bf::col::MTRN_toState, r);
    if ((f != 0u && f != from) || (t != 0u && t != to)) continue;
    Rule x;
    x.row = r;
    x.timing = tb.mtrn.U32(bf::col::MTRN_timing, r);
    if (x.timing > SND_WHEN_SEGMENT_END) x.timing = SND_WHEN_NEXT_BAR;  // clock kinds are not rule timings
    x.timingCount = tb.mtrn.U32(bf::col::MTRN_timingCount, r);
    x.fadeOutBeats = tb.mtrn.F32(bf::col::MTRN_fadeOutBeats, r);
    x.fadeInBeats = tb.mtrn.F32(bf::col::MTRN_fadeInBeats, r);
    if (!(x.fadeOutBeats >= 0.0f)) x.fadeOutBeats = 0.0f;
    if (!(x.fadeInBeats >= 0.0f)) x.fadeInBeats = 0.0f;
    x.bridge = tb.mtrn.U32(bf::col::MTRN_bridgeSegment, r);
    if (!tb.mseg.Has(x.bridge)) x.bridge = bf::kNone;
    const std::uint32_t sp = tb.mtrn.U32(bf::col::MTRN_startPosition, r);
    x.start = sp <= 3u ? static_cast<StartPosition>(sp) : StartPosition::Head;
    x.startMarker = tb.mtrn.U32(bf::col::MTRN_startMarker, r);
    x.interrupt = tb.mtrn.U32(bf::col::MTRN_interrupt, r) == 1u ? Interrupt::AfterBridge : Interrupt::Allowed;
    const std::uint32_t late = tb.mtrn.U32(bf::col::MTRN_lateRule, r);
    x.lateRule = static_cast<std::uint8_t>(late <= 2u ? late : 0u);
    x.flags = tb.mtrn.U32(bf::col::MTRN_flags, r);
    return x;
  }
  return DefaultRule();
}

std::uint32_t FindStateRow(const BankTables& tb, std::uint32_t eventRow, Id state) noexcept {
  for (std::uint32_t r = 0; r < tb.msta.Rows(); ++r) {
    if (tb.msta.U32(bf::col::MSTA_musicEvent, r) == eventRow && tb.msta.U32(bf::col::MSTA_id, r) == state) return r;
  }
  return bf::kNone;
}

std::uint32_t InitialStateRow(const BankTables& tb, std::uint32_t eventRow) noexcept {
  const std::uint32_t r = tb.evnt.U32(bf::col::EVNT_musicState, eventRow);
  if (tb.msta.Has(r) && tb.msta.U32(bf::col::MSTA_musicEvent, r) == eventRow) return r;
  for (std::uint32_t k = 0; k < tb.msta.Rows(); ++k) {
    if (tb.msta.U32(bf::col::MSTA_musicEvent, k) == eventRow) return k;
  }
  return bf::kNone;
}

std::uint32_t FindStinger(const BankTables& tb, Id id) noexcept {
  for (std::uint32_t r = 0; r < tb.mstg.Rows(); ++r) {
    if (tb.mstg.U32(bf::col::MSTG_id, r) == id) return r;
  }
  return bf::kNone;
}

}  // namespace snd::music::detail
