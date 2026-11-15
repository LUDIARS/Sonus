// DefaultFileSystem on real files in a temporary directory (D-24, 6.5), and the
// scheduler running on its own IO thread on top of it.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "io_test_util.hpp"
#include "snd/io/default_file_system.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_read_planner.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::iotest;
using snd::FileId;
using snd::IoState;
using snd::IoStatus;
using snd::IoToken;
using snd::kInvalidFile;
using snd::io::DefaultFileSystem;
using snd::io::DefaultFileSystemConfig;

namespace {

// Temporary root with: a.bin (seed 1, 100000 bytes), sub/b.bin (seed 2, 5000 bytes), sub/ (directory).
struct TempRoot {
  std::string root;
  std::vector<std::string> files;
  TempRoot() {
    char tmpl[] = "/tmp/snd-io-test-XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (d != nullptr) root = d;
    if (root.empty()) return;
    mkdir((root + "/sub").c_str(), 0700);
    Write("a.bin", MakePattern(1, 100000));
    Write("sub/b.bin", MakePattern(2, 5000));
    // A file outside the root, reachable only through "..".
    Write("../snd-io-outside-" + Base(), MakePattern(3, 10));
  }
  ~TempRoot() {
    for (const std::string& f : files) std::remove(f.c_str());
    rmdir((root + "/sub").c_str());
    rmdir(root.c_str());
  }
  std::string Base() const { return root.substr(root.find_last_of('/') + 1); }
  void Write(const std::string& rel, const std::vector<std::uint8_t>& data) {
    const std::string path = root + "/" + rel;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    files.push_back(path);
  }
};

IoStatus WaitDone(DefaultFileSystem& fs, std::uint64_t token) {
  for (int i = 0; i < 20000; ++i) {
    const IoStatus s = fs.Poll(IoToken{token});
    if (snd::StateOf(s) != IoState::Pending) return s;
    snd::platform::SleepFor(100000);
  }
  return IoStatus{SND_IO_PENDING, 0};
}

}  // namespace

SND_TEST(DefaultFileSystem, ValidatesPaths) {
  SND_CHECK(DefaultFileSystem::ValidatePath("a.bin"));
  SND_CHECK(DefaultFileSystem::ValidatePath("sub/b.bin"));
  SND_CHECK(DefaultFileSystem::ValidatePath("sub/../a.bin"));
  SND_CHECK(DefaultFileSystem::ValidatePath("./a.bin"));
  SND_CHECK(DefaultFileSystem::ValidatePath("sub//b.bin"));
  SND_CHECK(!DefaultFileSystem::ValidatePath(nullptr));
  SND_CHECK(!DefaultFileSystem::ValidatePath(""));
  SND_CHECK(!DefaultFileSystem::ValidatePath("/etc/passwd"));
  SND_CHECK(!DefaultFileSystem::ValidatePath(".."));
  SND_CHECK(!DefaultFileSystem::ValidatePath("../a.bin"));
  SND_CHECK(!DefaultFileSystem::ValidatePath("sub/../../a.bin"));
  SND_CHECK(!DefaultFileSystem::ValidatePath("sub/./../.."));
  SND_CHECK(!DefaultFileSystem::ValidatePath("sub\\..\\..\\a.bin"));
  std::string longPath(5000, 'x');
  SND_CHECK(!DefaultFileSystem::ValidatePath(longPath.c_str()));
}

