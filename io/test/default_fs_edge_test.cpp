// DefaultFileSystem edge cases on real files (6.5, D-24): path validation,
// symbolic links leaving the root, limits, Poll-once, token reuse, Close with
// reads in flight, offsets beyond 4 GiB (sparse file), several readers,
// Shutdown with reads queued, RT notes in every entry point.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <thread>
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

namespace {
// Opens `rel`, reads up to 64 bytes. Returns the bytes read, -1 if Open refused.
int OpenRead(DefaultFileSystem& fs, const char* rel, std::uint8_t* buf) {
  const FileId f = fs.Open(rel);
  if (f == kInvalidFile) return -1;
  int got = -2;
  if (fs.ReadAsync(f, 0, buf, 64, IoToken{4242})) {
    const IoStatus st = WaitFor(fs, 4242);
    got = snd::StateOf(st) == IoState::Done ? static_cast<int>(st.bytes) : -3;
  }
  fs.Close(f);
  return got;
}
}  // namespace

// Decision b: a path whose RESOLVED file lies outside the root is refused
// (links to files and to directories, relative or absolute, chains, links in
// the middle of the path); links that stay inside the root are followed.
SND_TEST(DefaultFsEdge, OpenRejectsEscapesAndDirectoriesButFollowsSymlinks) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  const std::string& R = sb.root;
  // Links (created before Init: nothing is cached anyway).
  auto L = [&](const std::string& target, const std::string& name) { return symlink(target.c_str(), (R + "/" + name).c_str()) == 0; };
  SND_REQUIRE(L("a.bin", "rel_in.bin"));                       // relative, inside
  SND_REQUIRE(L(R + "/a.bin", "abs_in.bin"));                  // absolute, inside
  SND_REQUIRE(L("../secret.bin", "rel_out.bin"));              // relative, outside
  SND_REQUIRE(L(sb.base + "/secret.bin", "abs_out.bin"));      // absolute, outside
  SND_REQUIRE(L("rel_in.bin", "chain_in.bin"));                // chain -> chain -> a.bin
  SND_REQUIRE(L("rel_out.bin", "chain_out.bin"));              // chain -> outside
  SND_REQUIRE(L("abs_in.bin", "chain_abs_in.bin"));            // relative -> absolute inside
  SND_REQUIRE(L("sub", "dir_rel_in"));                         // directory link, inside
  SND_REQUIRE(L(R + "/sub", "dir_abs_in"));                    // absolute directory link, inside
  SND_REQUIRE(L("..", "dir_up"));                              // directory link to the parent
  SND_REQUIRE(L(sb.base + "/root2", "dir_abs_out"));           // absolute directory link, outside (root's name as prefix)
  SND_REQUIRE(L("sub/../../root/a.bin", "dotdot_in.bin"));     // leaves and re-enters the root
  SND_REQUIRE(L("missing.bin", "dangling.bin"));
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  const std::string rootSlash = R + "///";  // trailing slashes are stripped
  cfg.root = rootSlash.c_str();
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  std::uint8_t buf[64];
  struct Case {
    const char* path;
    int expect;  // bytes read, or -1 = refused
    std::uint32_t seed;
  };
  const Case cases[] = {
      {"a.bin", 64, 1},           {"sub/../a.bin", 64, 1},     {"./sub//b.bin", 64, 2},
      {"rel_in.bin", 64, 1},      {"abs_in.bin", 64, 1},       {"chain_in.bin", 64, 1},
      {"chain_abs_in.bin", 64, 1}, {"dir_rel_in/b.bin", 64, 2}, {"dir_abs_in/b.bin", 64, 2},
      {"dir_rel_in/../a.bin", 64, 1}, {"dotdot_in.bin", 64, 1},
      {"rel_out.bin", -1, 0},     {"abs_out.bin", -1, 0},      {"chain_out.bin", -1, 0},
      {"dir_up/secret.bin", -1, 0}, {"dir_up/root/a.bin", 64, 1}, {"dir_abs_out/x.bin", -1, 0},
      {"dangling.bin", -1, 0},    {"sub", -1, 0},              {"sub/", -1, 0},
      {"a.bin/", -1, 0},          {"../secret.bin", -1, 0},    {"../root2/x.bin", -1, 0},
      {"missing.bin", -1, 0},
  };
  for (const Case& c : cases) {
    const int got = OpenRead(fs, c.path, buf);
    SND_CHECK_MSG(got == c.expect, "'%s': got %d, expected %d", c.path, got, c.expect);
    if (got == 64 && c.expect == 64) SND_CHECK_MSG(MatchesPattern(c.seed, 0, buf, 64), "'%s': wrong data", c.path);
  }
  SND_CHECK_EQ(fs.OpenFileCount(), 0u);
  fs.Shutdown();
}

// The root itself is a symbolic link: files below it are inside; links that
// point outside the link's target are refused; an absolute link spelled through
// the root link is inside.
SND_TEST(DefaultFsEdge, RootThatIsASymlink) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  const std::string rootLink = sb.base + "/rootlink";
  SND_REQUIRE(symlink(sb.root.c_str(), rootLink.c_str()) == 0);
  SND_REQUIRE(symlink((rootLink + "/a.bin").c_str(), (sb.root + "/via_link.bin").c_str()) == 0);
  SND_REQUIRE(symlink((sb.base + "/secret.bin").c_str(), (sb.root + "/out.bin").c_str()) == 0);
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  cfg.root = rootLink.c_str();
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  std::uint8_t buf[64];
  SND_CHECK_EQ(OpenRead(fs, "a.bin", buf), 64);
  SND_CHECK_EQ(OpenRead(fs, "via_link.bin", buf), 64);
  SND_CHECK_EQ(OpenRead(fs, "out.bin", buf), -1);
  // Retarget the root link after Init: openat2 resolves below the directory
  // opened at Init, the fallback below the canonical path taken at Init. Either
  // way the old root's files stay readable through relative paths, and the new
  // target is not reachable.
  SND_REQUIRE(unlink(rootLink.c_str()) == 0);
  SND_REQUIRE(symlink((sb.base + "/root2").c_str(), rootLink.c_str()) == 0);
  const int a = OpenRead(fs, "a.bin", buf);
  const int x = OpenRead(fs, "x.bin", buf);
  snd::test::Note("after retargeting the root link: a.bin -> %d, x.bin (new target) -> %d", a, x);
  SND_CHECK_EQ(x, -1);
  fs.Shutdown();
}

