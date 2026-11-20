// Oscillator kind (design.md 5 "Oscillator"; source_if.hpp OscillatorParams;
// CONTRACTS.md D-190, D-191, D-192).
//
// Renders at the internal rate (RendersAtOutputRate() == true) and applies the
// voice pitch itself: frequency = OscillatorParams::frequencyHz x the rate ratio,
// interpolated per sample from RenderItem::pitch (start -> end of the block), so
// a Parameter that moves the pitch (CurveTarget::Pitch) moves the frequency
// without zipper noise. The phase is continuous across blocks, pitch changes,
// virtualisation (AdvanceVirtual advances it in closed form) and late chunks.
//
// Waveforms (bank_format::Waveform), all at peak 1:
//   Sine      sin(2 pi phase), starts at phase 0.
//   Saw       rising, PolyBLEP at the wrap; starts at phase 0.5 (value 0).
//   Square    duty 0.5 (OscillatorParams::duty ignored), PolyBLEP at both edges;
//             starts at +1 (phase 0).
//   Triangle  PolyBLAMP at both corners (the integral of the band-limited square
//             residual); starts at phase 0.25 (value 0, rising).
//   Pulse     duty = OscillatorParams::duty + RenderItem::dutyOffset (per sample,
//             clamped to [0.001, 0.999]), PolyBLEP at both edges, DC removed
//             (the output is the pulse minus its mean 2 duty - 1), starts at phase 0.
// Wavetable (D-192): OscillatorParams::wavetable != nullptr (core passes the SYNP
// asset for SynthKind::Wavetable, and for any synth that names an asset). The
// asset must be resident PCM (PCMF / PC16, LoadForm::PcmResident); channel 0 is
// read. Table length T = loopEnd - loopStart when the asset has a loop, else 2048
// when the asset's frames are a multiple of 2048, else all its frames (one table).
// Table k starts at loopStart + k T with a loop (D-198), at k T without one;
// tables = (frames - origin) / T. Position = tablePosition + RenderItem::tablePositionOffset
// in [0, 1] over tables 0 .. count - 1, linear between neighbouring tables and
// linear inside a table. No band limiting (no mip levels in bank format 2.0).
// Frequencies at or above Nyquist are clamped to just below it (dt < 0.5).
// D-198: while the effective frequency is 0 (0, negative, NaN, +-Inf frequency or
// pitch) every waveform outputs 0 and the phase waits; it continues from there once
// the frequency is positive again. RenderItem inputs that are not finite are made
// safe per block (pitch -> 0, offsets -> 0): no NaN out, no undefined conversion.
//
// Control: Seek(frame) sets the phase to where an unmodulated voice would be
// after `frame` output frames (start phase + frame x f / rate). Pause / Resume /
// Virtualize / Devirtualize / SetLoop need nothing (the phase stays; core does
// not render a paused voice and advances a virtual one). The voice never ends by
// itself.
#ifndef SND_SOURCE_OSCILLATOR_KIND_HPP
#define SND_SOURCE_OSCILLATOR_KIND_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/internal/source_if.hpp"

namespace snd::source::detail {

struct OscInst {
  std::uint32_t state = 0;  // 0 free, 1 live (atomic_ref: RT workers read it)
  std::uint32_t shape = 0;  // Waveform, or kShapeWavetable
  double phase = 0.0;       // [0, 1) (render state)
  double baseDt = 0.0;      // frequencyHz / sampleRate (cycles per output sample at pitch 1)
  double startPhase = 0.0;
  float duty = 0.5f;
  float tablePosition = 0.0f;
  // Wavetable.
  const std::uint8_t* table = nullptr;  // resident PCM, interleaved
  std::uint32_t tableCodec = 0;
  std::uint32_t tableChannels = 0;
  std::uint32_t tableLength = 0;
  std::uint32_t tableCount = 0;
  std::uint32_t tableOrigin = 0;  // first frame of table 0 (loopStart with a loop, D-198)
};

class OscillatorKind final : public ISourceKind {
 public:
  static constexpr std::uint32_t kShapeWavetable = 100;

  OscillatorKind() noexcept = default;

  SourceKind Kind() const noexcept override { return SourceKind::Oscillator; }
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

 private:
  OscInst* inst_ = nullptr;
  std::uint32_t* freeInst_ = nullptr;
  std::uint32_t freeCount_ = 0;
  std::uint32_t cap_ = 0;
  double sampleRate_ = 48000.0;
  std::atomic<std::uint32_t> live_{0};
};

}  // namespace snd::source::detail

#endif  // SND_SOURCE_OSCILLATOR_KIND_HPP
