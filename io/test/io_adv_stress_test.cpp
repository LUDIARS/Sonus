// Independent concurrency tests of the io module (run under TSan and Release,
// repeated: `ctest --repeat until-fail:20 -R io`).
//   - three-thread conservation: audio (Post with both chunk modes, Cancel/seek),
//     IO thread, decode (Pop/Peek/Release, order and epoch checks); every chunk
//     back at the end; the file system used as the contract says;
//   - lost wake-ups: IO thread with a 10 s poll period, woken only by Post and
//     by the file system's completion notify;
//   - Shutdown racing with Post / Submit (nothing may be stranded);
//   - FileRequest Cancel racing with completion (one callback, one final state);
//   - StreamPool from several threads (no chunk handed out twice);
//   - Init / Start / Shutdown / Destroy cycles under load.
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "io_adversarial_util.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/io/default_file_system.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_read_planner.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"

using namespace snd::iotest;
using snd::io::ChunkRecord;
using snd::io::ChunkStatus;
using snd::io::FileRequest;
using snd::io::FileResult;
using snd::io::FileState;
using snd::io::IoClass;
using snd::io::kInvalidChunk;

namespace {

constexpr std::uint32_t kC = 4096;
constexpr std::uint32_t kFileSize = 300000;
const char* const kP[4] = {"t0.bin", "t1.bin", "t2.bin", "t3.bin"};

std::uint64_t NowNs() { return snd::platform::MonotonicNanoseconds(); }

}  // namespace

