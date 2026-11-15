// Independent tests: sparse-set rows (3.1), inline chains (3.1 可変長の子),
// block pools, allocators / arenas (3.3, 3.4) and the lock-free log
// (1.2 principle 1). Model tests use fixed seeds.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "adv_test_util.hpp"
#include "snd/base/allocator.hpp"
#include "snd/base/arena.hpp"
#include "snd/base/chain.hpp"
#include "snd/base/log.hpp"
#include "snd/base/pool.hpp"
#include "snd/base/sparse_set.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::base;
using snd::advtest::Rng;
using snd::advtest::StressScale;
namespace rows = snd::base::rows;

// --- sparse set --------------------------------------------------------------------

namespace {
struct RowModel {
  std::uint32_t generation = 0;  // last generation of the slot (kept after removal)
  bool live = false;
  std::uint64_t value = 0;       // column payload
};
}  // namespace

SND_TEST(AdvSparseSet, RandomOpsAgainstModelWithColumnsAndStaleHandles) {
  for (std::uint64_t seed : {1ull, 2ull, 3ull, 0xC0FFEEull}) {
    for (std::uint32_t cap : {1u, 2u, 7u, 64u, 65u}) {
      snd::schema::TableRows r;
      SND_REQUIRE(rows::Init(r, DefaultAllocator(), cap, snd::schema::Indexing::Sparse));
      std::vector<std::uint64_t> colA(cap, 0u);  // two columns indexed by dense position
      std::vector<std::uint32_t> colB(cap, 0u);
      std::vector<RowModel> m(cap);
      std::vector<snd::HandleRef> oldHandles;
      Rng rng(seed * 1000u + cap);
      std::uint32_t live = 0;
      for (int step = 0; step < 4000; ++step) {
        const std::uint32_t op = rng.Below(10);
        const std::uint32_t slot = rng.Below(cap + 1u);  // includes one out-of-range slot
        if (op < 4) {  // insert (game-chosen generation or audio-side InsertNext)
          const bool next = rng.Below(2) == 0u;
          const std::uint32_t gen = next ? 0u : 1u + rng.Below(0xFFFFFFF0u);
          const std::uint32_t d = next ? rows::InsertNext(r, slot) : rows::Insert(r, slot, gen);
          const bool shouldFail = slot >= cap || m[slot].live;
          if (shouldFail) {
            SND_REQUIRE_EQ(d, snd::kInvalidSlot);
          } else {
            SND_REQUIRE(d != snd::kInvalidSlot);
            SND_REQUIRE_EQ(d, live);  // appended at the end of the dense range
            const std::uint32_t expectGen = next ? NextGeneration(m[slot].generation) : gen;
            SND_REQUIRE_EQ(r.generation[slot], expectGen);
            m[slot].generation = expectGen;
            m[slot].live = true;
            m[slot].value = rng.Next();
            colA[d] = m[slot].value;
            colB[d] = slot * 3u;
            ++live;
          }
        } else if (op < 8) {  // remove by slot or by handle (possibly stale)
          const bool byHandle = rng.Below(2) == 0u && !oldHandles.empty();
          rows::RemoveResult res;
          bool expectRemoved;
          std::uint32_t target = slot;
          if (byHandle) {
            const snd::HandleRef h = oldHandles[rng.Below(static_cast<std::uint32_t>(oldHandles.size()))];
            target = h.slot;
            expectRemoved = h.slot < cap && m[h.slot].live && m[h.slot].generation == h.generation;
            res = rows::Remove(r, h);
          } else {
            expectRemoved = slot < cap && m[slot].live;
            if (expectRemoved) {
              res = rows::RemoveAndMove(r, slot, colA.data(), colB.data());
            } else {
              res = rows::Remove(r, slot);
            }
          }
          SND_REQUIRE_EQ(res.Removed(), expectRemoved);
          if (expectRemoved) {
            if (byHandle && res.Moved()) {  // plain Remove: caller moves the columns
              colA[res.dense] = colA[res.movedFrom];
              colB[res.dense] = colB[res.movedFrom];
            }
            m[target].live = false;
            --live;
            SND_REQUIRE_EQ(res.movedFrom, live);  // the last row moved into the hole
          }
        } else if (op == 8) {  // remember a handle (live or not) for later stale checks
          if (slot < cap) oldHandles.push_back(snd::HandleRef{slot, m[slot].generation});
          if (oldHandles.size() > 32u) oldHandles.erase(oldHandles.begin());
        } else if (rng.Below(50) == 0u) {
          rows::Clear(r);
          for (auto& x : m) x.live = false;
          live = 0;
        }
        // Full invariant check against the model.
        SND_REQUIRE(rows::Validate(r));
        SND_REQUIRE_EQ(r.count, live);
        for (std::uint32_t s = 0; s < cap; ++s) {
          SND_REQUIRE_EQ(rows::IsLive(r, s), m[s].live);
          if (!m[s].live) {
            SND_REQUIRE_EQ(rows::DenseOf(r, s), snd::kInvalidSlot);
            continue;
          }
          const std::uint32_t d = rows::DenseOf(r, s);
          SND_REQUIRE(d < r.count);
          SND_REQUIRE_EQ(rows::SlotAt(r, d), s);
          SND_REQUIRE_EQ(colA[d], m[s].value);  // columns followed the swaps
          SND_REQUIRE_EQ(colB[d], s * 3u);
          SND_REQUIRE(rows::IsCurrent(r, snd::HandleRef{s, m[s].generation}));
          SND_REQUIRE_EQ(rows::HandleAt(r, d).generation, m[s].generation);
        }
        for (const snd::HandleRef& h : oldHandles) {
          const bool cur = h.slot < cap && m[h.slot].live && m[h.slot].generation == h.generation && h.generation != 0u;
          SND_REQUIRE_EQ(rows::IsCurrent(r, h), cur);
          SND_REQUIRE_EQ(rows::DenseOf(r, h) != snd::kInvalidSlot, cur);
        }
      }
      rows::Destroy(r, DefaultAllocator());
    }
  }
}

