// Noise kind (design.md 5 "Noise", 2.5; source_if.hpp NoiseParams; CONTRACTS.md D-193).
//
// Renders at the internal rate (RendersAtOutputRate() == true) and applies the
// voice pitch itself: the noise is a sequence of "source samples" Y(c, k) that the
// voice plays at the rate ratio of RenderItem::pitch (per-sample ramp), linearly
// interpolated between neighbours (at ratio 1 the output is Y itself; below 1 the
// noise gets darker, above it brighter until Nyquist).
//
// Determinism (2.5): the white sequence is counter based, W(c, k) =
// splitmix64(channelKey(c) + (k + 1) * golden) -> 24-bit uniform in [-1, 1), with
// key = splitmix64(NoiseParams::seed << 32 | SourceStartRequest::seed) (the bank's
// synth seed and the per-playback seed, which core derives from the playback's
// random state and the voice slot). The source position is fixed point (32.32),
// the colour filters run once per source sample in order. Hence the output is a
// function of the key, the pitch per sample and nothing else: bit-identical for
// the same seed whatever the block size, the chunking or the worker that renders.
// Channels (D-193): every channel has its own key (channelKey(c) =
// splitmix64(key ^ (c + 1) * golden)): the channels are uncorrelated noises of the
// same colour. Mono noise is a 1-channel synth (pan / spread give the image).
//
// Colours (all scaled to the variance of the uniform white noise, RMS 1/sqrt(3)):
//   white  W
//   pink   -3 dB/oct: Kellett's 3-pole economy filter (b0 0.99765, b1 0.963,
//          b2 0.57 + a direct term), within about 0.5 dB of 1/f above ~10 Hz
//   brown  -6 dB/oct above ~38 Hz: leaky integrator y = 0.995 y + sqrt(1 - 0.995^2) W
//
// Virtual voices / Seek: AdvanceVirtual moves the source position in O(1) (white
// stays exactly on its sequence). The pink / brown filter state is then rebuilt by
// the next RenderBatch from the last kWarmFrames source samples (from zero state);
// with the slowest pole (0.99765) the state then differs from an uninterrupted run
// by < 1e-4 of its RMS (the voice fades in from virtual anyway, D-15). Seek(frame)
// moves to source sample `frame`. Pause / Resume / Virtualize / Devirtualize /
// SetLoop need nothing. The voice never ends by itself.
#ifndef SND_SOURCE_NOISE_KIND_HPP
#define SND_SOURCE_NOISE_KIND_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/internal/source_if.hpp"

namespace snd::source::detail {

struct NoiseInst {
  std::uint32_t state = 0;  // 0 free, 1 live (atomic_ref: RT workers read it)
  std::uint32_t color = 0;
  std::uint32_t channels = 1;
  std::uint32_t stale = 0;     // the filter state must be rebuilt (render state)
  std::uint64_t index = 0;     // source sample of y0 (render state)
  std::uint32_t fraction = 0;  // 2^-32 source samples past `index`
  std::uint32_t reserved = 0;
  std::uint64_t key[kMaxChannels] = {};
  float y0[kMaxChannels] = {};  // Y(c, index)
  float y1[kMaxChannels] = {};  // Y(c, index + 1)
  float f[kMaxChannels][3] = {};  // colour filter state after y1
};

class NoiseKind final : public ISourceKind {
 public:
  static constexpr std::uint32_t kWarmFrames = 4096;

  NoiseKind() noexcept = default;

  SourceKind Kind() const noexcept override { return SourceKind::Noise; }
  bool RendersAtOutputRate() const noexcept override { return true; }
  std::size_t PoolBytes(const SourceLimits& limits) const noexcept override;
  bool Init(void* pool, std::size_t bytes, const SourceLimits& limits, const SourceServices& services) noexcept override;
  void Shutdown() noexcept override;
  void Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept override;
  void Release(const SlotIndex* sourceSlots, std::uint32_t count) noexcept override;
  void Control(const SourceControlOp* ops, std::uint32_t count) noexcept override;
  void QueryReady(const SlotIndex* sourceSlots, std::uint32_t count, std::uint8_t* ready) const noexcept override;
  void RenderBatch(const source::RenderBatch& batch, VoiceRenderStatus* status) noexcept override;
  void AdvanceVirtual(const AdvanceItem* items, std::uint32_t count, VoiceRenderStatus* status) noexcept override;
  void ScheduleBackground(const BlockInfo&) noexcept override {}
  std::uint32_t PrimeFrames(const SourceAsset&, std::uint64_t) const noexcept override { return 0; }

  std::uint32_t LiveInstances() const noexcept { return live_.load(std::memory_order_relaxed); }

  // The white sequence (exposed for tests): uniform in [-1, 1).
  static float White(std::uint64_t channelKey, std::uint64_t index) noexcept;
  static std::uint64_t ChannelKey(std::uint64_t key, std::uint32_t channel) noexcept;
  static std::uint64_t VoiceKey(std::uint32_t paramsSeed, std::uint32_t startSeed) noexcept;

 private:
  NoiseInst* inst_ = nullptr;
  std::uint32_t* freeInst_ = nullptr;
  std::uint32_t freeCount_ = 0;
  std::uint32_t cap_ = 0;
  float pinkScale_ = 1.0f;   // Init: output variance = white variance
  float brownGain_ = 0.0f;   // Init: sqrt(1 - a^2)
  std::atomic<std::uint32_t> live_{0};

  float Step(std::uint32_t color, float* f, float w) const noexcept;
  void Rebuild(NoiseInst& in) const noexcept;
};

}  // namespace snd::source::detail

#endif  // SND_SOURCE_NOISE_KIND_HPP
