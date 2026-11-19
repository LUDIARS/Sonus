// snd_create's configuration check (A15-3, CONTRACTS.md D-91). Pure functions.
//
// NormalizeConfig copies the caller's snd_config into a full current-layout
// config:
//   - contract_version: the major must equal SND_CONTRACT_VERSION_MAJOR, else
//     SND_ERR_VERSION_MISMATCH. A newer minor is accepted (its fields are a
//     longer tail that this runtime ignores).
//   - struct_size: the 1.0 layout (ends before max_voice_channels), the 1.1
//     layout (before max_buses), the first 1.2 layout (before file_system_root,
//     D-102 / D-103) and this one or longer are accepted; the 1.0 layout is the
//     smallest; a struct_size inside a field, or smaller than 1.0, is
//     SND_ERR_VERSION_MISMATCH. The fields a shorter struct does not have get
//     the defaults of snd_config_init ("struct_size の補完": a 1.0 caller gets
//     the 1.1 and 1.2 defaults, a 1.1 caller the 1.2 defaults, a first-1.2 caller
//     the host defaults: no file_system_root, SND_OUTPUT_PLATFORM).
//   - the result has struct_size = sizeof(snd_config) and the current version.
// ValidateConfig then checks what snd_create itself must refuse before anything
// is allocated (SND_ERR_INVALID_ARGUMENT):
//   max_voice_channels 1..SND_MAX_CHANNELS; max_pitch_ratio finite and in
//   [1, SND_MAX_PITCH_RATIO] (NaN / infinities / < 1 / > 32); the 1.2
//   capacities 1..65535; plugin tables (count > 0 needs a pointer, at most 32 each);
//   max_registered_threads 1..63 when the live link runs (it takes one more);
//   1.2 (D-102, D-103): output_kind is SND_OUTPUT_PLATFORM / SILENT / MANUAL, and
//   PLATFORM when a game device is given; file_system_root of at most 2046 bytes.
//   (A root together with a game file system is refused by snd_create itself, which
//   sees the file_system argument.)
// Everything else (rates, block size, voice counts, arenas ...) is checked by
// core::System::Create with the same error.
#ifndef SND_API_COMPOSE_CONFIG_CHECK_HPP
#define SND_API_COMPOSE_CONFIG_CHECK_HPP

#include <cstddef>
#include <cstdint>

#include "snd/config.h"

namespace snd::compose {

// Sizes of the earlier layouts (prefixes of snd_config).
inline constexpr std::size_t kConfigSize10 = offsetof(snd_config, max_voice_channels);
inline constexpr std::size_t kConfigSize11 = offsetof(snd_config, max_buses);
// 1.2 as released (8c81f4e), before the host fields of D-102 / D-103.
inline constexpr std::size_t kConfigSize12 = offsetof(snd_config, file_system_root);
// The built-in file system's buffer for the root (io::DefaultFileSystem): length + NUL < this.
inline constexpr std::size_t kMaxFileSystemRootBytes = 2048;
static_assert(kConfigSize10 % 8 == 0 && kConfigSize11 % 8 == 0 && kConfigSize12 % 8 == 0,
              "layout prefixes end on 8-byte boundaries");

snd_result NormalizeConfig(const snd_config* in, snd_config* out) noexcept;
// `liveLink`: the live link will run (it needs one more registered thread).
snd_result ValidateConfig(const snd_config& c, bool liveLink) noexcept;
// True when this build runs the live link for `c` (Config.livelink.enabled and SND_LIVELINK=1).
bool LiveLinkWanted(const snd_config& c) noexcept;

}  // namespace snd::compose

#endif  // SND_API_COMPOSE_CONFIG_CHECK_HPP
