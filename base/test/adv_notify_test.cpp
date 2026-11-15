// Independent tests of the audio -> game notification channels
// (design.md 2.1, CONTRACTS.md D-16, snd/internal/notify.hpp protocol).
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "adv_test_util.hpp"
#include "snd/base/allocator.hpp"
#include "snd/base/notify.hpp"
#include "snd/test/test.hpp"

using namespace snd::base;
namespace nt = snd::base::notify;
using snd::advtest::Rng;
using snd::advtest::StressScale;

SND_TEST(AdvEndFlags, GameSideReissueModelNoLossNoDoubleAcrossGenerations) {
  // Model of 2.1: the game side issues slot+generation, the audio side ends
  // playbacks in arbitrary order, the game side consumes the end bit,
  // checks the generation and only then reissues the slot with gen+1.
  // Capacity 130 crosses two word boundaries.
  constexpr std::uint32_t kSlots = 130;
  const std::uint32_t kEnds = 60000u * StressScale();
  nt::EndFlags f;
  SND_REQUIRE(nt::InitEndFlags(f, DefaultAllocator(), kSlots));
  // issued[s] = generation the game side issued and the audio side may end (0 = not issued).
  std::vector<std::atomic<std::uint32_t>> issued(kSlots);
  for (std::uint32_t s = 0; s < kSlots; ++s) issued[s].store(1u, std::memory_order_relaxed);
  std::atomic<bool> stop{false};
  std::thread audio([&] {
    Rng rng(99);
    std::vector<std::uint32_t> endedGen(kSlots, 0u);
    while (!stop.load(std::memory_order_relaxed)) {
      const std::uint32_t s = rng.Below(kSlots);
      const std::uint32_t g = issued[s].load(std::memory_order_acquire);
      if (g == 0u || g == endedGen[s]) continue;  // not live or already ended this generation
      endedGen[s] = g;
      nt::PublishEnd(f, s, nt::EndRecord{g, static_cast<std::uint16_t>(g & 7u), static_cast<std::uint16_t>(s)});
    }
  });
  std::uint32_t consumed = 0, bad = 0;
  std::vector<std::uint32_t> expectGen(kSlots, 1u);
  while (consumed < kEnds) {
    consumed += nt::ConsumeEnds(f, [&](std::uint32_t s, const nt::EndRecord& r) {
      if (r.generation != expectGen[s] || r.reason != (r.generation & 7u) || r.detail != s) ++bad;
      ++expectGen[s];
      issued[s].store(expectGen[s], std::memory_order_release);  // reissue after consumption
    });
  }
  stop = true;
  audio.join();
  consumed += nt::ConsumeEnds(f, [&](std::uint32_t s, const nt::EndRecord& r) {
    if (r.generation != expectGen[s]) ++bad;
    ++expectGen[s];
  });
  SND_CHECK_EQ(bad, 0u);
  // After quiescence no bit is left and nothing is delivered twice.
  SND_CHECK_EQ(nt::ConsumeEnds(f, [](std::uint32_t, const nt::EndRecord&) {}), 0u);
  nt::DestroyEndFlags(f, DefaultAllocator());
}

SND_TEST(AdvReleaseFlags, ConcurrentManySlotsEveryReleaseDeliveredOnce) {
  constexpr std::uint32_t kSlots = 65;  // one bit into the second word
  nt::ReleaseFlags f;
  SND_REQUIRE(nt::InitReleaseFlags(f, DefaultAllocator(), kSlots));
  std::vector<std::atomic<std::uint32_t>> live(kSlots);  // generation handed to the audio side
  for (std::uint32_t s = 0; s < kSlots; ++s) live[s].store(1u);
  const std::uint32_t kTarget = 40000u * StressScale();
  std::atomic<bool> stop{false};
  std::thread audio([&] {
    std::vector<std::uint32_t> released(kSlots, 0u);
    while (!stop.load(std::memory_order_relaxed)) {
      for (std::uint32_t s = 0; s < kSlots; ++s) {
        const std::uint32_t g = live[s].load(std::memory_order_acquire);
        if (g != released[s]) {
          released[s] = g;
          nt::PublishRelease(f, s, g);
        }
      }
    }
  });
  std::uint32_t got = 0, bad = 0;
  std::vector<std::uint32_t> expect(kSlots, 1u);
  while (got < kTarget) {
    got += nt::ConsumeReleases(f, [&](std::uint32_t s, std::uint32_t g) {
      if (g != expect[s]) ++bad;
      ++expect[s];
      live[s].store(expect[s], std::memory_order_release);
    });
  }
  stop = true;
  audio.join();
  nt::ConsumeReleases(f, [&](std::uint32_t s, std::uint32_t g) {
    if (g != expect[s]) ++bad;
    ++expect[s];
  });
  SND_CHECK_EQ(bad, 0u);
  nt::DestroyReleaseFlags(f, DefaultAllocator());
}

