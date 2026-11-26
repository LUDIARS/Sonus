// Stage 7-D kinds through core (source_harness.hpp Rig, banks built by buildtool):
// a SYNP oscillator whose frequency follows a Parameter (pitch curve), the noise
// colours, a wavetable synth, and an input playback fed through the Runtime's port.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "snd/id.h"
#include "source_harness.hpp"
#include "synth_driver.hpp"

using namespace srctest;

namespace {

Built BuildSynths(const std::string& tag) {
  Built b;
  mini::Project proj("src-" + tag);
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true, \"channels\": 2");
  proj.Add("parameters/Speed.json", "parameter", "Speed", "\"min\": 0, \"max\": 1, \"default\": 0");
  proj.Add("events/Osc.json", "event", "Osc",
           "\"bank\": \"Main\", \"bus\": \"Master\", "
           "\"curves\": [{\"parameter\": \"Speed\", \"target\": \"pitch\", \"points\": [[0, 0], [1, 1200]]}], "
           "\"tracks\": [{\"clips\": [{\"synth\": {\"kind\": \"oscillator\", \"waveform\": \"sine\", "
           "\"frequencyHz\": 1000}}]}]");
  for (const char* color : {"white", "pink", "brown"}) {
    proj.Add(std::string("events/N_") + color + ".json", "event", std::string("N_") + color,
             std::string("\"bank\": \"Main\", \"bus\": \"Master\", \"tracks\": [{\"clips\": [{\"synth\": "
                         "{\"kind\": \"noise\", \"noiseColor\": \"") +
                 color + "\", \"channels\": 2}}]}]");
  }
  // A wavetable: one table of 2048 frames holding 2 periods of a sine (PCMF, resident).
  bt::PcmData wt;
  wt.channels = 1;
  wt.sampleRate = 48000;
  wt.frames = 2048;
  wt.samples.resize(2048);
  for (std::uint32_t j = 0; j < 2048; ++j) wt.samples[j] = static_cast<float>(0.5 * std::sin(6.283185307179586 * 2.0 * j / 2048.0));
  proj.Wav("audio/wt.wav", wt, bt::WavSampleFormat::Float32);
  proj.Add("assets/wt.json", "asset", "wt",
           "\"file\": \"audio/wt.wav\", \"loadForm\": \"pcm\", \"convert\": {\"default\": {\"codec\": \"PCMF\"}}");
  proj.Add("events/Wt.json", "event", "Wt",
           "\"bank\": \"Main\", \"bus\": \"Master\", \"tracks\": [{\"clips\": [{\"synth\": {\"kind\": \"wavetable\", "
           "\"asset\": \"wt\", \"frequencyHz\": 300}}]}]");
  if (tag == "granular") {
    proj.Add("events/Grains.json", "event", "Grains",
             R"("bank":"Main","bus":"Master","tracks":[{"clips":[{"synth":{"kind":"granular","asset":"wt"}}]}])");
  }
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics diag;
  bt::BuildResult res;
  if (!proj.Build(o, &diag, &res)) {
    b.diag = mini::Dump(diag);
    return b;
  }
  b.bank = mini::ReadBytes(proj.out() / "Main.sndb");
  b.ok = !b.bank.empty();
  return b;
}

std::vector<float> Channel(const Rig& r, std::uint32_t c, std::size_t from = 0) {
  std::vector<float> x;
  for (std::size_t f = from; f < r.Frames(); ++f) x.push_back(r.At(f, c));
  return x;
}

}  // namespace

// The oscillator plays its frequency; the Parameter moves it an octave up through the
// pitch curve (the kind applies RenderItem::pitch), without a phase jump.
SND_TEST(SourceSynthE2E, OscillatorFrequencyFollowsAParameter) {
  Built b = BuildSynths("osc");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Rig r;
  SND_REQUIRE(r.Ok());
  r.Load(b);
  const snd_playback_handle h = r.Play("Osc");
  r.Steps(40);
  const std::size_t from = r.Frames() - 8192;
  const double a1 = Tone(r, 0, from, 8192, 1000.0);
  SND_CHECK(a1 > 0.3);
  SND_CHECK(Tone(r, 0, from, 8192, 2000.0) < 0.01 * a1);
  SND_CHECK_NEAR(Tone(r, 1, from, 8192, 1000.0), a1, 1e-3);
  r.sys->SetParamPlayback(h, snd::HashId("Speed"), 1.0f);
  r.all.clear();
  r.Steps(40);
  const std::size_t from2 = r.Frames() - 8192;
  SND_CHECK_NEAR(Tone(r, 0, from2, 8192, 2000.0), a1, 0.02 * a1);
  SND_CHECK(Tone(r, 0, from2, 8192, 1000.0) < 0.01 * a1);
  // No discontinuity: the largest step between samples is what a 2 kHz sine of that
  // amplitude can do (2 pi f / rate x A, plus a little for the ramp).
  double step = 0;
  for (std::size_t f = 1; f < r.Frames(); ++f) step = std::fmax(step, std::fabs(r.At(f, 0) - r.At(f - 1, 0)));
  SND_CHECK_MSG(step < 1.05 * 6.283185307179586 * 2000.0 / 48000.0 * a1, "step %f", step);
  SND_CHECK(r.EndOf(h) == nullptr);
  SND_CHECK_EQ(r.runtime.Stats().oscillatorInstances, 1u);
}

