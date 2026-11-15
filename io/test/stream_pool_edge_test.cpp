// StreamPool edge cases (design.md 3.3): geometry, alignment, bad releases,
// owner books, Holdings capacity.
#include <cstdint>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/test/test.hpp"

using snd::io::kInvalidChunk;
using snd::io::kNoOwner;
using snd::io::StreamPool;

SND_TEST(StreamPoolEdge, GeometryAndAlignment) {
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  StreamPool p;
  SND_CHECK(!p.Init(alloc, 1000, 0, 4));          // chunkBytes 0
  SND_CHECK(!p.Init(alloc, 1000, 2000, 4));       // no chunk fits
  SND_CHECK(!p.Init(alloc, 0, 64, 4));            // no memory
  SND_CHECK(!p.Init(alloc, 4096, 64, 4, 48));     // align not a power of two
  SND_CHECK(!p.Initialized());
  SND_CHECK_EQ(p.Acquire(0), kInvalidChunk);      // uninitialised pool
  p.Release(0);
  SND_CHECK_EQ(alloc.TotalUsed(), 0u);
  // poolBytes not a multiple; chunkBytes not a multiple of the alignment.
  SND_REQUIRE(p.Init(alloc, 10000, 1000, 4));
  SND_CHECK_EQ(p.ChunkBytes(), 1000u);
  SND_CHECK_EQ(p.ChunkCount(), 10000u / 1024u);  // stride = AlignUp(1000, 64)
  SND_CHECK(p.ReservedBytes() <= 10000u);
  for (std::uint32_t i = 0; i < p.ChunkCount(); ++i) {
    SND_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p.Data(i)) % 64u, 0u);
    SND_CHECK_EQ(p.IndexOf(p.Data(i)), i);
    SND_CHECK_EQ(p.IndexOf(p.Data(i) + 1), kInvalidChunk);
    if (i + 1u < p.ChunkCount()) SND_CHECK(p.Data(i + 1u) >= p.Data(i) + p.ChunkBytes());
  }
  SND_CHECK_EQ(p.IndexOf(nullptr), kInvalidChunk);
  SND_CHECK_EQ(p.IndexOf(p.Data(0) - 64), kInvalidChunk);
  SND_CHECK_EQ(p.IndexOf(p.Data(p.ChunkCount() - 1u) + 1024), kInvalidChunk);
  p.Destroy();
  // Sector alignment for unbuffered IO.
  SND_REQUIRE(p.Init(alloc, 64u * 1024u, 4096, 2, 4096));
  for (std::uint32_t i = 0; i < p.ChunkCount(); ++i) SND_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p.Data(i)) % 4096u, 0u);
  SND_CHECK_EQ(p.ChunkCount(), 16u);
  p.Destroy();
  // Exactly one chunk.
  SND_REQUIRE(p.Init(alloc, 64, 64, 1));
  SND_CHECK_EQ(p.ChunkCount(), 1u);
  const std::uint32_t c = p.Acquire(0);
  SND_CHECK_EQ(c, 0u);
  SND_CHECK_EQ(p.Acquire(0), kInvalidChunk);
  SND_CHECK_EQ(p.Stats().failedAcquires, 1u);
  p.Release(c);
  p.Destroy();
  SND_CHECK_EQ(alloc.TotalUsed(), 0u);
}

SND_TEST(StreamPoolEdge, BadReleasesAreIgnoredAndCounted) {
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  StreamPool p;
  SND_REQUIRE(p.Init(alloc, 8u * 256u, 256, 3));
  const std::uint32_t a = p.Acquire(1);
  const std::uint32_t b = p.Acquire(2);
  {
    SND_RT_SCOPE();
    p.Release(a);
    p.Release(a);              // double
    p.Release(kInvalidChunk);  // invalid
    p.Release(8);              // out of range
    p.Release(5);              // never borrowed
  }
  SND_CHECK_EQ(p.Stats().badReleases, 4u);
  SND_CHECK_EQ(p.Used(), 1u);
  SND_CHECK_EQ(p.HeldBy(1), 0u);
  SND_CHECK_EQ(p.HeldBy(2), 1u);
  SND_CHECK(p.IsBorrowed(b));
  SND_CHECK(!p.IsBorrowed(a));
  SND_CHECK(!p.IsBorrowed(kInvalidChunk));
  SND_CHECK_EQ(p.OwnerOf(a), kNoOwner);
  SND_CHECK_EQ(p.OwnerOf(b), 2u);
  SND_CHECK_EQ(p.OwnerOf(1000), kNoOwner);
  // The double release did not put `a` twice on the free list: 7 distinct chunks remain.
  std::vector<std::uint32_t> got;
  for (;;) {
    const std::uint32_t c = p.Acquire(0);
    if (c == kInvalidChunk) break;
    for (std::uint32_t g : got) SND_CHECK_NE(g, c);
    SND_CHECK_NE(c, b);
    got.push_back(c);
  }
  SND_CHECK_EQ(got.size(), 7u);
  for (std::uint32_t c : got) p.Release(c);
  p.Release(b);
  SND_CHECK_EQ(p.Used(), 0u);
}

SND_TEST(StreamPoolEdge, OwnerBooksAtTheBoundaries) {
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  StreamPool p;
  SND_REQUIRE(p.Init(alloc, 16u * 128u, 128, 4));
  const std::uint32_t owners[] = {0, 3, 4, 100, kNoOwner, 0xFFFFFFFEu};
  std::vector<std::uint32_t> chunks;
  for (std::uint32_t o : owners) chunks.push_back(p.Acquire(o));
  SND_CHECK_EQ(p.HeldBy(0), 1u);
  SND_CHECK_EQ(p.HeldBy(3), 1u);
  SND_CHECK_EQ(p.HeldBy(kNoOwner), 4u);  // 4, 100, kNoOwner, kFree-like id
  SND_CHECK_EQ(p.HeldBy(4), 4u);         // ids >= maxOwners share the unattributed bucket
  SND_CHECK_EQ(p.OwnerOf(chunks[2]), kNoOwner);
  SND_CHECK_EQ(p.OwnerOf(chunks[5]), kNoOwner);
  std::uint32_t counts[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  SND_CHECK_EQ(p.Holdings(counts, 8), 4u);
  SND_CHECK_EQ(counts[0], 1u);
  SND_CHECK_EQ(counts[3], 1u);
  SND_CHECK_EQ(counts[4], 9u);  // untouched
  SND_CHECK_EQ(p.Holdings(counts, 2), 2u);
  SND_CHECK_EQ(p.Holdings(nullptr, 2), 0u);
  SND_CHECK_EQ(p.Holdings(counts, 0), 0u);
  for (std::uint32_t c : chunks) p.Release(c);
  SND_CHECK_EQ(p.HeldBy(kNoOwner), 0u);
  SND_CHECK_EQ(p.Used(), 0u);
  SND_CHECK_EQ(p.Stats().badReleases, 0u);
  SND_CHECK_EQ(p.Stats().peak, 6u);
  p.Destroy();
  // maxOwners = 0: everything is unattributed.
  SND_REQUIRE(p.Init(alloc, 4u * 128u, 128, 0));
  const std::uint32_t c = p.Acquire(0);
  SND_CHECK_EQ(p.HeldBy(kNoOwner), 1u);
  SND_CHECK_EQ(p.Holdings(counts, 8), 0u);
  p.Release(c);
  SND_CHECK(!p.Init(alloc, 4u * 128u, 128, 0xFFFFFFFEu));  // maxOwners too large
}
