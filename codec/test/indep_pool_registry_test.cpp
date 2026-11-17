// Independent tests of the decoder pool (3.3, D-14) and the codec registry
// (D-31), written by a tester. The concurrency test is meant for the TSan build.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "codec_test_util.hpp"
#include "indep_util.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/decoder_pool.hpp"
#include "snd/codec/encode.hpp"
#include "snd/codec/registry.hpp"
#include "snd/ext/codec.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using snd::codec::CodecRegistry;
using snd::codec::DecoderPool;
using snd::codec::DecoderPoolStatus;
using snd::codec::DecoderSlot;
using snd::codec::LookupFailure;
using snd::codec::RegistryStatus;

namespace {

class HeapAllocator final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag tag) override {
    ++allocs;
    tagsOk = tagsOk && tag == snd::MemTag::DecoderPool;
    return ::operator new(bytes, std::align_val_t{align});
  }
  void Free(void* p, snd::MemTag tag) override {
    ++frees;
    tagsOk = tagsOk && tag == snd::MemTag::DecoderPool;
    ::operator delete(p, std::align_val_t{64});
  }
  int allocs = 0, frees = 0;
  bool tagsOk = true;
};

}  // namespace

// Many threads on few slots (maximum contention on the Treiber stack). Every
// slot carries an owner word: a slot handed out twice at the same time is
// detected by the exchange. Counters must add up afterwards.
SND_TEST(IndepPool, ContendedAcquireReleaseNoDoubleHandOut) {
  HeapAllocator alloc;
  DecoderPool pool;
  constexpr std::uint32_t kSlots = 3;
  constexpr int kThreads = 8;
  constexpr int kIters = 20000;
  SND_REQUIRE(pool.Init(alloc, kSlots, 128));
  std::atomic<std::uint32_t> owner[kSlots];
  for (auto& o : owner) o.store(0);
  std::atomic<std::uint64_t> okCount{0}, fullCount{0}, doubleHandOut{0}, wrongPtr{0}, corrupt{0}, badRelease{0};
  std::atomic<std::uint64_t> bytesSum{0}, strayReleases{0};
  auto worker = [&](std::uint32_t id) {
    codec_test::Rng rng(id);
    for (int i = 0; i < kIters; ++i) {
      // Out-of-range releases interleaved with the real traffic must be ignored.
      if ((i & 63) == 0) {
        strayReleases.fetch_add(1, std::memory_order_relaxed);
        if (pool.Release(kSlots + id)) badRelease.fetch_add(1);
      }
      DecoderSlot s;
      const std::uint32_t want = 1 + rng.Below(128);
      const DecoderPoolStatus st = pool.Acquire(want, &s);
      if (st == DecoderPoolStatus::Full) {
        fullCount.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (st != DecoderPoolStatus::Ok || s.index >= kSlots) {
        corrupt.fetch_add(1);
        continue;
      }
      okCount.fetch_add(1, std::memory_order_relaxed);
      bytesSum.fetch_add(want, std::memory_order_relaxed);
      if (owner[s.index].exchange(id, std::memory_order_acq_rel) != 0u) doubleHandOut.fetch_add(1);
      if (s.state != pool.State(s.index)) wrongPtr.fetch_add(1);
      auto* p = static_cast<std::uint32_t*>(s.state);
      for (int k = 0; k < 32; ++k) p[k] = id ^ static_cast<std::uint32_t>(i * 31 + k);
      for (int k = 0; k < 32; ++k) {
        if (p[k] != (id ^ static_cast<std::uint32_t>(i * 31 + k))) corrupt.fetch_add(1);
      }
      if (owner[s.index].exchange(0u, std::memory_order_acq_rel) != id) doubleHandOut.fetch_add(1);
      if (!pool.Release(s.index)) badRelease.fetch_add(1);
    }
  };
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) ts.emplace_back(worker, static_cast<std::uint32_t>(t + 1));
  for (auto& t : ts) t.join();
  SND_CHECK_EQ(doubleHandOut.load(), 0u);
  SND_CHECK_EQ(wrongPtr.load(), 0u);
  SND_CHECK_EQ(corrupt.load(), 0u);
  SND_CHECK_EQ(badRelease.load(), 0u);
  const auto u = pool.Usage();
  SND_CHECK_EQ(u.inUse, 0u);
  SND_CHECK_EQ(u.stateBytesInUse, 0u);
  SND_CHECK_EQ(u.acquired, okCount.load());
  SND_CHECK_EQ(u.rejectedFull, fullCount.load());
  SND_CHECK_EQ(u.rejectedTooLarge, 0u);
  SND_CHECK_EQ(u.badReleases, strayReleases.load());
  SND_CHECK(u.peakInUse <= kSlots && u.peakInUse >= 1u);
  snd::test::Note("pool contention: %llu acquired, %llu full", static_cast<unsigned long long>(okCount.load()),
                  static_cast<unsigned long long>(fullCount.load()));
  // Free list intact: exactly kSlots distinct slots, then Full.
  DecoderSlot s[kSlots + 1];
  for (std::uint32_t i = 0; i < kSlots; ++i) SND_CHECK(pool.Acquire(1, &s[i]) == DecoderPoolStatus::Ok);
  SND_CHECK(s[0].index != s[1].index && s[1].index != s[2].index && s[0].index != s[2].index);
  SND_CHECK(pool.Acquire(1, &s[kSlots]) == DecoderPoolStatus::Full);
  for (std::uint32_t i = 0; i < kSlots; ++i) SND_CHECK(pool.Release(s[i].index));
  pool.Shutdown();
  SND_CHECK_EQ(alloc.frees, 1);
  SND_CHECK(alloc.tagsOk);
}

