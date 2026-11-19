// Independent tests: the root of the built-in file system (CONTRACTS.md D-103, config.h).
// Properties (from the spec, before reading the implementation):
//  F1 root NULL / "" = the working directory; a missing root or a regular file -> NOT_FOUND;
//     longer than 2046 bytes -> INVALID_ARGUMENT (2046 itself passes the length check);
//     with a game file system -> INVALID_ARGUMENT ("" with a game file system is fine).
//  F2 the string is copied: changing or freeing the caller's buffer after snd_create does not
//     move the root; a relative root is resolved at snd_create (a later chdir changes nothing).
//  F3 refused: "/abs", "a\b", "C:x", "x:y", ".." that leaves the root (also "a/../..",
//     "./../x", "a/b/../../../x"), links whose target is outside (relative, absolute, through
//     a directory link, a link chain, /proc magic links), directories, FIFOs.
//  F4 accepted: ".." that stays inside ("a/../b", "a/./b"), links inside (relative and
//     absolute), a root that is itself a link (its contents are inside).
//  F5 lexical edge cases: empty components ("a//b"), "./x", a trailing "/" (refused: it would
//     name a directory), "." alone, "" (refused), very long paths, case (POSIX: exact).
//  F6 a link swapped to the outside while opening never yields the outside content.
// The tests use the io module's DefaultFileSystem (what snd_create builds for file_system
// NULL) and, for F1 / F2, snd_create. POSIX only (links, FIFOs, mkdtemp): on Windows the file
// is reduced to the configuration rules.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "indep_rig.hpp"
#include "snd/io/default_file_system.hpp"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using apitest::SmallConfig;

namespace {

snd_result CreateWithRoot(const char* root, const snd_file_system* fs = nullptr) {
  snd_config c = SmallConfig();
  c.output_kind = SND_OUTPUT_MANUAL;
  c.file_system_root = root;
  snd_system* sys = reinterpret_cast<snd_system*>(&c);
  const snd_result r = snd_create(&c, nullptr, fs, &sys);
  if (r != SND_OK) SND_CHECK(sys == nullptr);
  if (r == SND_OK) snd_destroy(sys);
  return r;
}

}  // namespace

// F1 (portable part).
SND_TEST(IndepHostFs, RootLengthAndGameFileSystem) {
  snd::mock::MemoryFileSystem mfs;
  const snd_file_system ft = snd::ToC(mfs);
  SND_CHECK_EQ(CreateWithRoot("x", &ft), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(CreateWithRoot("", &ft), SND_OK);
  SND_CHECK_EQ(CreateWithRoot(nullptr, &ft), SND_OK);
  SND_CHECK_EQ(CreateWithRoot(nullptr), SND_OK);  // the working directory
  SND_CHECK_EQ(CreateWithRoot(""), SND_OK);
  std::string longRoot(2047, 'a');
  SND_CHECK_EQ(CreateWithRoot(longRoot.c_str()), SND_ERR_INVALID_ARGUMENT);
  longRoot.assign(5000, 'b');
  SND_CHECK_EQ(CreateWithRoot(longRoot.c_str()), SND_ERR_INVALID_ARGUMENT);
  // 2046 bytes passes the length check; it does not exist: NOT_FOUND.
  std::string r2046 = "./";
  r2046.append(2044, 'c');
  SND_CHECK_EQ(r2046.size(), 2046u);
  const snd_result r = CreateWithRoot(r2046.c_str());
  SND_CHECK_MSG(r == SND_ERR_NOT_FOUND, "2046-byte missing root -> %d", r);
  SND_CHECK_EQ(CreateWithRoot("surely/not/an/existing/dir/9f8e7d"), SND_ERR_NOT_FOUND);
}

#if !defined(_WIN32)

namespace {

std::vector<std::uint8_t> Pattern(std::uint8_t seed, std::size_t n) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(seed + i * 7u);
  return v;
}

void WriteFile(const std::string& path, const std::vector<std::uint8_t>& data) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return;
  if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
}

