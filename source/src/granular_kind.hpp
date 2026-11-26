#ifndef SND_SOURCE_GRANULAR_KIND_HPP
#define SND_SOURCE_GRANULAR_KIND_HPP
#include <array>

#include "resident_synth.hpp"

namespace snd::source::detail {
inline constexpr unsigned kGrainSlots = 8;

struct Grain {
  std::uint64_t birth = 0;
  std::uint32_t length = 0;
  double inverseLength = 0;
  // Adjacent cosine samples for the periodic Hann; re-anchor on an absolute
  // 32-sample grid so rendering and virtual returns follow the same recurrence.
  double twiceCosStep = 0, hannCos = 0, hannNextCos = 0;
  unsigned hannAge = UINT32_MAX;
  double origin = 0;
  double stride = 1;
};

struct GranularVoice {
  bool live = false;
  GranularParams params{};
  SourceAsset asset{};
  std::uint64_t begin = 0, end = 0;
  bool loop = false;
  std::uint64_t frame = 0, serial = 0, key = 0;
  std::uint32_t phase = UINT32_MAX;
  std::array<std::uint32_t, 5> fraction{};  // Q0.160, least significant word first
  Grain grains[kGrainSlots]{};
};

class GranularKind final : public ResidentSynth<GranularKind, GranularVoice> {
 public:
  std::size_t PoolBytes(const SourceLimits&) const noexcept override;
  bool Init(void*, std::size_t, const SourceLimits&, const SourceServices&) noexcept override;
  SourceKind Kind() const noexcept override { return SourceKind::Granular; }
  bool RendersAtOutputRate() const noexcept override { return false; }
  bool Prepare(GranularVoice&, const SourceStartRequest&, SourceStartResult&) noexcept;
  void Control(const SourceControlOp*, std::uint32_t) noexcept override;
  void RenderBatch(const source::RenderBatch&, VoiceRenderStatus*) noexcept override;
  void AdvanceVirtual(const AdvanceItem*, std::uint32_t, VoiceRenderStatus*) noexcept override;
 private:
  float* history_ = nullptr;
  std::uint32_t historyFrames_ = 0, historyChannels_ = 0;
};
}  // namespace snd::source::detail
#endif