// Holders keep slots for a while (other threads see Full), release from a
// different thread than the acquirer (audio thread acquires, worker releases).
SND_TEST(IndepPool, CrossThreadReleaseHandOff) {
  HeapAllocator alloc;
  DecoderPool pool;
  constexpr std::uint32_t kSlots = 4;
  SND_REQUIRE(pool.Init(alloc, kSlots, 256));
  constexpr std::uint32_t kRing = 64;
  std::atomic<std::uint32_t> ring[kRing];
  for (auto& r : ring) r.store(snd::codec::kInvalidDecoderSlot);
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> errors{0}, handed{0}, released{0};
  std::thread producer([&] {
    std::uint32_t w = 0;
    for (int i = 0; i < 30000; ++i) {
      DecoderSlot s;
      if (pool.Acquire(200, &s) != DecoderPoolStatus::Ok) continue;
      std::memset(s.state, static_cast<int>(i & 0xFF), 256);
      std::uint32_t expect = snd::codec::kInvalidDecoderSlot;
      while (!ring[w % kRing].compare_exchange_weak(expect, s.index)) expect = snd::codec::kInvalidDecoderSlot;
      ++w;
      handed.fetch_add(1);
    }
    done.store(true);
  });
  std::thread consumer([&] {
    std::uint32_t r = 0;
    for (;;) {
      const std::uint32_t idx = ring[r % kRing].exchange(snd::codec::kInvalidDecoderSlot);
      if (idx == snd::codec::kInvalidDecoderSlot) {
        if (done.load() && released.load() == handed.load()) break;
        std::this_thread::yield();
        continue;
      }
      ++r;
      const auto* p = static_cast<const std::uint8_t*>(pool.State(idx));
      for (int k = 1; k < 256; ++k) {
        if (p[k] != p[0]) errors.fetch_add(1);
      }
      if (!pool.Release(idx)) errors.fetch_add(1);
      released.fetch_add(1);
    }
  });
  producer.join();
  consumer.join();
  SND_CHECK_EQ(errors.load(), 0u);
  SND_CHECK_EQ(pool.Usage().inUse, 0u);
  SND_CHECK_EQ(pool.Usage().badReleases, 0u);
}

