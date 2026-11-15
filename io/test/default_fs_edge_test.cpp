// DefaultFileSystem edge cases on real files (6.5, D-24): path validation,
// symbolic links leaving the root, limits, Poll-once, token reuse, Close with
// reads in flight, offsets beyond 4 GiB (sparse file), several readers,
// Shutdown with reads queued, RT notes in every entry point.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "io_test_util.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/io/default_file_system.hpp"
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

// <tmp>/base/root is the FS root; <tmp>/base/secret.bin and <tmp>/base/root2/x.bin are outside it.
struct Sandbox {
  std::string base, root;
  Sandbox() {
    char tmpl[] = "/tmp/snd-iot-fs-XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (d == nullptr) return;
    base = d;
    root = base + "/root";
    mkdir(root.c_str(), 0700);
    mkdir((root + "/sub").c_str(), 0700);
    mkdir((base + "/root2").c_str(), 0700);
    Write(root + "/a.bin", MakePattern(1, 10000));
    Write(root + "/sub/b.bin", MakePattern(2, 3000));
    Write(root + "/empty.bin", {});
    Write(base + "/secret.bin", MakePattern(7, 64));
    Write(base + "/root2/x.bin", MakePattern(8, 64));
  }
  ~Sandbox() {
    if (!base.empty()) {
      const std::string cmd = "rm -rf '" + base + "'";
      if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
    }
  }
  static void Write(const std::string& path, const std::vector<std::uint8_t>& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
  }
};

IoStatus WaitFor(DefaultFileSystem& fs, std::uint64_t token) {
  for (int i = 0; i < 50000; ++i) {
    const IoStatus s = fs.Poll(IoToken{token});
    if (snd::StateOf(s) != IoState::Pending) return s;
    snd::platform::SleepFor(100000);
  }
  return IoStatus{SND_IO_PENDING, 0};
}

}  // namespace

SND_TEST(DefaultFsEdge, ValidatePathBoundaries) {
  struct Case {
    const char* path;
    bool ok;
  };
  const Case cases[] = {
      {"a.bin", true},       {"sub/b.bin", true},  {"a/../b", true},     {"./a", true},
      {"a/", true},          {"a//b", true},       {"a/..", true},       {"..a", true},
      {"a..", true},         {"...", true},        {".", true},          {"sub/./../a", true},
      {"", false},           {"/a", false},        {"..", false},        {"../a", false},
      {"a/../..", false},    {"a/../../b", false}, {"a\\b", false},      {"\\", false},
      {"./..", false},       {"sub/../../root/a.bin", false},            {"//a", false},
  };
  for (const Case& c : cases) {
    SND_CHECK_MSG(DefaultFileSystem::ValidatePath(c.path) == c.ok, "path '%s'", c.path);
  }
  SND_CHECK(!DefaultFileSystem::ValidatePath(nullptr));
  std::string longOk(2047, 'x');
  std::string tooLong(2048, 'x');
  SND_CHECK(DefaultFileSystem::ValidatePath(longOk.c_str()));
  SND_CHECK(!DefaultFileSystem::ValidatePath(tooLong.c_str()));
  std::string deep;
  for (int i = 0; i < 1000; ++i) deep += "d/";
  deep += "f";
  SND_CHECK(DefaultFileSystem::ValidatePath(deep.c_str()));
}

SND_TEST(DefaultFsEdge, OpenRejectsEscapesAndDirectoriesButFollowsSymlinks) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  const std::string rootSlash = sb.root + "///";  // trailing slashes are stripped
  cfg.root = rootSlash.c_str();
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  SND_CHECK(fs.Open("a.bin") != kInvalidFile);
  SND_CHECK(fs.Open("sub/../a.bin") != kInvalidFile);
  SND_CHECK(fs.Open("./sub//b.bin") != kInvalidFile);
  SND_CHECK_EQ(fs.Open("sub"), kInvalidFile);       // directory
  SND_CHECK_EQ(fs.Open("sub/"), kInvalidFile);
  SND_CHECK_EQ(fs.Open("a.bin/"), kInvalidFile);    // not a directory
  SND_CHECK_EQ(fs.Open("../secret.bin"), kInvalidFile);
  SND_CHECK_EQ(fs.Open("../root2/x.bin"), kInvalidFile);  // sibling with the root's name as prefix
  SND_CHECK_EQ(fs.Open("missing.bin"), kInvalidFile);
  // A symbolic link inside the root that points outside: the header says links
  // are followed unchecked. This test documents that a file outside the root
  // is readable through it (spec gap: the contract only says "relative to the root").
  const std::string link = sb.root + "/escape.bin";
  SND_REQUIRE(symlink((sb.base + "/secret.bin").c_str(), link.c_str()) == 0);
  const FileId e = fs.Open("escape.bin");
  SND_CHECK(e != kInvalidFile);
  if (e != kInvalidFile) {
    std::uint8_t buf[64] = {};
    SND_REQUIRE(fs.ReadAsync(e, 0, buf, 64, IoToken{1}));
    const IoStatus st = WaitFor(fs, 1);
    SND_CHECK(snd::StateOf(st) == IoState::Done && st.bytes == 64u && MatchesPattern(7, 0, buf, 64));
    snd::test::Note("a file outside the root was read through a symlink (spec gap, reported)");
    fs.Close(e);
  }
  // Directory symlink.
  SND_REQUIRE(symlink(sb.base.c_str(), (sb.root + "/up").c_str()) == 0);
  SND_CHECK(fs.Open("up/secret.bin") != kInvalidFile);
  fs.Shutdown();
}

