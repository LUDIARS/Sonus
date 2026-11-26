#include "snd/core/live_plugin_pool.hpp"

#include <new>
#include "snd/dsp/effects.hpp"

namespace snd::core {
namespace {
constexpr std::size_t kHeader = (sizeof(PreparedLivePlugin) + 63u) & ~std::size_t{63};
void FreeNode(IAllocator& a, PreparedLivePlugin* node) noexcept {
  node->~PreparedLivePlugin();
  a.Free(node, MemTag::LiveLink);
}
}
LivePluginPool::~LivePluginPool() {
  while (head_) {
    auto* next = head_->next;
    FreeNode(*alloc_, head_);
    head_ = next;
  }
}
snd_result LivePluginPool::Prepare(IAllocator& alloc, const snd_config& cfg, const snd_dsp_effect& plugin,
                                   std::uint32_t channels, const float* params, std::uint32_t count,
                                   std::uint64_t limit, PreparedLivePlugin** out) noexcept {
  *out = nullptr;
  if (count > dsp::kMaxEffectParams) return SND_ERR_INVALID_ARGUMENT;
  if (!channels || channels > kMaxChannels || !plugin.state_bytes || !plugin.init || !plugin.process)
    return SND_ERR_UNSUPPORTED;
  snd_effect_desc d{};
  d.effect_type = plugin.effect_type;
  d.channels = channels;
  d.sample_rate = cfg.sample_rate;
  d.max_frames = cfg.block_size;
  d.param_count = count;
  d.initial_params = params;
  const auto size = dsp::PluginStateBytes(plugin, d);
  if (!size) return SND_ERR_UNSUPPORTED;
  const std::uint64_t needed = kHeader + std::uint64_t{size};
  if (needed > limit || size > cfg.dev_reserve.bytes) return SND_ERR_OUT_OF_BUDGET;
  base::LockGuard lock(mutex_);
  alloc_ = &alloc;
  std::uint64_t used = 0;
  auto** at = &head_;
  while (*at) {
    auto* node = *at;
    if (node->held.load(std::memory_order_acquire)) {
      used += kHeader + node->bytes;
      at = &node->next;
    } else {
      *at = node->next;
      FreeNode(alloc, node);
    }
  }
  if (used > limit - needed) return SND_ERR_OUT_OF_BUDGET;
  auto* mem = static_cast<std::uint8_t*>(alloc.Allocate(static_cast<std::size_t>(needed), 64, MemTag::LiveLink));
  if (!mem) return SND_ERR_OUT_OF_BUDGET;
  auto* node = new (mem) PreparedLivePlugin;
  node->plugin = &plugin;
  node->state = mem + kHeader;
  node->bytes = size;
  node->channels = channels;
  plugin.init(plugin.user, node->state, &d, d.sample_rate);
  node->next = head_;
  head_ = node;
  *out = node;
  return SND_OK;
}
}  // namespace snd::core
