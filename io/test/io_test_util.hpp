// Shared helpers for the io unit tests.
#ifndef SND_IO_TEST_UTIL_HPP
#define SND_IO_TEST_UTIL_HPP

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/source_if.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/mocks/memory_file_system.hpp"

namespace snd::iotest {

// Byte i of test file `seed`.
inline std::uint8_t PatternByte(std::uint32_t seed, std::uint64_t i) noexcept {
  return static_cast<std::uint8_t>((i * 7u + seed * 13u + (i >> 8)) & 0xFFu);
}
inline std::vector<std::uint8_t> MakePattern(std::uint32_t seed, std::size_t bytes) {
  std::vector<std::uint8_t> v(bytes);
  for (std::size_t i = 0; i < bytes; ++i) v[i] = PatternByte(seed, i);
  return v;
}
inline bool MatchesPattern(std::uint32_t seed, std::uint64_t offset, const std::uint8_t* p, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    if (p[i] != PatternByte(seed, offset + i)) return false;
  }
  return true;
}

// Deterministic clock for the scheduler (test thread only, or atomic for threaded use).
struct ManualClock {
  std::atomic<std::uint64_t> now{0};
  static std::uint64_t Read(void* user) noexcept {
    return static_cast<ManualClock*>(user)->now.load(std::memory_order_relaxed);
  }
  io::IoClock Clock() noexcept { return io::IoClock{&Read, this}; }
  void AdvanceMs(std::uint64_t ms) noexcept { now.fetch_add(ms * 1000000ull, std::memory_order_relaxed); }
};

inline constexpr std::uint32_t kChunk = 16u << 10;

inline source::IoReadRequest StreamReq(SlotIndex slot, const char* path, std::uint64_t offset, std::uint32_t bytes,
                                       io::IoClass cls, std::uint32_t tag = 0, void* dst = nullptr) noexcept {
  source::IoReadRequest r{};
  r.streamSlot = slot;
  r.chunkIndex = tag;
  r.path = path;
  r.offset = offset;
  r.dst = dst;
  r.bytes = bytes;
  r.priority = static_cast<std::uint8_t>(cls);
  return r;
}

// Scheduler over the memory FS, driven manually (RunOnce) with a manual clock.
struct Rig {
  base::AccountingAllocator alloc{base::DefaultAllocator()};
  mock::MemoryFileSystem fs;
  io::StreamPool pool;
  io::IoScheduler io;
  ManualClock clock;
  io::IoSchedulerConfig cfg;

  explicit Rig(std::uint32_t poolChunks = 64, std::uint32_t chunkBytes = kChunk) {
    cfg.maxStreams = 8;
    cfg.ringChunks = 8;
    cfg.maxInFlight = 4;
    cfg.bytesPerWindow = 0;  // unlimited unless a test sets it
    cfg.clock = clock.Clock();
    poolChunks_ = poolChunks;
    chunkBytes_ = chunkBytes;
  }
  ~Rig() {
    io.Destroy();
    pool.Destroy();
  }
  bool Init() noexcept {
    return pool.Init(alloc, static_cast<std::uint64_t>(poolChunks_) * chunkBytes_, chunkBytes_, cfg.maxStreams) &&
           io.Init(alloc, fs, pool, cfg);
  }
  // Runs RunOnce until no work remains or `max` iterations.
  void Run(std::uint32_t max = 1000) noexcept {
    for (std::uint32_t i = 0; i < max; ++i) {
      if (!io.RunOnce()) return;
    }
  }
  // Pops every ready record of `slot`, checks data against `seed`, releases them.
  std::uint32_t Consume(SlotIndex slot, std::uint32_t seed, std::vector<io::ChunkRecord>* out = nullptr,
                        bool* dataOk = nullptr) {
    std::uint32_t n = 0;
    io::ChunkRecord rec;
    while (io.Channel(slot).Pop(rec)) {
      if (rec.status == io::ChunkStatus::Ok && dataOk != nullptr &&
          !MatchesPattern(seed, rec.offset, pool.Data(rec.chunk), rec.bytes)) {
        *dataOk = false;
      }
      if (out != nullptr) out->push_back(rec);
      io.Channel(slot).Release(rec);
      ++n;
    }
    return n;
  }

 private:
  std::uint32_t poolChunks_;
  std::uint32_t chunkBytes_;
};

}  // namespace snd::iotest

#endif  // SND_IO_TEST_UTIL_HPP
