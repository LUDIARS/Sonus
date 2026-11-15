// base: handles, sparse sets on schema::TableRows, hashing (design.md 3.1, D-04, D-05).
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/base/handle.hpp"
#include "snd/base/hash.hpp"
#include "snd/base/pool.hpp"
#include "snd/base/sparse_set.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::base;
using snd::HandleRef;
using snd::kInvalidSlot;
using snd::MemTag;
using snd::schema::Indexing;
using snd::schema::TableRows;

SND_TEST(Handle, BitLayoutMatchesContract) {
  const std::uint64_t h = MakeHandle(0x12345678u, 0x9ABCDEF0u);
  SND_CHECK_EQ(h, SND_HANDLE_MAKE(0x12345678u, 0x9ABCDEF0u));
  SND_CHECK_EQ(HandleSlot(h), 0x12345678u);
  SND_CHECK_EQ(HandleGeneration(h), 0x9ABCDEF0u);
  SND_CHECK(IsValidHandle(h));
  SND_CHECK(!IsValidHandle(SND_HANDLE_INVALID_VALUE));
  SND_CHECK_EQ(HandleRef::FromValue(h).Value(), h);
  SND_CHECK_EQ(NextGeneration(1u), 2u);
  SND_CHECK_EQ(NextGeneration(0xFFFFFFFFu), 1u);
}

SND_TEST(Handle, AllocatorIssuesAndDetectsStale) {
  AccountingAllocator acc(DefaultAllocator());
  HandleAllocator ha;
  SND_REQUIRE(ha.Init(acc, 2));
  const HandleRef a = ha.Acquire();
  const HandleRef b = ha.Acquire();
  SND_CHECK_EQ(a.slot, 0u);
  SND_CHECK_EQ(b.slot, 1u);
  SND_CHECK_EQ(a.generation, 1u);
  SND_CHECK(ha.IsCurrent(a));
  SND_CHECK(ha.Acquire().IsNone());  // full
  SND_CHECK(ha.Release(a));
  SND_CHECK(!ha.IsCurrent(a));
  SND_CHECK(!ha.Release(a));  // double release ignored
  const HandleRef c = ha.Acquire();
  SND_CHECK_EQ(c.slot, 0u);
  SND_CHECK_EQ(c.generation, 2u);
  SND_CHECK(!ha.IsCurrent(a));
  SND_CHECK(ha.IsCurrent(c));
  SND_CHECK(!ha.IsCurrent(HandleRef{0u, 0u}));
  SND_CHECK(!ha.IsCurrent(HandleRef{7u, 1u}));
}

SND_TEST(Handle, ConcurrentAcquireReleaseIsUnique) {
  AccountingAllocator acc(DefaultAllocator());
  HandleAllocator ha;
  constexpr std::uint32_t kSlots = 64;
  SND_REQUIRE(ha.Init(acc, kSlots));
  std::atomic<std::uint32_t> owner[kSlots];
  for (auto& o : owner) o.store(0);
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;
  for (std::uint32_t t = 1; t <= 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 20000; ++i) {
        HandleRef h = ha.Acquire();
        if (h.IsNone()) continue;
        std::uint32_t expected = 0;
        if (!owner[h.slot].compare_exchange_strong(expected, t)) failed = true;  // slot handed out twice
        if (!ha.IsCurrent(h)) failed = true;
        owner[h.slot].store(0);
        if (!ha.Release(h)) failed = true;
      }
    });
  }
  for (auto& th : threads) th.join();
  SND_CHECK(!failed.load());
  SND_CHECK_EQ(ha.Available(), kSlots);
}

namespace {
struct TestTable {
  TableRows rows;
  std::uint32_t* value = nullptr;
  std::atomic<std::uint64_t>* atomicValue = nullptr;
};
}  // namespace

