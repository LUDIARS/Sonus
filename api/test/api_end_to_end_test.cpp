// End to end through the C API only (13): project (JSON + WAV) -> buildtool ->
// bank + stream file in the game's file system (snd_file_system table) ->
// snd_create (IO scheduler, source kinds, music, core, audio driver) ->
// snd_load_bank_async / on_bank_loaded -> snd_play of a resident tone, an ADPCM
// tone and a looping stream -> the device output, checked per tone by DFT ->
// snd_stop with a fade, on_end, snd_get_position, meters, statistics ->
// languages (snd_declare_language / snd_set_language) -> snd_unload_bank and
// the release of every resource.
#include <cmath>
#include <string>
#include <vector>

#include "api_rig.hpp"
#include "mini_project.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/codec/codec.hpp"
#include "snd/source/runtime.hpp"

using namespace apitest;
namespace bt = snd::buildtool;
using snd::HashId;

namespace {

bt::PcmData Tone(std::uint32_t rate, std::uint64_t frames, double hz, double amp) {
  bt::PcmData p;
  p.channels = 1;
  p.sampleRate = rate;
  p.frames = frames;
  p.samples.resize(static_cast<std::size_t>(frames));
  for (std::uint64_t f = 0; f < frames; ++f) {
    p.samples[static_cast<std::size_t>(f)] = static_cast<float>(amp * std::sin(6.283185307179586 * hz * double(f) / rate));
  }
  return p;
}

}  // namespace

