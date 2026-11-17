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
