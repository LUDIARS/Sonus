#pragma once
#include <bit>
#include <cstring>
#include "snd/core/core.hpp"
#include "snd/core/live_plugin_pool.hpp"
#include "snd/livelink/struct_edit.hpp"
#include "snd/livelink/value_applier.hpp"
#include "snd/livelink/wire.hpp"

namespace snd::livelink {
// After wire validation, on a normal thread. For replay the recording retains the
// accepted layout; for live commands the audio thread publishes a layout by ID.
inline snd_result PrepareExternalEffect(core::LivePluginPool& pool, IAllocator& alloc,
                                        const snd_config& cfg, core::System* live, PendingChunk& p,
                                        std::uint64_t limit) noexcept {
  if (cfg.plugins.effect_count && !cfg.plugins.effects) return SND_ERR_INVALID_ARGUMENT;
  // A nonzero recorded layout identifies an externally prepared instance, including
  // plugin overrides of builtin names. A missing plugin cannot reproduce that run.
  const auto missing = !live && p.bankId ? SND_ERR_UNSUPPORTED : SND_OK;
  if (!cfg.plugins.effect_count) return missing;
  StructEdit h;
  std::memcpy(&h, p.data, sizeof(h));
  if (h.op != static_cast<std::uint32_t>(StructOp::AddEffect)) return SND_OK;
  const auto* args = reinterpret_cast<const std::uint64_t*>(p.data + sizeof(h));
  for (std::uint32_t i = 0; i < cfg.plugins.effect_count; ++i) {
    const auto& plugin = cfg.plugins.effects[i];
    if (plugin.effect_type != args[0]) continue;
    const auto channels = live ? live->PublishedBusChannels(h.targetId) : p.bankId;
    if (!channels) return SND_ERR_NOT_FOUND;
    float params[kMaxStructEffectParams]{};
    for (std::uint32_t j = 2; j < h.argCount; ++j) params[j - 2] = std::bit_cast<float>(static_cast<std::uint32_t>(args[j]));
    auto r = pool.Prepare(alloc, cfg, plugin, channels, params, h.argCount - 2, limit, &p.prepared);
    if (r == SND_OK) p.bankId = channels;  // StructEdit has no bankId; keep its accepted channel count for replay
    return r;
  }
  return missing;
}

inline void CancelPreparedEffect(PendingChunk& p) noexcept {
  if (p.prepared) {
    p.prepared->held.store(false, std::memory_order_release);
    p.prepared = nullptr;
  }
}
}  // namespace snd::livelink
