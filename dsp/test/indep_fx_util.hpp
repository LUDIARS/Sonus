// Helpers for the independent effect tests (test-only; allocation happens
// outside RT scopes). Every instance lives in a buffer with guard bytes before
// and after exactly EffectStateBytes() bytes.
#ifndef SND_DSP_INDEP_FX_UTIL_HPP
#define SND_DSP_INDEP_FX_UTIL_HPP

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/dsp/kernels.hpp"
#include "snd/internal/rt_check.hpp"

namespace indepfx {

using namespace snd::dsp;

inline constexpr std::uint32_t kPre = 64;
inline constexpr std::uint32_t kPost = 256;
inline constexpr std::uint8_t kGuardByte = 0xA5;
inline constexpr std::uint32_t kKindCount = static_cast<std::uint32_t>(EffectKind::Count);

inline const char* Short(EffectKind k) { return GetEffectInfo(k).name + 7; }

struct Fx {
  EffectKind kind;
  snd::EffectDesc desc{};
  float params[kMaxEffectParams] = {};
  std::uint32_t bytes = 0;
  dsptest::AlignedMem mem;
  bool ok = false;

  // sizingRate: rate EffectStateBytes is asked for; runRate: rate given to EffectInit.
  Fx(EffectKind k, std::uint32_t channels, std::uint32_t maxFrames, std::uint32_t sizingRate,
     const std::vector<std::pair<std::uint32_t, float>>& overrides = {}, std::uint32_t runRate = 0, bool initOnlyAtMax = false)
      : kind(k) {
    const EffectInfo& info = GetEffectInfo(k);
    EffectDefaultParams(k, params);
    for (const auto& [i, v] : overrides) params[i] = v;
    if (initOnlyAtMax)
      for (std::uint32_t p = 0; p < info.paramCount; ++p)
        if (info.params[p].flags & kParamInitOnly) params[p] = info.params[p].maxValue;
    desc.effect_type = info.typeId;
    desc.channels = channels;
    desc.max_frames = maxFrames;
    desc.param_count = info.paramCount;
    desc.initial_params = params;
    bytes = EffectStateBytes(desc, sizingRate);
    mem.Reset(kPre + bytes + kPost);
    std::memset(mem.get(), kGuardByte, mem.size());
    std::memset(state(), 0, bytes);  // pool memory is zeroed here; see InitOnDirtyMemory for the other case
    ok = bytes > 0 && EffectInit(state(), bytes, desc, runRate != 0 ? runRate : sizingRate);
  }
  void* state() const { return static_cast<std::uint8_t*>(mem.get()) + kPre; }
  bool GuardsOk() const {
    const auto* p = static_cast<const std::uint8_t*>(mem.get());
    for (std::uint32_t i = 0; i < kPre; ++i)
      if (p[i] != kGuardByte) return false;
    for (std::uint32_t i = 0; i < kPost; ++i)
      if (p[kPre + bytes + i] != kGuardByte) return false;
    return true;
  }
};

inline void Run(const Fx& fx, const float* const* in, float* const* out, std::uint32_t channels, std::uint32_t frames,
                const snd::ParamBlock* pb = nullptr, const EffectSidechain* sc = nullptr) {
  SND_RT_SCOPE();
  EffectProcess(fx.state(), in, out, channels, frames, pb, sc);
}

// Deterministic test material: sines + noise, per channel.
inline void Material(dsptest::Audio& a, std::uint64_t start, std::uint32_t frames, float amp = 0.5f,
                     std::uint32_t rate = 48000) {
  for (std::uint32_t c = 0; c < a.channels(); ++c) {
    dsptest::Noise nz(static_cast<std::uint32_t>(start * 7 + c * 131 + 1));
    for (std::uint32_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(start + i) / rate;
      a.ch(c)[i] = amp * (0.6f * static_cast<float>(std::sin(2 * dsptest::kPi * (180.0 + 70.0 * c) * t)) +
                          0.25f * static_cast<float>(std::sin(2 * dsptest::kPi * 2900.0 * t)) + 0.15f * nz.Next());
    }
  }
}

// max |x| over all channels; NaN counts as +inf.
inline float MaxAbs(const dsptest::Audio& a, std::uint32_t frames) {
  float m = 0.0f;
  for (std::uint32_t c = 0; c < a.channels(); ++c)
    for (std::uint32_t i = 0; i < frames; ++i) {
      const float v = std::fabs(a.ch(c)[i]);
      if (std::isnan(v)) return INFINITY;
      m = v > m ? v : m;
    }
  return m;
}

}  // namespace indepfx

#endif  // SND_DSP_INDEP_FX_UTIL_HPP
