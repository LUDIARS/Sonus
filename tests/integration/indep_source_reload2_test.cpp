// Independent re-check of the reload fixes (c7161e8, e7189ef):
//   - the PCM cache entries of an unregistered bank slot are never used again,
//     even when the slot is reused at once (unload and load back to back, many
//     times, alternating content, same address through a recycling allocator);
//   - io: a stream file replaced on disk between two plays. Stream reads keep
//     their file open (also while idle), bank loads (FileRequest) reopen.
//     Records what a second play reads, with and without a FileRequest of the
//     same path in between (default FS and memory FS).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <unistd.h>

#include "reference.hpp"
#include "snd/io/default_file_system.hpp"
#include "source_harness.hpp"

using namespace srctest;
namespace sfs = std::filesystem;

namespace {

struct Recycling final : snd::IAllocator {
  std::mutex m;
  std::multimap<std::tuple<std::size_t, std::size_t, int>, void*> spare;
  std::map<void*, std::tuple<std::size_t, std::size_t, int>> live;
  void* Allocate(std::size_t n, std::size_t al, snd::MemTag t) override {
    std::lock_guard<std::mutex> g(m);
    const auto key = std::make_tuple(n, al, static_cast<int>(t));
    auto it = spare.find(key);
    void* p = nullptr;
    if (it != spare.end()) {
      p = it->second;
      spare.erase(it);
    } else {
      p = snd::base::DefaultAllocator().Allocate(n, al, t);
    }
    if (p != nullptr) live[p] = key;
    return p;
  }
  void Free(void* p, snd::MemTag) override {
    if (p == nullptr) return;
    std::lock_guard<std::mutex> g(m);
    auto it = live.find(p);
    if (it == live.end()) return;
    spare.emplace(it->second, p);
    live.erase(it);
  }
  ~Recycling() override {
    for (auto& s : spare) snd::base::DefaultAllocator().Free(s.second, static_cast<snd::MemTag>(std::get<2>(s.first)));
  }
};

AssetSpec CacheAsset(double (*f)(std::uint32_t, std::uint64_t)) {
  AssetSpec a;
  a.name = "c";
  a.pcm = Signal(1, 48000, 12000, f);
  a.loadForm = "decompressCache";
  a.codec = "ADPC";
  return a;
}
// 1 when the output since `from` fits r1 (played at gain ~0.707), 2 for r2, 0 neither.
int Which(const Rig& r, const std::vector<float>& e1, const std::vector<float>& e2) {
  const std::size_t f0 = FirstNonZero(r, 0, 1e-4f);
  if (f0 == SIZE_MAX) return 0;
  const std::size_t at = f0 >= 8 ? f0 - 8 : 0;
  for (std::size_t s = at; s < at + 40; ++s) {
    const Fit f1 = FitGain(r, 0, s, e1), f2 = FitGain(r, 0, s, e2);
    if (std::fabs(f1.gain - 0.70710678) < 0.01 && f1.maxErr < 1e-3) return 1;
    if (std::fabs(f2.gain - 0.70710678) < 0.01 && f2.maxErr < 1e-3) return 2;
  }
  return 0;
}

}  // namespace

