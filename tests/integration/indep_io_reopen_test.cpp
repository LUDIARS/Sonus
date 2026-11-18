// Independent re-check of the io side of the fourth round (30813e1):
// StreamChannel::ReopenFile and IoScheduler::CloseIdleFiles.
//   - the reopen request survives a Post that the request queue refuses (the
//     Stream source retries the read next block);
//   - CloseIdleFiles against reads in flight, with a real IO thread and the
//     default file system (run under TSan): every read gets its file's bytes.
// Defect_source_* tests fail on purpose.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "snd/base/allocator.hpp"
#include "snd/io/default_file_system.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"

namespace sfs = std::filesystem;

namespace {
// One 4 KiB stream read at offset 0 on channel `ch` (RunOnce driven); first byte, '?' if refused.
char ReadOnce(snd::io::IoScheduler& io, snd::io::StreamPool& pool, std::uint32_t ch, const char* path, std::uint32_t tag,
              bool* posted) {
  snd::source::IoReadRequest rq{};
  rq.streamSlot = ch;
  rq.chunkIndex = tag;
  rq.path = path;
  rq.bytes = 4096;
  *posted = io.Post(rq);
  if (!*posted) return '?';
  snd::io::ChunkRecord rec{};
  for (int i = 0; i < 1000; ++i) {
    io.RunOnce();
    if (io.Channel(ch).Pop(rec)) {
      const char c = rec.status == snd::io::ChunkStatus::Ok ? static_cast<char>(pool.Data(rec.chunk)[0]) : '!';
      io.Channel(ch).Release(rec);
      return c;
    }
  }
  return '?';
}
}  // namespace

// A new playback asks for its file to be opened again (ReopenFile) and posts
// its first read; the request queue is full, the Post is refused, the source
// posts again next block. The read must use the file as it is now. Today the
// refused Post has already taken (and lost) the reopen request.
SND_TEST(IndepIoReopen, ReopenKeptWhenThePostIsRefused) {
  snd::IAllocator& al = snd::base::DefaultAllocator();
  snd::mock::MemoryFileSystem fs;
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(al, 64u << 10, 4096, 2));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig ic;
  ic.maxStreams = 2;
  ic.bytesPerWindow = 0;
  ic.requestQueueCapacity = 2;
  SND_REQUIRE(io.Init(al, fs, pool, ic));
  fs.AddFile("s.snds", std::vector<std::uint8_t>(8192, 'A'));
  bool posted = false;
  SND_CHECK_EQ(ReadOnce(io, pool, 0, "s.snds", 1, &posted), 'A');  // the file is open (idle afterwards)
  fs.AddFile("s.snds", std::vector<std::uint8_t>(8192, 'B'));        // rebuilt
  // Fill the request queue with reads of channel 1 (another file), not drained yet.
  fs.AddFile("o.snds", std::vector<std::uint8_t>(8192, 'O'));
  std::uint32_t filled = 0;
  for (std::uint32_t i = 0; i < 8; ++i) {
    snd::source::IoReadRequest rq{};
    rq.streamSlot = 1;
    rq.chunkIndex = 100 + i;
    rq.path = "o.snds";
    rq.bytes = 4096;
    rq.offset = 0;
    if (io.Post(rq)) ++filled;
  }
  io.Channel(0).Cancel();
  io.Channel(0).ReopenFile();  // a new playback in slot 0
  snd::source::IoReadRequest rq{};
  rq.streamSlot = 0;
  rq.chunkIndex = 2;
  rq.path = "s.snds";
  rq.bytes = 4096;
  const bool refused = !io.Post(rq);
  std::printf("  queue filled with %u reads; the new playback's first Post %s\n", filled, refused ? "refused" : "accepted");
  for (int i = 0; i < 50; ++i) io.RunOnce();
  snd::io::ChunkRecord rec{};
  while (io.Channel(1).Pop(rec)) io.Channel(1).Release(rec);
  while (io.Channel(0).Pop(rec)) io.Channel(0).Release(rec);
  io.Channel(0).Cancel();
  const char c = ReadOnce(io, pool, 0, "s.snds", 3, &posted);  // posted again next block
  std::printf("  read after the retry: '%c' (rebuilt: 'B')\n", c);
  SND_CHECK(refused);
  SND_CHECK_MSG(c == 'B', "the retried read uses the old open file ('%c')", c);
  io.Shutdown();
  io.Destroy();
  pool.Destroy();
}

