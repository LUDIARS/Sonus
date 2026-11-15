// base: SPSC / MPMC queues, arena cycle, lock-free free list under load
// (design.md 2.1). The threaded tests are sized to give ThreadSanitizer real
// interleavings.
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/base/mpmc.hpp"
#include "snd/base/pool.hpp"
#include "snd/base/spsc.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::base;
using snd::MemTag;

SND_TEST(Spsc, FifoAndBounds) {
  AccountingAllocator acc(DefaultAllocator());
  SpscRing<std::uint32_t> q;
  SND_REQUIRE(q.Init(acc, 3, MemTag::Misc));
  SND_CHECK_EQ(q.Capacity(), 4u);
  {
    SND_RT_SCOPE();
    for (std::uint32_t i = 0; i < 4; ++i) SND_CHECK(q.TryPush(i));
    SND_CHECK(!q.TryPush(99));
    SND_CHECK_EQ(q.SizeApprox(), 4u);
    SND_CHECK_EQ(*q.Peek(), 0u);
    std::uint32_t v = 0;
    for (std::uint32_t i = 0; i < 4; ++i) {
      SND_CHECK(q.TryPop(v));
      SND_CHECK_EQ(v, i);
    }
    SND_CHECK(!q.TryPop(v));
    SND_CHECK(q.Peek() == nullptr);
  }
  q.Destroy();
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(Spsc, ConcurrentTransfersEverythingInOrder) {
  SpscRing<std::uint64_t> q;
  SND_REQUIRE(q.Init(DefaultAllocator(), 64, MemTag::Misc));
  constexpr std::uint64_t kCount = 300000;
  std::atomic<bool> bad{false};
  std::thread consumer([&] {
    std::uint64_t expect = 0, v = 0;
    while (expect < kCount) {
      if (q.TryPop(v)) {
        if (v != expect) bad = true;
        ++expect;
      }
    }
  });
  for (std::uint64_t i = 0; i < kCount;) {
    if (q.TryPush(i)) ++i;
  }
  consumer.join();
  SND_CHECK(!bad.load());
}

namespace {
struct Payload {  // stands in for a command arena
  std::uint64_t words[8];
};
}  // namespace

SND_TEST(ArenaCycle, PublishTakeReturn) {
  ArenaCycle c;
  SND_REQUIRE(c.Init(DefaultAllocator(), 3));
  SND_CHECK_EQ(c.Current(), 0u);
  SND_CHECK(c.Publish());  // 0 published, now 1
  SND_CHECK(c.Publish());  // 1 published, now 2
  SND_CHECK(!c.Publish());  // nothing returned yet: keep recording into 2
  SND_CHECK_EQ(c.Current(), 2u);
  std::uint32_t i = 99;
  SND_CHECK(c.Take(i));
  SND_CHECK_EQ(i, 0u);
  c.Return(i);
  SND_CHECK(c.Publish());  // 2 published, now 0
  SND_CHECK_EQ(c.Current(), 0u);
  SND_CHECK(c.Take(i));
  SND_CHECK_EQ(i, 1u);
  SND_CHECK(c.Take(i));
  SND_CHECK_EQ(i, 2u);
  SND_CHECK(!c.Take(i));
  c.Destroy();
}

SND_TEST(ArenaCycle, ConcurrentProducerConsumerSeesArenaContents) {
  constexpr std::uint32_t kArenas = 3;
  Payload arenas[kArenas] = {};
  ArenaCycle c;
  SND_REQUIRE(c.Init(DefaultAllocator(), kArenas));
  constexpr std::uint64_t kPublishes = 50000;
  std::atomic<bool> bad{false};
  std::atomic<bool> done{false};
  std::thread audio([&] {
    std::uint64_t expect = 0;
    while (!done.load(std::memory_order_acquire) || expect < kPublishes) {
      std::uint32_t i = 0;
      if (!c.Take(i)) {
        if (done.load(std::memory_order_acquire) && expect >= kPublishes) break;
        continue;
      }
      for (std::uint64_t w : arenas[i].words) {
        if (w != expect) bad = true;
      }
      ++expect;
      c.Return(i);
    }
  });
  std::uint64_t seq = 0;
  while (seq < kPublishes) {
    Payload& p = arenas[c.Current()];
    for (auto& w : p.words) w = seq;  // "record" into the current arena
    if (c.Publish()) ++seq;
  }
  done.store(true, std::memory_order_release);
  audio.join();
  SND_CHECK(!bad.load());
  c.Destroy();
}

SND_TEST(Mpmc, FifoSingleThreadAndBounds) {
  MpmcRing<std::uint32_t> q;
  SND_REQUIRE(q.Init(DefaultAllocator(), 4, MemTag::Misc));
  SND_RT_SCOPE();
  for (std::uint32_t i = 0; i < 4; ++i) SND_CHECK(q.TryPush(i));
  SND_CHECK(!q.TryPush(4));
  std::uint32_t v;
  for (std::uint32_t i = 0; i < 4; ++i) {
    SND_CHECK(q.TryPop(v));
    SND_CHECK_EQ(v, i);
  }
  SND_CHECK(!q.TryPop(v));
  // wrap around many times
  for (std::uint32_t i = 0; i < 1000; ++i) {
    SND_CHECK(q.TryPush(i));
    SND_CHECK(q.TryPop(v));
    SND_CHECK_EQ(v, i);
  }
}

SND_TEST(Mpmc, ManyProducersManyConsumers) {
  MpmcRing<std::uint64_t> q;
  SND_REQUIRE(q.Init(DefaultAllocator(), 128, MemTag::Misc));
  constexpr std::uint32_t kProducers = 4, kConsumers = 3;
  constexpr std::uint64_t kPerProducer = 50000;
  std::atomic<std::uint64_t> sum{0}, count{0};
  std::atomic<bool> orderBad{false};
  std::vector<std::thread> threads;
  for (std::uint32_t p = 0; p < kProducers; ++p) {
    threads.emplace_back([&, p] {
      for (std::uint64_t i = 0; i < kPerProducer;) {
        if (q.TryPush((static_cast<std::uint64_t>(p) << 32) | i)) ++i;
      }
    });
  }
  for (std::uint32_t c = 0; c < kConsumers; ++c) {
    threads.emplace_back([&] {
      std::uint64_t last[kProducers];
      for (auto& l : last) l = ~0ull;
      while (count.load(std::memory_order_relaxed) < kProducers * kPerProducer) {
        std::uint64_t v;
        if (!q.TryPop(v)) continue;
        const std::uint32_t p = static_cast<std::uint32_t>(v >> 32);
        const std::uint64_t i = v & 0xFFFFFFFFu;
        if (last[p] != ~0ull && i <= last[p]) orderBad = true;  // per-producer FIFO per consumer
        last[p] = i;
        sum.fetch_add(i, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& t : threads) t.join();
  SND_CHECK_EQ(count.load(), kProducers * kPerProducer);
  SND_CHECK_EQ(sum.load(), kProducers * (kPerProducer * (kPerProducer - 1u) / 2u));
  SND_CHECK(!orderBad.load());
}

SND_TEST(FreeList, ConcurrentPopPushKeepsEveryIndex) {
  ConcurrentIndexFreeList l;
  constexpr std::uint32_t kCap = 32;
  SND_REQUIRE(l.Init(DefaultAllocator(), kCap, MemTag::Misc));
  std::atomic<std::uint32_t> inUse[kCap];
  for (auto& u : inUse) u.store(0);
  std::atomic<bool> bad{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      std::uint32_t held[4];
      for (int round = 0; round < 20000; ++round) {
        std::uint32_t n = 0;
        for (; n < 4; ++n) {
          held[n] = l.Pop();
          if (held[n] == kInvalidIndex) break;
          if (inUse[held[n]].fetch_add(1) != 0u) bad = true;
        }
        for (std::uint32_t k = 0; k < n; ++k) {
          inUse[held[k]].fetch_sub(1);
          l.Push(held[k]);
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  SND_CHECK(!bad.load());
  SND_CHECK_EQ(l.Available(), kCap);
  std::vector<bool> seen(kCap, false);
  for (std::uint32_t i = 0; i < kCap; ++i) {
    const std::uint32_t v = l.Pop();
    SND_REQUIRE(v < kCap);
    SND_CHECK(!seen[v]);
    seen[v] = true;
  }
  SND_CHECK_EQ(l.Pop(), kInvalidIndex);
}

SND_TEST(BlockPool, ConcurrentPoolHandsOutDisjointBlocks) {
  ConcurrentBlockPool pool;
  SND_REQUIRE(pool.Init(DefaultAllocator(), 64, 16, MemTag::StreamPool));
  std::vector<std::thread> threads;
  std::atomic<bool> bad{false};
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 10000; ++i) {
        auto* b = static_cast<std::uint64_t*>(pool.Acquire());
        if (b == nullptr) continue;
        for (int w = 0; w < 8; ++w) b[w] = static_cast<std::uint64_t>(t);
        for (int w = 0; w < 8; ++w) {
          if (b[w] != static_cast<std::uint64_t>(t)) bad = true;
        }
        pool.Release(b);
      }
    });
  }
  for (auto& th : threads) th.join();
  SND_CHECK(!bad.load());
  SND_CHECK_EQ(pool.Available(), 16u);
}
