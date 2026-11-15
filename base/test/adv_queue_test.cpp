// Independent tests: SPSC / MPMC rings, ArenaCycle, lock-free free lists and
// the handle allocator (design.md 2.1, 3.1). Written from the spec and the
// public headers; they target capacity edges, ownership hand-over and
// duplicate/loss detection under contention (run them under TSan).
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "adv_test_util.hpp"
#include "snd/base/allocator.hpp"
#include "snd/base/handle.hpp"
#include "snd/base/mpmc.hpp"
#include "snd/base/pool.hpp"
#include "snd/base/spsc.hpp"
#include "snd/test/test.hpp"

using namespace snd::base;
using snd::advtest::Rng;
using snd::advtest::StartGate;
using snd::advtest::StressScale;

// --- SPSC ------------------------------------------------------------------------

SND_TEST(AdvSpsc, CapacityRoundingAndExactBounds) {
  const std::uint32_t requests[] = {0u, 1u, 2u, 3u, 5u, 64u, 65u, 1000u};
  const std::uint32_t expected[] = {2u, 2u, 2u, 4u, 8u, 64u, 128u, 1024u};
  for (std::size_t k = 0; k < sizeof(requests) / sizeof(requests[0]); ++k) {
    AccountingAllocator acc(DefaultAllocator());
    SpscRing<std::uint64_t> r;
    SND_REQUIRE(r.Init(acc, requests[k], snd::MemTag::Misc));
    SND_CHECK_EQ(r.Capacity(), expected[k]);
    // Exactly Capacity() pushes succeed, then full; after one pop exactly one more fits.
    for (std::uint32_t i = 0; i < r.Capacity(); ++i) SND_CHECK(r.TryPush(i));
    SND_CHECK(!r.TryPush(999u));
    SND_CHECK_EQ(r.SizeApprox(), r.Capacity());
    std::uint64_t v = 0;
    SND_CHECK(r.TryPop(v));
    SND_CHECK_EQ(v, 0u);
    SND_CHECK(r.TryPush(777u));
    SND_CHECK(!r.TryPush(778u));
    for (std::uint32_t i = 1; i < r.Capacity(); ++i) {
      SND_CHECK(r.TryPop(v));
      SND_CHECK_EQ(v, i);
    }
    SND_CHECK(r.TryPop(v));
    SND_CHECK_EQ(v, 777u);
    SND_CHECK(!r.TryPop(v));
    SND_CHECK(r.Peek() == nullptr);
    SND_CHECK(r.EmptyApprox());
    r.Destroy();
    SND_CHECK_EQ(acc.TotalUsed(), 0u);
  }
}

SND_TEST(AdvSpsc, ManyLapsAroundTheRingKeepOrder) {
  // Indices are 64-bit and only masked, so the slot index wraps every
  // `capacity` pushes. Run many laps with a varying fill level.
  SpscRing<std::uint32_t> r;
  SND_REQUIRE(r.Init(DefaultAllocator(), 4, snd::MemTag::Misc));
  Rng rng(1234);
  std::uint32_t nextIn = 0, nextOut = 0;
  for (int step = 0; step < 200000; ++step) {
    if (rng.Below(2) == 0u) {
      if (r.TryPush(nextIn)) ++nextIn;
      else SND_REQUIRE_EQ(nextIn - nextOut, 4u);
    } else {
      std::uint32_t v = 0;
      if (r.TryPop(v)) {
        SND_REQUIRE_EQ(v, nextOut);
        ++nextOut;
      } else {
        SND_REQUIRE_EQ(nextIn, nextOut);
      }
    }
    const std::uint32_t* p = r.Peek();
    if (nextIn != nextOut) {
      SND_REQUIRE(p != nullptr);
      SND_REQUIRE_EQ(*p, nextOut);
    }
  }
}

namespace {
struct Big {
  std::uint64_t seq;
  std::uint64_t words[7];  // all equal to seq * 3 + 1 (torn element detection)
};
}  // namespace

SND_TEST(AdvSpsc, ConcurrentCapacityTwoNoLossNoDupNoTear) {
  SpscRing<Big> r;
  SND_REQUIRE(r.Init(DefaultAllocator(), 2, snd::MemTag::Misc));
  const std::uint64_t kN = 200000ull * StressScale();
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kN;) {
      Big b;
      b.seq = i;
      for (auto& w : b.words) w = i * 3u + 1u;
      if (r.TryPush(b)) ++i;
      else std::this_thread::yield();
    }
  });
  std::uint64_t expect = 0, sum = 0, bad = 0;
  while (expect < kN) {
    Big b;
    if (!r.TryPop(b)) continue;
    if (b.seq != expect) ++bad;
    for (auto w : b.words) bad += w != b.seq * 3u + 1u ? 1u : 0u;
    sum += b.seq;
    ++expect;
  }
  producer.join();
  SND_CHECK_EQ(bad, 0u);
  SND_CHECK_EQ(sum, kN * (kN - 1u) / 2u);
  Big tail;
  SND_CHECK(!r.TryPop(tail));
}

