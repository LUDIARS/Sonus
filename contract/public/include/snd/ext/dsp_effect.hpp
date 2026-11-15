// IDspEffect (design.md 4.5, 7, 12.3). FROZEN CONTRACT.
#ifndef SND_EXT_DSP_EFFECT_HPP
#define SND_EXT_DSP_EFFECT_HPP

#include <cstdint>

#include "snd/ext.h"
#include "snd/ext/types.hpp"

namespace snd {

// Plugin effects only: the standard effects are processed per kind in batch
// loops without virtual calls (12.3). StateBytes/Init run on a normal worker
// at bank load; Process/IsTailDone run on RT workers and the audio thread and
// must obey principle 1 (no allocation, lock, IO or formatted logging). One
// state is never processed by two threads at once.
struct IDspEffect {
  virtual ~IDspEffect() = default;
  virtual std::uint32_t StateBytes(const EffectDesc& desc) const = 0;
  virtual void Init(void* state, const EffectDesc& desc, std::uint32_t sampleRate) = 0;
  // in and out may alias. frames <= desc.max_frames. Non-interleaved buffers, 64-byte aligned.
  virtual void Process(void* state, const float* const* in, float* const* out,
                       std::uint32_t channels, std::uint32_t frames, const ParamBlock& params) = 0;
  virtual bool IsTailDone(const void* state) const = 0;
};

struct EffectPlugin {
  std::uint32_t effectType;  // id (snd::HashId of the effect kind name)
  IDspEffect* effect;
};

namespace bridge {
inline std::uint32_t SND_CALL FxStateBytes(void* u, const snd_effect_desc* d) {
  return static_cast<IDspEffect*>(u)->StateBytes(*d);
}
inline void SND_CALL FxInit(void* u, void* s, const snd_effect_desc* d, std::uint32_t sr) {
  static_cast<IDspEffect*>(u)->Init(s, *d, sr);
}
inline void SND_CALL FxProcess(void* u, void* s, const float* const* in, float* const* out,
                               std::uint32_t ch, std::uint32_t frames, const snd_param_block* p) {
  static_cast<IDspEffect*>(u)->Process(s, in, out, ch, frames, *p);
}
inline std::int32_t SND_CALL FxTail(void* u, const void* s) {
  return static_cast<IDspEffect*>(u)->IsTailDone(s) ? 1 : 0;
}
}  // namespace bridge

inline snd_dsp_effect ToC(IDspEffect& e, std::uint32_t effectType) noexcept {
  return snd_dsp_effect{&e,           effectType,         0u, &bridge::FxStateBytes,
                        &bridge::FxInit, &bridge::FxProcess, &bridge::FxTail};
}

class CDspEffect final : public IDspEffect {
 public:
  explicit CDspEffect(const snd_dsp_effect& c) noexcept : c_(c) {}
  std::uint32_t StateBytes(const EffectDesc& d) const override { return c_.state_bytes(c_.user, &d); }
  void Init(void* s, const EffectDesc& d, std::uint32_t sr) override { c_.init(c_.user, s, &d, sr); }
  void Process(void* s, const float* const* in, float* const* out, std::uint32_t ch,
               std::uint32_t frames, const ParamBlock& p) override {
    c_.process(c_.user, s, in, out, ch, frames, &p);
  }
  bool IsTailDone(const void* s) const override { return c_.is_tail_done(c_.user, s) != 0; }
  std::uint32_t effectType() const noexcept { return c_.effect_type; }

 private:
  snd_dsp_effect c_;
};

}  // namespace snd

#endif  // SND_EXT_DSP_EFFECT_HPP
