// IoScheduler over the memory FS mock, driven manually with a manual clock
// (design.md 6.1, 6.3, 14 "IOの遅延注入").
#include <cstdint>
#include <vector>

#include "io_test_util.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/test/test.hpp"

using namespace snd::iotest;
using snd::io::ChunkRecord;
using snd::io::ChunkStatus;
using snd::io::FileRequest;
using snd::io::FileState;
using snd::io::IoClass;
using snd::io::kInvalidChunk;

namespace {
constexpr std::uint32_t kFileBytes = 256u << 10;

void AddFiles(Rig& rig) {
  rig.fs.AddFile("music.bin", MakePattern(1, kFileBytes));
  rig.fs.AddFile("voice.bin", MakePattern(2, kFileBytes));
  rig.fs.AddFile("amb.bin", MakePattern(3, kFileBytes));
}
}  // namespace

SND_TEST(IoScheduler, DeliversDataInRequestOrderAndReturnsChunks) {
  Rig rig;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  {
    SND_RT_SCOPE();
    for (std::uint32_t i = 0; i < 4; ++i) {
      SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", i * kChunk, kChunk, IoClass::Music, 10 + i)));
    }
    SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 4u);
  }
  rig.Run();
  SND_CHECK_EQ(rig.io.Channel(0).Ready(), 4u);
  SND_CHECK_EQ(rig.io.Channel(0).ReadPos(), 4u * kChunk);
  SND_CHECK_EQ(rig.pool.HeldBy(0), 4u);
  std::vector<ChunkRecord> recs;
  recs.reserve(8);  // no allocation inside the RT scope
  bool dataOk = true;
  {
    SND_RT_SCOPE();  // the consumer side is RT-safe too
    SND_CHECK_EQ(rig.Consume(0, 1, &recs, &dataOk), 4u);
  }
  SND_CHECK(dataOk);
  SND_REQUIRE_EQ(recs.size(), 4u);
  for (std::uint32_t i = 0; i < 4; ++i) {
    SND_CHECK_EQ(recs[i].offset, i * kChunk);
    SND_CHECK_EQ(recs[i].bytes, kChunk);
    SND_CHECK_EQ(recs[i].tag, 10u + i);
    SND_CHECK(recs[i].status == ChunkStatus::Ok);
  }
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 0u);
  SND_CHECK_EQ(rig.io.Channel(0).Delivered(), 4u);
  SND_CHECK_EQ(rig.io.Stats().readsCompleted, 4u);
}

SND_TEST(IoScheduler, OutOfOrderCompletionIsDeliveredInOrder) {
  Rig rig;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  rig.fs.AddFile("tail.bin", MakePattern(1, kFileBytes));  // same content as music.bin
  rig.fs.SetLatency("music.bin", 5);
  SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music, 0)));
  SND_CHECK(rig.io.Post(StreamReq(0, "tail.bin", kChunk, kChunk, IoClass::Music, 1)));
  rig.io.RunOnce();  // issue both
  rig.io.RunOnce();  // second completes, first still pending: nothing deliverable
  SND_CHECK_EQ(rig.io.Channel(0).Ready(), 0u);
  rig.Run();
  std::vector<ChunkRecord> recs;
  bool ok = true;
  rig.Consume(0, 1, &recs, &ok);
  SND_CHECK(ok);
  SND_REQUIRE_EQ(recs.size(), 2u);
  SND_CHECK_EQ(recs[0].tag, 0u);
  SND_CHECK_EQ(recs[1].tag, 1u);
}

SND_TEST(IoScheduler, ServesHigherPriorityFirst) {
  Rig rig;
  rig.cfg.maxInFlight = 1;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  // Posted lowest first.
  SND_CHECK(rig.io.Post(StreamReq(2, "amb.bin", 0, kChunk, IoClass::Ambience)));
  SND_CHECK(rig.io.Post(StreamReq(1, "voice.bin", 0, kChunk, IoClass::Voice)));
  SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music)));
  std::vector<std::uint32_t> order;
  for (int i = 0; i < 10 && order.size() < 3; ++i) {
    rig.io.RunOnce();
    for (std::uint32_t s = 0; s < 3; ++s) {
      if (rig.Consume(s, s + 1) != 0u) order.push_back(s);
    }
  }
  SND_REQUIRE_EQ(order.size(), 3u);
  SND_CHECK_EQ(order[0], 0u);  // music
  SND_CHECK_EQ(order[1], 1u);  // voice
  SND_CHECK_EQ(order[2], 2u);  // ambience
}

