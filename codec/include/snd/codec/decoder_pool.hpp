// Decoder pool (design.md 3.3, CONTRACTS.md D-14).
//
// maxDecoders fixed slots of decoderSlotBytes (rounded up to 64), 64-byte
// aligned, taken once at initialisation (IAllocator, MemTag::DecoderPool, or
// memory given by the caller). A codec state whose StateBytes exceeds the slot
// size is rejected (StateTooLarge); an empty pool rejects with Full. Both map
// to SND_END_DETAIL_DECODER_POOL_FULL (EndDetailFor).
//
// Threads (decided in stage 1):
//   - Init / Shutdown: owner thread (snd_create / snd_destroy). Shutdown
//     requires every slot to be returned.
//   - Acquire: the audio thread, when RunEventLogic accepts a request and the
//     source kind starts a voice that decodes (ISourceKind::Start). Normal
//     workers may also call it.
//   - Release: the audio thread (ISourceKind::Release when no decode job is in
//     flight) or a normal worker of the prefetch lane (when the last in-flight
//     decode job of a released voice finishes, epoch reclamation 3.3).
//   - State / Usage / SlotBytes / Capacity: any thread.
// Acquire and Release are lock-free (a tagged Treiber stack over slot indices;
// no allocation, no lock, no system call) and may run concurrently on any
// number of threads. Usage counters are relaxed atomics: a snapshot taken
// while other threads acquire/release is approximate.
//
// Recommendation for source: compute ICodec::StateBytes when the asset is
// prepared (bank load, normal worker) and pass the cached value to Acquire on
// the audio thread. A plugin's state_bytes is "pure" by contract but is not
// promised to be RT-safe.
#ifndef SND_CODEC_DECODER_POOL_HPP
#define SND_CODEC_DECODER_POOL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/types.h"

namespace snd::codec {

enum class DecoderPoolStatus : std::uint32_t {
  Ok = 0,
  Full = 1,            // every slot is in use
  StateTooLarge = 2,   // stateBytes > slot size (D-14)
  NotInitialized = 3,
};
const char* DecoderPoolStatusName(DecoderPoolStatus s) noexcept;

// End detail of a playback rejected for this status (SND_END_DETAIL_NONE for Ok).
constexpr snd_end_detail EndDetailFor(DecoderPoolStatus s) noexcept {
  return s == DecoderPoolStatus::Ok ? SND_END_DETAIL_NONE : SND_END_DETAIL_DECODER_POOL_FULL;
}

inline constexpr std::uint32_t kInvalidDecoderSlot = 0xFFFFFFFFu;

struct DecoderSlot {
  std::uint32_t index = kInvalidDecoderSlot;
  void* state = nullptr;  // 64-byte aligned, SlotBytes() bytes
};

struct DecoderPoolUsage {
  std::uint32_t capacity;       // slots
  std::uint32_t slotBytes;      // per slot (rounded up to 64)
  std::uint64_t poolBytes;      // memory held by the pool (slots + bookkeeping)
  std::uint32_t inUse;          // slots currently acquired
  std::uint32_t peakInUse;      // since Init
  std::uint64_t stateBytesInUse;  // sum of the stateBytes requested by acquired slots
  std::uint64_t acquired;       // successful Acquire calls
  std::uint64_t rejectedFull;
  std::uint64_t rejectedTooLarge;
  std::uint64_t badReleases;    // Release of a free or out-of-range slot (ignored)
};

class DecoderPool {
 public:
  DecoderPool() noexcept = default;
  ~DecoderPool();
  DecoderPool(const DecoderPool&) = delete;
  DecoderPool& operator=(const DecoderPool&) = delete;

  // Bytes Init(memory, ...) needs; 0 if the size overflows.
  static std::size_t RequiredBytes(std::uint32_t maxDecoders, std::uint32_t slotBytes) noexcept;

  // Uses caller memory (64-byte aligned, >= RequiredBytes). Owner thread.
  bool Init(void* memory, std::size_t bytes, std::uint32_t maxDecoders, std::uint32_t slotBytes) noexcept;
  // Allocates RequiredBytes with MemTag::DecoderPool. Owner thread.
  bool Init(IAllocator& allocator, std::uint32_t maxDecoders, std::uint32_t slotBytes) noexcept;
  // Returns the memory if it came from an allocator. Owner thread.
  void Shutdown() noexcept;

  // Lock-free. On Ok, *out receives the slot; otherwise *out is reset.
  DecoderPoolStatus Acquire(std::uint32_t stateBytes, DecoderSlot* out) noexcept;
  // Lock-free. False (and counted in badReleases) for a free or invalid index.
  bool Release(std::uint32_t index) noexcept;

  void* State(std::uint32_t index) const noexcept;
  std::uint32_t SlotBytes() const noexcept { return slotBytes_; }
  std::uint32_t Capacity() const noexcept { return capacity_; }
  bool Initialized() const noexcept { return slots_ != nullptr || initializedEmpty_; }
  DecoderPoolUsage Usage() const noexcept;

 private:
  bool Setup(void* memory, std::size_t bytes, std::uint32_t maxDecoders, std::uint32_t slotBytes) noexcept;

  std::uint8_t* slots_ = nullptr;
  std::atomic<std::uint32_t>* next_ = nullptr;     // free-list links
  std::atomic<std::uint32_t>* reserved_ = nullptr; // requested stateBytes + 1 while acquired, 0 when free
  void* owned_ = nullptr;
  IAllocator* allocator_ = nullptr;
  std::size_t poolBytes_ = 0;
  std::uint32_t capacity_ = 0;
  std::uint32_t slotBytes_ = 0;
  bool initializedEmpty_ = false;

  std::atomic<std::uint64_t> head_{0};  // (tag << 32) | top index
  std::atomic<std::uint32_t> inUse_{0};
  std::atomic<std::uint32_t> peakInUse_{0};
  std::atomic<std::uint64_t> stateBytesInUse_{0};
  std::atomic<std::uint64_t> acquired_{0};
  std::atomic<std::uint64_t> rejectedFull_{0};
  std::atomic<std::uint64_t> rejectedTooLarge_{0};
  std::atomic<std::uint64_t> badReleases_{0};
};

}  // namespace snd::codec

#endif  // SND_CODEC_DECODER_POOL_HPP