// <base>/root is the root. Inside: in.bin (seed 1), sub/deep.bin (seed 2). Outside:
// <base>/secret.bin (seed 9), <base>/rootx/peer.bin (seed 8: a sibling whose name starts
// with the root's).
struct Sandbox {
  std::string base, root;
  Sandbox() {
    char tmpl[] = "/tmp/snd-indep-fs-XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (d == nullptr) return;
    base = d;
    root = base + "/root";
    mkdir(root.c_str(), 0700);
    mkdir((root + "/sub").c_str(), 0700);
    mkdir((base + "/rootx").c_str(), 0700);
    WriteFile(root + "/in.bin", Pattern(1, 100));
    WriteFile(root + "/sub/deep.bin", Pattern(2, 100));
    WriteFile(root + "/In.bin", Pattern(3, 100));
    WriteFile(base + "/secret.bin", Pattern(9, 100));
    WriteFile(base + "/rootx/peer.bin", Pattern(8, 100));
  }
  ~Sandbox() {
    if (!base.empty()) {
      const std::string cmd = "rm -rf '" + base + "'";
      if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
    }
  }
  void Link(const std::string& target, const std::string& name) const {
    if (symlink(target.c_str(), (root + "/" + name).c_str()) != 0) std::perror("symlink");
  }
};

struct Fs {
  snd::io::DefaultFileSystem fs;
  bool ok = false;
  explicit Fs(const std::string& root) {
    snd::io::DefaultFileSystemConfig c;
    c.root = root.c_str();
    ok = fs.Init(snd::base::DefaultAllocator(), c) && fs.RootAvailable();
  }
  ~Fs() { fs.Shutdown(); }
  // First byte of the file, or -1 if it does not open / read.
  int First(const char* path) {
    const snd::FileId id = fs.Open(path);
    if (id == snd::kInvalidFile) return -1;
    std::uint8_t b = 0;
    int out = -1;
    if (fs.ReadAsync(id, 0, &b, 1, snd::IoToken{77})) {
      for (int i = 0; i < 20000; ++i) {
        const snd::IoStatus s = fs.Poll(snd::IoToken{77});
        if (snd::StateOf(s) == snd::IoState::Done) {
          out = b;
          break;
        }
        if (snd::StateOf(s) != snd::IoState::Pending) break;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
    fs.Close(id);
    return out;
  }
};

constexpr int kIn = 1, kDeep = 2, kUpper = 3, kSecret = 9, kPeer = 8;

}  // namespace

// F3 / F4 / F5: lexical rules.
SND_TEST(IndepHostFs, LexicalRules) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  Fs f(sb.root);
  SND_REQUIRE(f.ok);
  struct Case {
    const char* path;
    int want;  // first byte, -1 = refused
  };
  const Case cases[] = {
      {"in.bin", kIn},
      {"./in.bin", kIn},
      {"sub/../in.bin", kIn},
      {"sub/./deep.bin", kDeep},
      {"sub//deep.bin", kDeep},
      {"./sub/../sub/deep.bin", kDeep},
      {"In.bin", kUpper},  // POSIX names are exact
      {"IN.BIN", -1},
      {"/in.bin", -1},
      {"", -1},
      {".", -1},
      {"sub", -1},
      {"sub/", -1},
      {"in.bin/", -1},
      {"../secret.bin", -1},
      {"sub/../../secret.bin", -1},
      {"./../secret.bin", -1},
      {"sub/x/../../../secret.bin", -1},
      {"../root/in.bin", -1},  // leaves and comes back: refused (lexically above the root)
      {"../rootx/peer.bin", -1},
      {"sub\\deep.bin", -1},
      {"C:in.bin", -1},
      {"in.bin:stream", -1},
      {"..", -1},
      {"...", -1},  // a name, not a parent: it does not exist
  };
  for (const Case& c : cases) {
    const int got = f.First(c.path);
    SND_CHECK_MSG(got == c.want, "\"%s\": %d, want %d", c.path, got, c.want);
  }
  // A long relative path inside (many "./" components): refused only for its length.
  std::string longOk;
  for (int i = 0; i < 400; ++i) longOk += "./";
  longOk += "in.bin";
  SND_CHECK_EQ(f.First(longOk.c_str()), kIn);  // 806 bytes
  std::string tooLong;
  for (int i = 0; i < 1100; ++i) tooLong += "./";
  tooLong += "in.bin";
  SND_CHECK_EQ(f.First(tooLong.c_str()), -1);
}