SND_TEST(IoAdvStress, ThreeThreadsConserveChunksOrderAndEpochs) {
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  ScriptFs fs;
  for (std::uint32_t s = 0; s < 4; ++s) fs.AddFile(kP[s], MakePattern(s + 1u, kFileSize));
  fs.SetRandomLatency(3, 99);
  fs.SetFailEvery(37);
  fs.SetMaxInFlight(5);
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 20u * kC, kC, 4));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig cfg;
  cfg.maxStreams = 4;
  cfg.ringChunks = 4;
  cfg.maxInFlight = 6;
  cfg.bytesPerWindow = 0;
  cfg.pollIntervalNs = 50000;
  cfg.maxOpenFiles = 4;
  SND_REQUIRE(io.Init(alloc, fs, pool, cfg));
  SND_REQUIRE(io.Start());

  std::atomic<bool> stopAudio{false}, stopDecode{false};
  std::atomic<std::uint32_t> badPeek{0}, badOwner{0}, badErrChunk{0}, badRefused{0}, badOrder{0}, staleEpoch{0}, badData{0};
  std::atomic<std::uint64_t> consumed{0}, posted{0};
  // Epochs written by the audio thread after each Cancel (decode reads them).
  std::atomic<std::uint32_t> audioEpoch[4] = {};

  std::thread audio([&] {
    snd::io::StreamReadPlanner planners[4];
    for (std::uint32_t s = 0; s < 4; ++s) planners[s].Reset(0, kFileSize, s == 1 ? 10000u : 0u, s == 1 ? 50000u : 0u, kC);
    std::uint32_t rng = 777u;
    while (!stopAudio.load(std::memory_order_relaxed)) {
      SND_RT_SCOPE();
      for (std::uint32_t s = 0; s < 4; ++s) {
        rng = rng * 1664525u + 1013904223u;
        snd::io::StreamChannel& ch = io.Channel(s);
        if ((rng >> 25) == 0u) {  // ~1/128: seek
          ch.Cancel();
          audioEpoch[s].store(ch.Epoch(), std::memory_order_release);
          planners[s].Seek(((rng >> 8) % (kFileSize / kC)) * kC);
        }
        for (int k = 0; k < 2 && ch.Outstanding() < ch.Capacity(); ++k) {
          snd::io::StreamRead r{};
          if (!planners[s].Next(r)) {
            ch.Cancel();
            audioEpoch[s].store(ch.Epoch(), std::memory_order_release);
            planners[s].Seek(0);
            break;
          }
          // Slots 0/1: the IO thread borrows (music/voice). Slots 2/3: caller chunks (ambience),
          // a class the dst=nullptr streams never use (see IoDefect.QueuedCallerChunksBehindChunklessHeadDeadlock).
          void* dst = nullptr;
          std::uint32_t c = kInvalidChunk;
          if (s >= 2u) {
            c = pool.Acquire(s);
            if (c == kInvalidChunk) break;
            dst = pool.Data(c);
          }
          const IoClass cls = s == 0 ? IoClass::Music : (s == 1 ? IoClass::Voice : IoClass::Ambience);
          if (!io.Post(StreamReq(s, kP[s], r.offset, r.bytes, cls, static_cast<std::uint32_t>(r.offset / kC), dst))) {
            if (c != kInvalidChunk) {
              if (!pool.IsBorrowed(c) || pool.OwnerOf(c) != s) badRefused.fetch_add(1);
              pool.Release(c);
            }
            planners[s].Seek(r.offset);
            break;
          }
          posted.fetch_add(1, std::memory_order_relaxed);
        }
      }
      snd::platform::YieldThread();
    }
  });

  std::thread decode([&] {
    std::uint32_t maxEpoch[4] = {};
    std::uint32_t lastEpoch[4] = {};
    std::uint64_t nextOffset[4] = {};
    bool have[4] = {};
    while (!stopDecode.load(std::memory_order_relaxed)) {
      SND_RT_SCOPE();
      for (std::uint32_t s = 0; s < 4; ++s) {
        snd::io::StreamChannel& ch = io.Channel(s);
        // Loaded before Pop: a Cancel that completed before this load must be seen by Pop.
        const std::uint32_t cancelledUpTo = audioEpoch[s].load(std::memory_order_acquire);
        const ChunkRecord* peek = ch.Peek();
        std::uint32_t peekTag = peek != nullptr ? peek->tag : 0xFFFFFFFFu;
        const std::uint32_t peekEpoch = peek != nullptr ? peek->epoch : 0u;
        ChunkRecord rec{};
        while (ch.Pop(rec)) {
          // Peek showed the record Pop returns (unless a Cancel in between made it stale).
          if (peekTag != 0xFFFFFFFFu && peekEpoch == rec.epoch && peekTag != rec.tag) badPeek.fetch_add(1);
          peekTag = 0xFFFFFFFFu;
          // Epochs only move forward: a record older than one already seen is stale data.
          if (rec.epoch < maxEpoch[s]) staleEpoch.fetch_add(1);
          // A record from before the last Cancel that the audio thread completed
          // before we started this Pop must not appear (Cancel happened-before).
          if (rec.epoch < cancelledUpTo) staleEpoch.fetch_add(1);
          if (rec.epoch > maxEpoch[s]) maxEpoch[s] = rec.epoch;
          if (rec.status == ChunkStatus::Ok) {
            if (!MatchesPattern(s + 1u, rec.offset, pool.Data(rec.chunk), rec.bytes)) badData.fetch_add(1);
            if (pool.OwnerOf(rec.chunk) != s) badOwner.fetch_add(1);
          } else if (rec.chunk != kInvalidChunk) {
            badErrChunk.fetch_add(1);
          }
          if (have[s] && rec.epoch == lastEpoch[s] && rec.offset != nextOffset[s]) {
            // Request order: next offset, or the loop start after the loop end on slot 1.
            if (!(s == 1u && nextOffset[s] == 50000u && rec.offset == 10000u)) badOrder.fetch_add(1);
          }
          have[s] = true;
          lastEpoch[s] = rec.epoch;
          nextOffset[s] = rec.offset + rec.requested;
          ch.Release(rec);
          consumed.fetch_add(1, std::memory_order_relaxed);
        }
      }
      snd::platform::YieldThread();
    }
  });

  const std::uint64_t until = NowNs() + 1500000000ull;
  while (NowNs() < until) {
    snd::platform::SleepFor(30000000);
    fs.RejectNextReads(2);
  }
  stopAudio.store(true);
  audio.join();
  for (std::uint32_t s = 0; s < 4; ++s) io.Channel(s).Cancel();
  snd::platform::SleepFor(50000000);
  stopDecode.store(true);
  decode.join();
  io.Shutdown();
  for (std::uint32_t s = 0; s < 4; ++s) SND_CHECK(io.Channel(s).Reclaim());
  const snd::io::IoStats st = io.Stats();
  snd::test::Note("posted %llu consumed %llu staleDropped %llu staleCompleted %llu errors %llu rejects %llu starved %llu",
                  static_cast<unsigned long long>(posted.load()), static_cast<unsigned long long>(consumed.load()),
                  static_cast<unsigned long long>(st.staleDropped), static_cast<unsigned long long>(st.staleCompleted),
                  static_cast<unsigned long long>(st.readErrors), static_cast<unsigned long long>(st.fsRejects),
                  static_cast<unsigned long long>(st.poolStarved));
  SND_CHECK(consumed.load() > 200u);
  SND_CHECK_EQ(badPeek.load(), 0u);
  SND_CHECK_EQ(badOwner.load(), 0u);
  SND_CHECK_EQ(badErrChunk.load(), 0u);
  SND_CHECK_EQ(badRefused.load(), 0u);
  SND_CHECK_EQ(badOrder.load(), 0u);
  SND_CHECK_EQ(staleEpoch.load(), 0u);
  SND_CHECK_EQ(badData.load(), 0u);
  SND_CHECK_EQ(fs.InFlight(), 0u);
  SND_CHECK_EQ(fs.OpenFiles(), 0u);
  const std::vector<std::string> v = fs.Violations();
  SND_CHECK_MSG(v.empty(), "%s", JoinViolations(v).c_str());
  io.Destroy();
  SND_CHECK_EQ(pool.Used(), 0u);
  SND_CHECK_EQ(pool.Stats().badReleases, 0u);
  pool.Destroy();
  SND_CHECK_EQ(alloc.TotalUsed(), 0u);
}