// Unload and load back to back (no step between), alternating banks A and B
// (same size, same asset name and length, other content) under two paths,
// 150 times: every play is the loaded bank's own audio.
SND_TEST(IndepSourceReload2, SlotReusedAtOnceNeverServesTheOldBank) {
  Built ba = Build("rr1", {CacheAsset(&Ramp)});
  Built bb = Build("rr2", {CacheAsset(&Sine1k)});
  SND_REQUIRE_MSG(ba.ok && bb.ok, "%s %s", ba.diag.c_str(), bb.diag.c_str());
  SND_REQUIRE_EQ(ba.bank.size(), bb.bank.size());
  const Decoded ra = Reference(ba, "c"), rb = Reference(bb, "c");
  SND_REQUIRE(ra.ok && rb.ok);
  const std::vector<float> ea = Resampled(ra.ch[0], ra.ch[0].size() + 64), eb = Resampled(rb.ch[0], rb.ch[0].size() + 64);
  Recycling alloc;
  Options o;
  o.allocator = &alloc;
  Rig r(o);
  SND_REQUIRE(r.Ok());
  r.fs.AddFile("A.sndb", ba.bank);
  r.fs.AddFile("B.sndb", bb.bank);
  snd_bank_handle cur = r.LoadAs(ba, "A.sndb");
  std::uint32_t wrong = 0, silent = 0;
  for (int i = 0; i < 150; ++i) {
    const bool isA = (i % 2) == 0;
    r.all.clear();
    const snd_playback_handle p = r.Play("c");
    for (int s = 0; s < 120 && r.EndOf(p) == nullptr; ++s) r.Step();
    const int w = Which(r, ea, eb);
    if (w == 0) ++silent;
    else if (w != (isA ? 1 : 2)) ++wrong;
    // Unload and load the other bank in the same frame (the slot is reused at once).
    SND_REQUIRE_EQ(r.sys->UnloadBank(cur, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
    cur = r.sys->LoadBankAsync(isA ? "B.sndb" : "A.sndb");
    for (int s = 0; s < 400 && r.sys->GetBankState(cur) != SND_BANK_STATE_LOADED; ++s) r.Step();
    SND_REQUIRE_EQ(r.sys->GetBankState(cur), SND_BANK_STATE_LOADED);
    r.sys->Update();
  }
  std::printf("  150 back-to-back reloads: %u plays of the wrong bank, %u unrecognised; cache hits %llu misses %llu\n", wrong,
              silent, static_cast<unsigned long long>(r.runtime.Stats().cacheHits),
              static_cast<unsigned long long>(r.runtime.Stats().cacheMisses));
  SND_CHECK_EQ(wrong, 0u);
  SND_CHECK_EQ(silent, 0u);
}

// Record (io): a stream file replaced between two plays, default FS and memory FS.
SND_TEST(IndepSourceReload2, RecordStreamFileReplacedBetweenPlays) {
  std::error_code ec;
  const sfs::path root = sfs::temp_directory_path(ec) / ("sonus-indep-reopen2-" + std::to_string(::getpid()));
  sfs::remove_all(root, ec);
  sfs::create_directories(root, ec);
  auto write = [&](const sfs::path& p, char c, std::size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    std::string s(n, c);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
  };
  snd::IAllocator& al = snd::base::DefaultAllocator();
  for (int which = 0; which < 2; ++which) {  // 0 default FS, 1 memory FS
    snd::io::DefaultFileSystem dfs;
    snd::mock::MemoryFileSystem mfs;
    snd::io::DefaultFileSystemConfig fc;
    const std::string rootStr = root.string();
    fc.root = rootStr.c_str();
    if (which == 0) SND_REQUIRE(dfs.Init(al, fc));
    snd::IFileSystem& fs = which == 0 ? static_cast<snd::IFileSystem&>(dfs) : static_cast<snd::IFileSystem&>(mfs);
    auto put = [&](const char* rel, char c, std::size_t n) {
      if (which == 0) {
        write(root / "tmp", c, n);
        sfs::rename(root / "tmp", root / rel, ec);  // replace (new inode)
      } else {
        mfs.AddFile(rel, std::vector<std::uint8_t>(n, static_cast<std::uint8_t>(c)));
      }
    };
    snd::io::StreamPool pool;
    SND_REQUIRE(pool.Init(al, 64u << 10, 4096, 1));
    snd::io::IoScheduler io;
    snd::io::IoSchedulerConfig ic;
    ic.maxStreams = 1;
    ic.bytesPerWindow = 0;
    SND_REQUIRE(io.Init(al, fs, pool, ic));
    std::uint32_t tag = 0;
    auto streamRead = [&](const char* path) -> char {  // one 4 KiB stream read at offset 0, first byte
      io.Channel(0).Cancel();
      snd::source::IoReadRequest rq{};
      rq.streamSlot = 0;
      rq.chunkIndex = tag++;
      rq.path = path;
      rq.offset = 0;
      rq.bytes = 4096;
      if (!io.Post(rq)) return '?';
      snd::io::ChunkRecord rec{};
      for (int i = 0; i < 20000; ++i) {
        io.RunOnce();
        if (io.Channel(0).Pop(rec)) {
          const char c = rec.status == snd::io::ChunkStatus::Ok ? static_cast<char>(pool.Data(rec.chunk)[0]) : '!';
          io.Channel(0).Release(rec);
          return c;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      return '?';
    };
    auto fileRead = [&](const char* path) -> char {
      snd::io::FileRequest q;
      std::vector<char> buf(8192, 0);
      q.path = path;
      q.dst = buf.data();
      q.bytes = buf.size();
      if (!io.Submit(q)) return '?';
      for (int i = 0; i < 20000 && !q.Finished(); ++i) {
        io.RunOnce();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      return q.Result().state == snd::io::FileState::Done ? buf[0] : '!';
    };
    const char* name = which == 0 ? "default FS" : "memory FS";
    put("s1.snds", 'A', 8192);
    put("bank.sndb", 'K', 8192);
    const char a1 = streamRead("s1.snds");
    put("s1.snds", 'B', 8192);
    const char a2 = streamRead("s1.snds");
    const char k = fileRead("bank.sndb");  // a bank load of another path in between
    const char a3 = streamRead("s1.snds");
    const char f = fileRead("s1.snds");  // a FileRequest of the stream's own path
    const char a4 = streamRead("s1.snds");
    std::printf("  %s: stream play 1 '%c'; file replaced by 'B': play 2 '%c'; after a bank load of another path '%c' "
                "(bank '%c'); FileRequest of the same path reads '%c', then play '%c'\n",
                name, a1, a2, a3, k, f, a4);
    io.Shutdown();
    io.Destroy();
    pool.Destroy();
    if (which == 0) dfs.Shutdown();
  }
  sfs::remove_all(root, ec);
}

// Through core: a bank rebuilt with other content for a stream asset keeps
// the stream file's path (streams/<name>_<asset id>.snds). Unload the bank,
// replace the stream file (memory FS: new bytes = a new inode on a real FS),
// load the rebuilt bank and play the stream: it must play the new content.
// Today the IO scheduler still holds the idle stream file open from the first
// play and reads the old bytes (the header still matches: same asset).
SND_TEST(IndepSourceReload2, RebuiltStreamFileIsReadFreshAfterABankReload) {
  auto spec = [](double (*f)(std::uint32_t, std::uint64_t)) {
    AssetSpec a;
    a.name = "s";
    a.pcm = Signal(1, 48000, 24000, f);
    a.loadForm = "stream";
    a.codec = "PC16";
    a.extra = "\"preloadMs\": 0";
    return a;
  };
  Built b1 = Build("rs1", {spec(&Ramp)});
  Built b2 = Build("rs2", {spec(&Sine1k)});
  SND_REQUIRE_MSG(b1.ok && b2.ok, "%s %s", b1.diag.c_str(), b2.diag.c_str());
  SND_REQUIRE(b1.files.size() == 1u && b2.files.size() == 1u);
  SND_REQUIRE_MSG(b1.files[0].first == b2.files[0].first, "stream paths differ: %s / %s", b1.files[0].first.c_str(),
                  b2.files[0].first.c_str());
  const Decoded r1 = Reference(b1, "s"), r2 = Reference(b2, "s");
  SND_REQUIRE(r1.ok && r2.ok);
  const std::vector<float> e1 = Resampled(r1.ch[0], r1.ch[0].size() + 64), e2 = Resampled(r2.ch[0], r2.ch[0].size() + 64);
  Rig r;
  SND_REQUIRE(r.Ok());
  const snd_bank_handle h1 = r.LoadAs(b1, "A.sndb");
  const snd_playback_handle p1 = r.Play("s");
  for (int i = 0; i < 300 && r.EndOf(p1) == nullptr; ++i) r.Step();
  SND_REQUIRE(r.EndOf(p1) != nullptr);
  SND_CHECK_EQ(Which(r, e1, e2), 1);
  SND_REQUIRE_EQ(r.sys->UnloadBank(h1, SND_UNLOAD_IF_UNUSED), SND_OK);
  for (int i = 0; i < 200 && r.sys->GetBankState(h1) != SND_BANK_STATE_UNLOADED; ++i) r.Step();
  r.LoadAs(b2, "B.sndb");  // also replaces the stream file's bytes in the memory FS
  r.all.clear();
  const snd_playback_handle p2 = r.Play("s");
  for (int i = 0; i < 300 && r.EndOf(p2) == nullptr; ++i) r.Step();
  const int w = Which(r, e1, e2);
  std::printf("  rebuilt stream after a bank reload plays %s\n", w == 1 ? "the OLD stream file" : (w == 2 ? "the new one" : "neither"));
  SND_CHECK_MSG(w == 2, "the rebuilt bank's stream plays %s", w == 1 ? "the old file" : "neither file");
}