// --- ArenaCycle ------------------------------------------------------------------

SND_TEST(AdvArenaCycle, CountOneIsRejectedTwoAndThreeCycle) {
  ArenaCycle one;
  SND_CHECK(!one.Init(DefaultAllocator(), 1));
  SND_CHECK(!one.Init(DefaultAllocator(), 0));

  for (std::uint32_t count : {2u, 3u, 5u}) {
    ArenaCycle c;
    SND_REQUIRE(c.Init(DefaultAllocator(), count));
    SND_CHECK_EQ(c.Count(), count);
    SND_CHECK_EQ(c.Current(), 0u);
    // The producer can publish count-1 times without any return, then never again.
    std::vector<std::uint32_t> published;
    for (std::uint32_t i = 0; i + 1u < count; ++i) {
      const std::uint32_t cur = c.Current();
      SND_CHECK(c.Publish());
      published.push_back(cur);
      SND_CHECK(c.Current() != cur);
    }
    const std::uint32_t stuck = c.Current();
    for (int k = 0; k < 10; ++k) {
      SND_CHECK(!c.Publish());               // nothing returned: keep appending (2.1)
      SND_CHECK_EQ(c.Current(), stuck);       // the current arena is unchanged
    }
    // Consumer sees them in publish order; none of them is the producer's current arena.
    std::vector<std::uint32_t> taken;
    std::uint32_t idx = 0;
    while (c.Take(idx)) {
      SND_CHECK(idx != c.Current());
      taken.push_back(idx);
    }
    SND_CHECK(taken == published);
    // A delayed return: Publish keeps failing until exactly one arena comes back.
    SND_CHECK(!c.Publish());
    c.Return(taken.back());
    SND_CHECK(c.Publish());
    SND_CHECK_EQ(c.Current(), taken.back());
    SND_CHECK(!c.Publish());
    std::uint32_t again = 0;
    SND_CHECK(c.Take(again));
    SND_CHECK_EQ(again, stuck);
    SND_CHECK(!c.Take(again));
    // Every index is in exactly one place: producer current, returned, or with the consumer.
    std::vector<int> owner(count, 0);
    owner[c.Current()]++;
    owner[again]++;
    for (std::size_t i = 0; i + 1u < taken.size(); ++i) owner[taken[i]]++;
    for (std::uint32_t i = 0; i < count; ++i) SND_CHECK_MSG(owner[i] == 1, "arena %u owned %d times", i, owner[i]);
  }
}

namespace {
// Per-arena ownership word: who holds the arena right now.
enum : std::uint32_t { kProducer = 1, kInFlight = 2, kConsumer = 3, kReturning = 4 };
}

