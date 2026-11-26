#ifndef SND_SOURCE_PATCH_KIND_HPP
#define SND_SOURCE_PATCH_KIND_HPP
#include "patch_decode.hpp"
#include "resident_synth.hpp"

namespace snd::source::detail {
struct PatchState {
  std::uint32_t phase = 0, initialPhase = 0;
  double baseDt = 0, filter = 0, coefficient = 0;
  float modStart = 0, modEnd = 0;
  std::uint64_t key = 0;
};
struct PatchVoice {
  bool live = false, finiteDuration = false;
  unsigned count = 0;
  std::uint64_t frame = 0, gate = 0, end = 0;
  bool refresh = true;
  PatchNode nodes[kPatchMaxNodes]{};
  PatchState state[kPatchMaxNodes]{};
};

class PatchKind final : public ResidentSynth<PatchKind, PatchVoice> {
 public:
  SourceKind Kind() const noexcept override { return SourceKind::Patch; }
  bool RendersAtOutputRate() const noexcept override { return true; }
  bool Prepare(PatchVoice&, const SourceStartRequest&, SourceStartResult&) noexcept;
  void Control(const SourceControlOp*, std::uint32_t) noexcept override;
  void RenderBatch(const source::RenderBatch&, VoiceRenderStatus*) noexcept override;
  void AdvanceVirtual(const AdvanceItem*, std::uint32_t, VoiceRenderStatus*) noexcept override;
};
}  // namespace snd::source::detail
#endif