// Sizes at the boundaries; caller memory of exactly RequiredBytes with fences:
// writing every byte of every slot must not disturb the free list.
SND_TEST(IndepPool, BoundariesAndBookkeepingIsolation) {
  struct Sz {
    std::uint32_t req, rounded;
  };
  for (const Sz& z : {Sz{1, 64}, Sz{64, 64}, Sz{65, 128}, Sz{32768, 32768}, Sz{32769, 32832}}) {
    for (std::uint32_t n : {1u, 2u, 7u}) {
      const std::size_t bytes = DecoderPool::RequiredBytes(n, z.req);
      SND_REQUIRE(bytes >= static_cast<std::size_t>(n) * z.rounded);
      indep::Fenced mem(bytes);
      DecoderPool pool;
      SND_REQUIRE(pool.Init(mem.get(), bytes, n, z.req));
      SND_CHECK_EQ(pool.SlotBytes(), z.rounded);
      SND_CHECK_EQ(pool.Usage().poolBytes, bytes);
      for (int round = 0; round < 3; ++round) {
        std::vector<DecoderSlot> s(n);
        for (std::uint32_t i = 0; i < n; ++i) {
          SND_REQUIRE(pool.Acquire(pool.SlotBytes(), &s[i]) == DecoderPoolStatus::Ok);  // exactly the slot
          SND_CHECK(reinterpret_cast<std::uintptr_t>(s[i].state) % 64u == 0u);
          std::memset(s[i].state, 0xFF, pool.SlotBytes());  // whole slot
        }
        DecoderSlot extra;
        SND_CHECK(pool.Acquire(1, &extra) == DecoderPoolStatus::Full);
        SND_CHECK(pool.Acquire(pool.SlotBytes() + 1, &extra) == DecoderPoolStatus::StateTooLarge);
        SND_CHECK_EQ(extra.index, snd::codec::kInvalidDecoderSlot);
        SND_CHECK_EQ(pool.Usage().stateBytesInUse, static_cast<std::uint64_t>(n) * pool.SlotBytes());
        for (std::uint32_t i = n; i-- > 0;) SND_CHECK(pool.Release(s[i].index));
        SND_CHECK_EQ(pool.Usage().inUse, 0u);
      }
      SND_CHECK(mem.Intact());
      pool.Shutdown();
    }
  }
  // StateTooLarge with UINT32_MAX and a pool whose slot is near UINT32_MAX.
  HeapAllocator alloc;  // declared before the pool: the pool frees into it on destruction
  DecoderPool pool;
  SND_CHECK_EQ(DecoderPool::RequiredBytes(1, UINT32_MAX), 0u);
  SND_REQUIRE(pool.Init(alloc, 2, 1000));
  DecoderSlot s;
  SND_CHECK(pool.Acquire(UINT32_MAX, &s) == DecoderPoolStatus::StateTooLarge);
  SND_CHECK(snd::codec::EndDetailFor(DecoderPoolStatus::NotInitialized) == SND_END_DETAIL_DECODER_POOL_FULL);
}

// Bad releases: never-acquired, double, out of range, kInvalidDecoderSlot, on an
// uninitialised pool. All ignored and counted; nothing else changes.
SND_TEST(IndepPool, BadReleasesAreIgnored) {
  DecoderPool idle;
  SND_CHECK(!idle.Release(0));
  SND_CHECK(!idle.Release(snd::codec::kInvalidDecoderSlot));
  SND_CHECK(idle.State(0) == nullptr);
  HeapAllocator alloc;
  DecoderPool pool;
  SND_REQUIRE(pool.Init(alloc, 3, 64));
  SND_CHECK(!pool.Release(0));  // free slot
  SND_CHECK(!pool.Release(3));
  SND_CHECK(!pool.Release(snd::codec::kInvalidDecoderSlot));
  DecoderSlot a;
  SND_REQUIRE(pool.Acquire(10, &a) == DecoderPoolStatus::Ok);
  SND_CHECK(pool.Release(a.index));
  SND_CHECK(!pool.Release(a.index));
  auto u = pool.Usage();
  SND_CHECK_EQ(u.badReleases, 4u);
  SND_CHECK_EQ(u.inUse, 0u);
  SND_CHECK_EQ(u.stateBytesInUse, 0u);
  // Still exactly 3 slots.
  DecoderSlot s[4];
  for (int i = 0; i < 3; ++i) SND_CHECK(pool.Acquire(1, &s[i]) == DecoderPoolStatus::Ok);
  SND_CHECK(pool.Acquire(1, &s[3]) == DecoderPoolStatus::Full);
  // Init again (without Shutdown) returns the old memory.
  for (int i = 0; i < 3; ++i) pool.Release(s[i].index);
  SND_REQUIRE(pool.Init(alloc, 5, 64));
  SND_CHECK_EQ(alloc.allocs, 2);
  SND_CHECK_EQ(alloc.frees, 1);
  SND_CHECK_EQ(pool.Usage().badReleases, 0u);  // counters restart
  SND_CHECK_EQ(pool.Usage().capacity, 5u);
}

