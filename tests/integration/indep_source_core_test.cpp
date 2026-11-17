// Independent tests of the source module through core (written from the spec):
//   - the end of every load form keeps its last frame (core's resampler
//     look-ahead is played out) and impulses stay exactly N-1 frames apart;
//   - the PCM cache must not serve the audio of an unloaded bank to a newly
//     loaded bank (same slot, same address through a recycling allocator, same
//     asset id and length, different content);
//   - how a stream IO error after the start ends the playback (spec gap, printed);
//   - threads: IO thread + worker pool + RT workers with IO holds, a tiny PCM
//     cache (evictions, fallbacks), time / sound priority stalls, Stop and
//     StopAndUnload while everything is in flight; everything returns.
// Uses the harness of source/test (source_harness.hpp, reference.hpp).
// Defect_source_* tests fail on purpose.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#include "reference.hpp"
#include "source_harness.hpp"

using namespace srctest;

namespace {

constexpr std::uint64_t kImpulseFrames = 30011;
double Impulses(std::uint32_t, std::uint64_t i) {
  if (i == 0) return 0.5;
  if (i == kImpulseFrames - 1) return -0.5;
  return 0.0;
}

// Recycles freed blocks for allocations of the same size and alignment (a
// pool allocator; legitimate behind IAllocator). A bank unloaded and a bank of
// the same size loaded next get the same address.
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

std::size_t PeakNear(const Rig& r, std::size_t from, std::size_t to) {
  std::size_t best = from;
  for (std::size_t f = from; f < to && f < r.Frames(); ++f) {
    if (std::fabs(r.At(f, 0)) > std::fabs(r.At(best, 0))) best = f;
  }
  return best;
}

}  // namespace

// A5 through core: two impulses at material frames 0 and N-1. In every load
// form they come out exactly N-1 output frames apart (ratio 1), the last one
// at full amplitude (not cut by the end of the voice: the resampler's
// look-ahead is played out), nothing after it, then COMPLETED.
SND_TEST(IndepSourceCore, EndKeepsTheLastFrameInEveryForm) {
  std::vector<AssetSpec> as;
  for (const char* form : {"pcm", "compressed", "decompressCache", "stream"}) {
    AssetSpec a;
    a.name = std::string("i_") + form;
    a.pcm = Signal(1, 48000, kImpulseFrames, &Impulses);
    a.loadForm = form;
    a.codec = "PC16";
    as.push_back(a);
  }
  Built b = Build("iend", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    Rig r;
    SND_REQUIRE(r.Ok());
    r.Load(b);
    const snd_playback_handle p = r.Play(a.name.c_str());
    for (int i = 0; i < 400 && r.EndOf(p) == nullptr; ++i) r.Step();
    r.Steps(4);
    const EndRec* e = r.EndOf(p);
    SND_REQUIRE_MSG(e != nullptr, "%s did not end", a.name.c_str());
    SND_CHECK_EQ(e->reason, SND_END_COMPLETED);
    const std::size_t f0 = FirstNonZero(r, 0, 1e-3f);
    SND_REQUIRE(f0 != SIZE_MAX);
    const std::size_t p0 = PeakNear(r, f0 >= 16 ? f0 - 16 : 0, f0 + 16);
    const std::size_t p1 = PeakNear(r, p0 + kImpulseFrames - 64, p0 + kImpulseFrames + 64);
    const double a0 = r.At(p0, 0), a1 = r.At(p1, 0);
    SND_CHECK_MSG(p1 - p0 == kImpulseFrames - 1, "%s: impulses %zu frames apart", a.name.c_str(), p1 - p0);
    SND_CHECK_MSG(a0 > 0.3 && std::fabs(a1 + a0) < 0.01 * a0, "%s: first %g last %g", a.name.c_str(), a0, a1);
    double tail = 0;
    for (std::size_t f = p1 + 64; f < r.Frames(); ++f) tail = std::fmax(tail, std::fabs(r.At(f, 0)));
    SND_CHECK_MSG(tail < 1e-4, "%s: %g after the end", a.name.c_str(), tail);
  }
}