SND_TEST(DefaultFileSystem, OpensSizesAndReadsAsynchronously) {
  TempRoot tmp;
  SND_REQUIRE(!tmp.root.empty());
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  cfg.root = tmp.root.c_str();
  cfg.maxInFlight = 2;
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  const FileId a = fs.Open("a.bin");
  const FileId b = fs.Open("sub/../sub/b.bin");
  SND_REQUIRE(a != kInvalidFile);
  SND_REQUIRE(b != kInvalidFile);
  SND_CHECK_EQ(fs.OpenFileCount(), 2u);
  SND_CHECK_EQ(fs.Size(a), 100000u);
  SND_CHECK_EQ(fs.Size(b), 5000u);
  SND_CHECK_EQ(fs.Size(12345u), UINT64_MAX);
  // Rejected: missing file, directory, escaping the root, absolute.
  SND_CHECK_EQ(fs.Open("missing.bin"), kInvalidFile);
  SND_CHECK_EQ(fs.Open("sub"), kInvalidFile);
  SND_CHECK_EQ(fs.Open(("../snd-io-outside-" + tmp.Base()).c_str()), kInvalidFile);
  SND_CHECK_EQ(fs.Open((tmp.root + "/a.bin").c_str()), kInvalidFile);

  std::vector<std::uint8_t> buf1(4096), buf2(4096), buf3(16);
  SND_REQUIRE(fs.ReadAsync(a, 50000, buf1.data(), 4096, IoToken{1}));
  SND_CHECK(!fs.ReadAsync(a, 0, buf2.data(), 4096, IoToken{1}));  // token in flight
  SND_REQUIRE(fs.ReadAsync(b, 4990, buf2.data(), 4096, IoToken{2}));  // past EOF: short
  SND_CHECK(!fs.ReadAsync(a, 0, buf3.data(), 16, IoToken{3}));      // maxInFlight reached
  SND_CHECK(!fs.ReadAsync(9999u, 0, buf3.data(), 16, IoToken{4}));  // invalid id
  SND_CHECK(!fs.ReadAsync(a, 0, nullptr, 16, IoToken{5}));          // no destination
  const IoStatus s1 = WaitDone(fs, 1);
  SND_CHECK(snd::StateOf(s1) == IoState::Done);
  SND_CHECK_EQ(s1.bytes, 4096u);
  SND_CHECK(MatchesPattern(1, 50000, buf1.data(), 4096));
  SND_CHECK(snd::StateOf(fs.Poll(IoToken{1})) == IoState::UnknownToken);  // reported once
  const IoStatus s2 = WaitDone(fs, 2);
  SND_CHECK(snd::StateOf(s2) == IoState::Done);
  SND_CHECK_EQ(s2.bytes, 10u);
  SND_CHECK(MatchesPattern(2, 4990, buf2.data(), 10));
  // Beyond the end: Done with 0 bytes.
  SND_REQUIRE(fs.ReadAsync(b, 100000, buf3.data(), 16, IoToken{6}));
  const IoStatus s3 = WaitDone(fs, 6);
  SND_CHECK(snd::StateOf(s3) == IoState::Done);
  SND_CHECK_EQ(s3.bytes, 0u);
  SND_CHECK(snd::StateOf(fs.Poll(IoToken{77})) == IoState::UnknownToken);
  // Close with a read in flight is deferred until the read is retired.
  SND_REQUIRE(fs.ReadAsync(a, 0, buf1.data(), 4096, IoToken{8}));
  fs.Close(a);
  SND_CHECK_EQ(fs.Size(a), UINT64_MAX);  // the id is gone immediately
  const IoStatus s4 = WaitDone(fs, 8);
  SND_CHECK(snd::StateOf(s4) == IoState::Done);
  SND_CHECK(MatchesPattern(1, 0, buf1.data(), 4096));
  SND_CHECK_EQ(fs.OpenFileCount(), 1u);
  fs.Close(b);
  SND_CHECK_EQ(fs.OpenFileCount(), 0u);
  SND_CHECK(!fs.ReadAsync(b, 0, buf3.data(), 16, IoToken{9}));  // stale id
  // Ids are not reused: reopening gives a different id.
  const FileId a2 = fs.Open("a.bin");
  SND_CHECK(a2 != kInvalidFile && a2 != a);
  fs.Shutdown();
  SND_CHECK_EQ(fs.OpenFileCount(), 0u);
}

SND_TEST(DefaultFileSystem, NotesFileIoInsideRtScope) {
  TempRoot tmp;
  SND_REQUIRE(!tmp.root.empty());
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  cfg.root = tmp.root.c_str();
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  snd::test::rt::Reset();
  {
    snd::test::rt::ExpectViolations expect;
    SND_RT_SCOPE();
    const FileId a = fs.Open("a.bin");
    fs.Size(a);
    fs.Poll(IoToken{1});
    fs.Close(a);
  }
  SND_CHECK(snd::test::rt::Count(snd::rt::Violation::FileIo) >= 4u);
}

