// Stream pool: fixed-size chunks for the compressed stream rings (design.md 3.3, 6).
//
// Config.memory.streamPoolBytes is cut into chunks of streamChunkBytes once at
// initialisation. A chunk is borrowed by one owner (a stream slot) and given
// back after use:
//
//   source (audio thread)  Acquire(slot)  --- dst of an IoReadRequest --.
//   IO thread              Acquire(slot)  (request posted with dst=null) |
//                          fills it, delivers it in a ChunkRecord        v
//   decode job             StreamChannel::Release(record) -> Release(chunk)
//
// Thread rules:
//   Init / Destroy        non-RT, owner thread (composition root), no other
//                         thread may use the pool meanwhile.
//   everything else       any thread, RT-safe and lock-free (atomics and a
//                         lock-free free list; a CAS may retry under
//                         contention, it never blocks or allocates). Callers:
//                         the audio thread (source: Acquire before postIo,
//                         usage queries for the virtualise/stop decision), the
//                         IO thread (Acquire for dst=null requests, Release of
//                         cancelled / failed reads) and decode jobs (Release
//                         after consumption, via StreamChannel::Release).
//
// When the pool is empty Acquire returns kInvalidChunk and counts a failure;
// it never waits. The pool does not decide what to stop: HeldBy() / Holdings()
// tell core / source who holds how many chunks so they can stop or virtualise
// lower-priority streams (3.3, 6.3).
#ifndef SND_IO_STREAM_POOL_HPP
#define SND_IO_STREAM_POOL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/base/atomic.hpp"
#include "snd/base/pool.hpp"
#include "snd/ext/types.hpp"
#include "snd/internal/common.hpp"

namespace snd::io {

inline constexpr std::uint32_t kInvalidChunk = 0xFFFFFFFFu;
// Owner id for chunks not attributed to a stream slot (counted in their own bucket).
inline constexpr std::uint32_t kNoOwner = 0xFFFFFFFFu;

struct StreamPoolStats {
  std::uint32_t chunkCount = 0;
  std::uint32_t chunkBytes = 0;      // usable bytes per chunk (as configured)
  std::uint32_t used = 0;            // chunks currently borrowed
  std::uint32_t peak = 0;            // high-water mark of `used`
  std::uint64_t failedAcquires = 0;  // Acquire on an empty pool
  std::uint64_t badReleases = 0;     // Release of a free / foreign chunk (ignored; a bug in the caller)
};

class StreamPool {
 public:
  StreamPool() noexcept = default;
  ~StreamPool() { Destroy(); }
  StreamPool(const StreamPool&) = delete;
  StreamPool& operator=(const StreamPool&) = delete;

  // Non-RT. chunkCount = poolBytes / AlignUp(chunkBytes, align); at least one
  // chunk must fit. maxOwners = number of owner ids (Config.maxStreams); owner
  // ids >= maxOwners are accounted as kNoOwner. align: chunk start alignment
  // (power of two, default 64; pass the sector size for unbuffered IO, 10.2).
  // Memory is tagged MemTag::StreamPool. false on invalid arguments / allocation failure.
  bool Init(IAllocator& allocator, std::uint64_t poolBytes, std::uint32_t chunkBytes,
            std::uint32_t maxOwners, std::size_t align = kBufferAlignment) noexcept;
  // Non-RT. Frees everything (borrowed chunks become invalid).
  void Destroy() noexcept;
  bool Initialized() const noexcept { return chunkCount_ != 0u; }

  // Any thread, RT-safe. kInvalidChunk when the pool is empty.
  std::uint32_t Acquire(std::uint32_t owner) noexcept;
  // Any thread, RT-safe. Returns a borrowed chunk. Releasing a chunk that is
  // not borrowed (double release, kInvalidChunk, out of range) is ignored and
  // counted in StreamPoolStats::badReleases.
  void Release(std::uint32_t chunk) noexcept;

  // Any thread, RT-safe. Chunk memory (chunk < ChunkCount()), ChunkBytes() usable bytes.
  std::uint8_t* Data(std::uint32_t chunk) const noexcept {
    return static_cast<std::uint8_t*>(blocks_.Block(chunk));
  }
  // Chunk index of a pointer to the START of a chunk, kInvalidChunk otherwise.
  std::uint32_t IndexOf(const void* p) const noexcept;
  // True if `chunk` is currently borrowed.
  bool IsBorrowed(std::uint32_t chunk) const noexcept;

  // --- usage (any thread, lock-free, approximate under concurrency) ---------
  // Owner the chunk was acquired for (kNoOwner if free or unattributed).
  std::uint32_t OwnerOf(std::uint32_t chunk) const noexcept;
  // Chunks currently held by `owner` (kNoOwner: the unattributed bucket).
  std::uint32_t HeldBy(std::uint32_t owner) const noexcept;
  // Copies HeldBy(i) for owners [0, min(capacity, MaxOwners())) into counts.
  // Returns the number of entries written.
  std::uint32_t Holdings(std::uint32_t* counts, std::uint32_t capacity) const noexcept;
  std::uint32_t Used() const noexcept { return used_.value.load(std::memory_order_relaxed); }
  std::uint32_t Available() const noexcept { return chunkCount_ - Used(); }
  std::uint32_t ChunkCount() const noexcept { return chunkCount_; }
  std::uint32_t ChunkBytes() const noexcept { return chunkBytes_; }
  std::uint32_t MaxOwners() const noexcept { return maxOwners_; }
  // Bytes reserved from the allocator (chunk stride x count).
  std::uint64_t ReservedBytes() const noexcept {
    return static_cast<std::uint64_t>(blocks_.BlockBytes()) * chunkCount_;
  }
  StreamPoolStats Stats() const noexcept;

 private:
  static constexpr std::uint32_t kFree = 0xFFFFFFFEu;  // owner_ value of a free chunk
  std::uint32_t Bucket(std::uint32_t owner) const noexcept {
    return owner < maxOwners_ ? owner : maxOwners_;
  }

  base::ConcurrentBlockPool blocks_;
  std::atomic<std::uint32_t>* owner_ = nullptr;  // per chunk: owner id or kFree
  std::atomic<std::uint32_t>* held_ = nullptr;   // per owner bucket (maxOwners_ + 1)
  base::CachePadded<std::atomic<std::uint32_t>> used_;
  std::atomic<std::uint32_t> peak_{0};
  std::atomic<std::uint64_t> failed_{0};
  std::atomic<std::uint64_t> badReleases_{0};
  IAllocator* alloc_ = nullptr;
  std::uint32_t chunkCount_ = 0;
  std::uint32_t chunkBytes_ = 0;
  std::uint32_t maxOwners_ = 0;
};

}  // namespace snd::io

#endif  // SND_IO_STREAM_POOL_HPP