SND_TEST(AdvEventRing, ConcurrentOverflowDroppedPlusDeliveredEqualsPushed) {
  for (std::uint32_t cap : {1u, 2u, 5u, 64u}) {
    nt::EventRing r;
    SND_REQUIRE(nt::InitEventRing(r, DefaultAllocator(), cap));
    SND_CHECK(IsPowerOfTwo(r.capacity) && r.capacity >= cap);
    const std::uint64_t kPush = 100000ull * StressScale();
    std::atomic<bool> done{false};
    std::uint64_t accepted = 0;
    std::thread producer([&] {
      for (std::uint64_t i = 0; i < kPush; ++i) {
        nt::EventRecord e{};
        e.type = static_cast<std::uint16_t>(snd::notify::EventType::Marker);
        e.object = i;
        e.arg0 = i * 7u;
        e.arg1 = ~i;
        if (nt::PushEvent(r, e)) ++accepted;
      }
      done.store(true);
    });
    std::uint64_t delivered = 0, droppedTaken = 0, bad = 0;
    std::uint64_t last = 0;
    bool first = true;
    auto take = [&](const nt::EventRecord& e) {
      if (e.arg0 != e.object * 7u || e.arg1 != ~e.object) ++bad;  // torn record
      if (!first && e.object <= last) ++bad;                     // duplicate / reordered
      first = false;
      last = e.object;
      ++delivered;
    };
    Rng rng(cap);
    while (!done.load()) {
      nt::DrainEvents(r, take, rng.Below(3));  // drain a few at a time: the ring overflows
      droppedTaken += nt::TakeDropped(r);
    }
    producer.join();
    nt::DrainEvents(r, take);
    droppedTaken += nt::TakeDropped(r);
    SND_CHECK_EQ(bad, 0u);
    SND_CHECK_EQ(delivered, accepted);
    SND_CHECK_MSG(delivered + droppedTaken == kPush, "cap=%u delivered=%llu dropped=%llu pushed=%llu", cap,
                  static_cast<unsigned long long>(delivered), static_cast<unsigned long long>(droppedTaken),
                  static_cast<unsigned long long>(kPush));
    nt::DestroyEventRing(r, DefaultAllocator());
  }
}

namespace {
constexpr std::uint32_t kTbPlaybacks = 9, kTbTransports = 3, kTbBuses = 4;

// Writer: every word of every snapshot struct and the header carry `v`.
void WriteUniform(nt::SnapshotTripleBuffer& tb, std::uint64_t v) {
  nt::SnapshotFrame& f = nt::BeginSnapshot(tb);
  snd_stats st;
  std::uint64_t* sw = reinterpret_cast<std::uint64_t*>(&st);
  for (std::size_t i = 0; i < sizeof(st) / 8u; ++i) sw[i] = v;
  nt::SetFrameHeader(f, v, v, st, static_cast<std::uint32_t>(v));
  nt::PlaybackSnapshot p;
  nt::TransportSnapshot t;
  nt::BusSnapshot b;
  for (std::uint64_t* w = reinterpret_cast<std::uint64_t*>(&p); w < reinterpret_cast<std::uint64_t*>(&p + 1); ++w) *w = v;
  for (std::uint64_t* w = reinterpret_cast<std::uint64_t*>(&t); w < reinterpret_cast<std::uint64_t*>(&t + 1); ++w) *w = v;
  for (std::uint64_t* w = reinterpret_cast<std::uint64_t*>(&b); w < reinterpret_cast<std::uint64_t*>(&b + 1); ++w) *w = v;
  for (std::uint32_t i = 0; i < kTbPlaybacks; ++i) nt::SnapshotStore(&f.playbacks[i], p);
  for (std::uint32_t i = 0; i < kTbTransports; ++i) nt::SnapshotStore(&f.transports[i], t);
  for (std::uint32_t i = 0; i < kTbBuses; ++i) nt::SnapshotStore(&f.buses[i], b);
  nt::EndSnapshot(tb);
}

// Reads every word; returns the common value or ~0 if any word differs.
std::uint64_t ReadUniform(const nt::SnapshotTripleBuffer& tb, bool& ok) {
  std::uint64_t first = 0;
  bool torn = false;
  ok = nt::ReadFront(tb, [&](const nt::SnapshotFrame& f) {
    torn = false;
    const nt::FrameHeader h = nt::GetFrameHeader(f);
    first = h.block;
    auto check = [&](std::uint64_t w) {
      if (w != first) torn = true;
    };
    check(h.clock);
    check(h.busCount);
    const std::uint64_t* sw = reinterpret_cast<const std::uint64_t*>(&h.stats);
    for (std::size_t i = 0; i < sizeof(h.stats) / 8u; ++i) check(sw[i]);
    for (std::uint32_t i = 0; i < kTbPlaybacks; ++i) {
      const nt::PlaybackSnapshot p = nt::SnapshotLoad(&f.playbacks[i]);
      const std::uint64_t* w = reinterpret_cast<const std::uint64_t*>(&p);
      for (std::size_t k = 0; k < sizeof(p) / 8u; ++k) check(w[k]);
    }
    for (std::uint32_t i = 0; i < kTbTransports; ++i) {
      const nt::TransportSnapshot t = nt::SnapshotLoad(&f.transports[i]);
      const std::uint64_t* w = reinterpret_cast<const std::uint64_t*>(&t);
      for (std::size_t k = 0; k < sizeof(t) / 8u; ++k) check(w[k]);
    }
    for (std::uint32_t i = 0; i < kTbBuses; ++i) {
      const nt::BusSnapshot b = nt::SnapshotLoad(&f.buses[i]);
      const std::uint64_t* w = reinterpret_cast<const std::uint64_t*>(&b);
      for (std::size_t k = 0; k < sizeof(b) / 8u; ++k) check(w[k]);
    }
  });
  return torn ? ~0ull : first;
}
}  // namespace

