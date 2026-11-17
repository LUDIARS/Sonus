// Independent check of the load-test method of 6f90e8f (process CPU time
// around RenderBlock, compared with 30 % of the block): it must still detect a
// render that is over the budget, not only pass on a busy machine. A light
// load (128 mono resident voices) and a heavy one (128 eight-channel voices:
// eight times the source and resampler work) are measured the same way. In
// Release the heavy one must measure over 30 % (the method detects it) and the
// light one under. Run it with other processes loading the machine to see that
// the measure does not move (shell: busy loops next to this test).
// Also prints the caveat: threads of the same process that are busy during the
// block (here: a spinning helper thread) are counted as rendering.
#include <atomic>
#include <cstdio>
#include <ctime>
#include <thread>
#include <vector>

#include "source_harness.hpp"

using namespace srctest;

namespace {
bool ProcessCpuNs(std::uint64_t* ns) {
  timespec ts{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return false;
  *ns = static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
  return true;
}
// Percent of the block: process CPU time per RenderBlock (as the load tests measure it).
double CpuPercent(std::uint32_t channels, bool spinner) {
  AssetSpec a;
  a.name = "v";
  a.pcm = Signal(channels, 48000, 48000, &Sine1k);
  a.loadForm = "pcm";
  a.codec = "PC16";
  a.extra = "\"loop\": {\"start\": 0, \"end\": 48000}";
  Built b = Build(channels == 1 ? "lcpu1" : "lcpu8", {a});
  if (!b.ok) return -1.0;
  Options o;
  o.rtWorkers = 2;
  o.maxVoiceChannels = 8;
  Rig r(o);
  if (!r.Ok()) return -1.0;
  r.Load(b);
  for (int i = 0; i < 128; ++i) r.Play("v");
  r.Steps(20);
  std::atomic<int> stop{0};
  std::thread spin;
  if (spinner) {
    spin = std::thread([&] {
      while (stop.load(std::memory_order_relaxed) == 0) {
      }
    });
  }
  std::uint64_t sum = 0;
  const int n = 200;
  for (int i = 0; i < n; ++i) {
    r.PumpIo();
    r.sys->Update();
    std::uint64_t c0 = 0, c1 = 0;
    if (!ProcessCpuNs(&c0)) return -1.0;
    r.sys->RenderBlock(r.out.data());
    ProcessCpuNs(&c1);
    sum += c1 - c0;
  }
  stop.store(1);
  if (spin.joinable()) spin.join();
  const double blockNs = 256.0 * 1e9 / 48000.0;
  return 100.0 * double(sum) / n / blockNs;
}
}  // namespace

SND_TEST(IndepLoadCpu, CpuTimeMethodDetectsAnOverBudgetRender) {
  const double light = CpuPercent(1, false);
  const double heavy = CpuPercent(8, false);
  const double lightSpin = CpuPercent(1, true);
  std::printf("  CPU of RenderBlock: 128 mono voices %.1f %%, 128 eight-channel voices %.1f %%; mono with a busy thread "
              "in the same process %.1f %%\n",
              light, heavy, lightSpin);
  SND_REQUIRE(light >= 0.0 && heavy >= 0.0);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
  SND_CHECK_MSG(light < 30.0, "light load %.1f %%", light);
  SND_CHECK_MSG(heavy > 30.0, "the heavy render (%.1f %%) is not over the budget: the method cannot be shown to detect it", heavy);
#endif
}
