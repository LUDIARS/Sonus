// Markers of effect clips (2.1 "マーカー"; stage 3 group 4). The bank has no column for
// them yet (music has MMRK): until it has, CoreDesc::clipMarkers can supply a clip's
// markers (tests), and this is the receiving end that turns a voice's movement into
// marker crossings. Pure (no state): Render calls it for real voices after their chunk.
#ifndef SND_CORE_SRC_MARKERS_HPP
#define SND_CORE_SRC_MARKERS_HPP

#include <cstdint>

namespace snd::core {

// Calls fn(index, played) for every marker crossed while the voice's played source frames
// (loops unrolled, from its start frame) move from `from` (included) to `to` (excluded): a
// marker at asset frame m is crossed at the played position p where the voice reads m. markerFrames: asset frames, any order. The clip plays from
// `start`; with a loop [lb, le) (le != 0) it wraps there, so a marker inside the loop is
// crossed once per pass. Crossings are reported in order of p.
template <class Fn>
void ForEachMarkerCrossing(const std::uint64_t* markerFrames, std::uint32_t count, std::uint64_t start,
                           std::uint64_t lb, std::uint64_t le, double from, double to, Fn&& fn) noexcept {
  if (markerFrames == nullptr || count == 0u || !(to > from)) return;
  if (from < 0.0) from = 0.0;
  const bool loop = le > lb && le > start;
  const double first = loop ? static_cast<double>(le - start) : 1.0e300;  // played frames of the first pass
  const double len = loop ? static_cast<double>(le - lb) : 0.0;
  // Passes in order: pass 0 covers played [0, first) = asset [start, ...); pass k >= 1
  // covers played [first + (k - 1) len, first + k len) = asset [lb, le).
  std::uint64_t pass = (!loop || from < first) ? 0u : 1u + static_cast<std::uint64_t>((from - first) / len);
  for (std::uint32_t guard = 0; guard < 1u << 20; ++guard) {
    const double base = pass == 0u ? 0.0 : first + static_cast<double>(pass - 1u) * len;  // played at the pass start
    const double assetBase = pass == 0u ? static_cast<double>(start) : static_cast<double>(lb);
    const double end = pass == 0u ? first : base + len;
    if (base >= to) break;
    // Report the markers of this pass inside [from, to), in order of position.
    double last = -1.0;
    std::uint32_t lastI = 0;
    for (;;) {
      double best = 1.0e300;
      std::uint32_t bi = 0;
      for (std::uint32_t i = 0; i < count; ++i) {
        const double m = static_cast<double>(markerFrames[i]);
        if (m < assetBase || (loop && m >= static_cast<double>(le))) continue;
        const double p = base + (m - assetBase);
        if (p < from || p >= to || p >= end || p < last || (p == last && i <= lastI)) continue;
        if (p < best || (p == best && i < bi)) {
          best = p;
          bi = i;
        }
      }
      if (best >= 1.0e300) break;
      fn(bi, best);
      last = best;
      lastI = bi;
    }
    if (!loop) break;
    ++pass;
  }
}

}  // namespace snd::core

#endif  // SND_CORE_SRC_MARKERS_HPP