SND_TEST(SparseSet, InsertRemoveSwapsLastAndKeepsGenerations) {
  AccountingAllocator acc(DefaultAllocator());
  TestTable t;
  SND_REQUIRE(rows::Init(t.rows, acc, 8, Indexing::Sparse));
  t.value = AllocateArray<std::uint32_t>(acc, 8, MemTag::Tables);
  t.atomicValue = AllocateArray<std::atomic<std::uint64_t>>(acc, 8, MemTag::Tables);
  SND_REQUIRE(t.value != nullptr && t.atomicValue != nullptr);

  {
    SND_RT_SCOPE();
    for (std::uint32_t s : {5u, 2u, 7u, 0u}) {
      const std::uint32_t d = rows::Insert(t.rows, s, 3u);
      SND_REQUIRE(d != kInvalidSlot);
      t.value[d] = s * 10u;
      t.atomicValue[d].store(s * 100u);
    }
    SND_CHECK_EQ(t.rows.count, 4u);
    SND_CHECK(rows::Validate(t.rows));
    SND_CHECK_EQ(rows::Insert(t.rows, 5u, 4u), kInvalidSlot);  // already live
    SND_CHECK_EQ(rows::Insert(t.rows, 8u, 1u), kInvalidSlot);  // out of range
    SND_CHECK_EQ(rows::Insert(t.rows, 1u, 0u), kInvalidSlot);  // generation 0
    SND_CHECK(rows::IsCurrent(t.rows, HandleRef{2u, 3u}));
    SND_CHECK(!rows::IsCurrent(t.rows, HandleRef{2u, 4u}));

    // Remove slot 5 (dense 0): slot 0 (dense 3) moves to dense 0.
    const rows::RemoveResult r = rows::RemoveAndMove(t.rows, 5u, t.value, t.atomicValue);
    SND_CHECK(r.Removed());
    SND_CHECK(r.Moved());
    SND_CHECK_EQ(r.dense, 0u);
    SND_CHECK_EQ(r.movedFrom, 3u);
    SND_CHECK_EQ(rows::SlotAt(t.rows, 0u), 0u);
    SND_CHECK_EQ(t.value[0], 0u);
    SND_CHECK_EQ(t.atomicValue[rows::DenseOf(t.rows, 7u)].load(), 700u);
    SND_CHECK_EQ(t.value[rows::DenseOf(t.rows, 2u)], 20u);
    SND_CHECK(!rows::IsLive(t.rows, 5u));
    SND_CHECK_EQ(t.rows.generation[5], 3u);  // kept: stale handles stay stale
    SND_CHECK(!rows::IsCurrent(t.rows, HandleRef{5u, 3u}));
    SND_CHECK(rows::Validate(t.rows));

    // Removing the last row moves nothing.
    const std::uint32_t lastSlot = rows::SlotAt(t.rows, t.rows.count - 1u);
    const rows::RemoveResult r2 = rows::Remove(t.rows, lastSlot);
    SND_CHECK(r2.Removed());
    SND_CHECK(!r2.Moved());
    SND_CHECK(!rows::Remove(t.rows, 5u).Removed());  // not live
    SND_CHECK(!rows::Remove(t.rows, HandleRef{2u, 9u}).Removed());  // stale handle
    SND_CHECK(rows::Validate(t.rows));

    // Audio-side slot choice bumps generations.
    const std::uint32_t d5 = rows::InsertNext(t.rows, 5u);
    SND_CHECK(d5 != kInvalidSlot);
    SND_CHECK_EQ(t.rows.generation[5], 4u);
    SND_CHECK_EQ(rows::HandleAt(t.rows, d5).generation, 4u);
    rows::Clear(t.rows);
    SND_CHECK_EQ(t.rows.count, 0u);
    SND_CHECK(rows::Validate(t.rows));
  }
  FreeArray(acc, t.value, 8, MemTag::Tables);
  FreeArray(acc, t.atomicValue, 8, MemTag::Tables);
  rows::Destroy(t.rows, acc);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(SparseSet, RandomizedAgainstReference) {
  AccountingAllocator acc(DefaultAllocator());
  constexpr std::uint32_t kCap = 64;
  TableRows r;
  SND_REQUIRE(rows::Init(r, acc, kCap, Indexing::Sparse));
  std::uint32_t* val = AllocateArray<std::uint32_t>(acc, kCap, MemTag::Tables);
  IndexFreeList free;
  SND_REQUIRE(free.Init(acc, kCap, MemTag::Tables));
  std::vector<std::uint32_t> expectGen(kCap, 0u);
  std::vector<bool> live(kCap, false);
  std::uint32_t rng = 12345u;
  auto next = [&] { return rng = rng * 1664525u + 1013904223u; };
  for (int i = 0; i < 20000; ++i) {
    if ((next() >> 16) % 3u != 0u) {
      const std::uint32_t s = free.Pop();
      if (s == kInvalidIndex) continue;
      const std::uint32_t d = rows::InsertNext(r, s);
      SND_REQUIRE(d != kInvalidSlot);
      expectGen[s] = NextGeneration(expectGen[s]);
      live[s] = true;
      val[d] = s ^ 0xABCDu;
    } else if (r.count > 0u) {
      const std::uint32_t d = (next() >> 8) % r.count;
      const std::uint32_t s = rows::SlotAt(r, d);
      rows::RemoveAndMove(r, s, val);
      live[s] = false;
      free.Push(s);
    }
  }
  SND_CHECK(rows::Validate(r));
  std::uint32_t liveCount = 0;
  for (std::uint32_t s = 0; s < kCap; ++s) {
    SND_CHECK_EQ(r.generation[s], expectGen[s]);
    SND_CHECK_EQ(rows::IsLive(r, s), static_cast<bool>(live[s]));
    if (live[s]) {
      ++liveCount;
      SND_CHECK_EQ(val[rows::DenseOf(r, s)], s ^ 0xABCDu);  // columns followed the swaps
    }
  }
  SND_CHECK_EQ(liveCount, r.count);
  FreeArray(acc, val, kCap, MemTag::Tables);
  rows::Destroy(r, acc);
}

SND_TEST(SparseSet, DirectIndexing) {
  AccountingAllocator acc(DefaultAllocator());
  TableRows r;
  SND_REQUIRE(rows::Init(r, acc, 4, Indexing::Direct));
  SND_CHECK(r.slotToDense == nullptr);
  SND_CHECK_EQ(rows::Insert(r, 2u, 1u), 2u);
  SND_CHECK(rows::IsLive(r, 2u));
  SND_CHECK_EQ(rows::DenseOf(r, 2u), 2u);
  SND_CHECK_EQ(rows::DenseOf(r, 1u), kInvalidSlot);
  SND_CHECK_EQ(r.count, 1u);
  const rows::RemoveResult rr = rows::Remove(r, 2u);
  SND_CHECK(rr.Removed() && !rr.Moved());
  SND_CHECK(!rows::IsLive(r, 2u));
  SND_CHECK(rows::Validate(r));
  rows::Destroy(r, acc);
}

SND_TEST(Hash, MatchesContractHash) {
  const char* names[] = {"", "a", "foobar", "SE/Explosion", "BGM/Field_01", "\xE6\x97\xA5\xE6\x9C\xAC"};
  for (const char* n : names) {
    SND_CHECK_EQ(HashIdCStr(n), snd_hash_id(n));
    SND_CHECK_EQ(HashId(n, std::strlen(n)), snd::HashId(n));
  }
  SND_CHECK_EQ(HashIdCStr(nullptr), snd_hash_id(nullptr));
  SND_CHECK_EQ(IdHasher().Append("SE/").Append("Explosion").Finish(), snd_hash_id("SE/Explosion"));
  const char bytes[] = {'a', 'b'};
  SND_CHECK_EQ(IdHasher().Append(bytes, 2).Finish(), snd_hash_id("ab"));
  SND_CHECK_NE(MixU32(1u), MixU32(2u));
}