SND_TEST(AdvSparseSet, GenerationWrapSkipsZeroAndFullTable) {
  snd::schema::TableRows r;
  SND_REQUIRE(rows::Init(r, DefaultAllocator(), 4, snd::schema::Indexing::Sparse));
  SND_CHECK_EQ(rows::Insert(r, 0, 0u), snd::kInvalidSlot);  // generation 0 never live
  SND_CHECK(rows::Insert(r, 0, 0xFFFFFFFFu) != snd::kInvalidSlot);
  const snd::HandleRef old{0u, 0xFFFFFFFFu};
  SND_CHECK(rows::Remove(r, 0u).Removed());
  SND_CHECK(!rows::IsCurrent(r, old));
  SND_CHECK(rows::InsertNext(r, 0) != snd::kInvalidSlot);
  SND_CHECK_EQ(r.generation[0], 1u);  // wrapped past 0
  SND_CHECK(!rows::IsCurrent(r, old));
  for (std::uint32_t s = 1; s < 4; ++s) SND_CHECK(rows::InsertNext(r, s) != snd::kInvalidSlot);
  SND_CHECK_EQ(r.count, 4u);
  SND_CHECK_EQ(rows::InsertNext(r, 4), snd::kInvalidSlot);  // out of range
  SND_CHECK_EQ(rows::Insert(r, 2, 9u), snd::kInvalidSlot);  // live already
  SND_CHECK(rows::Validate(r));
  // Remove the last dense row: nothing moves.
  const rows::RemoveResult res = rows::Remove(r, rows::SlotAt(r, 3));
  SND_CHECK(res.Removed() && !res.Moved());
  SND_CHECK(rows::Validate(r));
  // Init edge: capacity 0 and the invalid-slot value are rejected.
  snd::schema::TableRows z;
  SND_CHECK(!rows::Init(z, DefaultAllocator(), 0, snd::schema::Indexing::Sparse));
  rows::Destroy(r, DefaultAllocator());
}

