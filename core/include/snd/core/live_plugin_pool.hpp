// Worker-prepared, address-stable external DSP states. The owning LiveRuntime or
// Replayer outlives its bound System. Only the worker allocates and reclaims nodes.
#pragma once
#include <atomic>
#include <cstdint>

#include "snd/base/mutex.hpp"
#include "snd/config.h"
#include "snd/ext/allocator.hpp"

namespace snd::core {
struct PreparedLivePlugin {
  PreparedLivePlugin* next = nullptr;
  const snd_dsp_effect* plugin = nullptr;
  std::uint32_t channels = 0;
  std::uint32_t bytes = 0;
  void* state = nullptr;
  std::atomic<bool> held{true};
};

class LivePluginPool {
 public:
  ~LivePluginPool();
  snd_result Prepare(IAllocator& alloc, const snd_config& cfg, const snd_dsp_effect& plugin,
                     std::uint32_t channels, const float* params, std::uint32_t count,
                     std::uint64_t limit, PreparedLivePlugin** out) noexcept;
 private:
  base::Mutex mutex_;
  IAllocator* alloc_ = nullptr;
  PreparedLivePlugin* head_ = nullptr;
};
}  // namespace snd::core
