// Shared fixed-capacity lifecycle for resident synthesis. Core holds every slot
// a late worker can still touch (D-43). Only the audio thread touches the free list.
#ifndef SND_SOURCE_RESIDENT_SYNTH_HPP
#define SND_SOURCE_RESIDENT_SYNTH_HPP
#include <algorithm>
#include <cmath>
#include <new>

#include "detail.hpp"

namespace snd::source::detail {

inline double Modulation(Ramp ramp, double position) noexcept {
  if (!std::isfinite(ramp.start) || !std::isfinite(ramp.end)) return 0;
  return ramp.start + (double(ramp.end) - ramp.start) * position;
}

template <class Derived, class Voice>
class ResidentSynth : public ISourceKind {
 public:
  std::size_t PoolBytes(const SourceLimits& limits) const noexcept final {
    Carver measure{};
    measure.Take<Voice>(limits.maxVoices);
    measure.Take<SlotIndex>(limits.maxVoices);
    return measure.used;
  }
  bool Init(void* pool, std::size_t bytes, const SourceLimits& limits, const SourceServices&) noexcept final {
    if (!pool || bytes < PoolBytes(limits)) return false;
    Carver carve{static_cast<std::uint8_t*>(pool), 0, bytes};
    voices_ = carve.Take<Voice>(limits.maxVoices);
    slots_.Bind(carve.Take<SlotIndex>(limits.maxVoices), limits.maxVoices);
    for (SlotIndex i = 0; i < limits.maxVoices; ++i) new (&voices_[i]) Voice{};
    rate_ = limits.sampleRate ? limits.sampleRate : 48000;
    return true;
  }
  void Shutdown() noexcept final {
    voices_ = nullptr;
    slots_ = {};
    live_.store(0, std::memory_order_relaxed);
  }
  void Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept final {
    for (std::uint32_t i = 0; i < count; ++i) {
      results[i] = {kInvalidSlot, SND_END_DETAIL_NONE, 0, 0};
      if (!voices_ || !requests[i].params) continue;
      const auto slot = slots_.Pop();
      if (slot == kNone) {
        results[i].endDetail = SND_END_DETAIL_VOICE_LIMIT;
        continue;
      }
      Voice& voice = voices_[slot];
      voice = Voice{};
      if (!static_cast<Derived*>(this)->Prepare(voice, requests[i], results[i])) {
        slots_.Push(slot);
        continue;
      }
      voice.live = true;
      results[i].sourceSlot = slot;
      live_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void Release(const SlotIndex* ids, std::uint32_t count) noexcept final {
    for (std::uint32_t i = 0; i < count; ++i) {
      if (auto* v = Get(ids[i])) {
        v->live = false;
        slots_.Push(ids[i]);
        live_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  }
  void QueryReady(const SlotIndex* ids, std::uint32_t count, std::uint8_t* ready) const noexcept final {
    for (std::uint32_t i = 0; i < count; ++i) ready[i] = ids[i] < slots_.cap && voices_[ids[i]].live;
  }
  void ScheduleBackground(const BlockInfo&) noexcept final {}
  std::uint32_t PrimeFrames(const SourceAsset&, std::uint64_t) const noexcept final { return 0; }
  std::uint32_t LiveInstances() const noexcept { return live_.load(std::memory_order_relaxed); }

 protected:
  Voice* Get(SlotIndex i) noexcept { return i < slots_.cap && voices_[i].live ? &voices_[i] : nullptr; }
  std::uint32_t rate_ = 48000;

 private:
  Voice* voices_ = nullptr;
  FreeStack slots_{};
  std::atomic<std::uint32_t> live_{0};
};
}  // namespace snd::source::detail
#endif