SND_TEST(AdvTripleBuffer, InitialStateAndSwapOnlyWhenDirty) {
  nt::SnapshotTripleBuffer tb;
  SND_REQUIRE(nt::InitTripleBuffer(tb, DefaultAllocator(), kTbPlaybacks, kTbTransports, kTbBuses));
  bool ok = false;
  SND_CHECK_EQ(ReadUniform(tb, ok), 0u);  // all zero before the first publish
  SND_CHECK(ok);
  SND_CHECK(!nt::SwapFront(tb));
  WriteUniform(tb, 5);
  SND_CHECK_EQ(ReadUniform(tb, ok), 0u);  // not visible before Update swaps
  WriteUniform(tb, 6);                    // overwrites the pending middle
  SND_CHECK(nt::SwapFront(tb));
  SND_CHECK_EQ(ReadUniform(tb, ok), 6u);  // newest wins, never the stale 5
  SND_CHECK(!nt::SwapFront(tb));
  SND_CHECK_EQ(ReadUniform(tb, ok), 6u);
  for (std::uint64_t v = 7; v < 40; ++v) {
    WriteUniform(tb, v);
    if (v % 3u == 0u) {
      SND_CHECK(nt::SwapFront(tb));
      SND_CHECK_EQ(ReadUniform(tb, ok), v);
    }
  }
  // The three frames are distinct buffers at all times.
  SND_CHECK(tb.frames[0].playbacks != tb.frames[1].playbacks && tb.frames[1].playbacks != tb.frames[2].playbacks);
  nt::DestroyTripleBuffer(tb, DefaultAllocator());
}

SND_TEST(AdvTripleBuffer, FourQueryThreadsNeverSeeTornFrames) {
  nt::SnapshotTripleBuffer tb;
  SND_REQUIRE(nt::InitTripleBuffer(tb, DefaultAllocator(), kTbPlaybacks, kTbTransports, kTbBuses));
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> torn{0}, reads{0}, failedReads{0};
  std::thread writer([&] {
    for (std::uint64_t v = 1; !stop.load(std::memory_order_relaxed); ++v) WriteUniform(tb, v);
  });
  std::thread update([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      nt::SwapFront(tb);
      bool ok = false;
      if (ReadUniform(tb, ok) == ~0ull && ok) torn.fetch_add(1);
    }
  });
  std::vector<std::thread> qs;
  for (int q = 0; q < 4; ++q) {
    qs.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        bool ok = false;
        const std::uint64_t v = ReadUniform(tb, ok);
        if (!ok) {
          failedReads.fetch_add(1);
          continue;
        }
        if (v == ~0ull) torn.fetch_add(1);
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  stop = true;
  writer.join();
  update.join();
  for (auto& t : qs) t.join();
  SND_CHECK_EQ(torn.load(), 0u);
  SND_CHECK(reads.load() > 0u);
  snd::test::Note("reads=%llu failed(after 1024 retries)=%llu", static_cast<unsigned long long>(reads.load()),
                  static_cast<unsigned long long>(failedReads.load()));
  nt::DestroyTripleBuffer(tb, DefaultAllocator());
}
