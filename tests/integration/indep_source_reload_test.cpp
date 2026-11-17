// Independent re-check around bank reloads:
//   - 9: the PCM cache key's load generation is 16 bits (SourceAsset::reserved,
//     the low 16 bits of the bank handle's generation). After 65536 loads of the
//     same bank slot the value repeats: can an old entry match again?
//   - record only (not a source defect): reloading a bank under the SAME path
//     after the file changed. The IO scheduler keeps files open by path; the
//     memory FS serves the bytes of the open time, the default (OS) file system
//     keeps a descriptor: which content is read after a replace (new inode) and
//     after an in-place rewrite (same inode, same or larger size)?
// Defect_source_* tests fail on purpose.
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
  a.pcm = Signal(1, 48000, 30000, f);
  a.loadForm = "decompressCache";
  a.codec = "ADPC";
  return a;
}

// Which of two references the output of the last playback fits (gain ~0.707, small error).
int WhichFits(const Rig& r, const Decoded& r1, const Decoded& r2, double* err1, double* err2) {
  const std::size_t f0 = FirstNonZero(r, 0, 1e-4f);
  if (f0 == SIZE_MAX) return 0;
  const std::size_t at = f0 >= 8 ? f0 - 8 : 0;
  const std::vector<float> e1 = Resampled(r1.ch[0], r1.ch[0].size() + 64);
  const std::vector<float> e2 = Resampled(r2.ch[0], r2.ch[0].size() + 64);
  double b1 = 1e9, b2 = 1e9;
  for (std::size_t s = at; s < at + 40; ++s) {
    const Fit f1 = FitGain(r, 0, s, e1);
    const Fit f2 = FitGain(r, 0, s, e2);
    if (std::fabs(f1.gain - 0.70710678) < 0.01) b1 = std::fmin(b1, f1.maxErr);
    if (std::fabs(f2.gain - 0.70710678) < 0.01) b2 = std::fmin(b2, f2.maxErr);
  }
  *err1 = b1;
  *err2 = b2;
  return b1 < 1e-3 ? 1 : (b2 < 1e-3 ? 2 : 0);
}

bool WaitUnloaded(Rig& r, snd_bank_handle h) {
  for (int i = 0; i < 200 && r.sys->GetBankState(h) != SND_BANK_STATE_UNLOADED; ++i) r.Step();
  return r.sys->GetBankState(h) == SND_BANK_STATE_UNLOADED;
}

}  // namespace

