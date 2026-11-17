// End to end with the source module (stage 2): project (JSON + WAV) ->
// buildtool -> bank + stream files -> memory FS -> io::IoScheduler (IO thread)
// -> core (BankManager, voices, sinc resampling, bus effect) with the Sample
// and Stream kinds from the composition root (snd::compose::SourceKinds) ->
// output. Three sources at once: a resident PCM effect (440 Hz, 44.1 kHz), a
// compressed ADPCM one (3 kHz) and a looping stream (1 kHz, Opus or ADPCM) on a
// low-passed music bus. Checked analytically per tone, then Stop, the stream's
// loop, StopAndUnload and the release of every resource.
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "mini_project.hpp"
#include "snd/api/compose.hpp"
#include "snd/base/allocator.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/codec/codec.hpp"
#include "snd/core/core.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"

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
double Level(const std::vector<float>& x, std::size_t from, std::size_t n, double hz) {
  const double w = 6.283185307179586 * hz / 48000.0;
  double re = 0, im = 0, ws = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(6.283185307179586 * double(i) / double(n - 1));
    const double v = x[(from + i) * 2] * win;
    re += v * std::cos(w * double(i));
    im -= v * std::sin(w * double(i));
    ws += win;
  }
  return 2.0 * std::sqrt(re * re + im * im) / ws;
}
}  // namespace

