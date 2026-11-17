// MusicSystem: pool layout, lifecycle, notifications, conversions and
// introspection. UpdateTransport is in transport_update.cpp, the music logic
// in music_logic.cpp / transition.cpp.
#include "snd/music/music_system.hpp"

#include <cmath>
#include <new>

#include "music_state.hpp"

namespace snd::music {

namespace {

constexpr std::size_t kAlign = 64;
constexpr std::size_t Up(std::size_t n) noexcept { return (n + kAlign - 1u) / kAlign * kAlign; }

struct Layout {
  std::size_t state, timelines, pbs, pbOfSlot, res, due, notes, total;
};

Layout LayoutFor(const MusicLimits& l) noexcept {
  Layout o{};
  std::size_t at = 0;
  o.state = at;
  at += Up(sizeof(detail::State));
  o.timelines = at;
  at += Up(sizeof(detail::Timeline) * l.maxTransports);
  o.pbs = at;
  at += Up(sizeof(detail::MusicPb) * l.maxTransports);
  o.pbOfSlot = at;
  at += Up(sizeof(std::uint16_t) * l.maxPlaybacks);
  o.res = at;
  at += Up(sizeof(detail::ResState) * l.maxReservations);
  o.due = at;
  at += Up(sizeof(std::uint32_t) * l.maxReservations);
  o.notes = at;
  at += Up(sizeof(notify::EventRecord) * detail::kNotifyCapacity);
  o.total = at + kAlign;  // room to align the pool start
  return o;
}

}  // namespace

std::size_t MusicSystem::PoolBytes(const MusicLimits& limits) const noexcept { return LayoutFor(limits).total; }

bool MusicSystem::Init(void* pool, std::size_t bytes, const MusicLimits& limits, const MusicServices& services) noexcept {
  s_ = nullptr;
  if (pool == nullptr || limits.maxTransports == 0u || limits.maxPlaybacks == 0u || limits.sampleRate == 0u ||
      limits.blockFrames == 0u || limits.maxTransports > 0xFFFEu) {
    return false;
  }
  const Layout lo = LayoutFor(limits);
  if (bytes < lo.total) return false;
  auto* base = static_cast<std::uint8_t*>(pool);
  while (reinterpret_cast<std::uintptr_t>(base) % kAlign != 0u) ++base;
  auto* s = new (base + lo.state) detail::State();
  s->limits = limits;
  s->services = services;
  s->rate = static_cast<double>(limits.sampleRate);
  s->blockFrames = limits.blockFrames;
  s->timelines = reinterpret_cast<detail::Timeline*>(base + lo.timelines);
  for (std::uint32_t i = 0; i < limits.maxTransports; ++i) new (&s->timelines[i]) detail::Timeline();
  s->pbs = reinterpret_cast<detail::MusicPb*>(base + lo.pbs);
  s->pbCount = limits.maxTransports;  // every music playback owns a transport (8.1)
  for (std::uint32_t i = 0; i < s->pbCount; ++i) {
    new (&s->pbs[i]) detail::MusicPb();
    s->pbs[i].used = 0;
  }
  s->pbOfSlot = reinterpret_cast<std::uint16_t*>(base + lo.pbOfSlot);
  for (std::uint32_t i = 0; i < limits.maxPlaybacks; ++i) s->pbOfSlot[i] = detail::kNoPb;
  s->res = reinterpret_cast<detail::ResState*>(base + lo.res);
  for (std::uint32_t i = 0; i < limits.maxReservations; ++i) s->res[i] = detail::ResState{};
  s->due = reinterpret_cast<std::uint32_t*>(base + lo.due);
  s->notes = reinterpret_cast<notify::EventRecord*>(base + lo.notes);
  s_ = s;
  return true;
}

void MusicSystem::Shutdown() noexcept { s_ = nullptr; }

std::uint32_t MusicSystem::CollectNotifications(notify::EventRecord* out, std::uint32_t capacity) noexcept {
  if (s_ == nullptr) return 0;
  detail::State& s = *s_;
  const std::uint32_t n = s.noteCount < capacity ? s.noteCount : capacity;
  for (std::uint32_t i = 0; i < n; ++i) out[i] = s.notes[i];
  if (n < s.noteCount) s.dropped += s.noteCount - n;
  s.noteCount = 0;
  return n;
}

bool MusicSystem::TicksToClock(SlotIndex transport, MusicalTicks ticks, ClockSample* out) const noexcept {
  if (s_ == nullptr || transport >= s_->limits.maxTransports) return false;
  const detail::Timeline& tl = s_->timelines[transport];
  double c = 0.0;
  if (tl.generation == 0u || !tl.ClockAtTick(static_cast<double>(ticks), &c) || c < 0.0) return false;
  *out = detail::RoundClock(c);
  return true;
}

bool MusicSystem::ClockToTicks(SlotIndex transport, ClockSample clock, MusicalTicks* out) const noexcept {
  if (s_ == nullptr || transport >= s_->limits.maxTransports) return false;
  const detail::Timeline& tl = s_->timelines[transport];
  if (tl.generation == 0u) return false;
  *out = static_cast<MusicalTicks>(std::floor(tl.TickAtClock(static_cast<double>(clock)) + 1e-6));
  return true;
}

bool MusicSystem::GetPlaybackInfo(SlotIndex playback, MusicPlaybackInfo* out) const noexcept {
  if (s_ == nullptr || out == nullptr) return false;
  const detail::MusicPb* pb = detail::PbOf(*s_, playback);
  if (pb == nullptr) return false;
  MusicPlaybackInfo i;
  i.state = pb->stateId;
  i.phase = static_cast<std::uint32_t>(pb->phase);
  i.pendingState = pb->phase == detail::Phase::Pending ? pb->pendTarget : 0u;
  i.pendingTick = static_cast<MusicalTicks>(std::llround(pb->pendTick));
  i.queuedState = pb->hasQueued != 0u ? pb->queuedTarget : 0u;
  i.transitions = pb->transitions;
  i.deferrals = pb->deferrals;
  i.cancels = pb->cancels;
  i.started = pb->started;
  const detail::Timeline* tl =
      pb->transport < s_->limits.maxTransports ? &s_->timelines[pb->transport] : nullptr;
  if (tl != nullptr && tl->generation != 0u) {
    const double now = tl->TickAtClock(static_cast<double>(s_->blockStart));
    std::int32_t best = -1;
    for (std::uint32_t k = 0; k < detail::kMaxInstances; ++k) {
      const detail::Instance& in = pb->inst[k];
      if (in.role != detail::Role::Main || in.stopPending != 0u) continue;
      if (static_cast<double>(in.startTick) <= now + 1e-6 && now < static_cast<double>(in.EndTick())) {
        if (best < 0 || in.startTick > pb->inst[best].startTick) best = static_cast<std::int32_t>(k);
      }
    }
    if (best >= 0) {
      i.segmentStart = pb->inst[best].startTick;
      i.segment = pb->inst[best].segId;
    }
  }
  *out = i;
  return true;
}

std::uint32_t MusicSystem::TransitionLog(MusicTransitionRecord* out, std::uint32_t capacity) const noexcept {
  if (s_ == nullptr || out == nullptr) return 0;
  const std::uint32_t avail = s_->logCount < detail::kLogCapacity ? s_->logCount : detail::kLogCapacity;
  const std::uint32_t n = avail < capacity ? avail : capacity;
  const std::uint32_t first = s_->logCount - n;
  for (std::uint32_t k = 0; k < n; ++k) out[k] = s_->log[(first + k) % detail::kLogCapacity];
  return n;
}

std::uint32_t MusicSystem::DroppedNotifications() const noexcept { return s_ != nullptr ? s_->dropped : 0u; }

}  // namespace snd::music