SND_TEST(IoScheduler, ClassRankIsConfigurable) {
  Rig rig;
  rig.cfg.maxInFlight = 1;
  rig.cfg.classRank[static_cast<int>(IoClass::Ambience)] = 0;
  rig.cfg.classRank[static_cast<int>(IoClass::Voice)] = 1;
  rig.cfg.classRank[static_cast<int>(IoClass::Music)] = 2;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music)));
  SND_CHECK(rig.io.Post(StreamReq(2, "amb.bin", 0, kChunk, IoClass::Ambience)));
  std::vector<std::uint32_t> order;
  for (int i = 0; i < 10 && order.size() < 2; ++i) {
    rig.io.RunOnce();
    if (rig.Consume(0, 1) != 0u) order.push_back(0);
    if (rig.Consume(2, 3) != 0u) order.push_back(2);
  }
  SND_REQUIRE_EQ(order.size(), 2u);
  SND_CHECK_EQ(order[0], 2u);
  SND_CHECK_EQ(order[1], 0u);
}

SND_TEST(IoScheduler, BandwidthBudgetPerWindowIsRespected) {
  Rig rig;
  rig.cfg.bytesPerWindow = 4u * kChunk;
  rig.cfg.maxInFlight = 16;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  for (std::uint32_t i = 0; i < 6; ++i) {
    SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", i * kChunk, kChunk, IoClass::Music)));
    SND_CHECK(rig.io.Post(StreamReq(1, "voice.bin", i * kChunk, kChunk, IoClass::Voice)));
  }
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Stats().bytesIssued, 4u * kChunk);
  for (int i = 0; i < 5; ++i) rig.io.RunOnce();  // same window: nothing more
  SND_CHECK_EQ(rig.io.Stats().bytesIssued, 4u * kChunk);
  SND_CHECK_EQ(rig.io.Stats().budgetWaits, 1u);
  rig.clock.AdvanceMs(99);
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Stats().bytesIssued, 4u * kChunk);
  rig.clock.AdvanceMs(1);  // next window
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Stats().bytesIssued, 8u * kChunk);
  rig.clock.AdvanceMs(100);
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Stats().bytesIssued, 12u * kChunk);
  rig.Run();
  SND_CHECK(rig.io.Stats().maxWindowBytes <= 4u * kChunk);
  // Music (higher class) was served in the first windows.
  SND_CHECK_EQ(rig.Consume(0, 1), 6u);
  SND_CHECK_EQ(rig.Consume(1, 2), 6u);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
}

SND_TEST(IoScheduler, BandwidthSplitsFileRequestsIntoPieces) {
  Rig rig;
  rig.cfg.bytesPerWindow = 64u << 10;
  rig.cfg.filePieceBytes = 256u << 10;  // clamped to the budget
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  std::vector<std::uint8_t> dst(kFileBytes);
  FileRequest req;
  req.path = "amb.bin";
  req.bytes = kFileBytes;
  req.dst = dst.data();
  SND_REQUIRE(rig.io.Submit(req));
  std::uint32_t windows = 0;
  for (; windows < 20 && !req.Finished(); ++windows) {
    rig.Run(50);
    SND_CHECK(rig.io.Stats().maxWindowBytes <= (64u << 10));
    rig.clock.AdvanceMs(100);
  }
  SND_REQUIRE(req.Finished());
  SND_CHECK(req.State() == FileState::Done);
  SND_CHECK_EQ(req.Result().bytesRead, kFileBytes);
  SND_CHECK_EQ(windows, 4u);  // 256 KiB at 64 KiB per window
  SND_CHECK(MatchesPattern(3, 0, dst.data(), kFileBytes));
}