SND_TEST(AdvSparseSet, ValidateDetectsCorruption) {
  snd::schema::TableRows r;
  SND_REQUIRE(rows::Init(r, DefaultAllocator(), 8, snd::schema::Indexing::Sparse));
  for (std::uint32_t s : {5u, 1u, 7u}) rows::InsertNext(r, s);
  SND_CHECK(rows::Validate(r));
  std::swap(r.denseToSlot[0], r.denseToSlot[1]);  // dense/sparse disagree
  SND_CHECK(!rows::Validate(r));
  std::swap(r.denseToSlot[0], r.denseToSlot[1]);
  r.count = 2;  // a live slot points past count
  SND_CHECK(!rows::Validate(r));
  r.count = 3;
  r.generation[1] = 0;  // live row with generation 0
  SND_CHECK(!rows::Validate(r));
  r.generation[1] = 1;
  SND_CHECK(rows::Validate(r));
  rows::Destroy(r, DefaultAllocator());
}

// --- inline chain ------------------------------------------------------------------------

SND_TEST(AdvChain, RandomOpsAgainstVectorPoolExhaustionAndNoLeak) {
  using Pool = ChainPool<std::uint32_t>;
  for (std::uint32_t blocks : {1u, 2u, 5u}) {
    Pool pool;
    SND_REQUIRE(pool.Init(DefaultAllocator(), blocks));
    InlineChain<std::uint32_t> a, b;  // two owners share the pool
    std::vector<std::uint32_t> ma, mb;
    Rng rng(blocks * 31u);
    for (int step = 0; step < 20000; ++step) {
      const bool useA = rng.Below(2) == 0u;
      InlineChain<std::uint32_t>& c = useA ? a : b;
      std::vector<std::uint32_t>& mv = useA ? ma : mb;
      const std::uint32_t op = rng.Below(9);
      if (op < 5) {
        const std::uint32_t v = static_cast<std::uint32_t>(rng.Next());
        const std::uint32_t freeBefore = pool.Available();
        const bool needsBlock = mv.size() >= snd::kParamInlineSlots &&
                                (mv.size() - snd::kParamInlineSlots) % Pool::kBlockEntries == 0u;
        const bool ok = c.PushBack(pool, v);
        SND_REQUIRE_EQ(ok, !needsBlock || freeBefore > 0u);
        if (ok) mv.push_back(v);
        else SND_REQUIRE_EQ(pool.Available(), freeBefore);  // a failed push changes nothing
      } else if (op < 8) {
        if (!mv.empty()) {
          const std::uint32_t i = rng.Below(static_cast<std::uint32_t>(mv.size()));
          c.RemoveSwap(pool, i);
          mv[i] = mv.back();
          mv.pop_back();
        } else {
          c.RemoveSwap(pool, 0);  // no-op on empty
        }
      } else {
        c.Clear(pool);
        mv.clear();
      }
      // Contents, order and block accounting.
      for (InlineChain<std::uint32_t>* cc : {&a, &b}) {
        const std::vector<std::uint32_t>& mm = cc == &a ? ma : mb;
        SND_REQUIRE_EQ(cc->Size(), static_cast<std::uint32_t>(mm.size()));
        for (std::uint32_t i = 0; i < mm.size(); ++i) SND_REQUIRE_EQ(*cc->At(pool, i), mm[i]);
        SND_REQUIRE(cc->At(pool, static_cast<std::uint32_t>(mm.size())) == nullptr);
        std::uint32_t k = 0;
        bool same = true;
        cc->ForEach(pool, [&](std::uint32_t& v) { same = same && k < mm.size() && v == mm[k++]; });
        SND_REQUIRE(same && k == mm.size());
      }
      auto blocksFor = [](std::size_t n) -> std::uint32_t {
        if (n <= snd::kParamInlineSlots) return 0u;
        return static_cast<std::uint32_t>((n - snd::kParamInlineSlots + Pool::kBlockEntries - 1u) / Pool::kBlockEntries);
      };
      SND_REQUIRE_EQ(pool.Available() + blocksFor(ma.size()) + blocksFor(mb.size()), blocks);
    }
    a.Clear(pool);
    b.Clear(pool);
    SND_CHECK_EQ(pool.Available(), blocks);
  }
}