// TOCTOU: a link inside the root is flipped between an inside and an outside
// target while the IO thread opens it. Every successful Open must read the
// inside file. Relative links go through openat2(RESOLVE_BENEATH) (atomic in
// the kernel); absolute links fall back to open() + /proc/self/fd check of the
// opened descriptor (also race-free for the file that is read).
SND_TEST(DefaultFsEdge, LinkSwappedDuringOpenNeverReadsOutside) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  cfg.root = sb.root.c_str();
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  for (int absolute = 0; absolute < 2; ++absolute) {
    const std::string in = absolute != 0 ? sb.root + "/a.bin" : std::string("a.bin");
    const std::string out = absolute != 0 ? sb.base + "/secret.bin" : std::string("../secret.bin");
    const std::string link = sb.root + "/flip.bin";
    const std::string tmpIn = sb.root + "/.flip_in", tmpOut = sb.root + "/.flip_out";
    unlink(tmpIn.c_str());
    unlink(tmpOut.c_str());
    SND_REQUIRE(symlink(in.c_str(), tmpIn.c_str()) == 0);
    SND_REQUIRE(symlink(out.c_str(), tmpOut.c_str()) == 0);
    SND_REQUIRE(symlink(in.c_str(), link.c_str()) == 0);
    std::atomic<bool> stop{false};
    std::thread flipper([&] {
      // rename() replaces the link atomically; recreate the source each time.
      for (std::uint32_t i = 0; !stop.load(); ++i) {
        const bool o = (i & 1u) != 0u;
        const std::string tmp = sb.root + (o ? "/.n_out" : "/.n_in");
        unlink(tmp.c_str());
        if (symlink((o ? out : in).c_str(), tmp.c_str()) == 0) (void)rename(tmp.c_str(), link.c_str());
      }
    });
    std::uint32_t opened = 0, refused = 0, outside = 0;
    std::uint8_t buf[64];
    const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 700000000ull;
    while (snd::platform::MonotonicNanoseconds() < until) {
      const int got = OpenRead(fs, "flip.bin", buf);
      if (got < 0) {
        ++refused;
        continue;
      }
      ++opened;
      if (MatchesPattern(7, 0, buf, 64) || !MatchesPattern(1, 0, buf, 64)) ++outside;
    }
    stop.store(true);
    flipper.join();
    unlink(link.c_str());
    snd::test::Note("%s link: %u opens, %u refused, %u read outside data", absolute != 0 ? "absolute" : "relative", opened,
                    refused, outside);
    SND_CHECK_EQ(outside, 0u);
    SND_CHECK(opened > 0u && refused > 0u);
  }
  fs.Shutdown();
}

// A FIFO (or any special file) reachable from the root: Open must refuse it
// without blocking the IO thread (open(O_RDONLY) of a FIFO without a writer
// blocks; the outside one through a link is opened by the fallback BEFORE the
// "outside the root" check).
SND_TEST(IoDefect, FifoInTheRootOrBehindALinkBlocksOpen) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  SND_REQUIRE(mkfifo((sb.root + "/pipe").c_str(), 0600) == 0);
  SND_REQUIRE(mkfifo((sb.base + "/outpipe").c_str(), 0600) == 0);
  SND_REQUIRE(symlink((sb.base + "/outpipe").c_str(), (sb.root + "/outpipe_link").c_str()) == 0);
  DefaultFileSystem fs;
  DefaultFileSystemConfig cfg;
  cfg.root = sb.root.c_str();
  SND_REQUIRE(fs.Init(snd::base::DefaultAllocator(), cfg));
  const char* names[] = {"pipe", "outpipe_link"};
  const std::string fifos[] = {sb.root + "/pipe", sb.base + "/outpipe"};
  for (int k = 0; k < 2; ++k) {
    std::atomic<int> state{0};  // 0 running, 1 returned
    FileId id = kInvalidFile;
    std::thread t([&] {
      id = fs.Open(names[k]);
      state.store(1);
    });
    const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 2000000000ull;
    while (state.load() == 0 && snd::platform::MonotonicNanoseconds() < until) snd::platform::SleepFor(1000000);
    const bool blocked = state.load() == 0;
    if (blocked) {  // unblock it: open the write end
      const int w = ::open(fifos[k].c_str(), O_WRONLY | O_NONBLOCK);
      for (int i = 0; i < 200 && state.load() == 0; ++i) snd::platform::SleepFor(10000000);
      if (w >= 0) ::close(w);
    }
    t.join();
    SND_CHECK_MSG(!blocked, "Open(\"%s\") blocked the calling (IO) thread", names[k]);
    SND_CHECK_MSG(id == kInvalidFile, "Open(\"%s\") accepted a FIFO", names[k]);
    if (id != kInvalidFile) fs.Close(id);
  }
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