namespace {
struct TempDir {
  std::string root;
  TempDir() {
    char tmpl[] = "/tmp/snd-iot-stress-XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (d != nullptr) root = d;
  }
  ~TempDir() {
    if (!root.empty()) {
      const std::string cmd = "rm -rf '" + root + "'";
      if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
    }
  }
  void Write(const char* name, const std::vector<std::uint8_t>& d) const {
    std::FILE* f = std::fopen((root + "/" + name).c_str(), "wb");
    if (f == nullptr) return;
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
  }
};
}  // namespace

// The IO thread polls only every 10 s. Every request must still be served
// promptly: Post wakes the thread, the completion notify wakes it again.
// A lost wake-up shows as a ~10 s delay.
SND_TEST(IoAdvStress, NoLostWakeupsWithASlowPollPeriod) {
  TempDir tmp;
  SND_REQUIRE(!tmp.root.empty());
  tmp.Write("w.bin", MakePattern(5, 64u * kC));
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  snd::io::DefaultFileSystem fs;
  snd::io::DefaultFileSystemConfig fcfg;
  fcfg.root = tmp.root.c_str();
  fcfg.readerThreads = 2;
  SND_REQUIRE(fs.Init(alloc, fcfg));
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 16u * kC, kC, 2));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig cfg;
  cfg.maxStreams = 2;
  cfg.pollIntervalNs = 10000000000ull;
  SND_REQUIRE(io.Init(alloc, fs, pool, cfg));
  fs.SetCompletionNotify(&snd::io::IoScheduler::WakeThunk, &io);
  SND_REQUIRE(io.Start());
  std::uint64_t worst = 0;
  std::uint32_t late = 0, done = 0;
  std::atomic<bool> stopWaker{false};
  std::thread waker([&] {  // spurious wakes and cancels on slot 1 interleaved
    while (!stopWaker.load()) {
      io.Wake();
      io.Channel(1).Cancel();
      snd::platform::YieldThread();
    }
  });
  for (std::uint32_t i = 0; i < 3000; ++i) {
    const std::uint64_t t0 = NowNs();
    SND_REQUIRE(io.Post(StreamReq(0, "w.bin", (i % 64u) * kC, kC, IoClass::Music, i)));
    ChunkRecord rec{};
    bool got = false;
    while (!got && NowNs() - t0 < 3000000000ull) {
      got = io.Channel(0).Pop(rec);
      if (!got) snd::platform::YieldThread();
    }
    const std::uint64_t dt = NowNs() - t0;
    if (dt > worst) worst = dt;
    if (!got) {
      ++late;
      break;
    }
    if (rec.tag != i || rec.status != ChunkStatus::Ok || !MatchesPattern(5, rec.offset, pool.Data(rec.chunk), rec.bytes)) ++late;
    io.Channel(0).Release(rec);
    ++done;
  }
  stopWaker.store(true);
  waker.join();
  snd::test::Note("%u requests, worst latency %.3f ms", done, static_cast<double>(worst) / 1e6);
  SND_CHECK_EQ(late, 0u);
  SND_CHECK_EQ(done, 3000u);
  io.Shutdown();
  io.Destroy();
  SND_CHECK_EQ(pool.Used(), 0u);
  pool.Destroy();
  fs.Shutdown();
  SND_CHECK_EQ(alloc.TotalUsed(), 0u);
}