// F3 / F4: links.
SND_TEST(IndepHostFs, Links) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  sb.Link("in.bin", "rel_in");
  sb.Link(sb.root + "/sub/deep.bin", "abs_in");           // absolute, inside
  sb.Link("../secret.bin", "rel_out");
  sb.Link(sb.base + "/secret.bin", "abs_out");
  sb.Link(sb.base + "/rootx", "dir_peer");                // a directory outside (name shares the prefix)
  sb.Link("sub", "dir_in");
  sb.Link("rel_out", "chain_out");                        // link -> link -> outside
  sb.Link("rel_in", "chain_in");
  sb.Link("/proc/self/root" + sb.base + "/secret.bin", "magic_out");  // a magic link path
  sb.Link("dir_in/../../secret.bin", "dotdot_through_link");
  Fs f(sb.root);
  SND_REQUIRE(f.ok);
  const struct {
    const char* path;
    int want;
  } cases[] = {
      {"rel_in", kIn},
      {"abs_in", kDeep},
      {"chain_in", kIn},
      {"dir_in/deep.bin", kDeep},
      {"rel_out", -1},
      {"abs_out", -1},
      {"chain_out", -1},
      {"dir_peer/peer.bin", -1},
      {"magic_out", -1},
      {"dotdot_through_link", -1},
      {"dir_in/../in.bin", kIn},
  };
  for (const auto& c : cases) {
    const int got = f.First(c.path);
    SND_CHECK_MSG(got == c.want, "\"%s\": %d, want %d", c.path, got, c.want);
  }
  (void)kSecret;
  (void)kPeer;
}

// F4: the root itself is a link (to a directory outside the link's own directory).
SND_TEST(IndepHostFs, RootThatIsALink) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  const std::string alias = sb.base + "/alias";
  SND_REQUIRE_EQ(symlink(sb.root.c_str(), alias.c_str()), 0);
  Fs f(alias);
  SND_REQUIRE(f.ok);
  SND_CHECK_EQ(f.First("in.bin"), kIn);
  SND_CHECK_EQ(f.First("../secret.bin"), -1);
  // Re-pointing the alias after Init does not move the root (resolved once).
  SND_REQUIRE_EQ(unlink(alias.c_str()), 0);
  SND_REQUIRE_EQ(symlink((sb.base + "/rootx").c_str(), alias.c_str()), 0);
  SND_CHECK_EQ(f.First("in.bin"), kIn);
  SND_CHECK_EQ(f.First("peer.bin"), -1);
}

// F3: what is not a regular file.
SND_TEST(IndepHostFs, NotRegularFiles) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  SND_REQUIRE_EQ(mkfifo((sb.root + "/pipe").c_str(), 0600), 0);
  sb.Link("/dev/null", "devnull");
  sb.Link("/dev/zero", "devzero");
  Fs f(sb.root);
  SND_REQUIRE(f.ok);
  SND_CHECK_EQ(f.First("pipe"), -1);
  SND_CHECK_EQ(f.First("devnull"), -1);
  SND_CHECK_EQ(f.First("devzero"), -1);
  SND_CHECK_EQ(f.First("sub"), -1);
}