// --- pools ----------------------------------------------------------------------------------

SND_TEST(AdvPool, BlockAlignmentStrideAndBadArguments) {
  AccountingAllocator acc(DefaultAllocator());
  for (std::size_t align : {16u, 64u, 128u, 4096u}) {
    BlockPool p;
    SND_REQUIRE(p.Init(acc, 1, 5, snd::MemTag::Misc, align));
    SND_CHECK_EQ(p.BlockBytes(), align);
    for (std::uint32_t i = 0; i < 5; ++i) {
      void* b = p.Acquire();
      SND_REQUIRE(b != nullptr);
      SND_CHECK_EQ(reinterpret_cast<std::uintptr_t>(b) % align, 0u);
      SND_CHECK(p.Owns(b));
      SND_CHECK_EQ(p.IndexOf(b), i);  // deterministic order after Init
    }
    SND_CHECK(p.Acquire() == nullptr);
    SND_CHECK_EQ(p.AcquireIndex(), kInvalidIndex);
    p.Release(p.Block(3));
    SND_CHECK_EQ(p.AcquireIndex(), 3u);
    p.Release(nullptr);  // tolerated
    SND_CHECK(!p.Owns(reinterpret_cast<const std::uint8_t*>(p.Block(0)) + p.BlockBytes() * 5));
    p.Destroy();
  }
  BlockPool bad;
  SND_CHECK(!bad.Init(acc, 0, 4, snd::MemTag::Misc));
  SND_CHECK(!bad.Init(acc, 8, 0, snd::MemTag::Misc));
  SND_CHECK(!bad.Init(acc, 8, 4, snd::MemTag::Misc, 48));  // not a power of two
  SND_CHECK(!bad.Init(acc, 8, kInvalidIndex, snd::MemTag::Misc));
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

// --- allocators / arenas ------------------------------------------------------------------------

SND_TEST(AdvAllocator, AlignmentAndEdgeArguments) {
  snd::IAllocator& sys = DefaultAllocator();
  for (std::size_t align = 1; align <= 8192; align <<= 1) {
    void* p = sys.Allocate(24, align, snd::MemTag::Misc);
    SND_REQUIRE(p != nullptr);
    const std::size_t eff = align < 16u ? 16u : align;
    SND_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % eff, 0u);
    sys.Free(p, snd::MemTag::Misc);
  }
  SND_CHECK(sys.Allocate(0, 64, snd::MemTag::Misc) == nullptr);
  SND_CHECK(sys.Allocate(8, 0, snd::MemTag::Misc) == nullptr);
  SND_CHECK(sys.Allocate(8, 3, snd::MemTag::Misc) == nullptr);
  SND_CHECK(sys.Allocate(8, 96, snd::MemTag::Misc) == nullptr);
#if !defined(SND_SANITIZER_ACTIVE)  // sanitizer allocators abort on impossible sizes
  SND_CHECK(sys.Allocate(~static_cast<std::size_t>(0) - 100u, 64, snd::MemTag::Misc) == nullptr);
#endif
  sys.Free(nullptr, snd::MemTag::Misc);

  AccountingAllocator acc(sys);
  for (std::size_t align : {1u, 16u, 32u, 64u, 256u, 4096u}) {
    void* p = acc.Allocate(100, align, snd::MemTag::Tables);
    SND_REQUIRE(p != nullptr);
    SND_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % (align < 16u ? 16u : align), 0u);
    std::memset(p, 0xAB, 100);  // the header must not overlap the payload (ASan)
    acc.Free(p, snd::MemTag::Tables);
  }
  const snd::MemTag tag = snd::MemTag::Bank;
  SND_CHECK(acc.Allocate(0, 64, tag) == nullptr);
  SND_CHECK(acc.Allocate(16, 48, tag) == nullptr);
  SND_CHECK(acc.Allocate(~static_cast<std::size_t>(0) - 8u, 64, tag) == nullptr);
