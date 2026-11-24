#ifndef SND_SOURCE_GRANULAR_KIND_HPP
#define SND_SOURCE_GRANULAR_KIND_HPP
#include "resident_synth.hpp"

namespace snd::source::detail {
inline constexpr unsigned kGrainSlots = 8;

struct Grain {
  std::uint64_t birth = 0;
  std::uint32_t length = 0;
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
  double fraction = 0;
  Grain grains[kGrainSlots]{};
};

class GranularKind final : public ResidentSynth<GranularKind, GranularVoice> {
 public:
  SourceKind Kind() const noexcept override { return SourceKind::Granular; }
  bool RendersAtOutputRate() const noexcept override { return false; }
  bool Prepare(GranularVoice&, const SourceStartRequest&, SourceStartResult&) noexcept;
  void Control(const SourceControlOp*, std::uint32_t) noexcept override;
  void RenderBatch(const source::RenderBatch&, VoiceRenderStatus*) noexcept override;
  void AdvanceVirtual(const AdvanceItem*, std::uint32_t, VoiceRenderStatus*) noexcept override;
};
}  // namespace snd::source::detail
#endif