SND_TEST(IoScheduler, AgingPreventsStarvation) {
  for (int withAging = 0; withAging < 2; ++withAging) {
    Rig rig;
    rig.cfg.maxInFlight = 1;
    rig.cfg.bytesPerWindow = kChunk;  // one read per window
    rig.cfg.agingNs = withAging != 0 ? 300000000ull : ~0ull;
    SND_REQUIRE(rig.Init());
    AddFiles(rig);
    SND_CHECK(rig.io.Post(StreamReq(2, "amb.bin", 0, kChunk, IoClass::Ambience)));
    std::uint64_t musicOffset = 0;
    int ambienceWindow = -1;
    for (int w = 0; w < 12; ++w) {
      // Music always has fresh demand.
      while (rig.io.Channel(0).Outstanding() < 2u) {
        SND_REQUIRE(rig.io.Post(StreamReq(0, "music.bin", musicOffset % kFileBytes, kChunk, IoClass::Music)));
        musicOffset += kChunk;
      }
      rig.io.RunOnce();
      rig.io.RunOnce();
      rig.Consume(0, 1);
      if (ambienceWindow < 0 && rig.Consume(2, 3) != 0u) ambienceWindow = w;
      rig.clock.AdvanceMs(100);
    }
    if (withAging != 0) {
      SND_CHECK(ambienceWindow >= 3 && ambienceWindow <= 4);
      SND_CHECK(rig.io.Stats().agedServed >= 1u);
    } else {
      SND_CHECK_EQ(ambienceWindow, -1);  // strict priority starves it
    }
  }
}

SND_TEST(IoScheduler, CancelDropsQueuedAndDiscardsInFlight) {
  Rig rig;
  rig.cfg.maxInFlight = 2;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  rig.fs.SetLatency("music.bin", 3);
  for (std::uint32_t i = 0; i < 4; ++i) SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", i * kChunk, kChunk, IoClass::Music, i)));
  rig.io.RunOnce();  // two in flight, two queued
  SND_CHECK_EQ(rig.io.Stats().inFlight, 2u);
  const std::uint32_t oldEpoch = rig.io.Channel(0).Epoch();
  {
    SND_RT_SCOPE();
    rig.io.Channel(0).Cancel();  // seek
  }
  rig.fs.SetLatency("music.bin", 0);
  const std::uint64_t seekBase = 100u * 1024u;
  rig.io.RunOnce();  // drops the two queued
  SND_CHECK_EQ(rig.io.Stats().staleDropped, 2u);
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 2u);  // the in-flight ones
  for (std::uint32_t i = 0; i < 2; ++i) {
    SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", seekBase + i * kChunk, kChunk, IoClass::Music, 100 + i)));
  }
  rig.Run();
  std::vector<ChunkRecord> recs;
  bool ok = true;
  rig.Consume(0, 1, &recs, &ok);
  SND_CHECK(ok);
  SND_REQUIRE_EQ(recs.size(), 2u);  // only the new position arrives
  for (std::uint32_t i = 0; i < 2; ++i) {
    SND_CHECK_EQ(recs[i].offset, seekBase + i * kChunk);
    SND_CHECK_EQ(recs[i].tag, 100u + i);
    SND_CHECK(recs[i].epoch != oldEpoch);
  }
  SND_CHECK_EQ(rig.io.Stats().staleCompleted, 2u);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 0u);
}

SND_TEST(IoScheduler, StaleRecordsInRingAreSkippedAndReclaimed) {
  Rig rig;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  for (std::uint32_t i = 0; i < 3; ++i) SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", i * kChunk, kChunk, IoClass::Music)));
  rig.Run();
  SND_CHECK_EQ(rig.io.Channel(0).Ready(), 3u);
  SND_CHECK_EQ(rig.pool.Used(), 3u);
  ChunkRecord rec;
  {
    SND_RT_SCOPE();
    rig.io.Channel(0).Cancel();  // stop
    SND_CHECK(!rig.io.Channel(0).Pop(rec));
    SND_CHECK(rig.io.Channel(0).Reclaim());
  }
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 0u);
}

