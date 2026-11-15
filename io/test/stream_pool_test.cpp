// StreamPool (design.md 3.3) and StreamReadPlanner (6.1, 6.3).
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/io/stream_read_planner.hpp"
#include "snd/test/test.hpp"

using snd::io::kInvalidChunk;
using snd::io::kNoOwner;
using snd::io::StreamPool;
using snd::io::StreamRead;
using snd::io::StreamReadPlanner;

SND_TEST(StreamPool, CutsBudgetIntoChunks) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  StreamPool pool;
  SND_REQUIRE(pool.Init(acc, 8u << 20, 32u << 10, 32));
  SND_CHECK_EQ(pool.ChunkCount(), 256u);
  SND_CHECK_EQ(pool.ChunkBytes(), 32u << 10);
  SND_CHECK(acc.Usage(snd::MemTag::StreamPool).used >= (8u << 20));
  // A chunk size that is not a multiple of the alignment still stays within the budget.
  StreamPool odd;
  SND_REQUIRE(odd.Init(acc, 1000, 100, 4));
  SND_CHECK_EQ(odd.ChunkCount(), 7u);  // stride 128
  SND_CHECK(odd.ReservedBytes() <= 1000u);
  odd.Destroy();
  pool.Destroy();
  SND_CHECK_EQ(acc.Usage(snd::MemTag::StreamPool).used, 0u);
  // Invalid configurations.
  SND_CHECK(!pool.Init(acc, 100, 1000, 4));  // not even one chunk
  SND_CHECK(!pool.Init(acc, 1000, 0, 4));
  SND_CHECK(!pool.Init(acc, 1000, 100, 4, 48));  // alignment not a power of two
}

SND_TEST(StreamPool, AcquireReleaseIsRtSafeAndAccounted) {
  StreamPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), 4u * 1024u, 1024u, 4));
  std::uint32_t c[5];
  {
    SND_RT_SCOPE();
    c[0] = pool.Acquire(0);
    c[1] = pool.Acquire(0);
    c[2] = pool.Acquire(2);
    c[3] = pool.Acquire(99);  // unattributed bucket
    c[4] = pool.Acquire(1);   // empty
    SND_CHECK(c[0] != kInvalidChunk && c[1] != kInvalidChunk && c[2] != kInvalidChunk && c[3] != kInvalidChunk);
    SND_CHECK_EQ(c[4], kInvalidChunk);
    SND_CHECK_EQ(pool.HeldBy(0), 2u);
    SND_CHECK_EQ(pool.HeldBy(2), 1u);
    SND_CHECK_EQ(pool.HeldBy(kNoOwner), 1u);
    SND_CHECK_EQ(pool.OwnerOf(c[2]), 2u);
    SND_CHECK_EQ(pool.OwnerOf(c[3]), kNoOwner);
    std::uint32_t counts[8] = {};
    SND_CHECK_EQ(pool.Holdings(counts, 8), 4u);
    SND_CHECK_EQ(counts[0], 2u);
    SND_CHECK_EQ(counts[1], 0u);
    SND_CHECK_EQ(counts[2], 1u);
    SND_CHECK_EQ(pool.Used(), 4u);
    SND_CHECK_EQ(pool.Available(), 0u);
    SND_CHECK_EQ(pool.IndexOf(pool.Data(c[1])), c[1]);
    SND_CHECK_EQ(pool.IndexOf(pool.Data(c[1]) + 1), kInvalidChunk);  // not a chunk start
    SND_CHECK(pool.IsBorrowed(c[0]));
    pool.Release(c[0]);
    SND_CHECK(!pool.IsBorrowed(c[0]));
    pool.Release(c[0]);  // double release: ignored, counted
    pool.Release(kInvalidChunk);
  }
  const snd::io::StreamPoolStats s = pool.Stats();
  SND_CHECK_EQ(s.failedAcquires, 1u);
  SND_CHECK_EQ(s.badReleases, 2u);
  SND_CHECK_EQ(s.peak, 4u);
  SND_CHECK_EQ(s.used, 3u);
  SND_CHECK_EQ(pool.HeldBy(0), 1u);
  pool.Release(c[1]);
  pool.Release(c[2]);
  pool.Release(c[3]);
  SND_CHECK_EQ(pool.Used(), 0u);
  SND_CHECK_EQ(pool.HeldBy(kNoOwner), 0u);
  int x = 0;
  SND_CHECK_EQ(pool.IndexOf(&x), kInvalidChunk);
}