// B4: the PCM cache key is (asset id, bank slot, data pointer, frames, codec).
// Unload a bank, load another one of the same size (same slot; a recycling
// allocator returns the same address) whose asset of the same name and length
// has other content: the new bank must play its own audio. Today the cache
// entry of the unloaded bank matches and its PCM is played.
SND_TEST(IndepSourceCore, Defect_source_CacheServesTheAudioOfAnUnloadedBank) {
  AssetSpec a1;
  a1.name = "c";
  a1.pcm = Signal(1, 48000, 30000, &Ramp);
  a1.loadForm = "decompressCache";
  a1.codec = "ADPC";
  AssetSpec a2 = a1;
  a2.pcm = Signal(1, 48000, 30000, &Sine1k);
  Built b1 = Build("stale1", {a1});
  Built b2 = Build("stale2", {a2});
  SND_REQUIRE_MSG(b1.ok && b2.ok, "%s %s", b1.diag.c_str(), b2.diag.c_str());
  SND_REQUIRE_EQ(b1.bank.size(), b2.bank.size());
  const Decoded r1 = Reference(b1, "c");
  const Decoded r2 = Reference(b2, "c");
  SND_REQUIRE(r1.ok && r2.ok);
  Recycling alloc;
  Options o;
  o.allocator = &alloc;
  {
    Rig r(o);
    SND_REQUIRE(r.Ok());
    const snd_bank_handle h1 = r.Load(b1);
    const snd_playback_handle p1 = r.Play("c");
    for (int i = 0; i < 300 && r.EndOf(p1) == nullptr; ++i) r.Step();
    SND_REQUIRE(r.EndOf(p1) != nullptr);
    SND_CHECK_EQ(r.sys->UnloadBank(h1, SND_UNLOAD_IF_UNUSED), SND_OK);
    for (int i = 0; i < 200 && r.sys->GetBankState(h1) != SND_BANK_STATE_UNLOADED; ++i) r.Step();
    SND_REQUIRE_EQ(r.sys->GetBankState(h1), SND_BANK_STATE_UNLOADED);
    r.Load(b2);
    const snd_playback_handle p2 = r.Play("c");
    for (int i = 0; i < 300 && r.EndOf(p2) == nullptr; ++i) r.Step();
    const std::size_t f0 = FirstNonZero(r, 0, 1e-4f);
    SND_REQUIRE(f0 != SIZE_MAX);
    const std::size_t at = f0 >= 8 ? f0 - 8 : 0;
    const std::vector<float> e1 = Resampled(r1.ch[0], r1.ch[0].size() + 64);
    const std::vector<float> e2 = Resampled(r2.ch[0], r2.ch[0].size() + 64);
    Fit best1{0, 1e9}, best2{0, 1e9};
    for (std::size_t s = at; s < at + 40; ++s) {
      const Fit f1 = FitGain(r, 0, s, e1);
      const Fit f2 = FitGain(r, 0, s, e2);
      if (f1.maxErr < best1.maxErr) best1 = f1;
      if (f2.maxErr < best2.maxErr) best2 = f2;
    }
    std::printf("  second bank: fit to its own audio gain %.4f err %.3g; to the unloaded bank's gain %.4f err %.3g; cache hits %llu\n",
                best2.gain, best2.maxErr, best1.gain, best1.maxErr,
                static_cast<unsigned long long>(r.runtime.Stats().cacheHits));
    SND_CHECK_MSG(best2.maxErr < 1e-3 && std::fabs(best2.gain - 0.70710678) < 0.01,
                  "the second bank does not play its own audio (gain %g err %g)", best2.gain, best2.maxErr);
  }
}