SND_TEST(AdvArenaCycle, ConcurrentOwnershipAndContentWithSlowConsumer) {
  // Producer writes a pattern into its current arena, publishes; consumer
  // validates the whole arena while holding it, sometimes holding several
  // arenas before returning them (late return). The producer must never be
  // handed an arena the consumer still reads.
  for (std::uint32_t count : {2u, 3u, 4u}) {
    constexpr std::size_t kWords = 256;
    ArenaCycle c;
    SND_REQUIRE(c.Init(DefaultAllocator(), count));
    std::vector<std::uint64_t> mem(static_cast<std::size_t>(count) * kWords, 0u);
    std::vector<std::atomic<std::uint32_t>> owner(count);
    for (auto& o : owner) o.store(kReturning);  // 1..count-1 start in the return ring
    owner[0].store(kProducer);
    const std::uint64_t kPublishes = 20000ull * StressScale();
    std::atomic<std::uint64_t> violations{0};
    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
      std::uint64_t seq = 1;
      std::uint64_t ok = 0;
      while (ok < kPublishes) {
        const std::uint32_t cur = c.Current();
        std::uint64_t* a = mem.data() + static_cast<std::size_t>(cur) * kWords;
        for (std::size_t i = 0; i < kWords; ++i) a[i] = seq;  // record into the current arena
        if (owner[cur].exchange(kInFlight) != kProducer) violations.fetch_add(1);
        if (c.Publish()) {
          ++ok;
          ++seq;
          const std::uint32_t next = c.Current();
          const std::uint32_t prev = owner[next].exchange(kProducer);
          if (prev != kReturning) violations.fetch_add(1);  // must come from the return ring
        } else {
          owner[cur].store(kProducer);  // nothing published: still ours
          std::this_thread::yield();
        }
      }
      producerDone.store(true);
    });
    std::uint64_t lastSeq = 0, received = 0;
    std::vector<std::uint32_t> held;
    Rng rng(count * 77u);
    while (!producerDone.load() || !held.empty() || received < kPublishes) {
      std::uint32_t idx = 0;
      if (c.Take(idx)) {
        if (owner[idx].exchange(kConsumer) != kInFlight) violations.fetch_add(1);
        const std::uint64_t* a = mem.data() + static_cast<std::size_t>(idx) * kWords;
        const std::uint64_t s = a[0];
        for (std::size_t i = 0; i < kWords; ++i) {
          if (a[i] != s) violations.fetch_add(1);  // producer wrote while we read
        }
        if (s != lastSeq + 1u) violations.fetch_add(1);  // publish order
        lastSeq = s;
        ++received;
        held.push_back(idx);
      }
      // Return late: hold up to count-1 arenas, release in FIFO order at random times.
      if (!held.empty() && (held.size() + 1u >= count || rng.Below(4) == 0u)) {
        const std::uint32_t r = held.front();
        held.erase(held.begin());
        owner[r].store(kReturning);
        c.Return(r);
      }
      if (producerDone.load() && received >= kPublishes) {
        for (std::uint32_t r : held) {
          owner[r].store(kReturning);
          c.Return(r);
        }
        held.clear();
      }
    }
    producer.join();
    SND_CHECK_MSG(violations.load() == 0u, "count=%u: %llu ownership/content violations", count,
                  static_cast<unsigned long long>(violations.load()));
    SND_CHECK_EQ(received, kPublishes);
  }
}

// --- MPMC ------------------------------------------------------------------------

SND_TEST(AdvMpmc, CapacityEdgesSingleThread) {
  for (std::uint32_t req : {0u, 1u, 2u, 3u, 17u}) {
    MpmcRing<std::uint32_t> q;
    SND_REQUIRE(q.Init(DefaultAllocator(), req, snd::MemTag::Misc));
    const std::uint32_t cap = q.Capacity();
    SND_CHECK(IsPowerOfTwo(cap) && cap >= 2u && cap >= req);
    for (int lap = 0; lap < 5; ++lap) {  // several laps: sequence numbers advance by capacity
      for (std::uint32_t i = 0; i < cap; ++i) SND_CHECK(q.TryPush(lap * 1000u + i));
      SND_CHECK(!q.TryPush(0xDEADu));
      std::uint32_t v = 0;
      for (std::uint32_t i = 0; i < cap; ++i) {
        SND_CHECK(q.TryPop(v));
        SND_CHECK_EQ(v, lap * 1000u + i);
      }
      SND_CHECK(!q.TryPop(v));
    }
  }
}

SND_TEST(AdvMpmc, TinyRingManyThreadsExactlyOncePerProducerOrder) {
  constexpr std::uint32_t kProducers = 4, kConsumers = 4;
  const std::uint32_t kPer = 30000u * StressScale();
  MpmcRing<std::uint64_t> q;
  SND_REQUIRE(q.Init(DefaultAllocator(), 2, snd::MemTag::Misc));
  std::vector<std::atomic<std::uint8_t>> seen(static_cast<std::size_t>(kProducers) * kPer);
  for (auto& s : seen) s.store(0);
  std::atomic<std::uint64_t> orderViolations{0}, popped{0};
  StartGate gate(kProducers + kConsumers);
  std::vector<std::thread> ts;
  for (std::uint32_t p = 0; p < kProducers; ++p) {
    ts.emplace_back([&, p] {
      gate.ArriveAndWait();
      for (std::uint32_t i = 0; i < kPer;) {
        if (q.TryPush((static_cast<std::uint64_t>(p) << 32) | i)) ++i;
      }
    });
  }
  for (std::uint32_t c = 0; c < kConsumers; ++c) {
    ts.emplace_back([&] {
      gate.ArriveAndWait();
      std::uint32_t last[kProducers];
      for (auto& l : last) l = 0xFFFFFFFFu;
      while (popped.load(std::memory_order_relaxed) < static_cast<std::uint64_t>(kProducers) * kPer) {
        std::uint64_t v = 0;
        if (!q.TryPop(v)) continue;
        popped.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t p = static_cast<std::uint32_t>(v >> 32), i = static_cast<std::uint32_t>(v);
        if (p >= kProducers || i >= kPer) {
          orderViolations.fetch_add(1);
          continue;
        }
        // A single consumer pops positions in increasing order, so it sees each producer's items in order.
        if (last[p] != 0xFFFFFFFFu && i <= last[p]) orderViolations.fetch_add(1);
        last[p] = i;
        seen[static_cast<std::size_t>(p) * kPer + i].fetch_add(1);
      }
    });
  }
  for (auto& t : ts) t.join();
  std::uint64_t missing = 0, dup = 0;
  for (auto& s : seen) {
    const std::uint8_t n = s.load();
    missing += n == 0u ? 1u : 0u;
    dup += n > 1u ? 1u : 0u;
  }
  SND_CHECK_EQ(missing, 0u);
  SND_CHECK_EQ(dup, 0u);
  SND_CHECK_EQ(orderViolations.load(), 0u);
}

