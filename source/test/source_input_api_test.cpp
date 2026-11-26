// snd_write_input (D-195) through the C API: a system on the MANUAL output device,
// an input playback (snd_play_input) of port 0, the game writing a sine between
// periods; the output carries it. Entry rules of snd_write_input.
#include <cmath>
#include <cstdint>
#include <vector>

#include "snd/sonus.h"
#include "snd/test/test.hpp"
#include "synth_driver.hpp"

SND_TEST(SourceInputApi, WriteInputFeedsAnInputPlayback) {
  snd_config cfg;
  snd_config_init(&cfg);
  cfg.output.device = nullptr;
  cfg.output_kind = SND_OUTPUT_MANUAL;
  cfg.jobs.realtime_workers = 0;
  snd_system* sys = nullptr;
  SND_REQUIRE_EQ(snd_create(&cfg, nullptr, nullptr, &sys), SND_OK);
  const std::uint32_t block = cfg.block_size;
  // Entry rules.
  std::vector<float> chunk(block, 0.0f);
  const float* ch[1] = {chunk.data()};
  SND_CHECK_EQ(snd_write_input(nullptr, 0, ch, 1, block), 0u);
  SND_CHECK_EQ(snd_write_input(sys, 0, nullptr, 1, block), 0u);
  SND_CHECK_EQ(snd_write_input(sys, 0, ch, 0, block), 0u);
  SND_CHECK_EQ(snd_write_input(sys, 99, ch, 1, block), 0u);
  // Play port 0 (mono, centre of the stereo master).
  snd_input_desc d{};
  d.device_index = 0;
  d.channels = 1;
  const snd_playback_handle p = snd_play_input(sys, &d, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  snd_update(sys);
  std::vector<float> out(std::size_t{block} * 2u);
  for (int i = 0; i < 4; ++i) {
    SND_REQUIRE_EQ(snd_advance_output(sys, 1, out.data(), block), SND_OK);
    snd_update(sys);
  }
  // Feed 750 Hz, one period of output per block written (render-ahead keeps a few
  // blocks in flight: the port's backlog absorbs it).
  std::vector<float> all;
  std::uint64_t n = 0;
  for (int b = 0; b < 120; ++b) {
    for (float& v : chunk) v = static_cast<float>(0.5 * std::sin(6.283185307179586 * 750.0 * double(n++) / 48000.0));
    SND_CHECK_EQ(snd_write_input(sys, 0, ch, 1, block), block);
    SND_REQUIRE_EQ(snd_advance_output(sys, 1, out.data(), block), SND_OK);
    snd_update(sys);
    for (std::uint32_t f = 0; f < block; ++f) all.push_back(out[std::size_t{f} * 2u]);
  }
  const std::size_t from = all.size() - 8192;
  const double a = synthtest::Tone(all, from, 8192, 750.0);
  SND_CHECK_MSG(a > 0.25, "750 Hz %f", a);
  SND_CHECK(synthtest::Tone(all, from, 8192, 1500.0) < 0.02 * a);
  SND_CHECK_EQ(snd_get_playback_state(sys, p), SND_PLAYBACK_STATE_PLAYING);
  snd_destroy(sys);
}