#if !defined(SND_SANITIZER_ACTIVE)
  SND_CHECK(acc.Allocate(~static_cast<std::size_t>(0) / 2u, 64, tag) == nullptr);  // upstream refuses
  const std::uint64_t expectFailures = 4u;
#else
  const std::uint64_t expectFailures = 3u;
#endif
  const MemTagUsage u = acc.Usage(tag);
  SND_CHECK_EQ(u.used, 0u);
  SND_CHECK_EQ(u.allocations, 0u);
  SND_CHECK_EQ(u.failures, expectFailures);
  SND_CHECK_EQ(u.peak, 0u);  // failed requests never count toward the peak
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(AdvAllocator, BudgetBoundaryPerTagAndStats) {
  AccountingAllocator acc(DefaultAllocator());
  acc.SetBudget(snd::MemTag::StreamPool, 1000);
  void* a = acc.Allocate(600, 64, snd::MemTag::StreamPool);
  void* b = acc.Allocate(400, 16, snd::MemTag::StreamPool);  // exactly at the budget: allowed
  SND_REQUIRE(a != nullptr && b != nullptr);
  SND_CHECK(acc.Allocate(1, 16, snd::MemTag::StreamPool) == nullptr);  // one byte over
  void* other = acc.Allocate(5000, 16, snd::MemTag::PcmCache);          // other tags unaffected
  SND_REQUIRE(other != nullptr);
  std::uint64_t used[SND_MEM_TAG_COUNT] = {}, budget[SND_MEM_TAG_COUNT] = {};
  acc.FillStats(used, budget);
  SND_CHECK_EQ(used[static_cast<int>(snd::MemTag::StreamPool)], 1000u);
  SND_CHECK_EQ(budget[static_cast<int>(snd::MemTag::StreamPool)], 1000u);
  SND_CHECK_EQ(used[static_cast<int>(snd::MemTag::PcmCache)], 5000u);
  SND_CHECK_EQ(acc.TotalUsed(), 6000u);
  acc.Free(a, snd::MemTag::StreamPool);
  void* c = acc.Allocate(600, 16, snd::MemTag::StreamPool);  // freed bytes are available again
  SND_CHECK(c != nullptr);
  MemTagUsage u = acc.Usage(snd::MemTag::StreamPool);
  SND_CHECK_EQ(u.peak, 1000u);
  SND_CHECK_EQ(u.allocations, 2u);
  SND_CHECK_EQ(u.failures, 1u);
  acc.SetBudget(snd::MemTag::StreamPool, 100);  // lowering the budget frees nothing
  SND_CHECK_EQ(acc.Usage(snd::MemTag::StreamPool).used, 1000u);
  SND_CHECK(acc.Allocate(1, 16, snd::MemTag::StreamPool) == nullptr);
  acc.Free(b, snd::MemTag::StreamPool);
  acc.Free(c, snd::MemTag::StreamPool);
  acc.Free(other, snd::MemTag::PcmCache);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(AdvAllocator, ConcurrentAccountingBalancesAndPeakWithinBudget) {
  AccountingAllocator acc(DefaultAllocator());
  acc.SetBudget(snd::MemTag::DspPool, 64u * 1024u);
  constexpr int kThreads = 4;
  std::atomic<std::uint64_t> fails{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      Rng rng(static_cast<std::uint64_t>(t) + 7u);
      void* held[8] = {};
      for (int i = 0; i < 20000; ++i) {
        const std::uint32_t k = rng.Below(8);
        if (held[k] != nullptr) {
          acc.Free(held[k], snd::MemTag::DspPool);
          held[k] = nullptr;
        } else {
          held[k] = acc.Allocate(1u + rng.Below(2048), 64, snd::MemTag::DspPool);
          if (held[k] == nullptr) fails.fetch_add(1);
        }
      }
      for (void* p : held) acc.Free(p, snd::MemTag::DspPool);
    });
  }
  for (auto& t : ts) t.join();
  const MemTagUsage u = acc.Usage(snd::MemTag::DspPool);
  SND_CHECK_EQ(u.used, 0u);
  SND_CHECK_EQ(u.allocations, 0u);
  SND_CHECK(u.peak <= 64u * 1024u);  // 4 threads x 8 x 2 KiB = 64 KiB at most anyway
  SND_CHECK_EQ(u.failures, fails.load());
}