// Noise colours through core: the slope of the master output follows the colour; two
// channels of one noise voice differ.
SND_TEST(SourceSynthE2E, NoiseColoursPlayThroughCore) {
  Built b = BuildSynths("noise");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  double lowMinusHigh[3] = {0, 0, 0};
  int i = 0;
  for (const char* ev : {"N_white", "N_pink", "N_brown"}) {
    Rig r;
    SND_REQUIRE(r.Ok());
    r.Load(b);
    r.Play(ev);
    r.Steps(260);
    const std::vector<float> l = Channel(r, 0, 4096), rr = Channel(r, 1, 4096);
    SND_REQUIRE(l.size() > 60000u);
    const std::vector<double> pl = synthtest::Power(l, 0, 65536 > l.size() ? 32768 : 65536);
    auto band = [&](double lo, double hi) {
      const double bin = 48000.0 / double((pl.size() - 1) * 2);
      double s = 0;
      for (std::size_t k = std::size_t(lo / bin); k < std::size_t(hi / bin); ++k) s += pl[k];
      return 10.0 * std::log10(s / ((hi - lo) / bin));
    };
    lowMinusHigh[i] = band(400, 800) - band(6400, 12800);  // 4 octaves apart
    std::printf("%s: 400-800 Hz minus 6.4-12.8 kHz = %.1f dB\n", ev, lowMinusHigh[i]);
    SND_CHECK(l != rr);
    ++i;
  }
  SND_CHECK_NEAR(lowMinusHigh[0], 0.0, 1.5);
  SND_CHECK_NEAR(lowMinusHigh[1], 12.0, 2.0);
  SND_CHECK_NEAR(lowMinusHigh[2], 24.0, 2.5);
}

// A wavetable synth: one table of 2048 frames = 2 periods -> 600 Hz at 300 Hz.
SND_TEST(SourceSynthE2E, WavetableSynthPlaysItsTable) {
  Built b = BuildSynths("wt");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Rig r;
  SND_REQUIRE(r.Ok());
  r.Load(b);
  const snd_playback_handle h = r.Play("Wt");
  r.Steps(40);
  const std::size_t from = r.Frames() - 8192;
  const double a = Tone(r, 0, from, 8192, 600.0);
  SND_CHECK(a > 0.15);
  SND_CHECK(Tone(r, 0, from, 8192, 300.0) < 0.02 * a);
  SND_CHECK(r.EndOf(h) == nullptr);
}

// An input playback (core System::PlayInput) plays what the game writes to the port:
// a 750 Hz sine written block by block comes out continuous on both channels.
SND_TEST(SourceSynthE2E, InputPlaybackPlaysThePort) {
  Rig r;
  SND_REQUIRE(r.Ok());
  snd::source::InputPort* port = r.runtime.InputPortOf(0);
  SND_REQUIRE(port != nullptr);
  snd_input_desc d{};
  d.device_index = 0;
  d.channels = 1;
  const snd_playback_handle h = r.sys->PlayInput(&d, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(h.value != 0u);
  r.Steps(2);
  std::uint64_t n = 0;
  std::vector<float> chunk(256);
  for (int b = 0; b < 80; ++b) {
    for (float& v : chunk) v = static_cast<float>(0.5 * std::sin(6.283185307179586 * 750.0 * double(n++) / 48000.0));
    const float* ch[1] = {chunk.data()};
    SND_CHECK_EQ(port->Write(ch, 1, 256), 256u);
    r.Step();
  }
  const std::size_t from = r.Frames() - 8192;
  const double a = Tone(r, 0, from, 8192, 750.0);
  SND_CHECK(a > 0.25);
  SND_CHECK_NEAR(Tone(r, 1, from, 8192, 750.0), a, 1e-3);
  SND_CHECK(Tone(r, 0, from, 8192, 1500.0) < 0.01 * a);
  SND_CHECK(r.EndOf(h) == nullptr);
  SND_CHECK_EQ(r.runtime.Stats().inputInstances, 1u);
  SND_CHECK_EQ(port->Stats().underruns, std::uint64_t{0});
  // A second input playback on the same port is refused.
  const snd_playback_handle h2 = r.sys->PlayInput(&d, snd_emitter_handle{0}, nullptr);
  for (int i = 0; i < 20 && r.EndOf(h2) == nullptr; ++i) r.Step();
  const EndRec* e = r.EndOf(h2);
  SND_REQUIRE(e != nullptr);
  SND_CHECK_EQ(e->detail, snd_end_detail{SND_END_DETAIL_VOICE_LIMIT});
  // Stopping releases the port.
  r.sys->Stop(h, 0.0f, nullptr);
  for (int i = 0; i < 20 && r.EndOf(h) == nullptr; ++i) r.Step();
  r.Steps(2);
  SND_CHECK(r.EndOf(h) != nullptr);
  SND_CHECK_EQ(r.runtime.Stats().inputInstances, 0u);
}

// The writer omits optional columns at their defaults (legacy SYNP layout).
SND_TEST(SourceSynthE2E, GranularPlaysWithLegacyColumnDefaults) {
  const Built b = BuildSynths("granular");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Rig r;
  SND_REQUIRE(r.Ok());
  r.Load(b);
  const auto h = r.Play("Grains");
  SND_REQUIRE(h.value != 0u);
  r.Steps(16);
  SND_CHECK(r.EndOf(h) == nullptr);
  SND_CHECK_EQ(r.runtime.Stats().granularInstances, 1u);
  const auto samples = Channel(r, 0);
  SND_CHECK(std::any_of(samples.begin(), samples.end(), [](float x) { return std::abs(x) > 0.01f; }));
}