SND_TEST(IoScheduler, StallIsReportedWhenDataIsLate) {
  Rig rig;
  rig.cfg.stallNs = 200000000ull;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  rig.fs.Hold("voice.bin");
  SND_CHECK(rig.io.Post(StreamReq(1, "voice.bin", 0, kChunk, IoClass::Voice)));
  rig.io.RunOnce();
  rig.clock.AdvanceMs(150);
  rig.io.RunOnce();
  SND_CHECK(!rig.io.Channel(1).IoStalled());
  rig.clock.AdvanceMs(60);
  rig.io.RunOnce();
  {
    SND_RT_SCOPE();  // the audio thread reads the flags without waiting
    SND_CHECK(rig.io.Channel(1).IoStalled());
    SND_CHECK_EQ(rig.io.Channel(1).IoStallEvents(), 1u);
  }
  rig.clock.AdvanceMs(100);
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Channel(1).IoStallEvents(), 1u);  // once per request
  SND_CHECK_EQ(rig.io.Stats().stallEvents, 1u);
  rig.fs.Release("voice.bin");
  rig.Run();
  SND_CHECK(!rig.io.Channel(1).IoStalled());
  SND_CHECK_EQ(rig.Consume(1, 2), 1u);
}

SND_TEST(IoScheduler, ErrorsArePropagatedInOrder) {
  Rig rig;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  rig.fs.FailReads("music.bin", 0, kChunk);
  SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music, 0)));
  SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", kChunk, kChunk, IoClass::Music, 1)));
  SND_CHECK(rig.io.Post(StreamReq(0, "missing.bin", 0, kChunk, IoClass::Music, 2)));
  rig.Run();
  std::vector<ChunkRecord> recs;
  bool ok = true;
  rig.Consume(0, 1, &recs, &ok);
  SND_CHECK(ok);
  SND_REQUIRE_EQ(recs.size(), 3u);
  SND_CHECK(recs[0].status == ChunkStatus::ReadError);
  SND_CHECK_EQ(recs[0].chunk, kInvalidChunk);
  SND_CHECK(recs[1].status == ChunkStatus::Ok);
  SND_CHECK(recs[2].status == ChunkStatus::OpenFailed);
  SND_CHECK_EQ(rig.io.Channel(0).Errors(), 2u);
  SND_CHECK_EQ(rig.io.Stats().readErrors, 1u);
  SND_CHECK_EQ(rig.io.Stats().openFailures, 1u);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  // A file request on a missing file ends in Error.
  FileRequest req;
  req.path = "missing.bin";
  SND_REQUIRE(rig.io.Submit(req));
  rig.Run();
  SND_CHECK(req.State() == FileState::Error);
  SND_CHECK_EQ(req.Result().fileSize, UINT64_MAX);
  // The file can appear later: the failed open is not cached forever.
  rig.fs.AddFile("missing.bin", MakePattern(9, 100));
  SND_REQUIRE(rig.io.Submit(req));
  rig.Run();
  SND_CHECK(req.State() == FileState::Done);
  SND_CHECK_EQ(req.Result().fileSize, 100u);
}

SND_TEST(IoScheduler, PoolExhaustionWaitsAndReportsHolders) {
  Rig rig(4);  // 4 chunks only
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  for (std::uint32_t i = 0; i < 6; ++i) SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", i * kChunk, kChunk, IoClass::Music)));
  rig.Run(20);
  SND_CHECK_EQ(rig.io.Channel(0).Ready(), 4u);
  SND_CHECK_EQ(rig.pool.HeldBy(0), 4u);  // material for "stop or virtualise the lowest stream"
  SND_CHECK(rig.io.Stats().poolStarved >= 1u);
  SND_CHECK(rig.pool.Stats().failedAcquires >= 1u);
  // A file request does not need the pool and still progresses.
  std::vector<std::uint8_t> dst(1000);
  FileRequest req;
  req.path = "amb.bin";
  req.bytes = 1000;
  req.dst = dst.data();
  SND_REQUIRE(rig.io.Submit(req));
  rig.Run(20);
  SND_CHECK(req.State() == FileState::Done);
  // Consuming two chunks lets two more reads through.
  ChunkRecord rec;
  for (int i = 0; i < 2; ++i) {
    SND_REQUIRE(rig.io.Channel(0).Pop(rec));
    rig.io.Channel(0).Release(rec);
  }
  rig.Run(20);
  SND_CHECK_EQ(rig.io.Channel(0).Ready(), 4u);
  bool ok = true;
  SND_CHECK_EQ(rig.Consume(0, 1, nullptr, &ok), 4u);
  SND_CHECK(ok);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
}

