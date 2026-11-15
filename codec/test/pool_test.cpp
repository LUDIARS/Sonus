// Decoder pool tests (3.3, D-14).
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include "codec_test_util.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/decoder_pool.hpp"
#include "snd/codec/encode.hpp"
#include "snd/ext/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using snd::codec::DecoderPool;
using snd::codec::DecoderPoolStatus;
using snd::codec::DecoderSlot;

namespace {

class CountingAllocator final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag tag) override {
    if (failNext) {
      failNext = false;
      return nullptr;
    }
    ++allocations;
    lastBytes = bytes;
    lastTag = tag;
    lastAlign = align;
    return ::operator new(bytes, std::align_val_t{align});
  }
  void Free(void* p, snd::MemTag tag) override {
    if (p == nullptr) return;
    ++frees;
    freeTagOk = freeTagOk && tag == snd::MemTag::DecoderPool;
    ::operator delete(p, std::align_val_t{64});
  }
  int allocations = 0, frees = 0;
  std::size_t lastBytes = 0, lastAlign = 0;
  snd::MemTag lastTag = snd::MemTag::Misc;
  bool freeTagOk = true;
  bool failNext = false;
};

}  // namespace

SND_TEST(DecoderPool, AcquireUntilFullThenReject) {
  CountingAllocator alloc;
  DecoderPool pool;
  SND_REQUIRE(pool.Init(alloc, 4, 1000));
  SND_CHECK_EQ(alloc.allocations, 1);
  SND_CHECK(alloc.lastTag == snd::MemTag::DecoderPool);
  SND_CHECK_EQ(alloc.lastAlign, 64u);
  SND_CHECK_EQ(alloc.lastBytes, DecoderPool::RequiredBytes(4, 1000));
  SND_CHECK_EQ(pool.SlotBytes(), 1024u);  // rounded up to 64
  SND_CHECK_EQ(pool.Capacity(), 4u);

  DecoderSlot s[5];
  for (int i = 0; i < 4; ++i) {
    SND_REQUIRE(pool.Acquire(100u * (i + 1), &s[i]) == DecoderPoolStatus::Ok);
    SND_CHECK(s[i].index < 4u);
    SND_CHECK(reinterpret_cast<std::uintptr_t>(s[i].state) % 64u == 0u);
    SND_CHECK_EQ(pool.State(s[i].index), s[i].state);
    std::memset(s[i].state, i, pool.SlotBytes());  // slots do not overlap (ASan / pattern check below)
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) SND_CHECK(s[i].index != s[j].index);
    SND_CHECK_EQ(static_cast<const std::uint8_t*>(s[i].state)[pool.SlotBytes() - 1], static_cast<std::uint8_t>(i));
  }
  SND_CHECK(pool.Acquire(10, &s[4]) == DecoderPoolStatus::Full);
  SND_CHECK_EQ(s[4].index, snd::codec::kInvalidDecoderSlot);
  SND_CHECK(s[4].state == nullptr);
  SND_CHECK(pool.Acquire(1025, &s[4]) == DecoderPoolStatus::StateTooLarge);
  SND_CHECK_EQ(snd::codec::EndDetailFor(DecoderPoolStatus::Full), SND_END_DETAIL_DECODER_POOL_FULL);
  SND_CHECK_EQ(snd::codec::EndDetailFor(DecoderPoolStatus::StateTooLarge), SND_END_DETAIL_DECODER_POOL_FULL);
  SND_CHECK_EQ(snd::codec::EndDetailFor(DecoderPoolStatus::Ok), SND_END_DETAIL_NONE);

  snd::codec::DecoderPoolUsage u = pool.Usage();
  SND_CHECK_EQ(u.capacity, 4u);
  SND_CHECK_EQ(u.inUse, 4u);
  SND_CHECK_EQ(u.peakInUse, 4u);
  SND_CHECK_EQ(u.stateBytesInUse, 1000u);
  SND_CHECK_EQ(u.acquired, 4u);
  SND_CHECK_EQ(u.rejectedFull, 1u);
  SND_CHECK_EQ(u.rejectedTooLarge, 1u);
  SND_CHECK_EQ(u.poolBytes, DecoderPool::RequiredBytes(4, 1000));

  SND_CHECK(pool.Release(s[1].index));
  SND_CHECK(!pool.Release(s[1].index));  // double release is ignored and counted
  SND_CHECK(!pool.Release(99));
  u = pool.Usage();
  SND_CHECK_EQ(u.inUse, 3u);
  SND_CHECK_EQ(u.stateBytesInUse, 800u);
  SND_CHECK_EQ(u.badReleases, 2u);
  DecoderSlot again;
  SND_CHECK(pool.Acquire(1024, &again) == DecoderPoolStatus::Ok);  // exactly the slot size fits
  SND_CHECK_EQ(again.index, s[1].index);
  for (int i : {0, 2, 3}) SND_CHECK(pool.Release(s[i].index));
  SND_CHECK(pool.Release(again.index));
  SND_CHECK_EQ(pool.Usage().inUse, 0u);
  SND_CHECK_EQ(pool.Usage().peakInUse, 4u);
  pool.Shutdown();
  SND_CHECK_EQ(alloc.frees, 1);
  SND_CHECK(alloc.freeTagOk);
  SND_CHECK(pool.Acquire(1, &again) == DecoderPoolStatus::NotInitialized);
}

