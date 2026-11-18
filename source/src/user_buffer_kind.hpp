// UserBuffer kind (design.md 5.1; source_if.hpp UserBufferControl; CONTRACTS.md D-94).
//
// Plays game-owned sample memory (snd_create_user_buffer) at the buffer's rate
// (a source-rate kind: core resamples and applies the pitch).
//   Stream  a ring: the game writes ahead of the read position
//           (snd_write_user_buffer); RenderBatch reads what was written and
//           advances UserBufferControl::readPos. Missing frames are silence
//           (VoiceRenderStatus::starved once the voice had played). It never
//           ends by itself. One playback reads a stream buffer at a time (its
//           read position is the buffer's): a second start while one plays is
//           refused (SND_END_DETAIL_VOICE_LIMIT).
//   Static  written once: frames [0, frames) are played from the start, looped
//           when the buffer loops, then the voice ends.
// The control block's generation is checked at every block: a buffer released
// under a playing voice ends it (silence, `ended`).
#ifndef SND_SOURCE_USER_BUFFER_KIND_HPP
#define SND_SOURCE_USER_BUFFER_KIND_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/internal/source_if.hpp"

namespace snd::source::detail {

struct UserBufferInst {
  std::uint32_t state = 0;  // 0 free, 1 live (atomic_ref: RT workers read it)
  UserBufferControl* control = nullptr;
  std::uint32_t generation = 0;
  std::uint32_t played = 0;  // Stream: delivered frames once
};

class UserBufferKind final : public ISourceKind {
 public:
  UserBufferKind() noexcept = default;

  SourceKind Kind() const noexcept override { return SourceKind::UserBuffer; }
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
  // Reads up to `frames` frames for `in` (Stream: from readPos; Static: from *pos).
  std::uint32_t Read(UserBufferInst& in, std::uint64_t* pos, float* const* out, std::uint32_t channels,
                     std::uint32_t frames, bool* ended, bool* starved) noexcept;

  SourceServices sv_{};
  UserBufferInst* inst_ = nullptr;
  std::uint32_t* freeInst_ = nullptr;
  std::uint32_t freeCount_ = 0;
  std::uint32_t cap_ = 0;
  std::atomic<std::uint32_t> live_{0};
  std::atomic<std::uint64_t> underruns_{0};
};

}  // namespace snd::source::detail

#endif  // SND_SOURCE_USER_BUFFER_KIND_HPP
