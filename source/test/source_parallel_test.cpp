// Concurrency and load of the Sample / Stream kinds: real IO thread, real
// worker pool (prefetch-lane decode jobs) and RT workers (parallel render).
// Repeat under TSan / Release:
//   ctest --repeat until-fail:20 -R source_parallel_test
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <vector>

#include "source_harness.hpp"

#include <unistd.h>

using namespace srctest;

namespace {
// CPU time of the rendering (the audio thread and the RT workers): process CPU
// time, which this test spends only in RenderBlock (no other thread is busy).
// Wall time is not used: a loaded machine (other builds) stretches it.
// Returns false when the clock is not available.
inline bool ProcessCpuNs(std::uint64_t* ns) {
  timespec ts{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return false;
  *ns = static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
  return true;
}
// Without a CPU clock: compare only when the machine is not overloaded.
inline bool MachineCalm() {
  double load[1] = {0.0};
  const long cores = sysconf(_SC_NPROCESSORS_ONLN);
  return getloadavg(load, 1) == 1 && cores > 0 && load[0] < static_cast<double>(cores);
}
AssetSpec Spec(const std::string& name, std::uint64_t frames, const char* form, const char* codec, const char* extra = "") {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(1, 48000, frames, &Sine1k);
  a.loadForm = form;
  a.codec = codec;
  a.extra = extra;
  return a;
}
std::vector<AssetSpec> Mix() {
  std::vector<AssetSpec> as;
  as.push_back(Spec("pcm", 6000, "pcm", "PC16"));
  as.push_back(Spec("pcml", 6000, "pcm", "PC16", "\"loop\": {\"start\": 100, \"end\": 5000}"));
  as.push_back(Spec("rad", 20000, "compressed", "ADPC"));
  as.push_back(Spec("cad", 20000, "decompressCache", "ADPC"));
  as.push_back(Spec("spc", 60000, "stream", "PC16", "\"loop\": {\"start\": 1000, \"end\": 50000}"));
  as.push_back(Spec("sad", 60000, "stream", "ADPC"));
  if (snd::codec::OpusAvailable()) {
    as.push_back(Spec("rop", 20000, "compressed", "OPUS"));
    as.push_back(Spec("sop", 60000, "stream", "OPUS", "\"loop\": {\"start\": 4000, \"end\": 40000}"));
  }
  return as;
}
std::uint32_t Next(std::uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}
}  // namespace

SND_TEST(SourceParallel, RandomPlayStopUnloadWithThreads) {
  const std::vector<AssetSpec> as = Mix();
  Built b = Build("par", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Options o;
  o.threaded = true;
  o.rtWorkers = 2;
  o.maxStreams = 8;
  o.maxDecoders = 16;
  o.fsLatency = 2;
  o.chunkBytes = 8u << 10;
  o.poolBytes = 512u << 10;
  Rig r(o);
  SND_REQUIRE(r.Ok());
  const snd_bank_handle bank = r.Load(b);
  SND_REQUIRE_EQ(r.sys->GetBankState(bank), SND_BANK_STATE_LOADED);
  std::vector<snd_playback_handle> live;
  std::uint32_t rng = 12345;
  for (int blk = 0; blk < 400; ++blk) {
    const std::uint32_t op = Next(rng) % 8u;
    if (op < 4u) {
      live.push_back(r.Play(as[Next(rng) % as.size()].name.c_str()));
    } else if (op == 4u && !live.empty()) {
      const std::size_t i = Next(rng) % live.size();
      r.sys->Stop(live[i], (Next(rng) % 2u) != 0u ? 0.0f : 0.02f, nullptr);
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (blk % 97 == 50) r.fs.SetDefaultLatency(Next(rng) % 20u);
    r.Step();
  }
  // Unload with streams and jobs in flight.
  SND_CHECK_EQ(r.sys->UnloadBank(bank, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  for (int i = 0; i < 4000 && r.sys->GetBankState(bank) != SND_BANK_STATE_UNLOADED; ++i) {
    r.Step();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  SND_CHECK_EQ(r.sys->GetBankState(bank), SND_BANK_STATE_UNLOADED);
  for (int i = 0; i < 400; ++i) {
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
  // Reload and play again after the unload: slots and chunks are reusable.
  const snd_bank_handle again = r.Load(b);
  SND_REQUIRE_EQ(r.sys->GetBankState(again), SND_BANK_STATE_LOADED);
  const snd_playback_handle p = r.Play("sad");
  for (int i = 0; i < 200 && r.sys->GetPlaybackState(p) != SND_PLAYBACK_STATE_PLAYING; ++i) r.Step();
  r.Steps(60);
  SND_CHECK(FirstNonZero(r, 0) != SIZE_MAX);
}

// Load (design 14: 128 real voices < 30 % of the block): 32 streams and 128
// samples at once. Reports the block time; the bound is enforced in Release.
SND_TEST(SourceParallel, LoadStreams32Samples128) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("smp", 48000, "pcm", "PC16", "\"loop\": {\"start\": 0, \"end\": 48000}"));
  as.push_back(Spec("cmp", 48000, "compressed", "ADPC", "\"loop\": {\"start\": 0, \"end\": 48000}"));
  as.push_back(Spec("str", 480000, "stream", "ADPC", "\"loop\": {\"start\": 0, \"end\": 480000}"));
  Built b = Build("load", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (std::uint32_t workers : {0u, 2u}) {
    Options o;
    o.rtWorkers = workers;
    o.maxStreams = 32;
    o.maxDecoders = 64;
    o.chunkBytes = 32u << 10;
    o.poolBytes = 8u << 20;
    o.ringChunks = 4;
    Rig r(o);
    SND_REQUIRE(r.Ok());
    r.Load(b);
    for (int i = 0; i < 32; ++i) r.Play("str");
    for (int i = 0; i < 96; ++i) r.Play("smp");
    for (int i = 0; i < 32; ++i) r.Play("cmp");
    r.Steps(40);
    std::uint64_t sum = 0, worst = 0, cpuSum = 0;
    bool cpuOk = true;
    std::uint32_t n = 0, voices = 0;
    for (int i = 0; i < 300; ++i) {
      r.PumpIo();
      r.sys->Update();  // decode jobs run here (deterministic jobs): not part of the block
      std::uint64_t c0 = 0, c1 = 0;
      cpuOk = ProcessCpuNs(&c0) && cpuOk;
      r.sys->RenderBlock(r.out.data());
      cpuOk = ProcessCpuNs(&c1) && cpuOk;
      cpuSum += c1 - c0;
      const snd::core::BlockProfile& bp = r.sys->LastBlock();
      sum += bp.totalNs;
      worst = bp.totalNs > worst ? bp.totalNs : worst;
      voices = bp.renderedVoices > voices ? bp.renderedVoices : voices;  // late chunks hold voices now and then
      ++n;
    }
    const double budgetNs = 256.0 / 48000.0 * 1e9;
    const double avg = double(sum) / n;
    const double cpu = cpuOk ? double(cpuSum) / n : -1.0;
    std::printf("[ LOAD ] rt workers %u: %u voices (32 streams + 128 samples), CPU %.1f us (%.1f %% of the block); "
                "wall avg %.1f us, max %.1f us\n",
                workers, voices, cpu / 1000.0, 100.0 * cpu / budgetNs, avg / 1000.0, double(worst) / 1000.0);
    SND_CHECK(r.ends.empty());  // nothing refused
    snd_stats cs{};
    r.sys->GetStats(&cs);
    std::printf("[ LOAD ] playbacks %u, real %u, virtual %u (load LOD), streams %u\n", cs.playbacks, cs.real_voices,
                cs.virtual_voices, cs.streams);
    SND_CHECK_EQ(cs.playbacks, 160u);
    SND_CHECK_EQ(cs.streams, 32u);
    const snd::source::RuntimeStats s = r.runtime.Stats();
    std::printf("[ LOAD ] stalls %" PRIu64 ", underruns %" PRIu64 ", jobs %" PRIu64 ", reads %" PRIu64 "\n", s.streamStalls,
                s.sampleUnderruns, s.jobsPosted, s.ioPosted);
    SND_CHECK_EQ(s.streamStalls, 0u);
#if defined(NDEBUG)
    SND_CHECK_EQ(voices, 160u);  // no load LOD in an optimised build
    if (cpu >= 0.0) {
      SND_CHECK(cpu < 0.30 * budgetNs);  // CPU time of the audio thread and the RT workers
    } else if (MachineCalm()) {
      SND_CHECK(avg < 0.30 * budgetNs);
    } else {
      std::printf("[ LOAD ] no CPU clock and the machine is overloaded: recorded, not compared\n");
    }
#endif
  }
}