SND_TEST(DefaultFsEdge, LimitsPollOnceTokensAndClose) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  cfg.root = sb.root.c_str();
  cfg.maxInFlight = 3;
  cfg.maxOpenFiles = 2;
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  const FileId a = fs.Open("a.bin");
  const FileId b = fs.Open("sub/b.bin");
  SND_REQUIRE(a != kInvalidFile && b != kInvalidFile);
  SND_CHECK_EQ(fs.Open("empty.bin"), kInvalidFile);  // open-file limit
  SND_CHECK_EQ(fs.OpenFileCount(), 2u);
  SND_CHECK_EQ(fs.Size(a), 10000u);
  SND_CHECK_EQ(fs.Size(kInvalidFile), UINT64_MAX);
  SND_CHECK_EQ(fs.Size(a ^ 0x1000u), UINT64_MAX);  // other generation
  std::vector<std::uint8_t> buf(4 * 10000);
  SND_CHECK(!fs.ReadAsync(a, 0, nullptr, 10, IoToken{1}));          // null dst
  SND_CHECK(!fs.ReadAsync(kInvalidFile, 0, buf.data(), 10, IoToken{1}));
  SND_REQUIRE(fs.ReadAsync(a, 0, buf.data(), 10000, IoToken{1}));
  SND_CHECK(!fs.ReadAsync(a, 0, buf.data() + 10000, 10, IoToken{1}));  // token in flight
  SND_REQUIRE(fs.ReadAsync(a, 9990, buf.data() + 10000, 100, IoToken{2}));  // crosses EOF
  SND_REQUIRE(fs.ReadAsync(b, 0, buf.data() + 20000, 3000, IoToken{3}));
  SND_CHECK(!fs.ReadAsync(b, 0, buf.data() + 30000, 10, IoToken{4}));  // maxInFlight
  // Close with reads in flight (a caller error): deferred until they retire.
  fs.Close(a);
  SND_CHECK_EQ(fs.Size(a), UINT64_MAX);
  SND_CHECK(!fs.ReadAsync(a, 0, buf.data() + 30000, 10, IoToken{5}));
  SND_CHECK_EQ(fs.OpenFileCount(), 2u);
  const IoStatus s1 = WaitFor(fs, 1);
  const IoStatus s2 = WaitFor(fs, 2);
  const IoStatus s3 = WaitFor(fs, 3);
  SND_CHECK(snd::StateOf(s1) == IoState::Done && s1.bytes == 10000u);
  SND_CHECK(snd::StateOf(s2) == IoState::Done && s2.bytes == 10u);
  SND_CHECK(snd::StateOf(s3) == IoState::Done && s3.bytes == 3000u);
  SND_CHECK(MatchesPattern(1, 0, buf.data(), 10000));
  SND_CHECK(MatchesPattern(1, 9990, buf.data() + 10000, 10));
  SND_CHECK(MatchesPattern(2, 0, buf.data() + 20000, 3000));
  // Reported once, then retired.
  SND_CHECK(snd::StateOf(fs.Poll(IoToken{1})) == IoState::UnknownToken);
  SND_CHECK(snd::StateOf(fs.Poll(IoToken{99})) == IoState::UnknownToken);
  SND_CHECK_EQ(fs.InFlight(), 0u);
  SND_CHECK_EQ(fs.OpenFileCount(), 1u);  // the deferred close happened
  // Token reuse after retirement is fine; reading 0 bytes and past EOF.
  SND_REQUIRE(fs.ReadAsync(b, 5000, buf.data(), 10, IoToken{1}));
  const IoStatus s4 = WaitFor(fs, 1);
  SND_CHECK(snd::StateOf(s4) == IoState::Done && s4.bytes == 0u);
  // A stale id after close + reopen in the same slot.
  fs.Close(b);
  SND_CHECK_EQ(fs.OpenFileCount(), 0u);
  const FileId b2 = fs.Open("sub/b.bin");
  SND_CHECK(b2 != kInvalidFile && b2 != b);
  SND_CHECK_EQ(fs.Size(b), UINT64_MAX);
  fs.Close(b);  // stale: ignored
  SND_CHECK_EQ(fs.OpenFileCount(), 1u);
  fs.Shutdown();
  fs.Shutdown();
  SND_CHECK_EQ(fs.Open("a.bin"), kInvalidFile);
  SND_CHECK(snd::StateOf(fs.Poll(IoToken{1})) == IoState::UnknownToken);
}

