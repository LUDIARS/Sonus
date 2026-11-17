#include "snd/platform/load_monitor.hpp"

namespace snd::platform {

void LoadMonitor::Configure(const LoadMonitorDesc& desc) noexcept {
  desc_ = desc;
  if (desc_.downBlocks == 0u) desc_.downBlocks = 1u;
  if (desc_.upBlocks == 0u) desc_.upBlocks = 1u;
  Reset();
}

void LoadMonitor::Reset() noexcept {
  overStreak_ = 0;
  underStreak_ = 0;
  level_.store(0, std::memory_order_relaxed);
  overTotal_.store(0, std::memory_order_relaxed);
  changes_.store(0, std::memory_order_relaxed);
}

std::uint32_t LoadMonitor::Record(std::uint64_t renderNs, std::uint64_t budgetNs) noexcept {
  std::uint32_t level = level_.load(std::memory_order_relaxed);
  if (renderNs > budgetNs) {
    overTotal_.store(overTotal_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    underStreak_ = 0;
    if (++overStreak_ >= desc_.downBlocks) {
      overStreak_ = 0;
      if (level < desc_.maxLevel) {
        ++level;
        changes_.store(changes_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
      }
    }
  } else {
    overStreak_ = 0;
    if (++underStreak_ >= desc_.upBlocks) {
      underStreak_ = 0;
      if (level > 0u) {
        --level;
        changes_.store(changes_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
      }
    }
  }
  level_.store(level, std::memory_order_relaxed);
  return level;
}

std::uint64_t BlockBudgetNs(std::uint32_t frames, std::uint32_t sampleRate, double percent) noexcept {
  if (sampleRate == 0u || !(percent > 0.0)) return 0;
  const double ns = static_cast<double>(frames) * 1e9 / static_cast<double>(sampleRate) * (percent / 100.0);
  if (!(ns < 1.8e19)) return ~0ull;
  return static_cast<std::uint64_t>(ns + 0.5);
}

}  // namespace snd::platform