SND_TEST(DefaultFileSystem, DrivesTheSchedulerOnItsThread) {
  TempRoot tmp;
  SND_REQUIRE(!tmp.root.empty());
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  DefaultFileSystem fs;
  DefaultFileSystemConfig fcfg;
  fcfg.root = tmp.root.c_str();
  fcfg.readerThreads = 2;
  SND_REQUIRE(fs.Init(alloc, fcfg));
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 32u * 4096u, 4096u, 4));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig cfg;
  cfg.maxStreams = 4;
  cfg.ringChunks = 8;
  cfg.pollIntervalNs = 50u * 1000000u;  // slow polling: completion must come from the notify hook
  SND_REQUIRE(io.Init(alloc, fs, pool, cfg));
  fs.SetCompletionNotify(&snd::io::IoScheduler::WakeThunk, &io);
  SND_REQUIRE(io.Start());
  SND_CHECK(io.Running());

  snd::io::StreamReadPlanner planner;
  planner.Reset(0, 100000, 20000, 60000, 4096);
  std::uint32_t posted = 0, received = 0;
  bool ok = true;
  std::vector<std::uint8_t> bank(5000);
  snd::io::FileRequest req;
  req.path = "sub/b.bin";
  req.bytes = 5000;
  req.dst = bank.data();
  SND_REQUIRE(io.Submit(req));
  const std::uint64_t deadline = snd::platform::MonotonicNanoseconds() + 10ull * 1000000000ull;
  std::uint64_t expectOffset = 0;
  while (received < 40u && snd::platform::MonotonicNanoseconds() < deadline) {
    while (io.Channel(1).Outstanding() < 6u) {
      snd::io::StreamRead r{};
      SND_REQUIRE(planner.Next(r));
      SND_REQUIRE(io.Post(StreamReq(1, "a.bin", r.offset, r.bytes, snd::io::IoClass::Music, posted++)));
    }
    snd::io::ChunkRecord rec;
    while (io.Channel(1).Pop(rec)) {
      if (rec.status != snd::io::ChunkStatus::Ok || rec.offset != expectOffset ||
          !MatchesPattern(1, rec.offset, pool.Data(rec.chunk), rec.bytes)) {
        ok = false;
      }
      expectOffset = rec.offset + rec.bytes;
      if (expectOffset == 60000u) expectOffset = 20000u;  // loop wrap
      io.Channel(1).Release(rec);
      ++received;
    }
    snd::platform::SleepFor(200000);
  }
  SND_CHECK(ok);
  SND_CHECK(received >= 40u);
  for (int i = 0; i < 1000 && !req.Finished(); ++i) snd::platform::SleepFor(1000000);
  SND_CHECK(req.State() == snd::io::FileState::Done);
  SND_CHECK(MatchesPattern(2, 0, bank.data(), 5000));
  io.Channel(1).Cancel();
  io.Shutdown();
  SND_CHECK(!io.Running());
  SND_CHECK(io.Channel(1).Reclaim());
  SND_CHECK_EQ(fs.InFlight(), 0u);
  SND_CHECK_EQ(fs.OpenFileCount(), 0u);
  io.Destroy();
  SND_CHECK_EQ(pool.Used(), 0u);
  pool.Destroy();
  fs.Shutdown();
  SND_CHECK_EQ(alloc.TotalUsed(), 0u);
}

SND_TEST(DefaultFileSystem, RefusesSymlinksThatLeaveTheRoot) {
  TempRoot tmp;
  SND_REQUIRE(!tmp.root.empty());
  const std::string outside = tmp.root + "/../snd-io-outside-" + tmp.Base();
  const std::string links[] = {tmp.root + "/in_rel", tmp.root + "/in_abs", tmp.root + "/out_file",
                               tmp.root + "/out_dir", tmp.root + "/sub/in_up"};
  SND_REQUIRE(symlink("a.bin", links[0].c_str()) == 0);                      // relative, inside
  SND_REQUIRE(symlink((tmp.root + "/a.bin").c_str(), links[1].c_str()) == 0);  // absolute, inside
  SND_REQUIRE(symlink(outside.c_str(), links[2].c_str()) == 0);              // file outside
  SND_REQUIRE(symlink("..", links[3].c_str()) == 0);                         // directory outside (parent)
  SND_REQUIRE(symlink("../a.bin", links[4].c_str()) == 0);                   // relative up, still inside
  for (const std::string& l : links) tmp.files.push_back(l);
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  cfg.root = tmp.root.c_str();
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  SND_CHECK(fs.Open("in_rel") != kInvalidFile);
  SND_CHECK(fs.Open("in_abs") != kInvalidFile);
  SND_CHECK(fs.Open("sub/in_up") != kInvalidFile);
  SND_CHECK_EQ(fs.Open("out_file"), kInvalidFile);
  SND_CHECK_EQ(fs.Open(("out_dir/snd-io-outside-" + tmp.Base()).c_str()), kInvalidFile);
  // Leaving and re-entering ends inside the root: the resolved file counts (decision b).
  SND_CHECK(fs.Open(("out_dir/" + tmp.Base() + "/a.bin").c_str()) != kInvalidFile);
  fs.Shutdown();
}