// Spec gap (reported, not a defect test): a stream whose reads fail after the
// start ends; VoiceRenderStatus has no error field, so core cannot learn that
// it was SND_END_ERROR / SND_END_DETAIL_STREAM_IO. Prints what the game sees.
SND_TEST(IndepSourceCore, StreamIoErrorAfterTheStartEndReason) {
  AssetSpec a;
  a.name = "e";
  a.pcm = Signal(1, 48000, 96000, &Ramp);
  a.loadForm = "stream";
  a.codec = "PC16";
  AssetSpec g = a;
  g.name = "g";
  Built b = Build("ioerr", {a, g});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Rig r;
  SND_REQUIRE(r.Ok());
  r.Load(b);
  std::string ep, gp;
  for (const auto& f : b.files) (f.first.find("/e_") != std::string::npos ? ep : gp) = f.first;
  r.fs.FailReads(ep, 60000, UINT64_MAX);
  r.fs.RemoveFile(gp);
  const snd_playback_handle pe = r.Play("e");
  const snd_playback_handle pg = r.Play("g");
  r.Steps(300);
  const EndRec* ee = r.EndOf(pe);
  const EndRec* eg = r.EndOf(pg);
  SND_REQUIRE(ee != nullptr && eg != nullptr);
  std::printf("  read error mid-stream: reason %u detail %u; missing file: reason %u detail %u\n", ee->reason, ee->detail,
              eg->reason, eg->detail);
}