SND_TEST(IoScheduler, RejectsInvalidAndOverfullPosts) {
  Rig rig;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  std::uint8_t foreign[64];
  {
    SND_RT_SCOPE();
    SND_CHECK(!rig.io.Post(StreamReq(99, "music.bin", 0, kChunk, IoClass::Music)));        // slot
    SND_CHECK(!rig.io.Post(StreamReq(0, "music.bin", 0, 0, IoClass::Music)));              // bytes
    SND_CHECK(!rig.io.Post(StreamReq(0, "music.bin", 0, kChunk + 1, IoClass::Music)));     // > chunk
    SND_CHECK(!rig.io.Post(StreamReq(0, "music.bin", 0, 64, IoClass::Music, 0, foreign)));  // not a pool chunk
    for (std::uint32_t i = 0; i < 8; ++i) SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music)));
    SND_CHECK(!rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music)));  // channel full
    SND_CHECK(rig.io.Post(StreamReq(1, "music.bin", 0, kChunk, IoClass::Music)));   // other channel fine
  }
  SND_CHECK_EQ(rig.io.Stats().streamRejected, 5u);
  SND_CHECK_EQ(rig.io.Stats().streamPosted, 9u);
}

SND_TEST(IoScheduler, CallerProvidedChunkIsFilledAndReturned) {
  Rig rig;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  std::uint32_t chunk = kInvalidChunk;
  {
    SND_RT_SCOPE();
    chunk = rig.pool.Acquire(3);
    SND_REQUIRE(chunk != kInvalidChunk);
    SND_CHECK(rig.io.Post(StreamReq(3, "voice.bin", 1000, 4000, IoClass::Voice, 7, rig.pool.Data(chunk))));
  }
  rig.Run();
  ChunkRecord rec;
  SND_REQUIRE(rig.io.Channel(3).Pop(rec));
  SND_CHECK_EQ(rec.chunk, chunk);
  SND_CHECK_EQ(rec.bytes, 4000u);
  SND_CHECK(MatchesPattern(2, 1000, rig.pool.Data(chunk), 4000));
  rig.io.Channel(3).Release(rec);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  // Cancelled before issue: the IO thread gives the caller's chunk back.
  chunk = rig.pool.Acquire(3);
  SND_CHECK(rig.io.Post(StreamReq(3, "voice.bin", 0, 4000, IoClass::Voice, 0, rig.pool.Data(chunk))));
  rig.io.Channel(3).Cancel();
  rig.Run();
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK_EQ(rig.io.Channel(3).Outstanding(), 0u);
}

SND_TEST(IoScheduler, EndOfFileShortensTheLastRead) {
  Rig rig;
  SND_REQUIRE(rig.Init());
  rig.fs.AddFile("short.bin", MakePattern(4, 10000));
  SND_CHECK(rig.io.Post(StreamReq(0, "short.bin", 8192, kChunk, IoClass::Music)));
  rig.Run();
  ChunkRecord rec;
  SND_REQUIRE(rig.io.Channel(0).Pop(rec));
  SND_CHECK(rec.status == ChunkStatus::Ok);
  SND_CHECK_EQ(rec.bytes, 10000u - 8192u);
  SND_CHECK_EQ(rec.requested, kChunk);
  SND_CHECK(MatchesPattern(4, 8192, rig.pool.Data(rec.chunk), rec.bytes));
  rig.io.Channel(0).Release(rec);
}

