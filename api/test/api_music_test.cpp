// Music through the C API (8, CONTRACTS.md D-76, D-88, D-89): the composed music
// system (one per snd_system), snd_play_stinger with the stinger's own timing
// (MSTG.timing) and with an overriding When, overlap replace / stack, and
// snd_set_transport_pitch changing tempo and pitch together. Bar and beat clocks
// come from on_beat; the output is checked analytically (levels add up, the
// sinc resampler moves a step by at most a sample, sine frequencies by DFT).
#include <cmath>
#include <string>
#include <vector>

#include "api_rig.hpp"
#include "music_fixture.hpp"

using namespace apitest;
using mt::kBar;
using mt::kBeat;
using mt::Material;
using mt::MusicProject;
using snd::HashId;

namespace {

const char* kOneState = "\"initialState\": \"S\", \"states\": [{\"name\": \"S\", \"playlist\": \"PL\"}]";

std::vector<std::uint8_t> StingerBank() {
  MusicProject p("api-stingers");
  p.Asset("m", Material(4 * kBar, 0.1f));
  p.Asset("st", Material(4800, 0.4f));
  p.Asset("st2", Material(4800, 0.2f));
  p.Event("St", "\"stinger\": true, \"tracks\": [{\"clips\": [{\"asset\": \"st\"}]}]");
  p.Event("St2", "\"stinger\": true, \"tracks\": [{\"clips\": [{\"asset\": \"st2\"}]}]");
  p.Segment("M", "m", 4);
  p.Playlist("PL", "{\"segment\": \"M\"}");
  p.MusicEvent("Music", std::string(kOneState) +
                            ", \"stingers\": [{\"name\": \"Hit\", \"event\": \"St\", \"timing\": \"nextBar\", \"overlap\": \"replace\"},"
                            " {\"name\": \"Tap\", \"event\": \"St2\", \"timing\": \"nextBeat\", \"overlap\": \"stack\"}]");
  std::string diag;
  std::vector<std::uint8_t> b = p.Build(&diag);
  if (b.empty()) std::fprintf(stderr, "%s\n", diag.c_str());
  return b;
}

std::vector<std::uint64_t> BeatClocks(const Rig& r, bool barsOnly) {
  std::vector<std::uint64_t> v;
  for (const Beat& b : r.ev.beats) {
    if (!barsOnly || b.beat == 0) v.push_back(b.clock);
  }
  return v;
}

// First clock in [from, to) where the (normalised) output rises above `level`.
std::uint64_t Rise(const Rig& r, std::uint64_t from, std::uint64_t to, float level) {
  for (std::uint64_t c = from; c < to && c < r.Frames(); ++c) {
    if (r.At(c) > level) return c;
  }
  return UINT64_MAX;
}

// The next grid point >= `at` among `points`.
std::uint64_t NextOf(const std::vector<std::uint64_t>& points, std::uint64_t at) {
  for (std::uint64_t p : points) {
    if (p >= at) return p;
  }
  return UINT64_MAX;
}

std::uint64_t RenderClock(Rig& r) {  // common clock of the next block the audio thread renders
  return r.Driver()->Stats().core.blocksRendered * 256u;
}

}  // namespace

SND_TEST(ApiMusic, StingerOwnTimingIsTheNextBar) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE(r.LoadBank("Main.sndb", StingerBank()));
  const snd_playback_handle m = snd_play(r.sys, HashId("Music"), snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(m.value != 0u);
  r.RunTo(r.Frames() + kBar + kBar / 2);  // into the middle of bar 1: far from a bar line
  const std::uint64_t called = RenderClock(r);
  SND_CHECK_EQ(snd_play_stinger(r.sys, m, HashId("Hit"), nullptr), SND_OK);
  r.RunTo(r.Frames() + 2 * kBar);
  snd_update(r.sys);
  const std::vector<std::uint64_t> bars = BeatClocks(r, true);
  SND_REQUIRE(bars.size() >= 3u);
  const std::uint64_t bar = NextOf(bars, called);
  SND_REQUIRE(bar != UINT64_MAX);
  // 0.1 (music) -> 0.5 (+ stinger): the step crosses 0.3 at the bar line (+-1 sample).
  const std::uint64_t on = Rise(r, called > 2048 ? called - 2048 : 0, r.Frames(), 0.3f);
  SND_CHECK_MSG(mt::Diff(on, bar) <= 1u, "stinger at %llu, bar line at %llu", (unsigned long long)on,
                (unsigned long long)bar);
  SND_CHECK_NEAR(r.At(bar + 2000), 0.5f, 1e-3f);
  SND_CHECK_NEAR(r.At(bar + 6000), 0.1f, 1e-3f);  // 4800 frames later it is over
}

SND_TEST(ApiMusic, StingerWhenOverridesItsTiming) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE(r.LoadBank("Main.sndb", StingerBank()));
  const snd_playback_handle m = snd_play(r.sys, HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(r.Frames() + kBar + kBeat + kBeat / 2);  // the middle of a beat
  const std::uint64_t called = RenderClock(r);
  const snd_when w = mt::Kind(snd_get_playback_transport(m), SND_WHEN_NEXT_BEAT);
  SND_CHECK_EQ(snd_play_stinger(r.sys, m, HashId("Hit"), &w), SND_OK);  // Hit's own timing is nextBar
  r.RunTo(r.Frames() + kBar);
  snd_update(r.sys);
  const std::vector<std::uint64_t> beats = BeatClocks(r, false);
  const std::uint64_t beat = NextOf(beats, called);
  SND_REQUIRE(beat != UINT64_MAX);
  const std::vector<std::uint64_t> bars = BeatClocks(r, true);
  SND_CHECK(NextOf(bars, called) != beat);  // the override is not the bar line
  const std::uint64_t on = Rise(r, called > 2048 ? called - 2048 : 0, r.Frames(), 0.3f);
  SND_CHECK_MSG(mt::Diff(on, beat) <= 1u, "stinger at %llu, beat at %llu", (unsigned long long)on,
                (unsigned long long)beat);
}