SND_TEST(AdvArena, NestedMarkRewindOverflowAndZeroBytes) {
  LinearArena a;
  SND_REQUIRE(a.Init(DefaultAllocator(), 1024, snd::MemTag::FrameArena));
  SND_CHECK_EQ(reinterpret_cast<std::uintptr_t>(a.data()) % 64u, 0u);
  void* p0 = a.Allocate(10);
  SND_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p0) % 64u, 0u);
  const std::size_t m1 = a.Mark();
  void* p1 = a.Allocate(100, 16);
  const std::size_t m2 = a.Mark();
  void* p2 = a.Allocate(200, 128);
  SND_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p2) % 128u, 0u);
  a.Rewind(m2);  // inner first
  SND_CHECK_EQ(a.Used(), m2);
  void* p2b = a.Allocate(200, 128);
  SND_CHECK(p2b == p2);  // same memory handed out again after the inner rewind
  a.Rewind(m1);
  SND_CHECK_EQ(a.Used(), m1);
  a.Rewind(m2);  // a mark beyond the current position is ignored (would expose stale data)
  SND_CHECK_EQ(a.Used(), m1);
  SND_CHECK(a.Allocate(100, 16) == p1);
  // Zero bytes: never fails, does not advance beyond alignment.
  const std::size_t before = a.Used();
  void* z = a.Allocate(0, 1);
  SND_CHECK(z != nullptr);
  SND_CHECK_EQ(a.Used(), before);
  // Overflow: counted, nothing changes, the arena never grows.
  const std::size_t used = a.Used();
  SND_CHECK(a.Allocate(2048) == nullptr);
  SND_CHECK(a.Allocate(~static_cast<std::size_t>(0)) == nullptr);
  SND_CHECK(a.AllocateArray<std::uint64_t>(~static_cast<std::size_t>(0) / 4u) == nullptr);
  SND_CHECK(a.Allocate(8, std::size_t{1} << 62) == nullptr);
  SND_CHECK_EQ(a.Overflows(), 4u);
  SND_CHECK_EQ(a.Used(), used);
  // Exactly filling the arena works; one more byte does not.
  a.Reset();
  SND_CHECK(a.Allocate(1024, 64) != nullptr);
  SND_CHECK(a.Allocate(1, 1) == nullptr);
  SND_CHECK_EQ(a.HighWater(), 1024u);
  a.Reset();
  unsigned char* zz = static_cast<unsigned char*>(a.AllocateZeroed(64));
  bool allZero = true;
  for (int i = 0; i < 64; ++i) allZero = allZero && zz[i] == 0u;
  SND_CHECK(allZero);
  a.Destroy();
  a.Destroy();  // twice is safe
  SND_CHECK(a.Allocate(1) == nullptr);  // unbound arena fails cleanly
}

SND_TEST(AdvArena, ThreadArenaSetCacheLineSeparation) {
  ThreadArenaSet s;
  SND_REQUIRE(s.Init(DefaultAllocator(), 5, 100));  // rounded up to 128
  for (std::uint32_t i = 0; i < 5; ++i) {
    LinearArena& a = s.Arena(i);
    SND_CHECK_EQ(reinterpret_cast<std::uintptr_t>(a.data()) % 64u, 0u);
    SND_CHECK_EQ(a.Capacity(), 128u);
    if (i > 0) SND_CHECK(s.Arena(i - 1).data() + s.Arena(i - 1).Capacity() <= a.data());
  }
  // Each thread fills its own arena concurrently (ASan/TSan catch overlap).
  std::vector<std::thread> ts;
  for (std::uint32_t i = 0; i < 5; ++i) {
    ts.emplace_back([&s, i] {
      for (int k = 0; k < 2000; ++k) {
        LinearArena& a = s.Arena(i);
        a.Reset();
        auto* p = static_cast<std::uint8_t*>(a.Allocate(128, 64));
        if (p != nullptr) std::memset(p, static_cast<int>(i), 128);
      }
    });
  }
  for (auto& t : ts) t.join();
  for (std::uint32_t i = 0; i < 5; ++i) SND_CHECK_EQ(s.Arena(i).data()[127], static_cast<std::uint8_t>(i));
  SND_CHECK(!s.Init(DefaultAllocator(), 0, 64));
  SND_CHECK(!s.Init(DefaultAllocator(), 2, 0));
}

