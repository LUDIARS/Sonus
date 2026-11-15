// base: allocators, arenas, pools, chains (design.md 3.1, 3.3).
#include <cstdint>
#include <cstring>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/base/arena.hpp"
#include "snd/base/chain.hpp"
#include "snd/base/pool.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::base;
using snd::IAllocator;
using snd::MemTag;

namespace {
bool Aligned(const void* p, std::size_t a) { return (reinterpret_cast<std::uintptr_t>(p) & (a - 1u)) == 0u; }
}  // namespace

SND_TEST(Allocator, MallocAlignsAndRejectsBadArgs) {
  IAllocator& a = DefaultAllocator();
  for (std::size_t align : {16u, 64u, 128u, 4096u}) {
    void* p = a.Allocate(100, align, MemTag::Misc);
    SND_REQUIRE(p != nullptr);
    SND_CHECK(Aligned(p, align));
    a.Free(p, MemTag::Misc);
  }
  SND_CHECK(a.Allocate(0, 16, MemTag::Misc) == nullptr);
  SND_CHECK(a.Allocate(16, 24, MemTag::Misc) == nullptr);
  a.Free(nullptr, MemTag::Misc);
}

SND_TEST(Allocator, AccountingPerTagAndPeak) {
  AccountingAllocator acc(DefaultAllocator());
  void* a = acc.Allocate(1000, 64, MemTag::Tables);
  void* b = acc.Allocate(500, 16, MemTag::Tables);
  void* c = acc.Allocate(300, 128, MemTag::StreamPool);
  SND_REQUIRE(a != nullptr && b != nullptr && c != nullptr);
  SND_CHECK(Aligned(a, 64));
  SND_CHECK(Aligned(c, 128));
  std::memset(a, 0xAB, 1000);  // whole payload usable (ASan checks)
  SND_CHECK_EQ(acc.Usage(MemTag::Tables).used, 1500u);
  SND_CHECK_EQ(acc.Usage(MemTag::Tables).allocations, 2u);
  SND_CHECK_EQ(acc.Usage(MemTag::StreamPool).used, 300u);
  SND_CHECK_EQ(acc.TotalUsed(), 1800u);
  acc.Free(a, MemTag::Tables);
  SND_CHECK_EQ(acc.Usage(MemTag::Tables).used, 500u);
  SND_CHECK_EQ(acc.Usage(MemTag::Tables).peak, 1500u);
  acc.Free(b, MemTag::Tables);
  acc.Free(c, MemTag::StreamPool);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);

  std::uint64_t used[SND_MEM_TAG_COUNT] = {};
  std::uint64_t budget[SND_MEM_TAG_COUNT] = {};
  acc.SetBudget(MemTag::PcmCache, 4096);
  acc.FillStats(used, budget);
  SND_CHECK_EQ(budget[SND_MEM_PCM_CACHE], 4096u);
  SND_CHECK_EQ(used[SND_MEM_TABLES], 0u);
}

SND_TEST(Allocator, BudgetRefusesAndCounts) {
  AccountingAllocator acc(DefaultAllocator());
  acc.SetBudget(MemTag::DecoderPool, 1024);
  void* a = acc.Allocate(1000, 16, MemTag::DecoderPool);
  SND_REQUIRE(a != nullptr);
  SND_CHECK(acc.Allocate(100, 16, MemTag::DecoderPool) == nullptr);
  SND_CHECK_EQ(acc.Usage(MemTag::DecoderPool).failures, 1u);
  SND_CHECK_EQ(acc.Usage(MemTag::DecoderPool).used, 1000u);
  void* other = acc.Allocate(100, 16, MemTag::Misc);  // other tags unaffected
  SND_CHECK(other != nullptr);
  acc.Free(other, MemTag::Misc);
  acc.Free(a, MemTag::DecoderPool);
  void* b = acc.Allocate(1024, 16, MemTag::DecoderPool);
  SND_CHECK(b != nullptr);
  acc.Free(b, MemTag::DecoderPool);
}

