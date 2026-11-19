// Independent tests: music through the C API (CONTRACTS.md D-88, D-89).
// Properties (from the spec, before reading the implementation):
//  M1 (D-88) the transport pitch is applied to the voices of the music playback that owns
//     the transport, NOT to a stinger's child playback ("Stinger など子 Playback ...
//     には掛けない"): with pitch 2, a 500 Hz music sine is heard at 1000 Hz and a 700 Hz
//     stinger at 700 Hz.
//  M2 (D-88) a pitch on a standalone transport does not change the pitch of a sound
//     played quantised on it (only its tick rate).
//  M3 (D-89) snd_play_stinger on a playback handle that ended is INVALID_HANDLE once the
//     slot is known stale on the recording side.
#include <cmath>
#include <string>
#include <vector>

#include "api_rig.hpp"
#include "music_fixture.hpp"

using apitest::Level;
using apitest::Rig;
using mt::kBar;
using mt::kBeat;
using mt::MusicProject;
using snd::HashId;

namespace {

mt::bt::PcmData Sine(double hz, double amp, std::uint64_t frames) {
  mt::bt::PcmData s;
  s.channels = 1;
  s.sampleRate = 48000;
  s.frames = frames;
  s.samples.resize(static_cast<std::size_t>(frames));
  for (std::size_t i = 0; i < s.samples.size(); ++i) {
    s.samples[i] = static_cast<float>(amp * std::sin(6.283185307179586 * hz * double(i) / 48000.0));
  }
  return s;
}

std::vector<std::uint8_t> Bank() {
  MusicProject p("indep-pitch");
  p.Asset("m", Sine(500.0, 0.2, 8 * kBar));
  p.Asset("st", Sine(700.0, 0.3, 2 * kBar));
  p.Asset("fx", Sine(600.0, 0.3, 2 * kBar));
  p.Event("St", "\"stinger\": true, \"tracks\": [{\"clips\": [{\"asset\": \"st\"}]}]");
  p.Event("Fx", "\"tracks\": [{\"clips\": [{\"asset\": \"fx\"}]}]");
  p.Segment("M", "m", 8);
  p.Playlist("PL", "{\"segment\": \"M\"}");
  p.MusicEvent("Music",
               "\"initialState\": \"S\", \"states\": [{\"name\": \"S\", \"playlist\": \"PL\"}], "
               "\"stingers\": [{\"name\": \"Hit\", \"event\": \"St\", \"timing\": \"nextBeat\", \"overlap\": \"stack\"}]");
  std::string diag;
  std::vector<std::uint8_t> b = p.Build(&diag);
  if (b.empty()) std::fprintf(stderr, "%s\n", diag.c_str());
  return b;
}

std::uint64_t RenderClock(Rig& r) { return r.Driver()->Stats().core.blocksRendered * 256u; }

}  // namespace

SND_TEST(IndepMusic, TransportPitchIsNotAppliedToStingers) {
  const std::vector<std::uint8_t> bank = Bank();
  SND_REQUIRE(!bank.empty());
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE(r.LoadBank("Main.sndb", bank));
  const snd_playback_handle m = snd_play(r.sys, HashId("Music"), snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(m.value != 0u);
  r.RunTo(r.Frames() + kBar);
  SND_CHECK_EQ(snd_set_transport_pitch(r.sys, snd_get_playback_transport(m), 2.0f, nullptr), SND_OK);
  r.RunTo(r.Frames() + kBeat);
  SND_CHECK_EQ(snd_play_stinger(r.sys, m, HashId("Hit"), nullptr), SND_OK);
  const std::uint64_t called = RenderClock(r);
  r.RunTo(r.Frames() + kBar);
  snd_update(r.sys);
  // The stinger starts within a beat (now 12000 samples); measure well after it.
  const std::uint64_t at = called + kBeat;
  SND_CHECK_NEAR(Level(r, at, 16384, 1000.0), 0.2, 0.01);  // the music, pitched x2
  SND_CHECK(Level(r, at, 16384, 500.0) < 0.01);
  SND_CHECK_NEAR(Level(r, at, 16384, 700.0), 0.3, 0.01);   // the stinger, not pitched
  SND_CHECK(Level(r, at, 16384, 1400.0) < 0.01);
}

SND_TEST(IndepMusic, StandaloneTransportPitchDoesNotPitchItsSounds) {
  const std::vector<std::uint8_t> bank = Bank();
  SND_REQUIRE(!bank.empty());
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE(r.LoadBank("Main.sndb", bank));
  const snd_transport_desc td{120.0f, 4, 4, 1, 0};
  const snd_transport_handle t = snd_create_transport(r.sys, &td);
  SND_REQUIRE(t.value != 0u);
  SND_CHECK_EQ(snd_set_transport_pitch(r.sys, t, 2.0f, nullptr), SND_OK);
  r.Step(4);
  snd_when w{};
  w.kind = SND_WHEN_NEXT_BEAT;
  w.transport = t;
  const snd_playback_handle p = snd_play(r.sys, HashId("Fx"), snd_emitter_handle{0}, &w);
  SND_REQUIRE(p.value != 0u);
  const std::uint64_t called = RenderClock(r);
  r.RunTo(r.Frames() + kBar);
  snd_update(r.sys);
  const std::uint64_t at = called + kBeat;
  SND_CHECK_NEAR(Level(r, at, 16384, 600.0), 0.3, 0.01);
  SND_CHECK(Level(r, at, 16384, 1200.0) < 0.01);
}

SND_TEST(IndepMusic, StingerOnAnEndedPlaybackIsAStaleHandle) {
  const std::vector<std::uint8_t> bank = Bank();
  SND_REQUIRE(!bank.empty());
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE(r.LoadBank("Main.sndb", bank));
  const snd_playback_handle m = snd_play(r.sys, HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.Step(8);
  SND_CHECK_EQ(snd_stop(r.sys, m, 0.0f, nullptr), SND_OK);
  bool ended = false;
  for (int i = 0; i < 40 && !ended; ++i) {
    r.Step(1);
    for (const apitest::End& e : r.ev.ends) ended = ended || e.handle == m.value;
  }
  SND_REQUIRE(ended);
  snd_update(r.sys);  // the slot is back on the recording side
  SND_CHECK_EQ(snd_play_stinger(r.sys, m, HashId("Hit"), nullptr), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_set_transport_pitch(r.sys, snd_get_playback_transport(m), 2.0f, nullptr), SND_ERR_INVALID_HANDLE);
}