// Acceptance decision 3: DecoderSlot carries a generation, and a stale
// Release (index re-acquired by another owner since) is rejected and counted.
SND_TEST(IndepPool, StaleReleaseIsRejected) {
  HeapAllocator alloc;
  DecoderPool pool;
  SND_REQUIRE(pool.Init(alloc, 1, 64));
  DecoderSlot a, b, c;
  SND_REQUIRE(pool.Acquire(1, &a) == DecoderPoolStatus::Ok);
  SND_REQUIRE(pool.Release(a));
  SND_REQUIRE(pool.Acquire(1, &b) == DecoderPoolStatus::Ok);  // same index, new owner
  SND_CHECK_EQ(b.index, a.index);
  SND_CHECK(b.generation != a.generation);
  SND_CHECK(!pool.Release(a));                                    // owner A releases late: rejected
  SND_CHECK(!pool.Release(a.index, a.generation));
  SND_CHECK_EQ(pool.Usage().badReleases, 2u);
  SND_CHECK_EQ(pool.Usage().inUse, 1u);
  SND_CHECK(pool.Acquire(1, &c) == DecoderPoolStatus::Full);      // B still owns the slot
  SND_CHECK(pool.Release(b));
  SND_CHECK(!pool.Release(b));                                    // double release
  SND_CHECK_EQ(pool.Usage().inUse, 0u);
}

// Acquire / Release / Usage / State and registry Find inside an RT scope.
SND_TEST(IndepPool, RtScopeCleanUnderLoad) {
  HeapAllocator alloc;
  DecoderPool pool;
  SND_REQUIRE(pool.Init(alloc, 8, 4096));
  CodecRegistry reg;
  {
    SND_RT_SCOPE();
    DecoderSlot s[9];
    for (int round = 0; round < 100; ++round) {
      for (auto& x : s) (void)pool.Acquire(100, &x);
      (void)pool.Usage();
      for (auto& x : s) {
        (void)pool.State(x.index);
        (void)pool.Release(x.index);
      }
      (void)pool.Release(1000);
      LookupFailure why;
      (void)reg.Find(SND_CODEC_OPUS, &why);
      (void)reg.Find(SND_CODEC_ADPCM, &why);
      (void)CodecRegistry::FindForSource(&reg, SND_CODEC_PCM_F32);
      (void)CodecRegistry::FindForSource(nullptr, SND_CODEC_PCM_F32);
    }
  }
  SND_CHECK_EQ(pool.Usage().inUse, 0u);
}

