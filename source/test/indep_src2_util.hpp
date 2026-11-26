// Test fixtures using only the public source and internal core/source contracts.
#ifndef SND_TEST_INDEP_SRC2_UTIL_HPP
#define SND_TEST_INDEP_SRC2_UTIL_HPP
#include <array>
#include <vector>
#include "snd/source/patch_program.hpp"
#include "synth_driver.hpp"

namespace indep2 {
using synthtest::Driver;
namespace src = snd::source;
using src::PatchNode;
using src::PatchOp;

inline src::SourceAsset Pcm(const std::vector<float>& data, unsigned rate, unsigned channels = 1) {
  src::SourceAsset a{};
  a.codec.codec = SND_CODEC_PCM_F32;
  a.codec.channels = channels;
  a.codec.total_frames = data.size() / channels;
  a.codec.sample_rate = rate;
  a.dataBytes = data.size() * sizeof(float);
  a.data = reinterpret_cast<const std::uint8_t*>(data.data());
  return a;
}

inline std::vector<std::uint8_t> Program(const std::vector<PatchNode>& nodes) {
  const auto count = static_cast<unsigned>(nodes.size());
  std::vector<std::uint8_t> bytes(src::PatchProgramBytes(count));
  if (!src::BuildPatchProgram(nodes.data(), count, bytes.data(), bytes.size())) bytes.clear();
  return bytes;
}

inline src::PatchParams Patch(const std::vector<std::uint8_t>& bytes, std::uint64_t gate = 0) {
  return {bytes.data(), static_cast<unsigned>(bytes.size()), 0, gate};
}

// One custom item for offset ramps, scratch shortage and concurrent independent chunks.
inline std::vector<float> Item(Driver& d, Driver::Voice& voice, unsigned frames,
                               src::RenderItem it = {}, unsigned scratchBytes = 128) {
  std::vector<float> result(frames);
  auto* plane = result.data();
  it.out = &plane;
  it.sourceSlot = voice.slot;
  it.channels = 1;
  it.playhead = &voice.ph;
  it.framesRequested = frames;
  alignas(64) std::array<std::uint8_t, 128> storage{};
  src::Scratch scratch{storage.data(), scratchBytes, 0};
  src::RenderBatch b{};
  b.kind = d.Kind().Kind();
  b.count = 1;
  b.items = &it;
  b.block.frames = frames;
  b.scratch = &scratch;
  {
    snd::rt::RtScope rt;
    d.Kind().RenderBatch(b, &voice.last);
  }
  return result;
}
}  // namespace indep2
#endif