// D1-D7 with threads: IO thread, 2 workers, 2 RT workers. Random play / stop,
// IO holds on the stream files, a 2-page PCM cache (evictions, fallbacks to
// the ring), StopAndUnload with everything in flight, reload. Every resource
// returns; no decode / IO error (holds only delay). Run under TSan and
// Release 20 times (ctest --repeat until-fail:20 -R indep_source_core).
SND_TEST(IndepSourceCore, ThreadsHoldsTinyCacheAndUnload) {
  std::vector<AssetSpec> as;
  auto add = [&](const char* name, std::uint64_t frames, const char* form, const char* codec, const char* extra) {
    AssetSpec a;
    a.name = name;
    a.pcm = Signal(1, 48000, frames, &Sine1k);
    a.loadForm = form;
    a.codec = codec;
    a.extra = extra;
    as.push_back(a);
  };
  add("st", 120000, "stream", "PC16", "");
  add("ss", 120000, "stream", "ADPC", "\"stallPolicy\": \"sound\"");
  add("sl", 60000, "stream", "ADPC", "\"loop\": {\"start\": 1000, \"end\": 50001}");
  add("c1", 20000, "decompressCache", "ADPC", "");
  add("c2", 20000, "decompressCache", "ADPC", "");
  add("c3", 20000, "decompressCache", "PC16", "\"loop\": {\"start\": 10, \"end\": 19999}");
  add("ra", 40000, "compressed", "ADPC", "\"loop\": {\"start\": 0, \"end\": 40000}");
  add("pm", 20000, "pcm", "PC16", "");
  if (snd::codec::OpusAvailable()) add("ro", 40000, "compressed", "OPUS", "");
  Built b = Build("ithr", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Options o;
  o.threaded = true;
  o.rtWorkers = 2;
  o.maxStreams = 4;
  o.maxDecoders = 8;
  o.fsLatency = 2;
  o.pcmCacheBytes = 2u * (64u << 10);
  Rig r(o);
  SND_REQUIRE(r.Ok());
  std::uint32_t rng = 777;
  auto next = [&]() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  };
  std::vector<std::string> held;
  for (int round = 0; round < 3; ++round) {
    const snd_bank_handle bank = r.Load(b);
    SND_REQUIRE_EQ(r.sys->GetBankState(bank), SND_BANK_STATE_LOADED);
    std::vector<snd_playback_handle> live;
    for (int blk = 0; blk < 250; ++blk) {
      const std::uint32_t op = next() % 10u;
      if (op < 4u) {
        live.push_back(r.Play(as[next() % as.size()].name.c_str()));
      } else if (op == 4u && !live.empty()) {
        const std::size_t i = next() % live.size();
        r.sys->Stop(live[i], (next() % 2u) != 0u ? 0.0f : 0.01f, nullptr);
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
      } else if (op == 5u && held.empty()) {
        held.push_back(b.files[next() % b.files.size()].first);
        r.fs.Hold(held.back());
      } else if (op == 6u && !held.empty()) {
        r.fs.Release(held.back());
        held.clear();
      }
      r.Step();
    }
    // StopAndUnload while streams, holds, jobs and cache fills are in flight.
    SND_CHECK_EQ(r.sys->UnloadBank(bank, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
    for (int i = 0; i < 50; ++i) r.Step();
    for (const std::string& h : held) r.fs.Release(h);
    held.clear();
    for (int i = 0; i < 4000 && r.sys->GetBankState(bank) != SND_BANK_STATE_UNLOADED; ++i) {
      r.Step();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SND_CHECK_EQ(r.sys->GetBankState(bank), SND_BANK_STATE_UNLOADED);
    for (int i = 0; i < 1000; ++i) {
      r.Step();
      const snd::source::RuntimeStats s = r.runtime.Stats();
      if (s.streamInstances == 0u && s.sampleInstances == 0u && s.decodersInUse == 0u && r.pool.Used() == 0u) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const snd::source::RuntimeStats s = r.runtime.Stats();
    SND_CHECK_EQ(s.streamInstances, 0u);
    SND_CHECK_EQ(s.sampleInstances, 0u);
    SND_CHECK_EQ(s.decodersInUse, 0u);
    SND_CHECK_EQ(r.pool.Used(), 0u);
    SND_CHECK_EQ(s.decodeErrors, 0u);
    SND_CHECK_EQ(s.streamIoErrors, 0u);
    if (round == 2) {
      std::printf("  cache hits %llu misses %llu evictions %llu fallbacks %llu stalls %llu rejected dec %llu slot %llu\n",
                  static_cast<unsigned long long>(s.cacheHits), static_cast<unsigned long long>(s.cacheMisses),
                  static_cast<unsigned long long>(s.cacheEvictions), static_cast<unsigned long long>(s.cacheFallbacks),
                  static_cast<unsigned long long>(s.streamStalls), static_cast<unsigned long long>(s.rejectedDecoder),
                  static_cast<unsigned long long>(s.rejectedStreamSlot));
    }
  }
}

// A primed start (stream without preload) is not followed by a spurious
// Virtualize / Devirtualize from core (sys_event.cpp derives them from the
// previous block's real slot): the stream is seeked once (its start; one
// StreamChannel Cancel) and not primed a second time.
SND_TEST(IndepSourceCore, PrimedStartIsNotReseeked) {
  AssetSpec a;
  a.name = "np";
  a.pcm = Signal(1, 48000, 60000, &Ramp);
  a.loadForm = "stream";
  a.codec = "PC16";
  a.extra = "\"preloadMs\": 0";
  Built b = Build("idevirt", {a});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Options o;
  o.fsLatency = 2;
  o.maxStreams = 1;
  Rig r(o);
  SND_REQUIRE(r.Ok());
  r.Load(b);
  const std::uint32_t e0 = r.io.Channel(0).Epoch();
  const snd_playback_handle p = r.Play("np");
  std::uint32_t primingSteps = 0;
  for (int i = 0; i < 200; ++i) {
    r.Step();
    r.sys->Update();
    if (r.sys->GetPlaybackState(p) == SND_PLAYBACK_STATE_PRIMING) ++primingSteps;
  }
  const std::uint32_t cancels = r.io.Channel(0).Epoch() - e0;
  std::printf("  priming blocks %u, stream channel cancels %u, first sound at frame %zu\n", primingSteps, cancels,
              FirstNonZero(r, 0));
  SND_CHECK_MSG(cancels <= 1u, "%u cancels: the primed stream was re-seeked after its start", cancels);
}
