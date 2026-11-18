// Render load measurement and the load-LOD hysteresis (design.md 2.4).
//
// Budget of one block = block duration x loadTargetPercent. Every rendered
// block reports its processing time. Over budget for `downBlocks`
// consecutive blocks raises the level by one (one LOD step down in 2.4's
// order: fewer real voices, cheap resampler for new voices, bypass effects
// of low-priority buses); within budget for `upBlocks` consecutive blocks
// lowers it by one. Any block that breaks a streak resets that streak.
//
// The monitor only suggests the level; core applies the steps (2.4
// "切り替えの作法"). Single writer (the audio thread) for Record; Level()
// and the statistics may be read from any thread. RT-safe.
#ifndef SND_PLATFORM_LOAD_MONITOR_HPP
#define SND_PLATFORM_LOAD_MONITOR_HPP

#include <atomic>
#include <cstdint>

namespace snd::platform {

struct LoadMonitorDesc {
  std::uint32_t downBlocks = 8;   // Config.tuning.lodDownBlocks
  std::uint32_t upBlocks = 64;    // Config.tuning.lodUpBlocks
  std::uint32_t maxLevel = 3;     // the three steps of 2.4
};

class LoadMonitor {
 public:
  explicit LoadMonitor(const LoadMonitorDesc& desc = LoadMonitorDesc{}) noexcept { Configure(desc); }

  // Setup only.
  void Configure(const LoadMonitorDesc& desc) noexcept;
  void Reset() noexcept;

  // Audio thread. Returns the level after this block.
  std::uint32_t Record(std::uint64_t renderNs, std::uint64_t budgetNs) noexcept;

  std::uint32_t Level() const noexcept { return level_.load(std::memory_order_relaxed); }
  std::uint64_t OverBudgetBlocks() const noexcept { return overTotal_.load(std::memory_order_relaxed); }
  std::uint64_t LevelChanges() const noexcept { return changes_.load(std::memory_order_relaxed); }

 private:
  LoadMonitorDesc desc_{};
  std::uint32_t overStreak_ = 0;
  std::uint32_t underStreak_ = 0;
  std::atomic<std::uint32_t> level_{0};
  std::atomic<std::uint64_t> overTotal_{0};
  std::atomic<std::uint64_t> changes_{0};
};

// Budget in ns of `frames` at `sampleRate` x `percent` / 100, rounded (0 if rate is 0).
std::uint64_t BlockBudgetNs(std::uint32_t frames, std::uint32_t sampleRate, double percent) noexcept;

}  // namespace snd::platform

#endif  // SND_PLATFORM_LOAD_MONITOR_HPP