// Shutdown racing with Post and Submit. Afterwards no accepted request may be
// left inside the scheduler: accepted FileRequests are finished, accepted
// stream requests are in the ring (or dropped).
// DEFECT (fails intermittently, ~1-3 % of iterations per run): Post / Submit
// read `accepting` (io_scheduler.cpp 926 / 971) and push to the queue later
// (945 / 987). If Shutdown stores accepting = 0 and the IO thread runs
// FinalDrain in between, the request is accepted (returns true) but stays in
// the queue until Destroy: Outstanding() never returns to 0, Reclaim() stays
// false, and an accepted FileRequest stays Queued (a loader waiting for
// Finished() between Shutdown and Destroy hangs).
SND_TEST(IoDefect, ShutdownRacingWithPostAndSubmitStrandsNothing) {
  std::uint32_t strandedFiles = 0, strandedStreams = 0, acceptedAfter = 0;
  for (int iter = 0; iter < 400; ++iter) {
    snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
    ScriptFs fs;
    fs.AddFile("s.bin", MakePattern(1, 64u * kC));
    fs.SetRandomLatency(2, static_cast<std::uint32_t>(iter) + 1u);
    snd::io::StreamPool pool;
    SND_REQUIRE(pool.Init(alloc, 32u * kC, kC, 2));
    snd::io::IoScheduler io;
    snd::io::IoSchedulerConfig cfg;
    cfg.maxStreams = 2;
    cfg.ringChunks = 8;
    cfg.pollIntervalNs = 20000;
    SND_REQUIRE(io.Init(alloc, fs, pool, cfg));
    SND_REQUIRE(io.Start());
    std::atomic<bool> shutDone{false};
    std::atomic<bool> stop{false};
    FileRequest reqs[4];
    bool accepted[4] = {};
    std::vector<std::uint8_t> buf(4u * 1000u);
    std::thread poster([&] {
      std::uint32_t tag = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const bool after = shutDone.load(std::memory_order_acquire);
        if (io.Post(StreamReq(0, "s.bin", (tag % 64u) * kC, kC, IoClass::Music, tag)) && after) acceptedAfter++;
        ++tag;
        for (int k = 0; k < 4; ++k) {
          if (reqs[k].State() == FileState::Queued) continue;
          reqs[k].path = "s.bin";
          reqs[k].bytes = 1000;
          reqs[k].dst = buf.data() + k * 1000;
          const bool afterF = shutDone.load(std::memory_order_acquire);
          if (io.Submit(reqs[k])) {
            accepted[k] = true;
            if (afterF) acceptedAfter++;
          }
        }
        ChunkRecord r{};
        while (io.Channel(0).Pop(r)) io.Channel(0).Release(r);
      }
    });
    snd::platform::SleepFor(static_cast<std::uint64_t>(iter % 7) * 300000u);
    io.Shutdown();
    shutDone.store(true, std::memory_order_release);
    snd::platform::SleepFor(1000000);
    stop.store(true);
    poster.join();
    for (int k = 0; k < 4; ++k) {
      if (accepted[k] && !reqs[k].Finished()) ++strandedFiles;
    }
    if (io.Channel(0).Outstanding() != io.Channel(0).Ready()) ++strandedStreams;
    SND_CHECK_EQ(fs.InFlight(), 0u);
    io.Destroy();
    for (int k = 0; k < 4; ++k) {
      if (accepted[k] && !reqs[k].Finished()) SND_CHECK_MSG(false, "FileRequest still Queued after Destroy");
    }
    SND_CHECK_EQ(pool.Used(), 0u);
    pool.Destroy();
    SND_CHECK_EQ(alloc.TotalUsed(), 0u);
  }
  snd::test::Note("stranded after Shutdown: %u file requests, %u stream channels; accepted after Shutdown returned: %u",
                  strandedFiles, strandedStreams, acceptedAfter);
  SND_CHECK_EQ(acceptedAfter, 0u);
  SND_CHECK_EQ(strandedFiles, 0u);
  SND_CHECK_EQ(strandedStreams, 0u);
}