SND_TEST(IoScheduler, FileRequestsSizeRangeCancelAndCallback) {
  Rig rig;
  rig.cfg.filePieceBytes = 4096;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  struct Seen {
    int calls = 0;
    snd::io::FileResult last{};
  } seen;
  auto cb = [](void* user, const snd::io::FileResult& r) noexcept {
    auto* s = static_cast<Seen*>(user);
    ++s->calls;
    s->last = r;
  };
  // Size query.
  FileRequest size;
  size.path = "voice.bin";
  size.onComplete = cb;
  size.user = &seen;
  SND_REQUIRE(rig.io.Submit(size));
  SND_CHECK(!rig.io.Submit(size));  // already queued
  rig.Run();
  SND_CHECK(size.State() == FileState::Done);
  SND_CHECK_EQ(size.Result().fileSize, kFileBytes);
  SND_CHECK_EQ(size.Result().bytesRead, 0u);
  SND_CHECK_EQ(seen.calls, 1);
  SND_CHECK(seen.last.state == FileState::Done);
  // Range read crossing several pieces and running past the end of the file.
  std::vector<std::uint8_t> dst(20000);
  FileRequest range;
  range.path = "voice.bin";
  range.offset = kFileBytes - 10000;
  range.bytes = 20000;
  range.dst = dst.data();
  range.onComplete = cb;
  range.user = &seen;
  SND_REQUIRE(rig.io.Submit(range));
  rig.Run();
  SND_CHECK(range.State() == FileState::Done);
  SND_CHECK_EQ(range.Result().bytesRead, 10000u);
  SND_CHECK(MatchesPattern(2, kFileBytes - 10000, dst.data(), 10000));
  SND_CHECK_EQ(seen.calls, 2);
  SND_CHECK_EQ(seen.last.bytesRead, 10000u);
  // Cancel while queued.
  FileRequest cancelled;
  cancelled.path = "voice.bin";
  cancelled.bytes = 20000;
  cancelled.dst = dst.data();
  SND_REQUIRE(rig.io.Submit(cancelled));
  rig.io.Cancel(cancelled);
  rig.Run();
  SND_CHECK(cancelled.State() == FileState::Cancelled);
  // Cancel between pieces.
  rig.fs.SetLatency("voice.bin", 2);
  SND_REQUIRE(rig.io.Submit(cancelled));
  rig.io.RunOnce();
  rig.io.Cancel(cancelled);
  rig.Run();
  SND_CHECK(cancelled.State() == FileState::Cancelled);
  SND_CHECK(cancelled.Result().bytesRead <= 4096u);
  // Invalid requests.
  FileRequest bad;
  SND_CHECK(!rig.io.Submit(bad));  // no path
  bad.path = "voice.bin";
  bad.dst = dst.data();
  SND_CHECK(!rig.io.Submit(bad));  // dst without bytes
  SND_CHECK(bad.State() == FileState::Idle);
}

SND_TEST(IoScheduler, BankReadsYieldToStreams) {
  Rig rig;
  rig.cfg.maxInFlight = 1;
  rig.cfg.filePieceBytes = kChunk;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  std::vector<std::uint8_t> dst(2 * kChunk);
  FileRequest bank;
  bank.path = "amb.bin";
  bank.bytes = 2 * kChunk;
  bank.dst = dst.data();
  SND_REQUIRE(rig.io.Submit(bank));
  SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music)));
  rig.io.RunOnce();  // issues the music read first
  rig.io.RunOnce();  // music done, bank piece 1 issued
  SND_CHECK_EQ(rig.io.Channel(0).Ready(), 1u);
  SND_CHECK(!bank.Finished());
  rig.Run();
  SND_CHECK(bank.State() == FileState::Done);
  SND_CHECK(MatchesPattern(3, 0, dst.data(), 2 * kChunk));
  rig.Consume(0, 1);
}

SND_TEST(IoScheduler, FileSystemRejectionIsRetried) {
  Rig rig;
  rig.cfg.maxInFlight = 8;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  rig.fs.SetMaxInFlight(2);
  for (std::uint32_t i = 0; i < 6; ++i) SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", i * kChunk, kChunk, IoClass::Music)));
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Stats().inFlight, 2u);
  SND_CHECK(rig.io.Stats().fsRejects >= 1u);
  rig.Run();
  bool ok = true;
  SND_CHECK_EQ(rig.Consume(0, 1, nullptr, &ok), 6u);
  SND_CHECK(ok);
}