SND_TEST(StreamPool, ConcurrentAcquireReleaseKeepsCounts) {
  StreamPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), 64u * 256u, 256u, 4));
  std::atomic<bool> bad{false};
  std::vector<std::thread> threads;
  for (std::uint32_t t = 0; t < 4; ++t) {
    threads.emplace_back([&pool, &bad, t] {
      std::uint32_t held[8];
      for (int iter = 0; iter < 4000; ++iter) {
        std::uint32_t n = 0;
        for (; n < 8; ++n) {
          held[n] = pool.Acquire(t);
          if (held[n] == kInvalidChunk) break;
          pool.Data(held[n])[0] = static_cast<std::uint8_t>(t);
        }
        for (std::uint32_t i = 0; i < n; ++i) {
          if (pool.Data(held[i])[0] != static_cast<std::uint8_t>(t)) bad = true;  // two owners of one chunk
          pool.Release(held[i]);
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  SND_CHECK(!bad.load());
  SND_CHECK_EQ(pool.Used(), 0u);
  SND_CHECK_EQ(pool.Available(), 64u);
  for (std::uint32_t t = 0; t < 4; ++t) SND_CHECK_EQ(pool.HeldBy(t), 0u);
  SND_CHECK_EQ(pool.Stats().badReleases, 0u);
}

SND_TEST(StreamReadPlanner, SequentialWithoutLoop) {
  StreamReadPlanner p;
  p.Reset(100, 100 + 2500, 0, 0, 1000);
  StreamRead r{};
  SND_REQUIRE(p.Next(r));
  SND_CHECK_EQ(r.offset, 100u);
  SND_CHECK_EQ(r.bytes, 1000u);
  SND_CHECK(!r.wrapped && !r.loopEnd);
  SND_REQUIRE(p.Next(r));
  SND_CHECK_EQ(r.offset, 1100u);
  SND_REQUIRE(p.Next(r));
  SND_CHECK_EQ(r.offset, 2100u);
  SND_CHECK_EQ(r.bytes, 500u);
  SND_CHECK(!p.Next(r));
  SND_CHECK_EQ(p.BytesToBoundary(), 0u);
}

SND_TEST(StreamReadPlanner, WrapsToLoopStartAtLoopEnd) {
  StreamReadPlanner p;
  // data [0, 10000), loop [2000, 4500), chunk 1000
  p.Reset(0, 10000, 2000, 4500, 1000);
  SND_CHECK(p.Looping());
  std::vector<StreamRead> reads;
  StreamRead r{};
  for (int i = 0; i < 10 && p.Next(r); ++i) reads.push_back(r);
  SND_REQUIRE_EQ(reads.size(), 10u);
  const std::uint64_t expectOffset[10] = {0, 1000, 2000, 3000, 4000, 2000, 3000, 4000, 2000, 3000};
  const std::uint32_t expectBytes[10] = {1000, 1000, 1000, 1000, 500, 1000, 1000, 500, 1000, 1000};
  for (std::size_t i = 0; i < 10; ++i) {
    SND_CHECK_EQ(reads[i].offset, expectOffset[i]);
    SND_CHECK_EQ(reads[i].bytes, expectBytes[i]);
  }
  SND_CHECK(reads[4].loopEnd);
  SND_CHECK(reads[5].wrapped);
  SND_CHECK(!reads[6].wrapped);
  SND_CHECK(reads[7].loopEnd);
  SND_CHECK(reads[8].wrapped);
  // Seek inside / past the loop.
  p.Seek(3500);
  SND_REQUIRE(p.Next(r));
  SND_CHECK_EQ(r.offset, 3500u);
  SND_CHECK_EQ(r.bytes, 1000u);
  SND_CHECK(r.loopEnd);
  p.Seek(9000);
  SND_REQUIRE(p.Next(r));
  SND_CHECK_EQ(r.offset, 2000u);
  // Invalid loop range disables looping.
  p.Reset(0, 1000, 800, 2000, 300);
  SND_CHECK(!p.Looping());
}