// CloseIdleFiles while reads are in flight (IO thread + default FS): another
// thread calls it in a loop while two channels read two files and bank-style
// FileRequests read a third; every read returns its file's bytes, none fails.
SND_TEST(IndepIoReopen, CloseIdleFilesAgainstReadsInFlight) {
  std::error_code ec;
  const sfs::path root = sfs::temp_directory_path(ec) / ("sonus-indep-closeidle-" + std::to_string(::getpid()));
  sfs::remove_all(root, ec);
  sfs::create_directories(root, ec);
  for (const char* n : {"a", "b", "k"}) {
    std::ofstream f(root / (std::string(n) + ".bin"), std::ios::binary);
    std::string s(64 << 10, n[0]);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
  }
  snd::IAllocator& al = snd::base::DefaultAllocator();
  snd::io::DefaultFileSystem fs;
  snd::io::DefaultFileSystemConfig fc;
  const std::string rootStr = root.string();
  fc.root = rootStr.c_str();
  SND_REQUIRE(fs.Init(al, fc));
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(al, 256u << 10, 4096, 2));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig ic;
  ic.maxStreams = 2;
  ic.bytesPerWindow = 0;
  SND_REQUIRE(io.Init(al, fs, pool, ic));
  fs.SetCompletionNotify(&snd::io::IoScheduler::WakeThunk, &io);
  SND_REQUIRE(io.Start());
  std::atomic<int> stop{0};
  std::thread closer([&] {
    while (stop.load() == 0) {
      io.CloseIdleFiles();
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });
  std::thread banks([&] {
    std::vector<char> buf(64 << 10);
    while (stop.load() == 0) {
      snd::io::FileRequest q;
      q.path = "k.bin";
      q.dst = buf.data();
      q.bytes = buf.size();
      if (!io.Submit(q)) continue;
      while (!q.Finished()) std::this_thread::sleep_for(std::chrono::microseconds(50));
      if (q.Result().state != snd::io::FileState::Done || buf[0] != 'k' || buf.back() != 'k') stop.store(2);
    }
  });
  std::uint32_t bad = 0, reads = 0, tag = 0;
  const char* paths[2] = {"a.bin", "b.bin"};
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
    for (std::uint32_t ch = 0; ch < 2; ++ch) {
      snd::source::IoReadRequest rq{};
      rq.streamSlot = ch;
      rq.chunkIndex = tag++;
      rq.path = paths[ch];
      rq.offset = (tag * 4096u) % (60u << 10);
      rq.bytes = 4096;
      if ((tag % 7u) == 0u) io.Channel(ch).ReopenFile();
      io.Post(rq);
      snd::io::ChunkRecord rec{};
      while (io.Channel(ch).Pop(rec)) {
        ++reads;
        if (rec.status != snd::io::ChunkStatus::Ok || pool.Data(rec.chunk)[0] != static_cast<std::uint8_t>(paths[ch][0])) ++bad;
        io.Channel(ch).Release(rec);
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(30));
  }
  if (stop.load() == 0) stop.store(1);
  closer.join();
  banks.join();
  for (std::uint32_t ch = 0; ch < 2; ++ch) {
    io.Channel(ch).Cancel();
    for (int i = 0; i < 1000 && !io.Channel(ch).Reclaim(); ++i) std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  std::printf("  %u stream reads, %u wrong or failed; bank reads %s\n", reads, bad, stop.load() == 2 ? "FAILED" : "ok");
  SND_CHECK(reads > 1000u);
  SND_CHECK_EQ(bad, 0u);
  SND_CHECK(stop.load() == 1);
  io.Shutdown();
  io.Destroy();
  pool.Destroy();
  fs.Shutdown();
  sfs::remove_all(root, ec);
}
