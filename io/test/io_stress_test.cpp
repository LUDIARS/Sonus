// Concurrency stress (run under -DSND_SANITIZE=thread): IO thread + an
// "audio" thread posting and seeking + a "decode" thread consuming + a bank
// loader thread, over the memory FS with latency and failure injection.
// At the end every chunk must be back in the pool.
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "io_test_util.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_read_planner.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"

using namespace snd::iotest;

namespace {
constexpr std::uint32_t kStreams = 4;
constexpr std::uint32_t kStressFile = 200000;
constexpr std::uint32_t kStressChunk = 4096;
const char* const kPaths[kStreams] = {"s0.bin", "s1.bin", "s2.bin", "s3.bin"};
}  // namespace

SND_TEST(IoStress, ConcurrentPostSeekConsumeAndBankLoads) {
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  snd::mock::MemoryFileSystem fs;
  for (std::uint32_t s = 0; s < kStreams; ++s) fs.AddFile(kPaths[s], MakePattern(s, kStressFile));
  fs.AddFile("bank.bin", MakePattern(9, 50000));
  fs.SetDefaultLatency(1);
  fs.SetMaxInFlight(6);
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 24u * kStressChunk, kStressChunk, kStreams));  // fewer chunks than demand
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig cfg;
  cfg.maxStreams = kStreams;
  cfg.ringChunks = 8;
  cfg.maxInFlight = 8;
  cfg.bytesPerWindow = 0;
  cfg.filePieceBytes = 8192;
  cfg.pollIntervalNs = 100000;
  SND_REQUIRE(io.Init(alloc, fs, pool, cfg));
  SND_REQUIRE(io.Start());

  std::atomic<bool> stopAudio{false};
  std::atomic<bool> stopDecode{false};
  std::atomic<std::uint32_t> badData{0};
  std::atomic<std::uint32_t> badOrder{0};
  std::atomic<std::uint64_t> consumed{0};
  std::atomic<std::uint32_t> banksDone{0};
  std::atomic<std::uint32_t> bankBad{0};

  std::thread audio([&] {
    snd::io::StreamReadPlanner planners[kStreams];
    for (std::uint32_t s = 0; s < kStreams; ++s) planners[s].Reset(0, kStressFile, 0, 0, kStressChunk);
    std::uint32_t rng = 12345u;
    while (!stopAudio.load(std::memory_order_relaxed)) {
      SND_RT_SCOPE();
      for (std::uint32_t s = 0; s < kStreams; ++s) {
        rng = rng * 1664525u + 1013904223u;
        snd::io::StreamChannel& ch = io.Channel(s);
        if ((rng >> 24) == 0u) {  // seek (about 1 in 256)
          ch.Cancel();
          planners[s].Seek(((rng >> 8) % (kStressFile / kStressChunk)) * kStressChunk);
        }
        for (int k = 0; k < 3 && ch.Outstanding() < ch.Capacity(); ++k) {
          snd::io::StreamRead r{};
          if (!planners[s].Next(r)) {
            ch.Cancel();
            planners[s].Seek(0);
            break;
          }
          const auto cls = static_cast<snd::io::IoClass>(s % 3u);
          if (!io.Post(StreamReq(s, kPaths[s], r.offset, r.bytes, cls, 0))) {
            planners[s].Seek(r.offset);  // retry next time
            break;
          }
        }
      }
      snd::platform::YieldThread();
    }
  });

  std::thread decode([&] {
    std::uint32_t lastEpoch[kStreams] = {};
    std::uint64_t nextOffset[kStreams] = {};
    bool have[kStreams] = {};
    while (!stopDecode.load(std::memory_order_relaxed)) {
      SND_RT_SCOPE();
      for (std::uint32_t s = 0; s < kStreams; ++s) {
        snd::io::ChunkRecord rec;
        while (io.Channel(s).Pop(rec)) {
          if (rec.status == snd::io::ChunkStatus::Ok &&
              !MatchesPattern(s, rec.offset, pool.Data(rec.chunk), rec.bytes)) {
            badData.fetch_add(1);
          }
          if (have[s] && rec.epoch == lastEpoch[s] && rec.offset != nextOffset[s]) badOrder.fetch_add(1);
          have[s] = true;
          lastEpoch[s] = rec.epoch;
          nextOffset[s] = rec.offset + rec.requested;
          io.Channel(s).Release(rec);
          consumed.fetch_add(1);
        }
      }
      snd::platform::YieldThread();
    }
  });

  std::thread loader([&] {
    std::vector<std::uint8_t> dst(50000);
    for (int i = 0; i < 20; ++i) {
      snd::io::FileRequest req;
      req.path = "bank.bin";
      req.offset = static_cast<std::uint64_t>(i) * 1000u;
      req.bytes = 30000;
      req.dst = dst.data();
      while (!io.Submit(req)) snd::platform::SleepFor(100000);
      if (i % 5 == 4) io.Cancel(req);
      while (!req.Finished()) snd::platform::SleepFor(100000);
      if (req.State() == snd::io::FileState::Done) {
        if (req.Result().bytesRead != 30000u || !MatchesPattern(9, req.offset, dst.data(), 30000)) bankBad.fetch_add(1);
        banksDone.fetch_add(1);
      }
    }
  });

  // Inject read failures while running.
  for (int i = 0; i < 20; ++i) {
    snd::platform::SleepFor(20u * 1000000u);
    fs.FailNthRead(3);
  }
  loader.join();
  stopAudio.store(true);
  audio.join();
  for (std::uint32_t s = 0; s < kStreams; ++s) io.Channel(s).Cancel();
  snd::platform::SleepFor(20u * 1000000u);
  stopDecode.store(true);
  decode.join();
  io.Shutdown();
  for (std::uint32_t s = 0; s < kStreams; ++s) SND_CHECK(io.Channel(s).Reclaim());

  const snd::io::IoStats st = io.Stats();
  snd::test::Note("consumed %llu records, posted %llu, stale dropped %llu / completed %llu, errors %llu, banks %u",
                  static_cast<unsigned long long>(consumed.load()), static_cast<unsigned long long>(st.streamPosted),
                  static_cast<unsigned long long>(st.staleDropped), static_cast<unsigned long long>(st.staleCompleted),
                  static_cast<unsigned long long>(st.readErrors), banksDone.load());
  SND_CHECK(consumed.load() > 100u);
  SND_CHECK_EQ(badData.load(), 0u);
  SND_CHECK_EQ(badOrder.load(), 0u);
  SND_CHECK_EQ(bankBad.load(), 0u);
  SND_CHECK(banksDone.load() >= 1u);
  SND_CHECK_EQ(fs.InFlight(), 0u);
  SND_CHECK_EQ(fs.OpenFiles(), 0u);
  io.Destroy();
  SND_CHECK_EQ(pool.Used(), 0u);
  SND_CHECK_EQ(pool.Stats().badReleases, 0u);
  pool.Destroy();
  SND_CHECK_EQ(alloc.TotalUsed(), 0u);
}