// 9: bank A (slot s, load generation g) plays its cache asset and is unloaded;
// the entry stays cached (copies). Bank B (same size, same asset name and
// length, other content) is then loaded and unloaded in the same slot without
// playing until the low 16 bits of the generation are g again (65536 loads;
// the recycling allocator gives B the address A had), then B plays: it must
// play its own audio. Today the key (asset id, slot, 16-bit generation, data
// pointer, frames, codec) matches A's entry again and A's audio is played.
SND_TEST(IndepSourceReload, Defect_source_LoadGenerationWrapsAfter65536Loads) {
  Built b1 = Build("rl1", {CacheAsset(&Ramp)});
  Built b2 = Build("rl2", {CacheAsset(&Sine1k)});
  SND_REQUIRE_MSG(b1.ok && b2.ok, "%s %s", b1.diag.c_str(), b2.diag.c_str());
  SND_REQUIRE_EQ(b1.bank.size(), b2.bank.size());
  const Decoded r1 = Reference(b1, "c"), r2 = Reference(b2, "c");
  SND_REQUIRE(r1.ok && r2.ok);
  Recycling alloc;
  Options o;
  o.allocator = &alloc;
  Rig r(o);
  SND_REQUIRE(r.Ok());
  const snd_bank_handle h1 = r.LoadAs(b1, "A.sndb");
  const snd::HandleRef g1 = snd::HandleRef::FromValue(h1.value);
  const snd_playback_handle p1 = r.Play("c");
  for (int i = 0; i < 300 && r.EndOf(p1) == nullptr; ++i) r.Step();
  SND_REQUIRE(r.EndOf(p1) != nullptr);
  SND_REQUIRE_EQ(r.sys->UnloadBank(h1, SND_UNLOAD_IF_UNUSED), SND_OK);
  SND_REQUIRE(WaitUnloaded(r, h1));
  r.fs.AddFile("B.sndb", b2.bank);
  const auto t0 = std::chrono::steady_clock::now();
  snd_bank_handle hb{0};
  std::uint32_t loads = 0;
  for (; loads < 140000u; ++loads) {
    hb = r.sys->LoadBankAsync("B.sndb");
    for (int i = 0; i < 200 && r.sys->GetBankState(hb) != SND_BANK_STATE_LOADED; ++i) r.Step();
    r.sys->Update();
    r.all.clear();
    const snd::HandleRef g = snd::HandleRef::FromValue(hb.value);
    SND_REQUIRE_MSG(r.sys->GetBankState(hb) == SND_BANK_STATE_LOADED, "load %u failed", loads);
    if (g.slot == g1.slot && (g.generation & 0xFFFFu) == (g1.generation & 0xFFFFu)) break;
    SND_REQUIRE_EQ(r.sys->UnloadBank(hb, SND_UNLOAD_IF_UNUSED), SND_OK);
    SND_REQUIRE(WaitUnloaded(r, hb));
    r.all.clear();
  }
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const snd::HandleRef gb = snd::HandleRef::FromValue(hb.value);
  std::printf("  bank A slot %u generation %u; after %u more loads bank B slot %u generation %u (%.1f s)\n", g1.slot,
              g1.generation, loads + 1u, gb.slot, gb.generation, sec);
  SND_REQUIRE_MSG(gb.slot == g1.slot && (gb.generation & 0xFFFFu) == (g1.generation & 0xFFFFu),
                  "the 16-bit generation did not repeat within %u loads", loads);
  r.all.clear();
  const std::uint64_t hits0 = r.runtime.Stats().cacheHits;
  const snd_playback_handle p2 = r.Play("c");
  for (int i = 0; i < 300 && r.EndOf(p2) == nullptr; ++i) r.Step();
  double e1 = 0, e2 = 0;
  const int which = WhichFits(r, r1, r2, &e1, &e2);
  std::printf("  bank B plays %s (err to A %.3g, to B %.3g), cache hits +%llu\n",
              which == 1 ? "bank A's audio" : (which == 2 ? "its own audio" : "neither"), e1, e2,
              static_cast<unsigned long long>(r.runtime.Stats().cacheHits - hits0));
  SND_CHECK_MSG(which == 2, "bank B does not play its own audio after the generation wrapped");
}

// Record: the memory FS, a new bank under the SAME path after an unload (the
// premise error of the first cache test). Prints which content is played.
SND_TEST(IndepSourceReload, RecordSamePathReloadMemoryFs) {
  Built b1 = Build("rl3", {CacheAsset(&Ramp)});
  Built b2 = Build("rl4", {CacheAsset(&Sine1k)});
  SND_REQUIRE_MSG(b1.ok && b2.ok, "%s %s", b1.diag.c_str(), b2.diag.c_str());
  const Decoded r1 = Reference(b1, "c"), r2 = Reference(b2, "c");
  SND_REQUIRE(r1.ok && r2.ok);
  Rig r;
  SND_REQUIRE(r.Ok());
  const snd_bank_handle h1 = r.Load(b1);
  const snd_playback_handle p1 = r.Play("c");
  for (int i = 0; i < 300 && r.EndOf(p1) == nullptr; ++i) r.Step();
  SND_REQUIRE_EQ(r.sys->UnloadBank(h1, SND_UNLOAD_IF_UNUSED), SND_OK);
  SND_REQUIRE(WaitUnloaded(r, h1));
  r.Load(b2);  // same path "Main.sndb", new bytes in the memory FS
  const snd_playback_handle p2 = r.Play("c");
  for (int i = 0; i < 300 && r.EndOf(p2) == nullptr; ++i) r.Step();
  double e1 = 0, e2 = 0;
  const int which = WhichFits(r, r1, r2, &e1, &e2);
  std::printf("  memory FS, same path: the second load plays %s (err to old %.3g, to new %.3g)\n",
              which == 1 ? "the OLD content" : (which == 2 ? "the new content" : "neither"), e1, e2);
}

