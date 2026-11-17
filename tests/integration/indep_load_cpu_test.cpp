// Independent check of the load-test method of 6f90e8f (process CPU time
// around RenderBlock, compared with 30 % of the block): the measure must
// follow the work, not the machine load, and still show a render that is over
// the budget. A light load (128 mono resident voices) and a heavy one (128
// eight-channel voices, about 3 x the work) are measured the same way. Release:
// the light one under 30 %, the heavy one well above the light one (Release
// cannot be made over 30 % here: about 17 %). Unoptimised builds: the heavy
// one is far over 30 % (158 % in gcc Debug), i.e. the comparison would fail.
// Run it next to busy loops of other processes: the values do not move.
// Also prints the caveat: a busy thread of the same process is counted.
#include <atomic>
#include <cstdio>
#include <string>
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
double CpuPercent(std::uint32_t channels, std::uint32_t rate, int voices, bool spinner) {
  AssetSpec a;
  a.name = "v";
  a.pcm = Signal(channels, rate, 48000, &Sine1k);
  a.loadForm = "pcm";
  a.codec = "PC16";
  a.extra = "\"loop\": {\"start\": 0, \"end\": 48000}";
  Built b = Build("lcpu" + std::to_string(channels) + "_" + std::to_string(rate), {a});
  if (!b.ok) return -1.0;
  Options o;
  o.rtWorkers = 2;
  o.maxVoiceChannels = 8;
  Rig r(o);
  if (!r.Ok()) return -1.0;
  r.Load(b);
  for (int i = 0; i < voices; ++i) r.Play("v");
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
  const double light = CpuPercent(1, 48000, 128, false);
  const double heavy = CpuPercent(8, 48000, 128, false);  // 8 channels: about 3 x the work
  const double lightSpin = CpuPercent(1, 48000, 128, true);
  std::printf("  CPU of RenderBlock: 128 mono voices %.1f %%, 128 eight-channel voices %.1f %%; mono with a busy thread "
              "in the same process %.1f %%\n",
              light, heavy, lightSpin);
  SND_REQUIRE(light >= 0.0 && heavy >= 0.0);
  // The measure follows the work (heavy about 3 x light), whatever the machine load.
  SND_CHECK_MSG(heavy > 2.0 * light, "heavy %.1f %% is not well above light %.1f %%", heavy, light);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
  SND_CHECK_MSG(light < 30.0, "light load %.1f %%", light);
#else
  // Unoptimised builds: the heavy render is far over 30 % and the measure shows it
  // (the load tests compare only in Release; this is the detection, shown here).
  SND_CHECK_MSG(heavy > 30.0, "heavy %.1f %%", heavy);
#endif
}