// --- Treiber free list / handle allocator ----------------------------------------

SND_TEST(AdvFreeList, CapacityOneAndTwoUnderHeavyContentionNoDoubleOwnership) {
  // Small capacities maximise ABA opportunities: the same index is popped,
  // pushed and popped again while other threads hold stale heads.
  for (std::uint32_t cap : {1u, 2u, 3u}) {
    ConcurrentIndexFreeList fl;
    SND_REQUIRE(fl.Init(DefaultAllocator(), cap, snd::MemTag::Misc));
    std::vector<std::atomic<std::uint32_t>> owned(cap);
    for (auto& o : owned) o.store(0);
    std::atomic<std::uint64_t> doubleOwn{0}, badIndex{0}, ops{0};
    constexpr std::uint32_t kThreads = 8;
    const std::uint32_t kIters = 40000u * StressScale();
    StartGate gate(kThreads);
    std::vector<std::thread> ts;
    for (std::uint32_t t = 0; t < kThreads; ++t) {
      ts.emplace_back([&] {
        gate.ArriveAndWait();
        for (std::uint32_t i = 0; i < kIters; ++i) {
          const std::uint32_t idx = fl.Pop();
          if (idx == kInvalidIndex) continue;
          if (idx >= cap) {
            badIndex.fetch_add(1);
            continue;
          }
          if (owned[idx].exchange(1) != 0u) doubleOwn.fetch_add(1);
          ops.fetch_add(1, std::memory_order_relaxed);
          owned[idx].store(0);
          fl.Push(idx);
        }
      });
    }
    for (auto& t : ts) t.join();
    SND_CHECK_MSG(doubleOwn.load() == 0u && badIndex.load() == 0u, "cap=%u double=%llu bad=%llu", cap,
                  static_cast<unsigned long long>(doubleOwn.load()), static_cast<unsigned long long>(badIndex.load()));
    SND_CHECK(ops.load() > 0u);
    // Quiescent: every index exactly once, Available exact.
    SND_CHECK_EQ(fl.Available(), cap);
    std::vector<int> got(cap, 0);
    for (std::uint32_t i = 0; i < cap; ++i) {
      const std::uint32_t idx = fl.Pop();
      SND_REQUIRE(idx < cap);
      got[idx]++;
    }
    SND_CHECK_EQ(fl.Pop(), kInvalidIndex);
    for (std::uint32_t i = 0; i < cap; ++i) SND_CHECK_EQ(got[i], 1);
  }
}

SND_TEST(AdvFreeList, HoldersKeepIndicesWhileOthersChurn) {
  // Half the threads hold several indices for a while (so the list is
  // nearly empty and heads are recycled quickly), the other half churn.
  constexpr std::uint32_t kCap = 16, kThreads = 8;
  ConcurrentIndexFreeList fl;
  SND_REQUIRE(fl.Init(DefaultAllocator(), kCap, snd::MemTag::Misc));
  std::vector<std::atomic<std::uint32_t>> owned(kCap);
  for (auto& o : owned) o.store(0);
  std::atomic<std::uint64_t> doubleOwn{0};
  StartGate gate(kThreads);
  std::vector<std::thread> ts;
  for (std::uint32_t t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      gate.ArriveAndWait();
      Rng rng(t + 1u);
      std::uint32_t mine[kCap];
      std::uint32_t n = 0;
      for (std::uint32_t i = 0; i < 30000u * StressScale(); ++i) {
        const bool take = n == 0u || (n < (t % 2u == 0u ? 6u : 1u) && rng.Below(2) == 0u);
        if (take) {
          const std::uint32_t idx = fl.Pop();
          if (idx == kInvalidIndex) continue;
          if (owned[idx].exchange(1) != 0u) doubleOwn.fetch_add(1);
          mine[n++] = idx;
        } else {
          const std::uint32_t k = rng.Below(n);
          const std::uint32_t idx = mine[k];
          mine[k] = mine[--n];
          owned[idx].store(0);
          fl.Push(idx);
        }
      }
      while (n > 0u) {
        const std::uint32_t idx = mine[--n];
        owned[idx].store(0);
        fl.Push(idx);
      }
    });
  }
  for (auto& t : ts) t.join();
  SND_CHECK_EQ(doubleOwn.load(), 0u);
  SND_CHECK_EQ(fl.Available(), kCap);
}