SND_TEST(IoScheduler, ShutdownCancelsQueuedAndWaitsForInFlight) {
  Rig rig;
  rig.cfg.maxInFlight = 2;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  rig.fs.SetLatency("music.bin", 4);
  for (std::uint32_t i = 0; i < 5; ++i) SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", i * kChunk, kChunk, IoClass::Music)));
  std::vector<std::uint8_t> dst(1000);
  FileRequest req;
  req.path = "amb.bin";
  req.bytes = 1000;
  req.dst = dst.data();
  SND_REQUIRE(rig.io.Submit(req));
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.fs.InFlight(), 2u);
  rig.io.Shutdown();
  SND_CHECK_EQ(rig.fs.InFlight(), 0u);
  SND_CHECK_EQ(rig.fs.OpenFiles(), 0u);
  SND_CHECK(req.State() == FileState::Cancelled);
  SND_CHECK(!rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music)));
  SND_CHECK(!rig.io.Submit(req));
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 0u);
}

SND_TEST(IoScheduler, DrainEpochAdvances) {
  Rig rig;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  const std::uint64_t e0 = rig.io.DrainEpoch();
  SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music)));
  rig.io.RunOnce();
  SND_CHECK(rig.io.DrainEpoch() > e0);
  rig.Run();
  rig.Consume(0, 1);
}

SND_TEST(IoScheduler, MakeConfigFromSndConfig) {
  snd_config c;
  snd_config_init(&c);
  c.max_streams = 12;
  c.tuning.io_bytes_per_100ms = 1234;
  c.tuning.stream_target_ahead_ms = 150;
  const snd::io::IoSchedulerConfig ic = snd::io::MakeIoSchedulerConfig(c);
  SND_CHECK_EQ(ic.maxStreams, 12u);
  SND_CHECK_EQ(ic.bytesPerWindow, 1234u);
  SND_CHECK_EQ(ic.stallNs, 150000000ull);
  SND_CHECK_EQ(ic.windowNs, 100000000ull);
}

SND_TEST(IoScheduler, ReadPosIsEmptyAfterCancelUntilTheNextDelivery) {
  Rig rig;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  SND_CHECK_EQ(rig.io.Channel(0).ReadPos(), snd::io::StreamChannel::kNoReadPos);  // nothing yet
  SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music)));
  rig.Run();
  SND_CHECK_EQ(rig.io.Channel(0).ReadPos(), kChunk);
  {
    SND_RT_SCOPE();
    rig.io.Channel(0).Cancel();
    SND_CHECK_EQ(rig.io.Channel(0).ReadPos(), snd::io::StreamChannel::kNoReadPos);
  }
  SND_CHECK(rig.io.Channel(0).Reclaim());
  SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", 5u * kChunk, kChunk, IoClass::Music)));
  rig.Run();
  SND_CHECK_EQ(rig.io.Channel(0).ReadPos(), 6u * kChunk);
  rig.Consume(0, 1);
}

SND_TEST(IoScheduler, PostRefusesChunksOfOtherSlotsAndDoublePosts) {
  Rig rig;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  const std::uint32_t mine = rig.pool.Acquire(1);
  const std::uint32_t other = rig.pool.Acquire(2);
  const std::uint32_t loose = rig.pool.Acquire(snd::io::kNoOwner);
  SND_REQUIRE(mine != kInvalidChunk && other != kInvalidChunk && loose != kInvalidChunk);
  {
    SND_RT_SCOPE();
    SND_CHECK(!rig.io.Post(StreamReq(1, "music.bin", 0, 100, IoClass::Music, 0, rig.pool.Data(other))));  // slot 2's chunk
    SND_CHECK(rig.io.Post(StreamReq(1, "music.bin", 0, 100, IoClass::Music, 1, rig.pool.Data(mine))));
    SND_CHECK(!rig.io.Post(StreamReq(1, "music.bin", 100, 100, IoClass::Music, 2, rig.pool.Data(mine))));  // twice
    SND_CHECK(rig.io.Post(StreamReq(1, "music.bin", 100, 100, IoClass::Music, 3, rig.pool.Data(loose))));  // unattributed
  }
  rig.Run();
  std::vector<ChunkRecord> recs;
  bool ok = true;
  // After delivery the consumer owns the chunk: it may post it again (once).
  ChunkRecord rec;
  SND_REQUIRE(rig.io.Channel(1).Pop(rec));
  SND_CHECK_EQ(rec.chunk, mine);
  SND_CHECK(rig.io.Post(StreamReq(1, "music.bin", 200, 100, IoClass::Music, 4, rig.pool.Data(mine))));
  // The record's reservation ends without returning the re-posted chunk to the pool.
  rec.chunk = kInvalidChunk;
  rig.io.Channel(1).Release(rec);
  rig.Run();
  rig.Consume(1, 1, &recs, &ok);
  SND_CHECK(ok);
  SND_REQUIRE_EQ(recs.size(), 2u);
  SND_CHECK_EQ(recs[0].tag, 3u);
  SND_CHECK_EQ(recs[1].tag, 4u);
  rig.pool.Release(other);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK_EQ(rig.io.Stats().streamRejected, 2u);
}