namespace {
struct CountingCb {
  std::atomic<std::uint32_t> calls{0};
  std::atomic<std::uint32_t> state{0};
  static void Fn(void* user, const FileResult& r) noexcept {
    auto* c = static_cast<CountingCb*>(user);
    c->state.store(static_cast<std::uint32_t>(r.state));
    c->calls.fetch_add(1);
  }
};
}  // namespace

SND_TEST(IoAdvStress, FileRequestCancelRacingWithCompletion) {
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  ScriptFs fs;
  fs.AddFile("f.bin", MakePattern(3, 20000));
  fs.SetRandomLatency(2, 5);
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 4u * kC, kC, 1));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig cfg;
  cfg.maxStreams = 1;
  cfg.filePieceBytes = 4096;
  cfg.pollIntervalNs = 10000;
  SND_REQUIRE(io.Init(alloc, fs, pool, cfg));
  SND_REQUIRE(io.Start());
  std::vector<std::uint8_t> dst(20000);
  std::uint32_t wrongCalls = 0, mismatch = 0, badData = 0;
  std::uint32_t outcomes[5] = {};
  std::uint32_t rng = 1;
  for (int i = 0; i < 800; ++i) {
    CountingCb cb;
    FileRequest r;
    r.path = "f.bin";
    r.bytes = 20000;
    r.dst = dst.data();
    r.onComplete = &CountingCb::Fn;
    r.user = &cb;
    SND_REQUIRE(io.Submit(r));
    rng = rng * 1664525u + 1013904223u;
    const std::uint64_t waitNs = ((rng >> 12) % 1500u) * 1000u;  // 0 .. 1.5 ms: both outcomes happen
    const std::uint64_t ts = NowNs();
    while (NowNs() - ts < waitNs) snd::platform::YieldThread();
    io.Cancel(r);
    if ((rng & 1u) != 0u) io.Cancel(r);  // twice
    const std::uint64_t t0 = NowNs();
    while ((!r.Finished() || cb.calls.load() == 0u) && NowNs() - t0 < 5000000000ull) snd::platform::YieldThread();
    for (int k = 0; k < 50; ++k) snd::platform::YieldThread();
    if (cb.calls.load() != 1u) ++wrongCalls;
    const FileResult res = r.Result();
    if (cb.state.load() != static_cast<std::uint32_t>(res.state)) ++mismatch;
    if (static_cast<std::uint32_t>(res.state) < 5u) ++outcomes[static_cast<std::uint32_t>(res.state)];
    if (res.state == FileState::Done && res.bytesRead != 20000u) ++badData;
    if (res.bytesRead <= 20000u && !MatchesPattern(3, 0, dst.data(), static_cast<std::uint32_t>(res.bytesRead))) ++badData;
  }
  snd::test::Note("done %u cancelled %u error %u", outcomes[2], outcomes[4], outcomes[3]);
  SND_CHECK_EQ(wrongCalls, 0u);
  SND_CHECK_EQ(mismatch, 0u);
  SND_CHECK_EQ(badData, 0u);
  io.Shutdown();
  SND_CHECK_EQ(fs.InFlight(), 0u);
  io.Destroy();
  pool.Destroy();
  SND_CHECK_EQ(alloc.TotalUsed(), 0u);
}