SND_TEST(ApiMusic, StingerOverlapReplaceAndStack) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE(r.LoadBank("Main.sndb", StingerBank()));
  const snd_playback_handle m = snd_play(r.sys, HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(r.Frames() + kBar + kBar / 2);
  std::uint64_t called = RenderClock(r);
  // Replace: two requests of Hit before its bar line -> one stinger (0.1 + 0.4, not 0.9).
  SND_CHECK_EQ(snd_play_stinger(r.sys, m, HashId("Hit"), nullptr), SND_OK);
  r.Step(1);
  SND_CHECK_EQ(snd_play_stinger(r.sys, m, HashId("Hit"), nullptr), SND_OK);
  r.RunTo(r.Frames() + 2 * kBar);
  snd_update(r.sys);
  std::uint64_t bar = NextOf(BeatClocks(r, true), called);
  SND_REQUIRE(bar != UINT64_MAX);
  SND_CHECK_NEAR(r.At(bar + 2000), 0.5f, 1e-3f);
  // Stack: two requests of Tap (0.2 each, next beat) -> both sound: 0.1 + 0.4.
  r.RunTo(r.Frames() + kBeat / 2);
  called = RenderClock(r);
  SND_CHECK_EQ(snd_play_stinger(r.sys, m, HashId("Tap"), nullptr), SND_OK);
  SND_CHECK_EQ(snd_play_stinger(r.sys, m, HashId("Tap"), nullptr), SND_OK);
  r.RunTo(r.Frames() + kBar);
  snd_update(r.sys);
  const std::uint64_t beat = NextOf(BeatClocks(r, false), called);
  SND_REQUIRE(beat != UINT64_MAX);
  SND_CHECK_NEAR(r.At(beat + 2000), 0.5f, 1e-3f);
  // An unknown stinger plays nothing (and is not an error at the entry).
  const std::uint64_t quiet = r.Frames();
  SND_CHECK_EQ(snd_play_stinger(r.sys, m, HashId("Nope"), nullptr), SND_OK);
  r.RunTo(r.Frames() + 2 * kBar);
  for (std::uint64_t c = quiet + 4096; c < r.Frames(); c += 97) SND_CHECK_NEAR(r.At(c), 0.1f, 1e-3f);
}

SND_TEST(ApiMusic, TransportPitchChangesTempoAndPitchTogether) {
  MusicProject p("api-pitch");
  mt::bt::PcmData sine;
  sine.channels = 1;
  sine.sampleRate = 48000;
  sine.frames = 8 * kBar;
  sine.samples.resize(static_cast<std::size_t>(sine.frames));
  for (std::size_t i = 0; i < sine.samples.size(); ++i) {
    sine.samples[i] = static_cast<float>(0.4 * std::sin(6.283185307179586 * 500.0 * double(i) / 48000.0));
  }
  p.Asset("s", sine);
  p.Segment("S", "s", 8);
  p.Playlist("PL", "{\"segment\": \"S\"}");
  p.MusicEvent("Music", kOneState);
  std::string diag;
  const std::vector<std::uint8_t> bank = p.Build(&diag);
  SND_CHECK_MSG(!bank.empty(), "%s", diag.c_str());
  SND_REQUIRE(!bank.empty());
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE(r.LoadBank("Main.sndb", bank));
  const snd_playback_handle m = snd_play(r.sys, HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(r.Frames() + kBar);
  const std::uint64_t before = r.Frames() - 16384;
  const std::uint64_t called = RenderClock(r);
  SND_CHECK_EQ(snd_set_transport_pitch(r.sys, snd_get_playback_transport(m), 2.0f, nullptr), SND_OK);
  r.RunTo(r.Frames() + kBar);
  snd_update(r.sys);
  // Pitch: 500 Hz before, 1000 Hz after (the voice follows the transport's pitch).
  SND_CHECK_NEAR(Level(r, before, 16384, 500.0), 0.4, 0.01);
  SND_CHECK(Level(r, before, 16384, 1000.0) < 0.01);
  const std::uint64_t after = called + 8192;
  SND_CHECK_NEAR(Level(r, after, 16384, 1000.0), 0.4, 0.01);
  SND_CHECK(Level(r, after, 16384, 500.0) < 0.01);
  // Tempo: beats every 24000 samples at 120 BPM, then every 12000 (x 2).
  const std::vector<std::uint64_t> beats = BeatClocks(r, false);
  std::uint32_t slow = 0, fast = 0, other = 0;
  for (std::size_t i = 1; i < beats.size(); ++i) {
    const std::uint64_t d = beats[i] - beats[i - 1];
    if (beats[i] <= called) {
      slow += d == kBeat ? 1u : 0u;
    } else if (beats[i - 1] >= called + 512) {
      fast += d == kBeat / 2 ? 1u : 0u;
      other += d == kBeat / 2 ? 0u : 1u;
    }
  }
  SND_CHECK(slow >= 3u);
  SND_CHECK(fast >= 6u);
  SND_CHECK_EQ(other, 0u);
  // Back to 1: 500 Hz again.
  SND_CHECK_EQ(snd_set_transport_pitch(r.sys, snd_get_playback_transport(m), 1.0f, nullptr), SND_OK);
  const std::uint64_t back = RenderClock(r);
  r.RunTo(r.Frames() + kBar);
  SND_CHECK_NEAR(Level(r, back + 8192, 16384, 500.0), 0.4, 0.01);
}