SND_TEST(IoScheduler, ClockGoingBackStartsANewWindow) {
  Rig rig;
  rig.cfg.bytesPerWindow = kChunk;
  rig.cfg.maxInFlight = 8;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  for (std::uint32_t i = 0; i < 3; ++i) SND_CHECK(rig.io.Post(StreamReq(0, "music.bin", i * kChunk, kChunk, IoClass::Music)));
  rig.clock.now.store(10000000000ull);
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Stats().readsIssued, 1u);
  rig.clock.now.store(1000000000ull);  // 9 s back
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Stats().readsIssued, 2u);  // new window at once, not after 9 s
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Stats().readsIssued, 2u);  // and that window has its budget
  rig.clock.AdvanceMs(100);
  rig.Run();
  SND_CHECK_EQ(rig.Consume(0, 1), 3u);
}

// Class change inside a channel with dst=nullptr only (decision: pool chunks
// are handed out in channel order). Pool of N chunks, N channels each posting
// an ambience request then a music request: with a consumer that pops and
// releases, every channel delivers both records in request order. (Before the
// fix the music requests took all N chunks and nothing was ever delivered.)
SND_TEST(IoScheduler, ClassChangeInsideAChannelKeepsProgressingWithPoolChunks) {
  for (std::uint32_t n : {1u, 3u}) {
    Rig rig(n);
    rig.cfg.maxInFlight = 8;
    SND_REQUIRE(rig.Init());
    AddFiles(rig);
    for (std::uint32_t s = 0; s < n; ++s) {
      SND_REQUIRE(rig.io.Post(StreamReq(s, "music.bin", 0, kChunk, IoClass::Ambience, 1)));
      SND_REQUIRE(rig.io.Post(StreamReq(s, "music.bin", kChunk, kChunk, IoClass::Music, 2)));
    }
    std::vector<std::vector<ChunkRecord>> got(n);
    bool ok = true;
    for (int step = 0; step < 50; ++step) {
      rig.io.RunOnce();
      for (std::uint32_t s = 0; s < n; ++s) rig.Consume(s, 1, &got[s], &ok);
    }
    SND_CHECK(ok);
    for (std::uint32_t s = 0; s < n; ++s) {
      SND_REQUIRE_EQ(got[s].size(), 2u);
      SND_CHECK_EQ(got[s][0].tag, 1u);
      SND_CHECK_EQ(got[s][1].tag, 2u);
    }
    SND_CHECK_EQ(rig.pool.Used(), 0u);
  }
}

// With enough chunks the later music request is still issued first, but the
// earlier ambience request got its chunk before it (channel order of chunks).
SND_TEST(IoScheduler, LaterHigherClassRequestIsIssuedFirstAfterEarlierOnesGotChunks) {
  Rig rig(4);
  rig.cfg.maxInFlight = 1;
  SND_REQUIRE(rig.Init());
  AddFiles(rig);
  rig.fs.SetLatency("music.bin", 2);
  SND_REQUIRE(rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Ambience, 1)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "music.bin", kChunk, kChunk, IoClass::Music, 2)));
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Stats().readsIssued, 1u);
  SND_CHECK_EQ(rig.pool.HeldBy(0), 2u);  // the in-flight music read and the waiting ambience request
  rig.Run();
  std::vector<ChunkRecord> recs;
  rig.Consume(0, 1, &recs);
  SND_REQUIRE_EQ(recs.size(), 2u);
  SND_CHECK_EQ(recs[0].tag, 1u);
  SND_CHECK_EQ(recs[1].tag, 2u);
}
