// base: notification channels (design.md 2.1, D-16), lock-free log, mutex wrapper.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/base/log.hpp"
#include "snd/base/mutex.hpp"
#include "snd/base/notify.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::base;
namespace nt = snd::base::notify;

SND_TEST(EndFlags, PublishConsumeInSlotOrder) {
  AccountingAllocator acc(DefaultAllocator());
  nt::EndFlags f;
  SND_REQUIRE(nt::InitEndFlags(f, acc, 130));
  {
    SND_RT_SCOPE();
    nt::PublishEnd(f, 129, nt::EndRecord{7, SND_END_STOPPED, 0});
    nt::PublishEnd(f, 3, nt::EndRecord{2, SND_END_COMPLETED, 0});
    nt::PublishEnd(f, 64, nt::EndRecord{1, SND_END_REJECTED, 5});
    nt::PublishEnd(f, 500, nt::EndRecord{1, 1, 0});  // out of range: ignored
  }
  std::vector<std::uint32_t> slots;
  std::vector<nt::EndRecord> recs;
  const std::uint32_t n = nt::ConsumeEnds(f, [&](std::uint32_t s, const nt::EndRecord& r) {
    slots.push_back(s);
    recs.push_back(r);
  });
  SND_REQUIRE_EQ(n, 3u);
  SND_CHECK_EQ(slots[0], 3u);
  SND_CHECK_EQ(slots[1], 64u);
  SND_CHECK_EQ(slots[2], 129u);
  SND_CHECK_EQ(recs[1].detail, 5u);
  SND_CHECK_EQ(recs[2].generation, 7u);
  SND_CHECK_EQ(nt::ConsumeEnds(f, [](std::uint32_t, const nt::EndRecord&) {}), 0u);
  nt::DestroyEndFlags(f, acc);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(EndFlags, ConcurrentNoLossAcrossThreads) {
  // Audio thread ends every slot repeatedly; the game thread consumes and
  // "reissues" (lets the audio side end it again only after consumption).
  constexpr std::uint32_t kSlots = 256;
  constexpr std::uint32_t kRounds = 200;
  nt::EndFlags f;
  SND_REQUIRE(nt::InitEndFlags(f, DefaultAllocator(), kSlots));
  std::atomic<std::uint32_t> reissued[kSlots];
  for (auto& r : reissued) r.store(1);
  std::atomic<bool> stop{false};
  std::thread audio([&] {
    std::uint32_t gen[kSlots] = {};
    std::uint32_t remaining = kSlots * kRounds;
    while (remaining > 0u && !stop.load(std::memory_order_relaxed)) {
      for (std::uint32_t s = 0; s < kSlots; ++s) {
        if (gen[s] < kRounds && reissued[s].load(std::memory_order_acquire) > gen[s]) {
          ++gen[s];
          nt::PublishEnd(f, s, nt::EndRecord{gen[s], SND_END_COMPLETED, 0});
          --remaining;
        }
      }
    }
  });
  std::uint32_t got = 0;
  bool bad = false;
  std::uint32_t lastGen[kSlots] = {};
  while (got < kSlots * kRounds) {
    got += nt::ConsumeEnds(f, [&](std::uint32_t s, const nt::EndRecord& r) {
      if (r.generation != lastGen[s] + 1u) bad = true;
      lastGen[s] = r.generation;
      reissued[s].fetch_add(1, std::memory_order_release);
    });
  }
  stop = true;
  audio.join();
  SND_CHECK(!bad);
  SND_CHECK_EQ(got, kSlots * kRounds);
  nt::DestroyEndFlags(f, DefaultAllocator());
}

SND_TEST(ReleaseFlags, RoundTrip) {
  nt::ReleaseFlags f;
  SND_REQUIRE(nt::InitReleaseFlags(f, DefaultAllocator(), 70));
  nt::PublishRelease(f, 69, 4);
  nt::PublishRelease(f, 0, 9);
  std::uint32_t sum = 0, n = 0;
  n = nt::ConsumeReleases(f, [&](std::uint32_t s, std::uint32_t g) { sum += s * 100u + g; });
  SND_CHECK_EQ(n, 2u);
  SND_CHECK_EQ(sum, 69u * 100u + 4u + 9u);
  nt::DestroyReleaseFlags(f, DefaultAllocator());
}

SND_TEST(EventRing, DropsWhenFullAndCounts) {
  nt::EventRing r;
  SND_REQUIRE(nt::InitEventRing(r, DefaultAllocator(), 3));
  SND_CHECK_EQ(r.capacity, 4u);
  {
    SND_RT_SCOPE();
    for (std::uint32_t i = 0; i < 6; ++i) {
      nt::EventRecord e{};
      e.type = static_cast<std::uint16_t>(snd::notify::EventType::Beat);
      e.a = i;
      nt::PushEvent(r, e);
    }
  }
  SND_CHECK_EQ(nt::TakeDropped(r), 2u);
  std::vector<std::uint32_t> got;
  nt::DrainEvents(r, [&](const nt::EventRecord& e) { got.push_back(e.a); });
  SND_REQUIRE_EQ(got.size(), 4u);
  SND_CHECK_EQ(got[0], 0u);
  SND_CHECK_EQ(got[3], 3u);
  nt::DestroyEventRing(r, DefaultAllocator());
}

SND_TEST(EventRing, ConcurrentProducerConsumer) {
  nt::EventRing r;
  SND_REQUIRE(nt::InitEventRing(r, DefaultAllocator(), 64));
  constexpr std::uint32_t kCount = 100000;
  std::atomic<bool> producerDone{false};
  std::thread audio([&] {
    for (std::uint32_t i = 0; i < kCount; ++i) {
      nt::EventRecord e{};
      e.a = i;
      e.arg0 = i * 3ull;
      nt::PushEvent(r, e);
    }
    producerDone.store(true, std::memory_order_release);
  });
  std::uint32_t received = 0, last = 0;
  bool bad = false;
  for (;;) {
    const bool finished = producerDone.load(std::memory_order_acquire);
    const std::uint32_t k = nt::DrainEvents(r, [&](const nt::EventRecord& e) {
      if (e.arg0 != e.a * 3ull) bad = true;
      if (received > 0u && e.a <= last) bad = true;
      last = e.a;
      ++received;
    });
    if (finished && k == 0u) break;
  }
  audio.join();
  const std::uint32_t dropped = nt::TakeDropped(r);
  SND_CHECK(!bad);
  SND_CHECK_EQ(received + dropped, kCount);  // nothing lost silently
  SND_CHECK(received >= 64u);
  nt::DestroyEventRing(r, DefaultAllocator());
}

namespace {
void WriteFrame(nt::SnapshotTripleBuffer& tb, std::uint64_t v, std::uint32_t playbacks) {
  nt::SnapshotFrame& f = nt::BeginSnapshot(tb);
  snd_stats stats{};
  stats.block_index = v;
  stats.clock_sample = v * 256u;
  nt::SetFrameHeader(f, v, v * 256u, stats, 1u);
  for (std::uint32_t i = 0; i < playbacks; ++i) {
    nt::PlaybackSnapshot p{};
    p.generation = static_cast<std::uint32_t>(v);
    p.frames = v * 1000u + i;
    nt::SnapshotStore(&f.playbacks[i], p);
  }
  nt::EndSnapshot(tb);
}
}  // namespace

SND_TEST(TripleBuffer, ReaderSeesLatestUpdate) {
  AccountingAllocator acc(DefaultAllocator());
  nt::SnapshotTripleBuffer tb;
  SND_REQUIRE(nt::InitTripleBuffer(tb, acc, 4, 2, 3));
  SND_CHECK(!nt::SwapFront(tb));  // nothing new
  {
    SND_RT_SCOPE();
    WriteFrame(tb, 1, 4);
    WriteFrame(tb, 2, 4);  // overwrites the middle before the reader swapped
  }
  SND_CHECK(nt::SwapFront(tb));
  std::uint64_t block = 0, frames = 0;
  SND_CHECK(nt::ReadFront(tb, [&](const nt::SnapshotFrame& f) {
    block = nt::GetFrameHeader(f).block;
    frames = nt::SnapshotLoad(&f.playbacks[3]).frames;
  }));
  SND_CHECK_EQ(block, 2u);
  SND_CHECK_EQ(frames, 2003u);
  SND_CHECK(!nt::SwapFront(tb));
  WriteFrame(tb, 3, 4);
  // Without SwapFront, queries still see 2 ("as of the latest Update()").
  SND_CHECK(nt::ReadFront(tb, [&](const nt::SnapshotFrame& f) { block = nt::GetFrameHeader(f).block; }));
  SND_CHECK_EQ(block, 2u);
  SND_CHECK(nt::SwapFront(tb));
  SND_CHECK(nt::ReadFront(tb, [&](const nt::SnapshotFrame& f) { block = nt::GetFrameHeader(f).block; }));
  SND_CHECK_EQ(block, 3u);
  nt::DestroyTripleBuffer(tb, acc);
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(TripleBuffer, ConcurrentWriterSwapperAndQueriesAreConsistent) {
  constexpr std::uint32_t kPlaybacks = 16;
  nt::SnapshotTripleBuffer tb;
  SND_REQUIRE(nt::InitTripleBuffer(tb, DefaultAllocator(), kPlaybacks, 1, 1));
  std::atomic<bool> stop{false};
  std::atomic<bool> torn{false};
  std::atomic<std::uint64_t> reads{0};
  std::thread writer([&] {
    for (std::uint64_t v = 1; !stop.load(std::memory_order_relaxed); ++v) WriteFrame(tb, v, kPlaybacks);
  });
  std::thread update([&] {
    std::uint64_t lastBlock = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      if (!nt::SwapFront(tb)) continue;
      std::uint64_t b = 0;
      nt::ReadFront(tb, [&](const nt::SnapshotFrame& f) { b = nt::GetFrameHeader(f).block; });
      if (b < lastBlock) torn = true;  // fronts only move forward
      lastBlock = b;
    }
  });
  std::vector<std::thread> queries;
  for (int q = 0; q < 2; ++q) {
    queries.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        std::uint64_t block = 0;
        std::uint64_t vals[kPlaybacks];
        const bool ok = nt::ReadFront(tb, [&](const nt::SnapshotFrame& f) {
          block = nt::GetFrameHeader(f).block;
          for (std::uint32_t i = 0; i < kPlaybacks; ++i) vals[i] = nt::SnapshotLoad(&f.playbacks[i]).frames;
        });
        if (!ok || block == 0u) continue;
        for (std::uint32_t i = 0; i < kPlaybacks; ++i) {
          if (vals[i] != block * 1000u + i) torn = true;  // every field from the same publish
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop = true;
  writer.join();
  update.join();
  for (auto& t : queries) t.join();
  SND_CHECK(!torn.load());
  SND_CHECK(reads.load() > 0u);
  nt::DestroyTripleBuffer(tb, DefaultAllocator());
}

SND_TEST(Log, RtPushAndNonRtFormat) {
  LogRing log;
  SND_REQUIRE(log.Init(DefaultAllocator(), 8));
  {
    SND_RT_SCOPE();
    SND_CHECK(log.Log(LogLevel::Warn, "voice %u starved after %.2f ms (%s) %d%%", 7u, 1.5, "stream", -3));
    SND_CHECK(log.LogAt(42u, LogLevel::Info, "hex %08x %X %lld %zu", 0xBEEFu, 255, -9ll, static_cast<std::size_t>(12)));
    SND_CHECK(log.Log(LogLevel::Info, "%c|%5s|%-4d|", 'Z', "ab", 7));
    SND_CHECK(log.Log(LogLevel::Error, "missing %d %d", 1));
    SND_CHECK(log.Log(LogLevel::Debug, "no args, unknown %q and dangling %"));
  }
  std::vector<std::string> lines;
  std::vector<std::uint64_t> stamps;
  const std::uint32_t n = log.Drain([&](const LogEntry& e, const char* text) {
    lines.emplace_back(text);
    stamps.push_back(e.timestamp);
  });
  SND_REQUIRE_EQ(n, 5u);
  SND_CHECK_MSG(lines[0] == "voice 7 starved after 1.50 ms (stream) -3%", "got '%s'", lines[0].c_str());
  SND_CHECK_MSG(lines[1] == "hex 0000beef FF -9 12", "got '%s'", lines[1].c_str());
  SND_CHECK_EQ(stamps[1], 42u);
  SND_CHECK_MSG(lines[2] == "Z|   ab|7   |", "got '%s'", lines[2].c_str());
  SND_CHECK_MSG(lines[3] == "missing 1 <?>", "got '%s'", lines[3].c_str());
  SND_CHECK_MSG(lines[4] == "no args, unknown %q and dangling %", "got '%s'", lines[4].c_str());
}

SND_TEST(Log, LevelFilterDropsAndConcurrentProducers) {
  LogRing log;
  SND_REQUIRE(log.Init(DefaultAllocator(), 16));
  log.SetMinLevel(LogLevel::Warn);
  SND_CHECK(!log.Log(LogLevel::Info, "filtered"));
  SND_CHECK_EQ(log.Dropped(), 0u);
  std::vector<std::thread> producers;
  for (int t = 0; t < 3; ++t) {
    producers.emplace_back([&, t] {
      SND_RT_SCOPE();
      for (int i = 0; i < 5000; ++i) log.Log(LogLevel::Error, "t%d i%d", t, i);
    });
  }
  std::uint32_t drained = 0;
  std::atomic<bool> done{false};
  std::thread consumer([&] {
    while (!done.load()) drained += log.Drain([](const LogEntry&, const char*) {});
  });
  for (auto& p : producers) p.join();
  done = true;
  consumer.join();
  drained += log.Drain([](const LogEntry&, const char*) {});
  SND_CHECK_EQ(drained + log.TakeDropped(), 15000u);
}

SND_TEST(Log, FormattingInRtScopeIsReported) {
  snd::test::rt::ExpectViolations expect;
  snd::test::rt::Reset();
  LogEntry e{"x %d", {LogArg(1)}, 0, 0, 1};
  char buf[16];
  {
    SND_RT_SCOPE();
    FormatLogEntry(e, buf, sizeof(buf));
  }
  SND_CHECK(snd::test::rt::Count(snd::rt::Violation::Log) >= 1u);
  SND_CHECK(std::strcmp(buf, "x 1") == 0);
  snd::test::rt::Reset();
}

SND_TEST(Mutex, LockIsReportedInRtScopeOnly) {
  Mutex m;
  {
    LockGuard g(m);  // outside an RT scope: fine
  }
  SND_CHECK(m.TryLock());
  m.Unlock();
  snd::test::rt::ExpectViolations expect;
  snd::test::rt::Reset();
  {
    SND_RT_SCOPE();
    LockGuard g(m);
  }
  SND_CHECK(snd::test::rt::Count(snd::rt::Violation::Lock) >= 1u);
  snd::test::rt::Reset();
}

SND_TEST(Mutex, ProtectsSharedCounter) {
  Mutex m;
  std::uint64_t counter = 0;
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t) {
    ts.emplace_back([&] {
      for (int i = 0; i < 20000; ++i) {
        LockGuard g(m);
        ++counter;
      }
    });
  }
  for (auto& t : ts) t.join();
  SND_CHECK_EQ(counter, 80000u);
}