// ---------------------------------------------------------------------------------
// Registry
namespace {

struct Call {
  int stateBytes = 0, init = 0, decode = 0, seek = 0;
  void* lastUser = nullptr;
  void* lastState = nullptr;
  std::uint64_t lastSeek = 0;
  std::uint32_t lastSrcBytes = 0, lastMax = 0;
};
Call gCall;

std::uint32_t SND_CALL TStateBytes(void* u, const snd_codec_header* h) {
  ++gCall.stateBytes;
  gCall.lastUser = u;
  return 1000u + h->channels;
}
void SND_CALL TInit(void* u, void* s, const snd_codec_header*) {
  ++gCall.init;
  gCall.lastUser = u;
  gCall.lastState = s;
}
std::uint32_t SND_CALL TDecode(void* u, void* s, const std::uint8_t*, std::uint32_t n, float* const*,
                               std::uint32_t max) {
  ++gCall.decode;
  gCall.lastUser = u;
  gCall.lastState = s;
  gCall.lastSrcBytes = n;
  gCall.lastMax = max;
  return 3;
}
void SND_CALL TSeek(void* u, void* s, std::uint64_t f) {
  ++gCall.seek;
  gCall.lastUser = u;
  gCall.lastState = s;
  gCall.lastSeek = f;
}
snd_codec T(std::uint32_t fourcc, void* user) { return snd_codec{user, fourcc, 0u, &TStateBytes, &TInit, &TDecode, &TSeek}; }

class Marker final : public snd::ICodec {
 public:
  std::uint32_t StateBytes(const snd::CodecHeader&) const override { return 42; }
  void Init(void*, const snd::CodecHeader&) override {}
  std::uint32_t Decode(void*, const std::uint8_t*, std::uint32_t, float* const*, std::uint32_t) override { return 0; }
  void Seek(void*, std::uint64_t) override {}
};

}  // namespace

// User codecs win over every built-in; a C table is called with its own user
// pointer and the caller's arguments; FindForSource agrees with Find.
SND_TEST(IndepRegistry, UserWinsAndCallsAreForwarded) {
  int u1 = 0, u2 = 0, u3 = 0;
  const snd_codec tables[] = {T(SND_CODEC_PCM_F32, &u1), T(SND_CODEC_PCM_S16, &u2), T(SND_CODEC_ADPCM, &u3)};
  CodecRegistry reg;
  SND_REQUIRE(reg.Init(tables, 3) == RegistryStatus::Ok);
  void* users[] = {&u1, &u2, &u3};
  const std::uint32_t fourccs[] = {SND_CODEC_PCM_F32, SND_CODEC_PCM_S16, SND_CODEC_ADPCM};
  for (int i = 0; i < 3; ++i) {
    LookupFailure why = LookupFailure::NotRegistered;
    snd::ICodec* c = reg.Find(fourccs[i], &why);
    SND_REQUIRE(c != nullptr);
    SND_CHECK(why == LookupFailure::None);
    SND_CHECK(c != snd::codec::BuiltinCodec(fourccs[i]));
    SND_CHECK(CodecRegistry::FindForSource(&reg, fourccs[i]) == c);
    SND_CHECK(reg.IsUserCodec(fourccs[i]));
    snd::CodecHeader h{};
    h.channels = 5;
    gCall = Call{};
    int state = 0;
    float* dst[1] = {nullptr};
    SND_CHECK_EQ(c->StateBytes(h), 1005u);
    c->Init(&state, h);
    SND_CHECK_EQ(c->Decode(&state, nullptr, 77, dst, 99), 3u);
    c->Seek(&state, 123456789012ull);
    SND_CHECK(gCall.lastUser == users[i]);
    SND_CHECK(gCall.lastState == &state);
    SND_CHECK_EQ(gCall.lastSrcBytes, 77u);
    SND_CHECK_EQ(gCall.lastMax, 99u);
    SND_CHECK_EQ(gCall.lastSeek, 123456789012ull);
    SND_CHECK(gCall.stateBytes == 1 && gCall.init == 1 && gCall.decode == 1 && gCall.seek == 1);
  }
  // Built-ins only through FindForSource(nullptr).
  SND_CHECK(CodecRegistry::FindForSource(nullptr, SND_CODEC_ADPCM) == snd::codec::BuiltinCodec(SND_CODEC_ADPCM));
  // Re-Init with nothing: the built-ins are back.
  SND_REQUIRE(reg.Init(nullptr, 0) == RegistryStatus::Ok);
  SND_CHECK(reg.Find(SND_CODEC_PCM_F32) == snd::codec::BuiltinCodec(SND_CODEC_PCM_F32));
  SND_CHECK(!reg.IsUserCodec(SND_CODEC_PCM_F32));
}