// --- log ------------------------------------------------------------------------------------

SND_TEST(AdvLog, RtSideNeverAllocatesOrFormatsEvenWhenFull) {
  LogRing log;
  SND_REQUIRE(log.Init(DefaultAllocator(), 4));
  snd::test::rt::Reset();
  {
    SND_RT_SCOPE();
    for (int i = 0; i < 100; ++i) {
      log.Log(LogLevel::Warn, "v %d %u %f %s", -i, static_cast<unsigned>(i), 0.5 * i, "static");
      log.LogAt(static_cast<std::uint64_t>(i), LogLevel::Error, "%p", static_cast<const void*>(&log));
    }
    LogEntry e;
    while (log.TryPop(e)) {
    }
  }
  SND_CHECK_EQ(snd::test::rt::Total(), 0u);  // no alloc, lock, or Log note on the RT side
  SND_CHECK_EQ(log.Dropped(), 200u - 4u);    // exactly the overflow beyond the 4 slots
  SND_CHECK_EQ(log.TakeDropped(), 196u);
  SND_CHECK_EQ(log.Dropped(), 0u);
}

SND_TEST(AdvLog, ArgumentCountsAndConversions) {
  LogRing log;
  SND_REQUIRE(log.Init(DefaultAllocator(), 32));
  log.Log(LogLevel::Info, "none");
  log.Log(LogLevel::Info, "%d %d %d %d", 1, -2, 3, -4);
  log.Log(LogLevel::Info, "%d %d %d %d %d", 1, 2, 3, 4);  // fifth has no argument
  log.Log(LogLevel::Info, "%u %x %o %c", 4294967295u, 255u, 8u, 'q');
  log.Log(LogLevel::Info, "%lld %llu %hhd %zu", -5ll, 18446744073709551615ull, static_cast<signed char>(-1), std::size_t{7});
  log.Log(LogLevel::Info, "%+.3f|%e|%g|%5.1f", 1.0 / 3.0, 12345.0f, 0.5, -2.25);
  log.Log(LogLevel::Info, "%s|%s", static_cast<const char*>(nullptr), "x");
  log.Log(LogLevel::Info, "%d %s", true, "b");
  log.Log(LogLevel::Info, "100%% %d%%", 50);
  std::vector<std::string> lines;
  log.Drain([&](const LogEntry&, const char* text) { lines.emplace_back(text); });
  SND_REQUIRE_EQ(lines.size(), 9u);
  const char* expect[] = {"none",
                          "1 -2 3 -4",
                          "1 2 3 4 <?>",
                          "4294967295 ff 10 q",
                          "-5 18446744073709551615 -1 7",
                          "+0.333|1.234500e+04|0.5| -2.2",
                          "(null)|x",
                          "1 b",
                          "100% 50%"};
  for (std::size_t i = 0; i < lines.size(); ++i) {
    SND_CHECK_MSG(lines[i] == expect[i], "line %zu: got '%s' expected '%s'", i, lines[i].c_str(), expect[i]);
  }
}