SND_TEST(Allocator, TypedHelpers) {
  AccountingAllocator acc(DefaultAllocator());
  auto* f = AllocateArray<float>(acc, 33, MemTag::DspPool);
  SND_REQUIRE(f != nullptr);
  SND_CHECK(Aligned(f, 64));
  SND_CHECK_EQ(f[32], 0.0f);
  FreeArray(acc, f, 33, MemTag::DspPool);
  SND_CHECK(AllocateArray<float>(acc, 0, MemTag::DspPool) == nullptr);

  AlignedBuffer<double> buf;
  SND_REQUIRE(buf.Allocate(acc, 10, MemTag::Misc));
  buf[9] = 3.0;
  AlignedBuffer<double> moved(std::move(buf));
  SND_CHECK(buf.empty());
  SND_CHECK_EQ(moved.size(), 10u);
  SND_CHECK_EQ(moved[9], 3.0);
  moved.Release();
  SND_CHECK_EQ(acc.TotalUsed(), 0u);

  struct Obj {
    int v;
    explicit Obj(int x) : v(x) {}
  };
  Obj* o = New<Obj>(acc, MemTag::Misc, 7);
  SND_REQUIRE(o != nullptr);
  SND_CHECK_EQ(o->v, 7);
  Delete(acc, o, MemTag::Misc);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(Allocator, NotesAllocInRtScope) {
  snd::test::rt::ExpectViolations expect;
  snd::test::rt::Reset();
  AccountingAllocator acc(DefaultAllocator());
  {
    SND_RT_SCOPE();
    void* p = acc.Allocate(64, 16, MemTag::Misc);
    acc.Free(p, MemTag::Misc);
  }
  SND_CHECK(snd::test::rt::Count(snd::rt::Violation::Alloc) >= 1u);
  SND_CHECK(snd::test::rt::Count(snd::rt::Violation::Free) >= 1u);
  snd::test::rt::Reset();
}

SND_TEST(Arena, BumpAlignResetRewind) {
  AccountingAllocator acc(DefaultAllocator());
  LinearArena arena;
  SND_REQUIRE(arena.Init(acc, 1024, MemTag::FrameArena));
  SND_CHECK_EQ(acc.Usage(MemTag::FrameArena).used, 1024u);
  {
    SND_RT_SCOPE();
    void* a = arena.Allocate(10, 16);
    void* b = arena.Allocate(10, 64);
    SND_CHECK(a != nullptr && b != nullptr);
    SND_CHECK(Aligned(b, 64));
    const std::size_t mark = arena.Mark();
    float* f = arena.AllocateArray<float>(100);
    SND_CHECK(f != nullptr);
    arena.Rewind(mark);
    SND_CHECK_EQ(arena.Used(), mark);
    SND_CHECK(arena.Allocate(2000) == nullptr);  // overflow
    SND_CHECK_EQ(arena.Overflows(), 1u);
    SND_CHECK(arena.HighWater() >= mark + 400u);
    arena.Reset();
    SND_CHECK_EQ(arena.Used(), 0u);
    void* z = arena.AllocateZeroed(1024);
    SND_CHECK(z != nullptr);
    SND_CHECK(arena.Allocate(1) == nullptr);
  }
  arena.Destroy();
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(Arena, ThreadArenasDoNotOverlap) {
  AccountingAllocator acc(DefaultAllocator());
  ThreadArenaSet set;
  SND_REQUIRE(set.Init(acc, 3, 100));
  SND_CHECK_EQ(set.Count(), 3u);
  std::uint8_t* prevEnd = nullptr;
  for (std::uint32_t i = 0; i < 3; ++i) {
    LinearArena& a = set.Arena(i);
    SND_CHECK_EQ(a.Capacity(), 128u);
    SND_CHECK(Aligned(a.data(), 64));
    if (prevEnd != nullptr) SND_CHECK(a.data() >= prevEnd);
    prevEnd = a.data() + a.Capacity();
    SND_CHECK(a.Allocate(128) != nullptr);
  }
  set.Destroy();
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(Pool, IndexFreeListDeterministic) {
  AccountingAllocator acc(DefaultAllocator());
  IndexFreeList l;
  SND_REQUIRE(l.Init(acc, 4, MemTag::Tables));
  SND_CHECK_EQ(l.Pop(), 0u);
  SND_CHECK_EQ(l.Pop(), 1u);
  l.Push(0);
  SND_CHECK_EQ(l.Pop(), 0u);
  SND_CHECK_EQ(l.Pop(), 2u);
  SND_CHECK_EQ(l.Pop(), 3u);
  SND_CHECK_EQ(l.Pop(), kInvalidIndex);
  l.Reset();
  SND_CHECK_EQ(l.Available(), 4u);
}

SND_TEST(Pool, BlockPoolAcquireRelease) {
  AccountingAllocator acc(DefaultAllocator());
  BlockPool pool;
  SND_REQUIRE(pool.Init(acc, 100, 3, MemTag::StreamPool));
  SND_CHECK_EQ(pool.BlockBytes(), 128u);
  void* blocks[3];
  {
    SND_RT_SCOPE();
    for (auto& b : blocks) {
      b = pool.Acquire();
      SND_CHECK(b != nullptr);
      SND_CHECK(Aligned(b, 64));
    }
    SND_CHECK(pool.Acquire() == nullptr);
    SND_CHECK(pool.Owns(blocks[2]));
    SND_CHECK_EQ(pool.IndexOf(pool.Block(2)), 2u);
    pool.Release(blocks[1]);
    SND_CHECK(pool.Acquire() == blocks[1]);
  }
  std::memset(blocks[2], 1, 128);
  pool.Destroy();
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(Chain, InlineThenBlocksAndRemoveSwap) {
  AccountingAllocator acc(DefaultAllocator());
  ChainPool<std::uint32_t> pool;
  SND_REQUIRE(pool.Init(acc, 3));
  InlineChain<std::uint32_t, 8> c;
  {
    SND_RT_SCOPE();
    for (std::uint32_t i = 0; i < 8 + 16 * 3; ++i) SND_CHECK(c.PushBack(pool, i));
    SND_CHECK(!c.PushBack(pool, 999));  // pool exhausted
    SND_CHECK_EQ(c.Size(), 56u);
    SND_CHECK_EQ(pool.Available(), 0u);
    for (std::uint32_t i = 0; i < 56; ++i) SND_CHECK_EQ(*c.At(pool, i), i);
    SND_CHECK(c.At(pool, 56) == nullptr);
    std::uint64_t sum = 0;
    c.ForEach(pool, [&](std::uint32_t& v) { sum += v; });
    SND_CHECK_EQ(sum, 55u * 56u / 2u);
    // Remove the element at 3: last (55) moves there; the last block now has 15 entries.
    c.RemoveSwap(pool, 3);
    SND_CHECK_EQ(*c.At(pool, 3), 55u);
    SND_CHECK_EQ(pool.Available(), 0u);
    for (int k = 0; k < 15; ++k) c.RemoveSwap(pool, c.Size() - 1u);
    SND_CHECK_EQ(c.Size(), 40u);
    SND_CHECK_EQ(pool.Available(), 1u);  // third block returned
    c.Clear(pool);
    SND_CHECK_EQ(pool.Available(), 3u);
    SND_CHECK(c.Empty());
  }
  // A second chain shares the pool.
  InlineChain<std::uint32_t, 2> a, b;
  for (std::uint32_t i = 0; i < 2 + 16; ++i) SND_CHECK(a.PushBack(pool, i));
  for (std::uint32_t i = 0; i < 2 + 32; ++i) SND_CHECK(b.PushBack(pool, 100 + i));
  SND_CHECK_EQ(pool.Available(), 0u);
  SND_CHECK_EQ(*a.At(pool, 17), 17u);
  SND_CHECK_EQ(*b.At(pool, 33), 133u);
  a.Clear(pool);
  b.Clear(pool);
  SND_CHECK_EQ(pool.Available(), 3u);
}