// ToC bridges unwrap to the very ICodec; Opus reasons; 32 codecs is the limit.
SND_TEST(IndepRegistry, LimitsReasonsAndBridges) {
  CodecRegistry reg;
  LookupFailure why = LookupFailure::None;
  // Contract 1.1 + D-56: Opus is built in unless SND_CODEC_OPUS=OFF (then the A0-3 reason).
  snd::ICodec* const builtinOpus = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  const LookupFailure opusWhy = snd::codec::OpusAvailable() ? LookupFailure::None : LookupFailure::OpusUnavailable;
  SND_CHECK(reg.Find(SND_CODEC_OPUS, &why) == builtinOpus);
  SND_CHECK(why == opusWhy);
  SND_CHECK(reg.Find(snd::Fourcc('Z', 'Z', 'Z', 'Z'), &why) == nullptr);
  SND_CHECK(why == LookupFailure::NotRegistered);
  SND_CHECK(reg.Find(0, &why) == nullptr);
  SND_CHECK(why == LookupFailure::NotRegistered);
  SND_CHECK(reg.Find(SND_CODEC_OPUS) == builtinOpus);  // why == nullptr is allowed
  SND_CHECK(std::strlen(snd::codec::LookupFailureName(LookupFailure::OpusUnavailable)) > 0);
  Marker markers[CodecRegistry::kMaxUserCodecs];
  snd_codec tables[CodecRegistry::kMaxUserCodecs + 1];
  for (std::uint32_t i = 0; i < CodecRegistry::kMaxUserCodecs; ++i) tables[i] = snd::ToC(markers[i], 0x100u + i);
  tables[CodecRegistry::kMaxUserCodecs - 1].codec = SND_CODEC_OPUS;
  SND_REQUIRE(reg.Init(tables, CodecRegistry::kMaxUserCodecs) == RegistryStatus::Ok);
  SND_CHECK_EQ(reg.UserCodecCount(), CodecRegistry::kMaxUserCodecs);
  for (std::uint32_t i = 0; i + 1 < CodecRegistry::kMaxUserCodecs; ++i) SND_CHECK(reg.Find(0x100u + i) == &markers[i]);
  SND_CHECK(reg.Find(SND_CODEC_OPUS, &why) == &markers[CodecRegistry::kMaxUserCodecs - 1]);
  SND_CHECK(why == LookupFailure::None);
  // 33: rejected, and the previous user codecs are gone.
  tables[CodecRegistry::kMaxUserCodecs] = snd::ToC(markers[0], 0x999u);
  SND_CHECK(reg.Init(tables, CodecRegistry::kMaxUserCodecs + 1) == RegistryStatus::TooManyCodecs);
  SND_CHECK_EQ(reg.UserCodecCount(), 0u);
  SND_CHECK(reg.Find(0x100u) == nullptr);
  SND_CHECK(reg.Find(SND_CODEC_OPUS, &why) == builtinOpus && why == opusWhy);
  // Duplicates far apart, each null function, fourcc 0.
  for (std::uint32_t i = 0; i < CodecRegistry::kMaxUserCodecs; ++i) tables[i] = snd::ToC(markers[i], 0x100u + i);
  tables[CodecRegistry::kMaxUserCodecs - 1].codec = 0x100u;
  SND_CHECK(reg.Init(tables, CodecRegistry::kMaxUserCodecs) == RegistryStatus::DuplicateFourcc);
  SND_CHECK_EQ(reg.UserCodecCount(), 0u);
  int dummy = 0;
  for (int which = 0; which < 5; ++which) {
    snd_codec t[2] = {T(0x200u, &dummy), T(0x201u, &dummy)};
    if (which == 0) t[1].codec = 0;
    if (which == 1) t[1].state_bytes = nullptr;
    if (which == 2) t[1].init = nullptr;
    if (which == 3) t[1].decode = nullptr;
    if (which == 4) t[1].seek = nullptr;
    SND_CHECK(reg.Init(t, 2) == RegistryStatus::InvalidEntry);
    SND_CHECK(reg.Find(0x200u) == nullptr);
  }
}