SND_TEST(AdvHandle, FullCapacityStaleAndDoubleRelease) {
  HandleAllocator h;
  SND_REQUIRE(h.Init(DefaultAllocator(), 3));
  snd::HandleRef a = h.Acquire(), b = h.Acquire(), c = h.Acquire();
  SND_CHECK(a.generation != 0u && b.generation != 0u && c.generation != 0u);
  const snd::HandleRef none = h.Acquire();
  SND_CHECK_EQ(none.slot, snd::kInvalidSlot);
  SND_CHECK_EQ(none.generation, 0u);
  SND_CHECK_EQ(MakeHandle(none.slot, none.generation) >> 32, 0u);  // value with generation 0 = invalid
  SND_CHECK(h.Release(b));
  SND_CHECK(!h.Release(b));  // double release ignored
  SND_CHECK(!h.IsCurrent(b));
  const snd::HandleRef b2 = h.Acquire();
  SND_CHECK_EQ(b2.slot, b.slot);
  SND_CHECK(b2.generation != b.generation);
  SND_CHECK(!h.IsCurrent(b));
  SND_CHECK(!h.Release(b));  // stale release must not free the new owner's slot
  SND_CHECK(h.IsCurrent(b2));
  SND_CHECK(!h.Release(snd::HandleRef{99u, 1u}));
  SND_CHECK(!h.Release(snd::HandleRef{a.slot, 0u}));
  SND_CHECK(!h.IsCurrent(snd::HandleRef{a.slot, 0u}));
  SND_CHECK_EQ(h.Available(), 0u);
}

SND_TEST(AdvHandle, GenerationsStayBelowLiveBitAndSkipZeroOverManyCycles) {
  HandleAllocator h;
  SND_REQUIRE(h.Init(DefaultAllocator(), 1));
  std::uint32_t last = 0;
  for (std::uint32_t i = 0; i < 100000u; ++i) {
    const snd::HandleRef r = h.Acquire();
    SND_REQUIRE_EQ(r.slot, 0u);
    SND_REQUIRE(r.generation != 0u);
    SND_REQUIRE((r.generation & HandleAllocator::kLiveBit) == 0u);
    SND_REQUIRE(r.generation != last);
    last = r.generation;
    SND_REQUIRE(h.Release(r));
  }
}

SND_TEST(AdvHandle, ConcurrentAcquireReleaseStaleNeverCurrent) {
  // Each thread keeps its last released handle and checks it stays stale
  // while other threads reuse the slot.
  constexpr std::uint32_t kThreads = 6;
  HandleAllocator h;
  SND_REQUIRE(h.Init(DefaultAllocator(), 4));
  std::atomic<std::uint64_t> staleCurrent{0}, staleReleased{0}, doubleOwn{0};
  std::vector<std::atomic<std::uint32_t>> owner(4);
  for (auto& o : owner) o.store(0);
  StartGate gate(kThreads);
  std::vector<std::thread> ts;
  for (std::uint32_t t = 0; t < kThreads; ++t) {
    ts.emplace_back([&] {
      gate.ArriveAndWait();
      snd::HandleRef old{snd::kInvalidSlot, 0u};
      for (std::uint32_t i = 0; i < 30000u * StressScale(); ++i) {
        const snd::HandleRef r = h.Acquire();
        if (r.generation == 0u) continue;
        if (owner[r.slot].exchange(1) != 0u) doubleOwn.fetch_add(1);
        if (old.generation != 0u) {
          if (h.IsCurrent(old)) staleCurrent.fetch_add(1);
          if (h.Release(old)) staleReleased.fetch_add(1);
        }
        owner[r.slot].store(0);
        if (!h.Release(r)) doubleOwn.fetch_add(1);
        old = r;
      }
    });
  }
  for (auto& t : ts) t.join();
  SND_CHECK_EQ(staleCurrent.load(), 0u);
  SND_CHECK_EQ(staleReleased.load(), 0u);
  SND_CHECK_EQ(doubleOwn.load(), 0u);
  SND_CHECK_EQ(h.Available(), 4u);
}
