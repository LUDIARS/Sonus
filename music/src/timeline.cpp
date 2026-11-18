#include "timeline.hpp"

#include <cmath>

namespace snd::music::detail {

namespace {
constexpr double kEps = 1e-7;
}

void Timeline::Reset(double rate, double bpm, std::uint16_t beatsPerBar, std::uint16_t beatUnit) noexcept {
  state = SND_TRANSPORT_STOPPED;
  anchorClock = 0;
  anchorTick = 0.0;
  overrideBpm = 0.0;
  pitch = 1.0;
  sampleRate = rate > 0.0 ? rate : 48000.0;
  beatCursor = 0;
  loop = MusicalRange{0, 0};
  count = 1;
  points[0] = TempoPoint{0, bpm > 0.0 ? bpm : 120.0, 0.0, beatsPerBar == 0u ? std::uint16_t{4} : beatsPerBar,
                         beatUnit == 0u ? std::uint16_t{4} : beatUnit, 0u};
}

std::uint32_t Timeline::PointAt(double tick) const noexcept {
  std::uint32_t i = 0;
  while (i + 1u < count && static_cast<double>(points[i + 1u].tick) <= tick + kEps) ++i;
  return i;
}

double Timeline::BpmOf(std::uint32_t point) const noexcept {
  if (overrideBpm > 0.0) return overrideBpm;
  return point < count ? points[point].bpm : 120.0;
}

double Timeline::TicksPerSample(std::uint32_t point) const noexcept {
  const double tps = BpmOf(point) / 60.0 * kTicksPerQuarter * pitch / sampleRate;
  return tps > 1e-12 ? tps : 1e-12;
}

double Timeline::BeatTicks(std::uint32_t point) const noexcept {
  const std::uint16_t u = point < count ? points[point].beatUnit : std::uint16_t{4};
  return 4.0 * kTicksPerQuarter / static_cast<double>(u == 0u ? 4u : u);
}

double Timeline::BarTicks(std::uint32_t point) const noexcept {
  const std::uint16_t b = point < count ? points[point].beatsPerBar : std::uint16_t{4};
  return BeatTicks(point) * static_cast<double>(b == 0u ? 4u : b);
}

bool Timeline::AddPoint(MusicalTicks tick, double bpm, std::uint16_t beatsPerBar, std::uint16_t beatUnit) noexcept {
  if (beatsPerBar == 0u) beatsPerBar = 4;
  if (beatUnit == 0u) beatUnit = 4;
  if (!(bpm > 0.0)) bpm = 120.0;
  const double t = static_cast<double>(tick);
  const std::uint32_t p = PointAt(t);
  const TempoPoint& prev = points[p];
  std::uint32_t changed = p;
  if (prev.tick == tick) {  // replace in place
    points[p].bpm = bpm;
    points[p].beatsPerBar = beatsPerBar;
    points[p].beatUnit = beatUnit;
  } else if (t > static_cast<double>(prev.tick) && prev.bpm == bpm && prev.beatsPerBar == beatsPerBar &&
             prev.beatUnit == beatUnit) {
    return true;  // nothing changes
  } else {
    if (count >= kMaxTempoPoints) {
      // Full: forget the oldest point if it is already behind the position (its bar
      // number lives on in the next point), otherwise refuse.
      if (static_cast<double>(points[1].tick) > anchorTick + kEps || t <= static_cast<double>(points[1].tick)) {
        return false;
      }
      for (std::uint32_t j = 0; j + 1u < count; ++j) points[j] = points[j + 1u];
      --count;
      return AddPoint(tick, bpm, beatsPerBar, beatUnit);
    }
    // Insertion position: after p, or before the first point.
    const std::uint32_t at = t < static_cast<double>(points[0].tick) ? 0u : p + 1u;
    for (std::uint32_t j = count; j > at; --j) points[j] = points[j - 1u];
    ++count;
    changed = at;
    TempoPoint& n = points[at];
    n = TempoPoint{tick, bpm, 0.0, beatsPerBar, beatUnit, 0u};
    if (at == 0u) {
      n.bar = points[1].bar - (static_cast<double>(points[1].tick) - t) / BarTicks(0);
    } else {
      n.bar = points[at - 1u].bar + (t - static_cast<double>(points[at - 1u].tick)) / BarTicks(at - 1u);
    }
  }
  // Bar positions of the later points follow; earlier ones (and an explicit bar set by
  // SetPointAt) stay as they are.
  for (std::uint32_t j = changed + 1u; j < count; ++j) {
    points[j].bar = points[j - 1u].bar +
                    static_cast<double>(points[j].tick - points[j - 1u].tick) / BarTicks(j - 1u);
  }
  return true;
}

void Timeline::SetPointAt(MusicalTicks tick, double bar, double bpm, std::uint16_t beatsPerBar,
                          std::uint16_t beatUnit) noexcept {
  TruncateFrom(tick);
  TempoPoint n{tick, bpm > 0.0 ? bpm : 120.0, bar, beatsPerBar == 0u ? std::uint16_t{4} : beatsPerBar,
               beatUnit == 0u ? std::uint16_t{4} : beatUnit, 0u};
  if (points[0].tick >= tick) {
    points[0] = n;
    count = 1;
    return;
  }
  if (count >= kMaxTempoPoints) {  // make room: the oldest point goes
    for (std::uint32_t j = 0; j + 1u < count; ++j) points[j] = points[j + 1u];
    --count;
  }
  points[count++] = n;
}

void Timeline::TruncateFrom(MusicalTicks tick) noexcept {
  while (count > 1u && points[count - 1u].tick >= tick) --count;
}

double Timeline::TickAtClock(double clock) const noexcept {
  if (!Playing()) return anchorTick;
  double t = anchorTick;
  double s = clock - static_cast<double>(anchorClock);
  std::uint32_t i = PointAt(t);
  if (s >= 0.0) {
    for (std::uint32_t guard = 0; guard <= kMaxTempoPoints; ++guard) {
      if (i + 1u < count && static_cast<double>(points[i + 1u].tick) > t) {
        const double need = (static_cast<double>(points[i + 1u].tick) - t) / TicksPerSample(i);
        if (s >= need) {
          s -= need;
          t = static_cast<double>(points[i + 1u].tick);
          ++i;
          continue;
        }
      }
      break;
    }
    return t + s * TicksPerSample(i);
  }
  s = -s;
  for (std::uint32_t guard = 0; guard <= kMaxTempoPoints; ++guard) {
    if (i > 0u) {
      const double need = (t - static_cast<double>(points[i].tick)) / TicksPerSample(i);
      if (s > need) {
        s -= need;
        t = static_cast<double>(points[i].tick);
        --i;
        continue;
      }
    }
    break;
  }
  return t - s * TicksPerSample(i);
}

bool Timeline::ClockAtTick(double tick, double* clock) const noexcept {
  if (!Playing()) return false;
  double c = static_cast<double>(anchorClock);
  double t = anchorTick;
  std::uint32_t i = PointAt(t);
  if (tick >= t) {
    for (std::uint32_t guard = 0; guard <= kMaxTempoPoints && i + 1u < count; ++guard) {
      const double next = static_cast<double>(points[i + 1u].tick);
      if (next <= t || next >= tick) break;
      c += (next - t) / TicksPerSample(i);
      t = next;
      ++i;
    }
    *clock = c + (tick - t) / TicksPerSample(i);
    return true;
  }
  for (std::uint32_t guard = 0; guard <= kMaxTempoPoints && i > 0u; ++guard) {
    const double start = static_cast<double>(points[i].tick);
    if (start <= tick) break;
    c -= (t - start) / TicksPerSample(i);
    t = start;
    --i;
  }
  *clock = c - (t - tick) / TicksPerSample(i);
  return true;
}

void Timeline::AnchorAt(ClockSample clock) noexcept {
  anchorTick = TickAtClock(static_cast<double>(clock));
  anchorClock = clock;
}

void Timeline::StartAt(ClockSample clock, double tick) noexcept {
  anchorClock = clock;
  anchorTick = tick;
  state = SND_TRANSPORT_PLAYING;
  beatCursor = clock;
}

void Timeline::PauseAt(ClockSample clock) noexcept {
  if (state != SND_TRANSPORT_PLAYING) return;
  AnchorAt(clock);
  state = SND_TRANSPORT_PAUSED;
}

void Timeline::ResumeAt(ClockSample clock) noexcept {
  if (state == SND_TRANSPORT_PLAYING) return;
  anchorClock = clock;
  state = SND_TRANSPORT_PLAYING;
  beatCursor = clock;
}

void Timeline::StopAt(ClockSample clock) noexcept {
  anchorClock = clock;
  anchorTick = 0.0;
  state = SND_TRANSPORT_STOPPED;
}

void Timeline::SeekAt(ClockSample clock, double tick) noexcept {
  AnchorAt(clock);
  anchorTick = tick;
  if (beatCursor > clock) beatCursor = clock;
}

void Timeline::SetTempoAt(ClockSample clock, double bpm) noexcept {
  AnchorAt(clock);
  overrideBpm = bpm > 0.0 ? bpm : 0.0;
}

void Timeline::SetPitchAt(ClockSample clock, double ratio) noexcept {
  AnchorAt(clock);
  if (ratio > 0.0) pitch = ratio;
}

void Timeline::AdvanceTo(ClockSample clock) noexcept {
  if (Playing()) {
    AnchorAt(clock);
  } else {
    anchorClock = clock;
    if (beatCursor < clock) beatCursor = clock;
  }
  // Passed points stay: musical positions (seek, TickOfMusical) are resolved on the
  // whole map from tick 0 (D-71). The oldest ones go only when the table is full.
}

double Timeline::BarPosition(double tick) const noexcept {
  const std::uint32_t i = PointAt(tick);
  return points[i].bar + (tick - static_cast<double>(points[i].tick)) / BarTicks(i);
}

snd_musical_time Timeline::Musical(double tick) const noexcept {
  const std::uint32_t i = PointAt(tick);
  const double pos = BarPosition(tick);
  const double bar = std::floor(pos + kEps);
  double inBar = (pos - bar) * BarTicks(i);
  if (inBar < 0.0) inBar = 0.0;
  const double bt = BeatTicks(i);
  double beat = std::floor(inBar / bt + kEps);
  const std::uint16_t bpb = points[i].beatsPerBar;
  if (beat >= bpb) beat = bpb - 1.0;
  double rest = std::floor(inBar - beat * bt + 0.5);
  if (rest < 0.0) rest = 0.0;
  if (rest >= kTicksPerQuarter) rest = kTicksPerQuarter - 1.0;
  return snd_musical_time{static_cast<std::int32_t>(bar), static_cast<std::int32_t>(beat), static_cast<std::int32_t>(rest)};
}

double Timeline::TickOfMusical(const snd_musical_time& m) const noexcept {
  std::uint32_t i = 0;
  while (i + 1u < count && points[i + 1u].bar <= static_cast<double>(m.bar) + kEps) ++i;
  return static_cast<double>(points[i].tick) + (static_cast<double>(m.bar) - points[i].bar) * BarTicks(i) +
         static_cast<double>(m.beat) * BeatTicks(i) + static_cast<double>(m.tick);
}

namespace {
// Tick of the bar line with bar position `bar` (exact for points on bar lines).
double TickOfBar(const Timeline& tl, double bar) noexcept {
  std::uint32_t i = 0;
  while (i + 1u < tl.count && tl.points[i + 1u].bar <= bar + kEps) ++i;
  return static_cast<double>(tl.points[i].tick) + (bar - tl.points[i].bar) * tl.BarTicks(i);
}
}  // namespace

double Timeline::TickAtBar(double bar) const noexcept { return TickOfBar(*this, bar); }

double Timeline::NextBar(double from) const noexcept {
  const double pos = BarPosition(from);
  return TickOfBar(*this, std::ceil(pos - kEps));
}

double Timeline::BarsAfter(double from, std::uint32_t n) const noexcept {
  const double pos = BarPosition(from);
  return TickOfBar(*this, std::floor(pos + kEps) + static_cast<double>(n == 0u ? 1u : n));
}

double Timeline::NextGrid(double from, std::uint32_t division) const noexcept {
  const double grid = 4.0 * kTicksPerQuarter / static_cast<double>(division == 0u ? 4u : division);
  const double barStart = TickOfBar(*this, std::floor(BarPosition(from) + kEps));
  const double k = std::ceil((from - barStart) / grid - kEps);
  return barStart + (k < 0.0 ? 0.0 : k) * grid;
}

double Timeline::NextBeat(double from) const noexcept {
  const std::uint32_t i = PointAt(from);
  const double bt = BeatTicks(i);
  const double barStart = TickOfBar(*this, std::floor(BarPosition(from) + kEps));
  const double k = std::ceil((from - barStart) / bt - kEps);
  return barStart + (k < 0.0 ? 0.0 : k) * bt;
}

double Timeline::NextOfKind(std::uint32_t kind, std::uint32_t division, double tick) const noexcept {
  switch (kind) {
    case SND_WHEN_NEXT_BEAT: return NextBeat(tick + 0.5);
    case SND_WHEN_NEXT_BAR:
    case SND_WHEN_BARS_AFTER: return NextBar(tick + 0.5);
    case SND_WHEN_GRID: return NextGrid(tick + 0.5, division);
    default: return tick;
  }
}

}  // namespace snd::music::detail