SND_TEST(ApiEndToEnd, BankTonesStreamStopUnload) {
  mini::Project proj("api-e2e");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true, \"channels\": 2");
  proj.Wav("audio/fx.wav", Tone(44100, 44100 * 2, 440.0, 0.4), bt::WavSampleFormat::Float32);
  proj.Wav("audio/adp.wav", Tone(48000, 96000, 3000.0, 0.3), bt::WavSampleFormat::Float32);
  proj.Wav("audio/bgm.wav", Tone(48000, 96000, 1000.0, 0.5), bt::WavSampleFormat::Float32);
  proj.Add("assets/fx.json", "asset", "fx", "\"file\": \"audio/fx.wav\", \"loadForm\": \"pcm\"");
  proj.Add("assets/adp.json", "asset", "adp",
           "\"file\": \"audio/adp.wav\", \"loadForm\": \"compressed\", \"convert\": {\"default\": {\"codec\": \"ADPC\"}}");
  proj.Add("assets/bgm.json", "asset", "bgm",
           "\"file\": \"audio/bgm.wav\", \"loadForm\": \"stream\", \"loop\": {\"start\": 0, \"end\": 96000}, "
           "\"convert\": {\"default\": {\"codec\": \"ADPC\"}}");
  proj.Add("events/Fx.json", "event", "Fx", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"fx\"}]}]");
  proj.Add("events/Adp.json", "event", "Adp", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"adp\"}]}]");
  proj.Add("events/Bgm.json", "event", "Bgm", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"bgm\"}]}]");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics diag;
  bt::BuildResult res;
  const bool built = proj.Build(o, &diag, &res);
  SND_CHECK_MSG(built, "%s", mini::Dump(diag).c_str());
  SND_REQUIRE(built);
  SND_REQUIRE_EQ(res.streams.size(), 1u);

  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  r.fs.SetDefaultLatency(1);
  r.fs.AddFile(res.streams[0], mini::ReadBytes(proj.out() / res.streams[0]));
  snd_bank_handle bank{};
  SND_REQUIRE(r.LoadBank("Main.sndb", mini::ReadBytes(proj.out() / "Main.sndb"), &bank));
  bool loaded = false;
  for (const auto& b : r.ev.banks) loaded = loaded || (b.first == bank.value && b.second == SND_OK);
  SND_CHECK(loaded);  // on_bank_loaded, once

  const snd_playback_handle fx = snd_play(r.sys, HashId("Fx"), snd_emitter_handle{0}, nullptr);
  const snd_playback_handle adp = snd_play(r.sys, HashId("Adp"), snd_emitter_handle{0}, nullptr);
  const snd_playback_handle bgm = snd_play(r.sys, HashId("Bgm"), snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(fx.value != 0u && adp.value != 0u && bgm.value != 0u);
  const std::uint64_t start = r.Frames();
  r.RunTo(start + 48000);
  // Mono voices on the stereo master: x 0.7071 (At() divides it out).
  const std::uint64_t w = start + 12000;
  SND_CHECK_NEAR(Level(r, w, 16384, 440.0), 0.4, 0.01);
  SND_CHECK_NEAR(Level(r, w, 16384, 3000.0), 0.3, 0.01);
  SND_CHECK_NEAR(Level(r, w, 16384, 1000.0), 0.5, 0.015);
  snd_update(r.sys);
  SND_CHECK_EQ(snd_get_playback_state(r.sys, fx), SND_PLAYBACK_STATE_PLAYING);
  snd_position pos{};
  SND_CHECK_EQ(snd_get_position(r.sys, fx, &pos), SND_OK);
  SND_CHECK(pos.frames > 30000u && pos.frames < 60000u);  // about a second played
  SND_CHECK_NEAR(pos.seconds, static_cast<double>(pos.frames) / 48000.0, 1e-9);
  snd_meter meter{};
  SND_CHECK_EQ(snd_get_meter(r.sys, 0u, &meter), SND_OK);
  SND_CHECK_EQ(meter.channels, 2u);
  SND_CHECK(meter.peak[0] > 0.5f * 0.7071f && meter.peak[0] < 1.3f * 0.7071f);  // 0.4 + 0.3 + 0.5 peaks, partly
  snd_stats st{};
  SND_CHECK_EQ(snd_get_stats(r.sys, &st), SND_OK);
  SND_CHECK_EQ(st.playbacks, 3u);
  SND_CHECK_EQ(st.streams, 1u);
  // Stop the effect with a 0.1 s fade: on_end STOPPED, its tone is gone afterwards.
  SND_CHECK_EQ(snd_stop(r.sys, fx, 0.1f, nullptr), SND_OK);
  const std::uint64_t stopAt = r.Driver()->Stats().core.blocksRendered * 256u;
  r.RunTo(r.Frames() + 48000);
  snd_update(r.sys);
  bool stopped = false;
  for (const End& e : r.ev.ends) stopped = stopped || (e.handle == fx.value && e.reason == SND_END_STOPPED);
  SND_CHECK(stopped);
  SND_CHECK(Level(r, stopAt + 4800 + 256, 16384, 440.0) < 0.002);
  SND_CHECK_NEAR(Level(r, stopAt + 4800 + 256, 16384, 1000.0), 0.5, 0.015);  // the stream loops on
  // Languages (D-50): declare, switch; an undeclared id is NOT_FOUND.
  SND_CHECK_EQ(snd_declare_language(r.sys, "ja"), SND_OK);
  SND_CHECK_EQ(snd_declare_language(r.sys, "ja"), SND_OK);  // again: nothing
  SND_CHECK_EQ(snd_set_language(r.sys, snd_hash_id("ja")), SND_OK);
  SND_CHECK_EQ(snd_set_language(r.sys, snd_hash_id("fr")), SND_ERR_NOT_FOUND);
  // Unload while the stream plays: discarded, then everything is released.
  SND_CHECK_EQ(snd_unload_bank(r.sys, bank, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  for (int i = 0; i < 400 && snd_get_bank_state(r.sys, bank) != SND_BANK_STATE_UNLOADED; ++i) r.Step();
  SND_CHECK_EQ(snd_get_bank_state(r.sys, bank), SND_BANK_STATE_UNLOADED);
  snd_update(r.sys);
  bool discarded = false;
  for (const End& e : r.ev.ends) discarded = discarded || (e.handle == bgm.value && e.reason == SND_END_DISCARDED);
  SND_CHECK(discarded);
  const snd::compose::Parts parts = snd::compose::PartsOf(r.sys);
  SND_REQUIRE(parts.sources != nullptr);
  for (int i = 0; i < 100 && parts.sources->Stats().streamInstances != 0u; ++i) r.Step();
  SND_CHECK_EQ(parts.sources->Stats().streamInstances, 0u);
  SND_CHECK_EQ(parts.sources->Stats().sampleUnderruns, 0u);
  SND_CHECK_EQ(parts.sources->Stats().streamStalls, 0u);
  SND_CHECK_EQ(r.refillTimeouts, 0u);
}
