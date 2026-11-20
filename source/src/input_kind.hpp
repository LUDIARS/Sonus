// Input kind (design.md 5 "Input"; source_if.hpp InputParams; CONTRACTS.md D-194).
//
// Plays an input port (snd/source/input_port.hpp; snd_input_desc::device_index =
// port index) as a voice. A source-rate kind (D-22): it hands core the port's
// frames and core resamples (and applies the voice pitch); the port's rate goes
// to core through SourceStartResult::reserved (D-194).
//   Start      claims the port (one voice per port at a time, a second start is
//              refused with SND_END_DETAIL_VOICE_LIMIT) and skips what was
//              written before: the voice plays live. An unknown port, a missing
//              desc or no port at all: the start fails (core ends the playback).
//   Render     reads what was written; missing frames are silence, reported as
//              `starved` once the voice had delivered frames (an underrun). The
//              voice never ends by itself.
//   Virtual    the frames the voice would have played are consumed (discarded), so
//              the voice stays live when it becomes real again.
//   Control    Seek / Resume / Devirtualize: skip to live (a paused voice resumes
//              with what is written from then on, not with the backlog). Pause,
//              Virtualize and SetLoop: nothing (live input has no position).
// Threads: Start / Release / Control / AdvanceVirtual on the audio thread,
// RenderBatch on RT workers or the audio thread; the port's producer anywhere.
// A port's consumer position is touched only by its claiming voice (render
// state, D-43): core sends no Control to a held voice and releases none.
#ifndef SND_SOURCE_INPUT_KIND_HPP
#define SND_SOURCE_INPUT_KIND_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/internal/source_if.hpp"
#include "snd/source/input_port.hpp"

namespace snd::source::detail {

struct InputInst {
  std::uint32_t state = 0;  // 0 free, 1 live (atomic_ref: RT workers read it)
  std::uint32_t port = 0;
  std::uint32_t played = 0;  // delivered frames once (RenderBatch only)
};

class InputKind final : public ISourceKind {
 public:
  InputKind() noexcept = default;

  // Owner thread, before core's Create: the ports (Runtime-owned).
  void BindPorts(InputPort* ports, std::uint32_t count) noexcept {
    ports_ = ports;
    portCount_ = count;
  }

  SourceKind Kind() const noexcept override { return SourceKind::Input; }
  bool RendersAtOutputRate() const noexcept override { return false; }
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
  std::uint64_t Underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }

 private:
  bool Live(const InputInst& in) const noexcept {
    return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(in.state)).load(std::memory_order_acquire) == 1u;
  }

  InputPort* ports_ = nullptr;
  std::uint32_t portCount_ = 0;
  std::uint32_t sampleRate_ = 48000;
  InputInst* inst_ = nullptr;
  std::uint32_t* freeInst_ = nullptr;
  std::uint32_t* portOwner_ = nullptr;  // audio thread: claiming instance per port (kInvalidSlot: none)
  std::uint32_t freeCount_ = 0;
  std::uint32_t cap_ = 0;
  std::atomic<std::uint32_t> live_{0};
  std::atomic<std::uint64_t> underruns_{0};
};

}  // namespace snd::source::detail

#endif  // SND_SOURCE_INPUT_KIND_HPP
