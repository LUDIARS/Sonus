// Reading a file again after it changed (bank reload under the same path): the
// IO scheduler reopens the file for every FileRequest and asks for its size
// again, so a replaced or rewritten file is read with its new content and size.
// Memory FS (snapshot at Open) and the default OS file system.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "io_test_util.hpp"
#include "snd/base/allocator.hpp"
#include "snd/io/default_file_system.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"

namespace {
using snd::io::FileRequest;
using snd::io::FileState;

std::vector<std::uint8_t> Bytes(std::uint8_t seed, std::size_t n) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(seed + i * 3u);
  return v;
}

// Whole-file read through the scheduler; `pump` drives it (RunOnce or a running IO thread).
template <class Pump>
bool ReadWhole(snd::io::IoScheduler& io, const char* path, std::vector<std::uint8_t>* out, std::uint64_t* size, Pump pump) {
  std::vector<std::uint8_t> buf(1u << 16, 0);
  FileRequest req;
  req.path = path;
  req.bytes = buf.size();
  req.dst = buf.data();
  if (!io.Submit(req)) return false;
  for (int i = 0; i < 10000 && !req.Finished(); ++i) pump();
  if (req.State() != FileState::Done) return false;
  *size = req.Result().fileSize;
  out->assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(req.Result().bytesRead));
  return true;
}
}  // namespace

SND_TEST(IoReload, MemoryFsReplacedFileIsReadAgain) {
  snd::IAllocator& alloc = snd::base::DefaultAllocator();
  snd::mock::MemoryFileSystem fs;
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 64u << 10, 16u << 10, 2));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig cfg;
  cfg.maxStreams = 2;
  cfg.bytesPerWindow = 0;
  SND_REQUIRE(io.Init(alloc, fs, pool, cfg));
  auto pump = [&] { io.RunOnce(); };
  std::vector<std::uint8_t> got;
  std::uint64_t size = 0;
  for (std::size_t n : {std::size_t{5000}, std::size_t{9000}, std::size_t{1200}}) {
    const std::vector<std::uint8_t> want = Bytes(static_cast<std::uint8_t>(n), n);
    fs.AddFile("bank.sndb", want);  // replaced (the memory FS serves what was there at Open)
    SND_REQUIRE(ReadWhole(io, "bank.sndb", &got, &size, pump));
    SND_CHECK_EQ(size, n);
    SND_CHECK(got == want);
  }
  SND_CHECK_EQ(fs.OpenFiles(), 0u);  // closed once the request is done
  io.Shutdown();
  io.Destroy();
  pool.Destroy();
}

SND_TEST(IoReload, DefaultFsReplacedAndRewrittenFileIsReadAgain) {
  namespace fsys = std::filesystem;
  std::error_code ec;
  const fsys::path root = fsys::temp_directory_path(ec) / ("sonus-io-reload-" + std::to_string(::getpid()));
  fsys::remove_all(root, ec);
  fsys::create_directories(root, ec);
  auto write = [&](const char* name, const std::vector<std::uint8_t>& b) {
    std::ofstream f(root / name, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  };
  snd::IAllocator& alloc = snd::base::DefaultAllocator();
  snd::io::DefaultFileSystem fs;
  snd::io::DefaultFileSystemConfig fc;
  const std::string rootStr = root.string();
  fc.root = rootStr.c_str();
  SND_REQUIRE(fs.Init(alloc, fc));
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 64u << 10, 16u << 10, 2));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig cfg;
  cfg.maxStreams = 2;
  cfg.bytesPerWindow = 0;
  SND_REQUIRE(io.Init(alloc, fs, pool, cfg));
  fs.SetCompletionNotify(&snd::io::IoScheduler::WakeThunk, &io);
  SND_REQUIRE(io.Start());
  auto pump = [&] { snd::platform::SleepFor(200000); };
  std::vector<std::uint8_t> got;
  std::uint64_t size = 0;
  // 1. First version.
  std::vector<std::uint8_t> want = Bytes(1, 6000);
  write("bank.sndb", want);
  SND_REQUIRE(ReadWhole(io, "bank.sndb", &got, &size, pump));
  SND_CHECK(got == want && size == 6000u);
  // 2. Replaced by rename (a new inode).
  want = Bytes(2, 8000);
  write("bank.tmp", want);
  fsys::rename(root / "bank.tmp", root / "bank.sndb", ec);
  SND_REQUIRE(ReadWhole(io, "bank.sndb", &got, &size, pump));
  SND_CHECK(got == want && size == 8000u);
  // 3. Deleted and created again.
  fsys::remove(root / "bank.sndb", ec);
  want = Bytes(3, 3000);
  write("bank.sndb", want);
  SND_REQUIRE(ReadWhole(io, "bank.sndb", &got, &size, pump));
  SND_CHECK(got == want && size == 3000u);
  // 4. Rewritten in place, larger, then smaller (same inode).
  for (std::size_t n : {std::size_t{12000}, std::size_t{700}}) {
    want = Bytes(static_cast<std::uint8_t>(n), n);
    {
      std::fstream f(root / "bank.sndb", std::ios::binary | std::ios::in | std::ios::out);
      f.write(reinterpret_cast<const char*>(want.data()), static_cast<std::streamsize>(want.size()));
    }
    fsys::resize_file(root / "bank.sndb", n, ec);
    SND_REQUIRE(ReadWhole(io, "bank.sndb", &got, &size, pump));
    SND_CHECK_MSG(got == want && size == n, "in place %zu: size %llu", n, static_cast<unsigned long long>(size));
  }
  SND_CHECK_EQ(fs.OpenFileCount(), 0u);
  io.Shutdown();
  io.Destroy();
  pool.Destroy();
  fs.Shutdown();
  fsys::remove_all(root, ec);
}