SND_TEST(DecoderPool, CallerMemoryAndEdgeCases) {
  DecoderPool pool;
  DecoderSlot s;
  SND_CHECK(pool.Acquire(1, &s) == DecoderPoolStatus::NotInitialized);
  const std::size_t bytes = DecoderPool::RequiredBytes(3, 64);
  SND_REQUIRE(bytes >= 3u * 64u);
  void* mem = ::operator new(bytes, std::align_val_t{64});
  SND_CHECK(!pool.Init(mem, bytes - 1, 3, 64));                              // too small
  SND_CHECK(!pool.Init(static_cast<std::uint8_t*>(mem) + 16, bytes, 3, 64));  // misaligned
  SND_REQUIRE(pool.Init(mem, bytes, 3, 64));
  for (int i = 0; i < 3; ++i) SND_CHECK(pool.Acquire(64, &s) == DecoderPoolStatus::Ok);
  SND_CHECK(pool.Acquire(0, &s) == DecoderPoolStatus::Full);
  pool.Shutdown();  // caller memory is not freed by the pool
  ::operator delete(mem, std::align_val_t{64});

  // Zero decoders: valid, every request is Full.
  SND_REQUIRE(pool.Init(nullptr, 0, 0, 32768));
  SND_CHECK(pool.Acquire(1, &s) == DecoderPoolStatus::Full);
  // Overflowing sizes are refused.
  SND_CHECK_EQ(DecoderPool::RequiredBytes(1, 0xFFFFFFF0u), 0u);
  CountingAllocator alloc;
  alloc.failNext = true;
  SND_CHECK(!pool.Init(alloc, 4, 1024));
  SND_CHECK(pool.Acquire(1, &s) == DecoderPoolStatus::NotInitialized);
}

// Acquire/Release are called from the audio thread: no allocation, no lock.
SND_TEST(DecoderPool, AcquireReleaseAreRtSafe) {
  CountingAllocator alloc;
  DecoderPool pool;
  SND_REQUIRE(pool.Init(alloc, 8, 32768));
  DecoderSlot s[8];
  {
    SND_RT_SCOPE();
    for (auto& x : s) SND_CHECK(pool.Acquire(20000, &x) == DecoderPoolStatus::Ok);
    DecoderSlot extra;
    SND_CHECK(pool.Acquire(1, &extra) == DecoderPoolStatus::Full);
    SND_CHECK(pool.Acquire(40000, &extra) == DecoderPoolStatus::StateTooLarge);
    (void)pool.Usage();
    for (auto& x : s) SND_CHECK(pool.Release(x.index));
    SND_CHECK(!pool.Release(s[0].index));
  }
}