SND_TEST(IoAdvStress, StreamPoolNeverHandsOutAChunkTwice) {
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 12u * 256u, 256, 4));
  std::atomic<std::uint32_t> collisions{0}, books{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> ts;
  for (std::uint32_t t = 0; t < 4; ++t) {
    ts.emplace_back([&, t] {
      while (!go.load()) {
      }
      std::uint32_t held[3];
      for (int it = 0; it < 60000; ++it) {
        SND_RT_SCOPE();
        std::uint32_t n = 0;
        for (int k = 0; k < 3; ++k) {
          const std::uint32_t c = pool.Acquire(t);
          if (c == kInvalidChunk) break;
          // Owner word: the whole chunk carries (thread, iteration).
          auto* w = reinterpret_cast<std::uint32_t*>(pool.Data(c));
          const std::uint32_t tagv = (t << 24) | static_cast<std::uint32_t>(it & 0xFFFFFF);
          for (int j = 0; j < 64; ++j) w[j] = tagv;
          if (pool.OwnerOf(c) != t) books.fetch_add(1);
          held[n++] = c;
        }
        snd::platform::YieldThread();
        for (std::uint32_t k = 0; k < n; ++k) {
          const auto* w = reinterpret_cast<const std::uint32_t*>(pool.Data(held[k]));
          const std::uint32_t tagv = (t << 24) | static_cast<std::uint32_t>(it & 0xFFFFFF);
          for (int j = 0; j < 64; ++j) {
            if (w[j] != tagv) {
              collisions.fetch_add(1);
              break;
            }
          }
          pool.Release(held[k]);
        }
      }
    });
  }
  go.store(true);
  for (auto& th : ts) th.join();
  SND_CHECK_EQ(collisions.load(), 0u);
  SND_CHECK_EQ(books.load(), 0u);
  SND_CHECK_EQ(pool.Used(), 0u);
  for (std::uint32_t o = 0; o < 4; ++o) SND_CHECK_EQ(pool.HeldBy(o), 0u);
  SND_CHECK_EQ(pool.Stats().badReleases, 0u);
  SND_CHECK(pool.Stats().peak <= 12u);
}

SND_TEST(IoAdvStress, InitStartShutdownDestroyCyclesUnderLoad) {
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 16u * kC, kC, 2));
  std::uint32_t bad = 0;
  for (int cycle = 0; cycle < 60; ++cycle) {
    ScriptFs fs;
    fs.AddFile("c.bin", MakePattern(2, 32u * kC));
    fs.SetRandomLatency(2, static_cast<std::uint32_t>(cycle) + 3u);
    snd::io::IoScheduler io;
    snd::io::IoSchedulerConfig cfg;
    cfg.maxStreams = 2;
    cfg.pollIntervalNs = 20000;
    if (!io.Init(alloc, fs, pool, cfg)) {
      ++bad;
      continue;
    }
    if (cycle % 3 != 0) SND_CHECK(io.Start());
    std::thread poster([&] {
      for (std::uint32_t i = 0; i < 200; ++i) {
        (void)io.Post(StreamReq(i % 2u, "c.bin", (i % 32u) * kC, kC, IoClass::Music, i));
        ChunkRecord r{};
        if (io.Channel(i % 2u).Pop(r)) io.Channel(i % 2u).Release(r);
      }
    });
    poster.join();
    if (cycle % 3 == 0) {
      for (int k = 0; k < 20; ++k) io.RunOnce();
    }
    io.Shutdown();
    if (cycle % 2 == 0) io.Destroy();  // else the destructor does it
    if (fs.InFlight() != 0u) ++bad;
  }
  SND_CHECK_EQ(bad, 0u);
  SND_CHECK_EQ(pool.Used(), 0u);
  pool.Destroy();
  SND_CHECK_EQ(alloc.TotalUsed(), 0u);
}
