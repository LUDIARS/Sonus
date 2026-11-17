// Independent tests of core that FAIL ON PURPOSE: defects found while
// re-checking the stage-2 fixes. One executable per defect (CMake defines
// SND_INDEP_DEFECT_<key>), so a defect never hides another.
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "indep_fixture.hpp"

using namespace indep;
using snd::HashId;

namespace {
[[maybe_unused]] std::vector<std::uint8_t> OneEvent(const char* name, float f, std::uint32_t priority = 128) {
  Bank b;
  b.Event(name, f, priority);
  return b.Build("B");
}
}  // namespace

#if defined(SND_INDEP_DEFECT_reregister)
// sonus.h: order is kept per thread. A thread unregisters while its current
// arena cannot be published yet (the producer retires, 38e9fc9) and registers
// again at once: a Play recorded before, a Stop after the re-registration. The
// Stop must stop the Play.
// Cause (desk check): the retired producer's arena is published only by a later
// Update (recorder.cpp UnregisterThread, active = 2), while the thread's new
// producer publishes at once (Flush / Unregister); ApplyCommands applies the
// Stop first (no playback yet: ignored), then the Play.
SND_TEST(IndepDefect, Defect_core_reregistered_thread_reorders_its_commands) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  fx.Steps(2);
  snd_playback_handle p{0};
  std::thread t([&] {
    SND_REQUIRE_EQ(fx.sys->RegisterThread(), SND_OK);
    for (int i = 0; i < 3; ++i) {  // 3 publishes, no block in between: the next arena is retained
      fx.sys->SetParamGlobal(HashId("Junk"), float(i));
      fx.sys->Flush();
    }
    p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);  // retained in the current arena
    SND_CHECK_EQ(fx.sys->UnregisterThread(), SND_OK);    // retires
    SND_REQUIRE_EQ(fx.sys->RegisterThread(), SND_OK);    // a new producer number
    SND_CHECK_EQ(fx.sys->Stop(p, 0.0f, nullptr), SND_OK);
    SND_CHECK_EQ(fx.sys->UnregisterThread(), SND_OK);
  });
  t.join();
  fx.Steps(12);
  SND_CHECK_MSG(fx.Ended(p.value, SND_END_STOPPED), "Stop recorded after Play on the same thread did not stop it");
  SND_CHECK_EQ(fx.LastMean(), 0.0f);
}
#endif

#if defined(SND_INDEP_DEFECT_paused_timed_stop)
// A playback with a timed stop at X is paused before X (silent). When X comes
// it must end STOPPED without sounding again; core makes it audible from the
// start of the block containing X (fading in) and releases it over the short
// fade from X (peak about half its level). Likely cause: once Stopping, the
// playback loop takes the "resume from Paused" path (un-pauses the voices,
// sys_event.cpp) and Render applies the per-sample release from X.
SND_TEST(IndepDefect, Defect_core_timed_stop_of_paused_playback_sounds) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(2);
  const std::size_t x = fx.Total() + 8 * 256 + 100;
  const snd_when w = AtClock(x);
  fx.sys->Stop(p, 0.0f, &w);
  fx.Steps(2);
  fx.sys->Pause(p, 0.0f);
  const std::size_t pausedFrom = fx.Total() + 2 * 256;
  fx.Steps(20);
  std::size_t first = 0, last = 0;
  for (std::size_t f = pausedFrom; f < fx.Total(); ++f) {
    if (fx.At(f) != 0.0f) {
      if (first == 0) first = f;
      last = f;
    }
  }
  SND_CHECK_MSG(fx.MaxAbs(pausedFrom, fx.Total()) == 0.0f, "stop clock %zu: audible again %zu..%zu (peak %f)", x,
                first, last, static_cast<double>(fx.MaxAbs(pausedFrom, fx.Total())));
  SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
}
#endif
