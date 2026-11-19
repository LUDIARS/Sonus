// snd_create's configuration check (config_check.hpp).
#include "config_check.hpp"

#include <cmath>
#include <cstring>

namespace snd::compose {

snd_result NormalizeConfig(const snd_config* in, snd_config* out) noexcept {
  if (in == nullptr || out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if ((in->contract_version >> 16) != SND_CONTRACT_VERSION_MAJOR) return SND_ERR_VERSION_MISMATCH;
  const std::size_t size = in->struct_size;
  // A known layout end: 1.0, 1.1, this version, or a longer (newer minor) struct.
  if (size != kConfigSize10 && size != kConfigSize11 && size < sizeof(snd_config)) return SND_ERR_VERSION_MISMATCH;
  snd_config_init(out);  // defaults for the fields a shorter struct does not have
  std::memcpy(static_cast<void*>(out), in, size < sizeof(snd_config) ? size : sizeof(snd_config));
  out->struct_size = static_cast<std::uint32_t>(sizeof(snd_config));
  out->contract_version = SND_CONTRACT_VERSION;
#if !SND_LIVELINK
  out->livelink.enabled = 0u;  // ignored when built with SND_LIVELINK=0 (config.h)
#endif
  return SND_OK;
}

bool LiveLinkWanted(const snd_config& c) noexcept {
#if SND_LIVELINK
  return c.livelink.enabled != 0u;
#else
  (void)c;
  return false;
#endif
}

snd_result ValidateConfig(const snd_config& c, bool liveLink) noexcept {
  if (c.max_voice_channels == 0u || c.max_voice_channels > SND_MAX_CHANNELS) return SND_ERR_INVALID_ARGUMENT;
  if (!std::isfinite(c.max_pitch_ratio) || c.max_pitch_ratio < 1.0f || c.max_pitch_ratio > SND_MAX_PITCH_RATIO) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  const std::uint32_t caps[] = {c.max_buses,        c.max_categories, c.max_snapshots,
                                c.max_limit_groups, c.max_param_defs, c.max_duck_rules};
  for (const std::uint32_t v : caps) {
    if (v == 0u || v > 0xFFFFu) return SND_ERR_INVALID_ARGUMENT;
  }
  if ((c.plugins.codec_count != 0u && c.plugins.codecs == nullptr) || c.plugins.codec_count > 32u) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  if ((c.plugins.effect_count != 0u && c.plugins.effects == nullptr) || c.plugins.effect_count > 32u) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  if (c.max_registered_threads == 0u) return SND_ERR_INVALID_ARGUMENT;
  if (liveLink && c.max_registered_threads >= 64u) return SND_ERR_INVALID_ARGUMENT;  // core takes at most 64
  return SND_OK;
}

}  // namespace snd::compose