namespace {
// One stream read of 4 KiB at offset 0 on channel `ch` (RunOnce driven): first
// byte of the data, '!' on a failed read, '?' when refused or not delivered.
char StreamRead(snd::io::IoScheduler& io, snd::io::StreamPool& pool, std::uint32_t ch, const char* path, std::uint32_t tag) {
  snd::source::IoReadRequest rq{};
  rq.streamSlot = ch;
  rq.chunkIndex = tag;
  rq.path = path;
  rq.bytes = 4096;
  if (!io.Post(rq)) return '?';
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

// Playback binding (StreamChannel::ReopenFile / CloseFile): a playback opens
// its file anew once and reads that open file to its end (Cancel / seek reads
// included), even when the file is rebuilt and another playback opens the new
// one; the reopen is not lost when the first request is dropped as stale or
// refused; the files are closed when the playbacks end.
SND_TEST(IoReload, PlaybackKeepsItsFileAndANewPlaybackOpensAnew) {
  snd::IAllocator& alloc = snd::base::DefaultAllocator();
  snd::mock::MemoryFileSystem fs;
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 64u << 10, 4096, 2));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig cfg;
  cfg.maxStreams = 2;
  cfg.bytesPerWindow = 0;
  cfg.requestQueueCapacity = 2;
  SND_REQUIRE(io.Init(alloc, fs, pool, cfg));
  fs.AddFile("s.snds", std::vector<std::uint8_t>(8192, 'A'));
  io.Channel(0).ReopenFile();  // playback 1 in slot 0
  SND_CHECK_EQ(StreamRead(io, pool, 0, "s.snds", 1), 'A');
  const std::uint32_t opens = fs.OpenCalls();
  fs.AddFile("s.snds", std::vector<std::uint8_t>(8192, 'B'));  // rebuilt while playback 1 plays
  io.Channel(0).Cancel();                                         // a seek of playback 1
  SND_CHECK_EQ(StreamRead(io, pool, 0, "s.snds", 2), 'A');
  SND_CHECK_EQ(fs.OpenCalls(), opens);
  io.Channel(1).ReopenFile();  // playback 2 in slot 1: the rebuilt file
  SND_CHECK_EQ(StreamRead(io, pool, 1, "s.snds", 3), 'B');
  SND_CHECK_EQ(StreamRead(io, pool, 0, "s.snds", 4), 'A');  // playback 1 still reads its file
  SND_CHECK_EQ(fs.OpenFiles(), 2u);
  io.Channel(0).CloseFile();  // playback 1 ends: its file is closed
  io.RunOnce();
  SND_CHECK_EQ(fs.OpenFiles(), 1u);
  fs.AddFile("s.snds", std::vector<std::uint8_t>(8192, 'C'));
  // Playback 3 in slot 0: its first request is made stale (start + seek in one
  // block) before the IO thread sees it; the next request still opens anew.
  io.Channel(0).ReopenFile();
  snd::source::IoReadRequest rq{};
  rq.streamSlot = 0;
  rq.chunkIndex = 5;
  rq.path = "s.snds";
  rq.bytes = 4096;
  SND_CHECK(io.Post(rq));
  io.Channel(0).Cancel();
  SND_CHECK_EQ(StreamRead(io, pool, 0, "s.snds", 6), 'C');
  SND_CHECK_EQ(StreamRead(io, pool, 1, "s.snds", 7), 'B');  // playback 2 unchanged
  // Playback 4 in slot 1: its first Post is refused (queue full); the retry opens anew.
  fs.AddFile("s.snds", std::vector<std::uint8_t>(8192, 'D'));
  io.Channel(1).CloseFile();
  io.Channel(1).ReopenFile();
  std::uint32_t queued = 0;
  for (std::uint32_t i = 0; i < 4; ++i) {
    rq.streamSlot = 0;
    rq.chunkIndex = 10 + i;
    if (io.Post(rq)) ++queued;
  }
  rq.streamSlot = 1;
  rq.chunkIndex = 20;
  SND_CHECK(!io.Post(rq));
  for (int i = 0; i < 50; ++i) io.RunOnce();
  snd::io::ChunkRecord rec{};
  std::uint32_t popped = 0;
  while (io.Channel(0).Pop(rec)) {
    SND_CHECK_EQ(static_cast<char>(pool.Data(rec.chunk)[0]), 'C');
    io.Channel(0).Release(rec);
    ++popped;
  }
  SND_CHECK_EQ(popped, queued);
  SND_CHECK_EQ(StreamRead(io, pool, 1, "s.snds", 21), 'D');
  io.Channel(0).CloseFile();
  io.Channel(1).CloseFile();
  io.RunOnce();
  SND_CHECK_EQ(fs.OpenFiles(), 0u);
  io.Shutdown();
  io.Destroy();
  pool.Destroy();
}