SND_TEST(DefaultFsEdge, OffsetsBeyondFourGiBInASparseFile) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  const std::string path = sb.root + "/sparse.bin";
  const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0600);
  SND_REQUIRE(fd >= 0);
  const std::uint64_t off = (5ull << 32) + 12345u;
  const std::vector<std::uint8_t> tail = MakePattern(9, 4096);
  const bool wrote = ::pwrite(fd, tail.data(), tail.size(), static_cast<off_t>(off)) == 4096;
  ::close(fd);
  SND_REQUIRE(wrote);
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  cfg.root = sb.root.c_str();
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  const FileId f = fs.Open("sparse.bin");
  SND_REQUIRE(f != kInvalidFile);
  SND_CHECK_EQ(fs.Size(f), off + 4096u);
  std::vector<std::uint8_t> buf(8192, 0xEE);
  SND_REQUIRE(fs.ReadAsync(f, off - 100u, buf.data(), 8192, IoToken{7}));
  const IoStatus s = WaitFor(fs, 7);
  SND_CHECK(snd::StateOf(s) == IoState::Done);
  SND_CHECK_EQ(s.bytes, 4196u);
  bool zeros = true;
  for (int i = 0; i < 100; ++i) zeros = zeros && buf[i] == 0u;
  SND_CHECK(zeros);
  SND_CHECK(MatchesPattern(9, 0, buf.data() + 100, 4096));
  fs.Shutdown();
}

SND_TEST(DefaultFsEdge, SeveralReadersAndShutdownWithReadsQueued) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  for (std::uint32_t readers : {1u, 3u}) {
    DefaultFileSystem fs;
    DefaultFileSystemConfig cfg;
    cfg.root = sb.root.c_str();
    cfg.maxInFlight = 32;
    cfg.readerThreads = readers;
    SND_REQUIRE(fs.Init(alloc, cfg));
    const FileId a = fs.Open("a.bin");
    std::vector<std::uint8_t> buf(32u * 1000u);
    for (std::uint64_t t = 0; t < 32; ++t) {
      SND_REQUIRE(fs.ReadAsync(a, t * 300u, buf.data() + t * 1000u, 1000, IoToken{100 + t}));
    }
    bool ok = true;
    for (std::uint64_t t = 0; t < 16; ++t) {
      const IoStatus s = WaitFor(fs, 100 + t);
      ok = ok && snd::StateOf(s) == IoState::Done && s.bytes == 1000u &&
           MatchesPattern(1, t * 300u, buf.data() + t * 1000u, 1000);
    }
    SND_CHECK(ok);
    // 16 reads not retired yet: Shutdown finishes them and closes everything.
    fs.Shutdown();
    SND_CHECK_EQ(fs.InFlight(), 0u);
    SND_CHECK_EQ(alloc.TotalUsed(), 0u);
  }
  DefaultFileSystem bad;
  DefaultFileSystemConfig c0;
  c0.maxInFlight = 0;
  SND_CHECK(!bad.Init(alloc, c0));
  c0.maxInFlight = 1;
  c0.readerThreads = 0;
  SND_CHECK(!bad.Init(alloc, c0));
  c0.readerThreads = 1;
  c0.maxOpenFiles = 5000;
  SND_CHECK(!bad.Init(alloc, c0));
  std::string longRoot(3000, 'r');
  c0.maxOpenFiles = 4;
  c0.root = longRoot.c_str();
  SND_CHECK(!bad.Init(alloc, c0));
  SND_CHECK_EQ(alloc.TotalUsed(), 0u);
}

SND_TEST(DefaultFsEdge, EveryEntryPointNotesFileIo) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  cfg.root = sb.root.c_str();
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  std::uint8_t buf[16];
  const char* names[] = {"Open", "Size", "ReadAsync", "Poll", "Close"};
  FileId f = kInvalidFile;
  for (int k = 0; k < 5; ++k) {
    snd::test::rt::Reset();
    {
      snd::test::rt::ExpectViolations expect;
      SND_RT_SCOPE();
      switch (k) {
        case 0: f = fs.Open("a.bin"); break;
        case 1: (void)fs.Size(f); break;
        case 2: (void)fs.ReadAsync(f, 0, buf, 16, IoToken{1}); break;
        case 3: (void)fs.Poll(IoToken{1}); break;
        default: break;
      }
    }
    if (k == 4) {
      (void)WaitFor(fs, 1);
      snd::test::rt::Reset();
      snd::test::rt::ExpectViolations expect;
      SND_RT_SCOPE();
      fs.Close(f);
    }
    SND_CHECK_MSG(snd::test::rt::Count(snd::rt::Violation::FileIo) >= 1u, "%s did not note FileIo", names[k]);
  }
  fs.Shutdown();
}