// Record: io::IoScheduler over the default (OS) file system. A file is read
// whole (size query, then the bytes), changed on disk, and read again under
// the same path, for: replace by rename (new inode), delete + create (new
// inode), rewrite in place with the same size, rewrite in place larger.
SND_TEST(IndepSourceReload, RecordSamePathReloadDefaultFs) {
  std::error_code ec;
  const sfs::path root = sfs::temp_directory_path(ec) / ("sonus-indep-reopen-" + std::to_string(::getpid()));
  sfs::remove_all(root, ec);
  sfs::create_directories(root, ec);
  auto write = [&](const sfs::path& p, char c, std::size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    std::string s(n, c);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
  };
  snd::IAllocator& al = snd::base::DefaultAllocator();
  snd::io::DefaultFileSystem fs;
  snd::io::DefaultFileSystemConfig fc;
  const std::string rootStr = root.string();
  fc.root = rootStr.c_str();
  SND_REQUIRE(fs.Init(al, fc));
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(al, 64u << 10, 4096, 1));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig ic;
  ic.maxStreams = 1;
  ic.bytesPerWindow = 0;
  SND_REQUIRE(io.Init(al, fs, pool, ic));
  auto readAll = [&](const char* path, std::string* out) -> std::uint64_t {
    snd::io::FileRequest q;
    q.path = path;
    SND_CHECK(io.Submit(q));
    for (int i = 0; i < 20000 && !q.Finished(); ++i) {
      io.RunOnce();
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    const std::uint64_t size = q.Result().fileSize;
    std::string buf(static_cast<std::size_t>(size == UINT64_MAX ? 0 : size + 1000), '\0');
    snd::io::FileRequest d;
    d.path = path;
    d.dst = buf.data();
    d.bytes = buf.size();
    SND_CHECK(io.Submit(d));
    for (int i = 0; i < 20000 && !d.Finished(); ++i) {
      io.RunOnce();
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    buf.resize(static_cast<std::size_t>(d.Result().bytesRead));
    *out = buf;
    return size;
  };
  auto summary = [](const std::string& s) {
    std::string r = s.empty() ? std::string("<empty>") : std::string(1, s.front()) + ".." + std::string(1, s.back());
    return r + " (" + std::to_string(s.size()) + " bytes read)";
  };
  // Each case on its own path: first read (opens and keeps the descriptor), change, read again.
  struct Case {
    const char* name;
    int kind;  // 0 rename over, 1 delete + create, 2 in place same size, 3 in place larger, 4 in place smaller
  };
  const Case cases[] = {{"replaced by rename (new inode)", 0}, {"deleted + created (new inode)", 1},
                        {"rewritten in place, same size", 2}, {"rewritten in place, larger", 3},
                        {"rewritten in place, smaller", 4}};
  int k = 0;
  for (const Case& c : cases) {
    const std::string rel = "bank" + std::to_string(k++) + ".sndb";
    const sfs::path f = root / rel;
    write(f, 'A', 100000);
    std::string got;
    readAll(rel.c_str(), &got);
    switch (c.kind) {
      case 0:
        write(root / "tmp", 'B', 100000);
        sfs::rename(root / "tmp", f, ec);
        break;
      case 1:
        sfs::remove(f, ec);
        write(f, 'B', 100000);
        break;
      case 2: write(f, 'B', 100000); break;
      case 3: write(f, 'B', 150000); break;
      default: write(f, 'B', 50000); break;
    }
    const std::uint64_t size = readAll(rel.c_str(), &got);
    std::printf("  %s: second read size %llu, %s (new content: B)\n", c.name, static_cast<unsigned long long>(size),
                summary(got).c_str());
  }
  io.Shutdown();
  io.Destroy();
  pool.Destroy();
  fs.Shutdown();
  sfs::remove_all(root, ec);
}