SND_TEST(AdvLog, MalformedFormatsDoNotCrashAndStayBounded) {
  // Hostile but static format strings: unknown conversions, %n, '*', runs of
  // flags/digits longer than the internal spec buffer, a trailing '%'.
  const char* formats[] = {
      "%",        "%%%",          "%n",          "%*d",       "%.*f",         "%-",          "%5",
      "%llll",    "%q%w%y",       "%0000000000000000000000000000000000000000000000000000d",
      "%-+ #0-+ #0-+ #0-+ #0d",   "%.99999999999999999999f", "%999999999999999999999999999d",
      "%lc %lu",  "%1$d",         "%'d",          "%#o %#x %#X", "%+d %0p %#c",
  };
  char out[64];
  for (const char* f : formats) {
    LogEntry e{f, {1u, 2u, 3u, 4u}, 0u, 0u, 4u};
    const std::size_t n = FormatLogEntry(e, out, sizeof(out));
    SND_CHECK(n < sizeof(out));
    SND_CHECK_EQ(std::strlen(out), n);
  }
  // Tiny output buffers: always terminated, never overrun.
  LogEntry e{"abcdef %d", {12345u}, 0u, 0u, 1u};
  char one[1] = {'x'};
  SND_CHECK_EQ(FormatLogEntry(e, one, 1), 0u);
  SND_CHECK_EQ(one[0], '\0');
  char four[4];
  SND_CHECK_EQ(FormatLogEntry(e, four, 4), 3u);
  SND_CHECK(std::strcmp(four, "abc") == 0);
  SND_CHECK_EQ(FormatLogEntry(e, nullptr, 10), 0u);
  LogEntry nullFmt{nullptr, {}, 0u, 0u, 0u};
  SND_CHECK(FormatLogEntry(nullFmt, out, sizeof(out)) > 0u);
  // argCount larger than the storage is clamped (no read past args[3]).
  LogEntry many{"%d %d %d %d %d %d", {1u, 2u, 3u, 4u}, 0u, 0u, 99u};
  FormatLogEntry(many, out, sizeof(out));
  SND_CHECK_MSG(std::strcmp(out, "1 2 3 4 <?> <?>") == 0, "got '%s'", out);
  // Long output is truncated at the Drain buffer (512) and terminated.
  LogRing log;
  SND_REQUIRE(log.Init(DefaultAllocator(), 2));
  static const char kLong[] =
      "%s%s%s%s"
      "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
      "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
      "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
      "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
      "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
      "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789";
  log.Log(LogLevel::Info, kLong, "a", "b", "c", "d");
  std::size_t len = 0;
  log.Drain([&](const LogEntry&, const char* text) { len = std::strlen(text); });
  SND_CHECK_EQ(len, 511u);
}

SND_TEST(AdvLog, ConcurrentProducersExactAccountingAndPerProducerOrder) {
  constexpr std::uint32_t kProducers = 4;
  const std::uint32_t kPer = 20000u * StressScale();
  LogRing log;
  SND_REQUIRE(log.Init(DefaultAllocator(), 8));
  std::atomic<std::uint64_t> accepted{0};
  std::atomic<bool> done{false};
  std::vector<std::thread> ts;
  for (std::uint32_t p = 0; p < kProducers; ++p) {
    ts.emplace_back([&, p] {
      SND_RT_SCOPE();
      for (std::uint32_t i = 0; i < kPer; ++i) {
        if (log.Log(LogLevel::Info, "%u %u", p, i)) accepted.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::thread joiner([&] {
    for (auto& t : ts) t.join();
    done.store(true);
  });
  std::uint64_t got = 0, bad = 0;
  std::vector<std::int64_t> last(kProducers, -1);
  auto handle = [&](const LogEntry& e) {
    ++got;
    if (e.argCount != 2u || e.args[0] >= kProducers) {
      ++bad;
      return;
    }
    const auto p = static_cast<std::uint32_t>(e.args[0]);
    if (static_cast<std::int64_t>(e.args[1]) <= last[p]) ++bad;  // per-producer FIFO (single consumer)
    last[p] = static_cast<std::int64_t>(e.args[1]);
  };
  LogEntry e;
  for (;;) {
    const bool fin = done.load();
    while (log.TryPop(e)) handle(e);
    if (fin) break;
  }
  joiner.join();
  SND_CHECK_EQ(bad, 0u);
  SND_CHECK_EQ(got, accepted.load());
  SND_CHECK_EQ(got + log.Dropped(), static_cast<std::uint64_t>(kProducers) * kPer);
}
