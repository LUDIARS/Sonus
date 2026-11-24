// G1 x G4. Sample 357 is audible; sample 358 and the remaining block(s)
// must be digital silence even while the path-specific LP has live state.
#include "indep_g1_scene.hpp"

namespace {
using namespace indep_g1;
enum class Ending { Continuous, LoopCount, Scheduled };

std::vector<float> StopSignal(bool wet, Ending ending) {
  auto b = Signal(wet, 0, 179);
  if (ending == Ending::LoopCount) b.events[0].tracks[0].clips[0].loopCount = 2;
  Scene s(b);
  SND_CHECK_EQ(s.h.sys->SetParamEmitter(s.em, HashId("snd.occlusion"), 1), SND_OK);
  if (wet) {
    const auto zone = s.h.sys->CreateEmitter();
    const snd_vec3 ext{1, 1, 1};
    SND_CHECK_EQ(s.h.sys->SetEmitterShape(zone, SND_EMITTER_SHAPE_BOX, &ext), SND_OK);
    SND_CHECK_EQ(s.h.sys->SetReverbZone(zone, kWet, 1, 4), SND_OK);
  }
  s.h.Steps(3);
  const auto now = s.h.TotalFrames();
  auto playback = s.Play();
  if (ending == Ending::Scheduled) {
    snd_when when{};
    when.kind = SND_WHEN_CLOCK_SAMPLE;
    when.value = now + 358;
    SND_CHECK_EQ(s.h.sys->SetScheduledStop(playback, &when, 0), SND_OK);
  }
  s.h.Steps(4);
  return s.h.all;
}

void CheckEnd(bool wet, Ending ending) {
  const auto continuing = StopSignal(wet, Ending::Continuous);
  const auto stopped = StopSignal(wet, ending);
  SND_REQUIRE_EQ(stopped.size(), continuing.size());
  SND_REQUIRE(stopped.size() >= 2 * 1024);
  for (std::size_t ch = 0; ch < 2; ++ch) {
    SND_CHECK(std::fabs(stopped[357 * 2 + ch]) > 1e-7f);
    SND_CHECK(std::fabs(continuing[358 * 2 + ch]) > 0.001f);
    std::size_t residual = 0;
    for (auto i = 358 * 2 + ch; i < stopped.size(); i += 2)
      if (stopped[i] != 0.0f) ++residual;
    SND_CHECK_EQ(residual, 0u);
  }
}

SND_TEST(IndepG1Stop, FiniteLoopCutsDirectFilterAtItsLastSample) { CheckEnd(false, Ending::LoopCount); }
SND_TEST(IndepG1Stop, FiniteLoopCutsZoneSendFilterAtItsLastSample) { CheckEnd(true, Ending::LoopCount); }
SND_TEST(IndepG1Stop, ScheduledStopCutsDirectFilterAtItsClock) { CheckEnd(false, Ending::Scheduled); }
SND_TEST(IndepG1Stop, ScheduledStopCutsZoneSendFilterAtItsClock) { CheckEnd(true, Ending::Scheduled); }
}  // namespace