SND_TEST(SourceEndToEnd, ResidentCompressedAndStreamThroughCore) {
  const bool opus = snd::codec::OpusAvailable();
  mini::Project proj("src-e2e");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true, \"channels\": 2");
  proj.Add("buses/Music.json", "bus", "Music",
           "\"parent\": \"Master\", \"channels\": 2, \"effects\": [{\"type\": \"snd.fx.lowpass\", \"params\": [8000, 0.707]}]");
  proj.Wav("audio/fx.wav", Tone(44100, 44100, 440.0, 0.4), bt::WavSampleFormat::Float32);
  proj.Wav("audio/adp.wav", Tone(48000, 48000, 3000.0, 0.3), bt::WavSampleFormat::Float32);
  proj.Wav("audio/bgm.wav", Tone(48000, 96000, 1000.0, 0.5), bt::WavSampleFormat::Float32);
  proj.Add("assets/fx.json", "asset", "fx", "\"file\": \"audio/fx.wav\", \"loadForm\": \"pcm\"");
  proj.Add("assets/adp.json", "asset", "adp",
           "\"file\": \"audio/adp.wav\", \"loadForm\": \"compressed\", \"convert\": {\"default\": {\"codec\": \"ADPC\"}}");
  proj.Add("assets/bgm.json", "asset", "bgm",
           std::string("\"file\": \"audio/bgm.wav\", \"loadForm\": \"stream\", \"loop\": {\"start\": 0, \"end\": 96000}, "
                       "\"convert\": {\"default\": {\"codec\": \"") +
               (opus ? "OPUS" : "ADPC") + "\"}}");
  proj.Add("events/Fx.json", "event", "Fx", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"fx\"}]}]");
  proj.Add("events/Adp.json", "event", "Adp", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"adp\"}]}]");
  proj.Add("events/Bgm.json", "event", "Bgm",
           "\"bank\": \"Main\", \"bus\": \"Music\", \"tracks\": [{\"clips\": [{\"asset\": \"bgm\"}]}]");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics diag;
  bt::BuildResult res;
  if (!proj.Build(o, &diag, &res)) {
    SND_CHECK_MSG(false, "%s", mini::Dump(diag).c_str());
    return;
  }
  SND_REQUIRE_EQ(res.streams.size(), 1u);

  snd::IAllocator& alloc = snd::base::DefaultAllocator();
  snd::mock::MemoryFileSystem fs;
  fs.SetDefaultLatency(1);
  fs.AddFile("Main.sndb", mini::ReadBytes(proj.out() / "Main.sndb"));
  fs.AddFile(res.streams[0], mini::ReadBytes(proj.out() / res.streams[0]));
  snd::jobs::WorkerPool workers;
  snd::jobs::WorkerPoolConfig wc;
  wc.workers = 2;
  SND_REQUIRE(workers.Init(alloc, wc));
  snd_config cfg;
  snd_config_init(&cfg);
  cfg.jobs.realtime_workers = 2;
  cfg.max_streams = 4;
  snd::io::StreamPool streamPool;
  SND_REQUIRE(streamPool.Init(alloc, cfg.memory.stream_pool_bytes, cfg.memory.stream_chunk_bytes, cfg.max_streams));
  snd::io::IoScheduler io;
  SND_REQUIRE(io.Init(alloc, fs, streamPool, snd::io::MakeIoSchedulerConfig(cfg)));
  SND_REQUIRE(io.Start());
  snd::compose::SourceKinds kinds;
  SND_REQUIRE(kinds.Init(alloc, &io, &streamPool, cfg.max_voice_channels, cfg.max_banks));

  snd::core::CoreDesc d;
  d.config = &cfg;
  d.allocator = &alloc;
  d.sources = kinds.Kinds();
  d.jobs = &workers;
  d.bankScheduler = &io;
  d.bankUnregistered = &snd::compose::SourceKinds::BankUnregistered;
  d.bankUser = kinds.BankUser();
  d.rtWorkerRealtimePriority = false;
  snd::core::System* sys = nullptr;
  SND_REQUIRE_EQ(snd::core::System::Create(d, &sys), SND_OK);
  std::vector<float> out(256 * 2), all;
  auto step = [&] {
    sys->Update();
    sys->RenderBlock(out.data());
    all.insert(all.end(), out.begin(), out.end());
    // About real time (a block is 5.3 ms), so that the IO thread and the decode
    // workers keep up even on a loaded machine (TSan): the analytic checks below
    // assume no stream underrun.
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  };
  const snd_bank_handle bank = sys->LoadBankAsync("Main.sndb");
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (sys->GetBankState(bank) != SND_BANK_STATE_LOADED && std::chrono::steady_clock::now() < until) step();
  SND_REQUIRE_EQ(sys->GetBankState(bank), SND_BANK_STATE_LOADED);
  all.clear();
  const snd_playback_handle fx = sys->Play(HashId("Fx"), snd_emitter_handle{0}, nullptr);
  const snd_playback_handle adp = sys->Play(HashId("Adp"), snd_emitter_handle{0}, nullptr);
  const snd_playback_handle bgm = sys->Play(HashId("Bgm"), snd_emitter_handle{0}, nullptr);
  for (int i = 0; i < 120; ++i) step();
  // Mono voices on the stereo master: x 0.7071. The 8 kHz lowpass passes 1 kHz (-0.07 dB).
  const double k = 0.70710678;
  SND_CHECK_NEAR(Level(all, 4096, 16384, 440.0), 0.4 * k, 0.01);
  SND_CHECK_NEAR(Level(all, 4096, 16384, 3000.0), 0.3 * k, 0.01);
  SND_CHECK_NEAR(Level(all, 4096, 16384, 1000.0), 0.5 * k, 0.015);
  sys->Update();
  SND_CHECK_EQ(sys->GetPlaybackState(bgm), SND_PLAYBACK_STATE_PLAYING);
  // The 1 s effects end; the 2 s stream loop keeps playing through its loop point (4 s).
  for (int i = 0; i < 280; ++i) step();
  sys->Update();
  SND_CHECK_EQ(sys->GetPlaybackState(fx), SND_PLAYBACK_STATE_NONE);
  SND_CHECK_EQ(sys->GetPlaybackState(adp), SND_PLAYBACK_STATE_NONE);
  SND_CHECK_EQ(sys->GetPlaybackState(bgm), SND_PLAYBACK_STATE_PLAYING);
  const std::size_t frames = all.size() / 2;
  SND_CHECK_NEAR(Level(all, frames - 16384, 16384, 1000.0), 0.5 * k, 0.015);
  SND_CHECK(Level(all, frames - 16384, 16384, 440.0) < 0.002);
  snd_stats st{};
  SND_CHECK_EQ(sys->GetStats(&st), SND_OK);
  SND_CHECK_EQ(st.streams, 1u);
  // Unload while the stream plays: discarded, the bank released after the epoch.
  SND_CHECK_EQ(sys->UnloadBank(bank, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  for (int i = 0; i < 400 && sys->GetBankState(bank) != SND_BANK_STATE_UNLOADED; ++i) step();
  SND_CHECK_EQ(sys->GetBankState(bank), SND_BANK_STATE_UNLOADED);
  for (int i = 0; i < 100 && streamPool.Used() != 0u; ++i) step();
  SND_CHECK_EQ(streamPool.Used(), 0u);
  io.Shutdown();
  workers.Shutdown();
  snd::core::System::Destroy(sys);
  kinds.Destroy();
  io.Destroy();
  streamPool.Destroy();
}
