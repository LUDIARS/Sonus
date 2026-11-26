#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using namespace synthtest;

SND_TEST(SourceInput, SmallPortKeepsLatestBlockAndCountsShortageOnsets) {
  Driver::Opt options;
  options.inputPortFrames = 64;
  options.blockFrames = 128;
  Driver driver(snd::SourceKind::Input, options);
  SND_REQUIRE(driver.Ok());
  auto* port = driver.runtime.InputPortOf(0);
  SND_REQUIRE(port != nullptr);
  SND_CHECK_EQ(port->Capacity(), 512u);
  port->Configure(0, 1);
  SND_CHECK_EQ(port->MaxLatencyFrames(), 256u);
  float pcm[512]{};
  const float* channels[] = {pcm};
  SND_CHECK_EQ(port->Write(channels, 1, 512), 512u);
  Driver::Voice voice;
  src::InputParams params{};
  SND_REQUIRE(driver.Start(voice, &params, 1));
  SND_CHECK_EQ(port->Buffered(), 128u);
  driver.Block({&voice});
  driver.Block({&voice});
  driver.Block({&voice});
  SND_CHECK_EQ(port->Stats().underruns, 1u);
  SND_CHECK_EQ(port->Write(channels, 1, 128), 128u);
  driver.Block({&voice});
  driver.Block({&voice});
  SND_CHECK_EQ(port->Stats().underruns, 2u);
}