// Several threads acquire, write, verify and release concurrently (TSan run).
SND_TEST(DecoderPool, ConcurrentAcquireRelease) {
  CountingAllocator alloc;
  DecoderPool pool;
  constexpr std::uint32_t kSlots = 6;
  SND_REQUIRE(pool.Init(alloc, kSlots, 256));
  std::atomic<std::uint32_t> corrupt{0};
  std::atomic<std::uint32_t> ok{0};
  std::atomic<std::uint64_t> stale{0};
  auto worker = [&](std::uint32_t id) {
    for (int i = 0; i < 4000; ++i) {
      DecoderSlot s;
      if (pool.Acquire(128, &s) != DecoderPoolStatus::Ok) continue;
      auto* p = static_cast<std::uint32_t*>(s.state);
      for (int k = 0; k < 64; ++k) p[k] = id * 100000u + static_cast<std::uint32_t>(i);
      for (int k = 0; k < 64; ++k) {
        if (p[k] != id * 100000u + static_cast<std::uint32_t>(i)) corrupt.fetch_add(1);
      }
      ok.fetch_add(1, std::memory_order_relaxed);
      if (!pool.Release(s)) corrupt.fetch_add(1);
      // A late second release with the old generation must always be rejected,
      // whether the slot is free or already owned by another thread.
      if (pool.Release(s)) corrupt.fetch_add(1);
      stale.fetch_add(1, std::memory_order_relaxed);
    }
  };
  std::vector<std::thread> threads;
  for (std::uint32_t t = 0; t < 4; ++t) threads.emplace_back(worker, t + 1);
  for (auto& t : threads) t.join();
  SND_CHECK_EQ(corrupt.load(), 0u);
  const auto u = pool.Usage();
  SND_CHECK_EQ(u.inUse, 0u);
  SND_CHECK(u.peakInUse <= kSlots);
  SND_CHECK_EQ(u.acquired, ok.load());
  SND_CHECK_EQ(u.badReleases, stale.load());
  // All slots are back on the free list.
  DecoderSlot s[kSlots];
  for (auto& x : s) SND_CHECK(pool.Acquire(1, &x) == DecoderPoolStatus::Ok);
  DecoderSlot extra;
  SND_CHECK(pool.Acquire(1, &extra) == DecoderPoolStatus::Full);
}

// The decoder pool and a codec state together, as source will use them.
SND_TEST(DecoderPool, DecodesInsideASlot) {
  using namespace codec_test;
  const snd::CodecHeader h = snd::codec::encode::MakeAdpcmHeader(8, 48000, 3000);
  auto data = EncodeOrDie(h, MakeSignal(8, 3000));
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_ADPCM);
  CountingAllocator alloc;
  DecoderPool pool;
  SND_REQUIRE(pool.Init(alloc, 2, 32u * 1024u));  // Config default slot
  DecoderSlot s;
  SND_REQUIRE(pool.Acquire(c->StateBytes(h), &s) == DecoderPoolStatus::Ok);
  c->Init(s.state, h);
  FeedResult r;
  Feed(*c, s.state, 8, data.data(), data.size(), {1500}, 256, &r);
  SND_CHECK(!r.error);
  SND_CHECK_EQ(Frames(r.out), 3000u);
  SND_CHECK(pool.Release(s.index));
}

// Generations (acceptance decision 3) and the null-output argument error (decision 7).
SND_TEST(DecoderPool, GenerationsAndArgumentErrors) {
  CountingAllocator alloc;
  DecoderPool pool;
  SND_REQUIRE(pool.Init(alloc, 2, 64));
  SND_CHECK(pool.Acquire(1, nullptr) == DecoderPoolStatus::InvalidArgument);
  SND_CHECK_EQ(snd::codec::EndDetailFor(DecoderPoolStatus::InvalidArgument), SND_END_DETAIL_DECODER_POOL_FULL);
  SND_CHECK_EQ(pool.Usage().inUse, 0u);
  SND_CHECK_EQ(pool.Usage().acquired, 0u);
  DecoderSlot a, b, c;
  SND_REQUIRE(pool.Acquire(1, &a) == DecoderPoolStatus::Ok);
  SND_REQUIRE(pool.Acquire(1, &b) == DecoderPoolStatus::Ok);  // both slots: nullptr took none
  SND_CHECK(a.generation != 0u && b.generation != 0u);
  SND_CHECK_EQ(pool.Generation(a.index), a.generation);
  SND_CHECK(!pool.Release(a.index, a.generation + 1u));  // wrong generation
  SND_CHECK(!pool.Release(a.index, 0u));
  SND_CHECK(pool.Release(a));
  SND_CHECK_EQ(pool.Generation(a.index), 0u);  // free
  SND_REQUIRE(pool.Acquire(1, &c) == DecoderPoolStatus::Ok);
  SND_CHECK_EQ(c.index, a.index);
  SND_CHECK_EQ(c.generation, a.generation + 1u);
  SND_CHECK(!pool.Release(a));                 // stale
  SND_CHECK_EQ(pool.Usage().inUse, 2u);
  SND_CHECK_EQ(pool.Usage().badReleases, 3u);
  SND_CHECK(pool.Release(c.index, c.generation));
  SND_CHECK(pool.Release(b.index));            // index-only form: no generation check
  SND_CHECK_EQ(pool.Usage().inUse, 0u);
  {
    SND_RT_SCOPE();
    DecoderSlot x;
    SND_CHECK(pool.Acquire(1, &x) == DecoderPoolStatus::Ok);
    SND_CHECK(pool.Release(x));
    SND_CHECK(!pool.Release(x));
    (void)pool.Generation(0);
  }
}