// F6: a link flipped between inside and outside while another thread opens it: the outside
// content is never read.
SND_TEST(IndepHostFs, LinkSwapNeverReadsOutside) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  sb.Link("in.bin", "flip");
  Fs f(sb.root);
  SND_REQUIRE(f.ok);
  std::atomic<bool> stop{false};
  std::thread flipper([&] {
    const std::string tmpIn = sb.root + "/flip.tmp_in";
    const std::string tmpOut = sb.root + "/flip.tmp_out";
    const std::string link = sb.root + "/flip";
    bool out = false;
    while (!stop.load()) {
      const std::string& tmp = out ? tmpOut : tmpIn;
      unlink(tmp.c_str());
      if (symlink(out ? (sb.base + "/secret.bin").c_str() : "in.bin", tmp.c_str()) == 0) {
        rename(tmp.c_str(), link.c_str());  // atomic replacement
      }
      out = !out;
    }
  });
  int secret = 0, inside = 0, refused = 0;
  for (int i = 0; i < 3000; ++i) {
    const int b = f.First("flip");
    if (b == kSecret) ++secret;
    if (b == kIn) ++inside;
    if (b == -1) ++refused;
  }
  stop.store(true);
  flipper.join();
  snd::test::Note("inside %d, refused %d, outside %d", inside, refused, secret);
  SND_CHECK_EQ(secret, 0);
  SND_CHECK(inside > 0);
}

// F2: the root string is copied and resolved at snd_create.
SND_TEST(IndepHostFs, RootIsCopiedAndResolvedOnce) {
  Sandbox sb;
  SND_REQUIRE(!sb.base.empty());
  WriteFile(sb.root + "/Main.sndb", Pattern(1, 10));  // not a bank: the load fails as CORRUPT, not IO
  char cwd[4096];
  SND_REQUIRE(getcwd(cwd, sizeof(cwd)) != nullptr);
  SND_REQUIRE_EQ(chdir(sb.base.c_str()), 0);
  std::vector<char> rootBuf(std::begin("root"), std::end("root"));  // relative to <base>
  snd_config c = SmallConfig();
  c.output_kind = SND_OUTPUT_MANUAL;
  c.file_system_root = rootBuf.data();
  snd_system* sys = nullptr;
  const snd_result r = snd_create(&c, nullptr, nullptr, &sys);
  // Move away and scribble over the caller's string.
  SND_CHECK_EQ(chdir("/"), 0);
  std::memcpy(rootBuf.data(), "xxxx", 4);
  SND_REQUIRE_EQ(r, SND_OK);
  struct Ev {
    std::vector<snd_result> loads;
  } ev;
  snd_callbacks cb{};
  cb.user = &ev;
  cb.on_bank_loaded = [](void* u, snd_bank_handle, snd_result res) { static_cast<Ev*>(u)->loads.push_back(res); };
  snd_set_callbacks(sys, &cb);
  const snd_bank_handle h = snd_load_bank_async(sys, "Main.sndb");
  const snd_bank_handle h2 = snd_load_bank_async(sys, "missing.sndb");
  for (int i = 0; i < 4000 && ev.loads.size() < 2u; ++i) {
    snd_update(sys);
    snd_advance_output(sys, 1, nullptr, 0);
  }
  SND_CHECK(h.value != 0u && h2.value != 0u);
  SND_REQUIRE_EQ(ev.loads.size(), 2u);
  // The file was found under the original root (a bank error, not an IO error); the missing one is IO.
  int io = 0, other = 0;
  for (const snd_result x : ev.loads) (x == SND_ERR_IO ? io : other) += 1;
  SND_CHECK_EQ(io, 1);
  SND_CHECK_EQ(other, 1);
  snd_destroy(sys);
  SND_CHECK_EQ(chdir(cwd), 0);
  // A root that is a regular file: NOT_FOUND.
  SND_CHECK_EQ(CreateWithRoot((sb.root + "/in.bin").c_str()), SND_ERR_NOT_FOUND);
}

#endif  // !_WIN32
