// End to end through the stage-1 modules (stage 1.5 integration, item 18):
//
//   project (JSON + WAVs generated here: a PCM asset at 48 kHz, an ADPCM asset
//   at 44.1 kHz, a bus with a lowpass + limiter chain)
//   -> buildtool -> bank file -> memory FS
//   -> io::IoScheduler (IO thread) + BankManager (Scheduler mode) + jobs::WorkerPool
//   -> PreparedBank -> assets -> codec decode
//   -> dsp resampler (44.1 kHz -> 48 kHz internal rate)
//   -> bus effect chain initialised in a pool of exactly ComputeDspPoolNeed bytes
//   -> output, compared with tests/integration/golden/e2e_bus_mix.sndg and
//      checked analytically (tone levels by Goertzel, lowpass attenuation, RMS).
// Everything the audio thread would do (resampling, mixing, effects) runs
// inside SND_RT_SCOPE: the RT hooks must record no violation.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "pipeline_util.hpp"
#include "snd/bank/bank_manager.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/dsp/resampler.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/golden.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

namespace bt = snd::buildtool;
namespace bk = snd::bank;
namespace bf = snd::bank_format;
namespace dsp = snd::dsp;
using snd::HashId;

namespace {

constexpr std::uint32_t kRate = 48000;   // internal rate (Config.sampleRate default)
constexpr std::uint32_t kBlock = 256;    // Config.blockSize default
constexpr std::uint32_t kBlocks = 160;   // ~0.85 s of output
constexpr std::uint32_t kFrames = kBlock * kBlocks;
constexpr double kPi = 3.14159265358979323846;

bt::PcmData Sines(std::uint32_t rate, std::uint64_t frames, std::initializer_list<std::pair<double, double>> tones) {
  bt::PcmData p;
  p.channels = 1;
  p.sampleRate = rate;
  p.frames = frames;
  p.samples.resize(static_cast<std::size_t>(frames));
  for (std::uint64_t f = 0; f < frames; ++f) {
    double v = 0;
    for (const auto& t : tones) v += t.second * std::sin(2 * kPi * t.first * static_cast<double>(f) / rate);
    p.samples[static_cast<std::size_t>(f)] = static_cast<float>(v);
  }
  return p;
}

// Amplitude of the `hz` component of x[from, from + n) (Goertzel, Hann window, amplitude-normalised).
double ToneAmplitude(const float* x, std::size_t from, std::size_t n, double hz, double rate) {
  const double w = 2 * kPi * hz / rate;
  double re = 0, im = 0, wsum = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
    const double v = x[from + i] * win;
    re += v * std::cos(w * static_cast<double>(i));
    im -= v * std::sin(w * static_cast<double>(i));
    wsum += win;
  }
  return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

double Db(double a, double b) { return 20.0 * std::log10(a / b); }

// Aligned scratch owned by the test (allocated before the RT part).
struct Planar {
  std::vector<float> mem;
  std::vector<float*> ptr;
  Planar(std::uint32_t channels, std::uint32_t frames) : mem(static_cast<std::size_t>(channels) * frames + 64) {
    float* p = mem.data();
    while (reinterpret_cast<std::uintptr_t>(p) % 64 != 0) ++p;
    for (std::uint32_t c = 0; c < channels; ++c) ptr.push_back(p + static_cast<std::size_t>(c) * frames);
  }
};

}  // namespace

SND_TEST(EndToEnd, BuildLoadDecodeResampleEffectsGolden) {
  // ---- 1. Project -> banks (buildtool, codec encoders) ----
  mini::Project proj("e2e");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true");
  proj.Add("buses/Music.json", "bus", "Music",
           "\"parent\": \"Master\", \"channels\": 2, \"effects\": ["
           "{\"type\": \"snd.fx.lowpass\", \"params\": [4000, 0.707]}, "
           "{\"type\": \"snd.fx.limiter\", \"params\": [-0.3, 50, 2]}]");
  const std::uint64_t pcmFrames = kFrames + 4096;
  const std::uint64_t adpcmFrames = static_cast<std::uint64_t>(kFrames) * 44100 / kRate + 4096;
  proj.Wav("audio/pcm.wav", Sines(kRate, pcmFrames, {{440.0, 0.25}, {12000.0, 0.15}}));
  proj.Wav("audio/adpcm.wav", Sines(44100, adpcmFrames, {{1000.0, 0.4}}));
  proj.Add("assets/pcm.json", "asset", "pcm",
           "\"file\": \"audio/pcm.wav\", \"loadForm\": \"pcm\", \"convert\": {\"default\": {\"codec\": \"PC16\"}}");
  proj.Add("assets/adpcm.json", "asset", "adpcm",
           "\"file\": \"audio/adpcm.wav\", \"loadForm\": \"pcm\", \"convert\": {\"default\": {\"codec\": \"ADPC\"}}");
  proj.Add("events/A.json", "event", "A",
           "\"bank\": \"Main\", \"bus\": \"Music\", \"tracks\": [{\"clips\": [{\"asset\": \"pcm\"}]}, "
           "{\"clips\": [{\"asset\": \"adpcm\"}]}]");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics diag;
  SND_REQUIRE_MSG(proj.Build(o, &diag), "%s", mini::Dump(diag).c_str());
  SND_CHECK_MSG(diag.items().empty(), "%s", mini::Dump(diag).c_str());

  // ---- 2. Memory FS -> IoScheduler (IO thread) -> BankManager -> worker pool ----
  snd::IAllocator& alloc = snd::base::DefaultAllocator();
  snd::mock::MemoryFileSystem fs;
  fs.SetDefaultLatency(1);
  fs.AddFile("Main.sndb", mini::ReadBytes(proj.out() / "Main.sndb"));
  snd::jobs::WorkerPool workers;
  snd::jobs::WorkerPoolConfig wc;
  wc.workers = 2;
  SND_REQUIRE(workers.Init(alloc, wc));
  snd::io::StreamPool streamPool;
  SND_REQUIRE(streamPool.Init(alloc, 64u << 10, 32u << 10, 2));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig ic;
  ic.maxStreams = 2;
  ic.filePieceBytes = 16u << 10;
  SND_REQUIRE(io.Init(alloc, fs, streamPool, ic));
  SND_REQUIRE(io.Start());
  bk::BankManagerDesc md;
  md.allocator = &alloc;
  md.scheduler = &io;  // default mode: through the IO scheduler
  md.jobs = &workers;
  bk::BankManager* manager = nullptr;
  SND_REQUIRE_EQ(bk::BankManager::Create(md, &manager), SND_OK);
  const snd::HandleRef bank = manager->RequestLoad("Main.sndb");
  snd::cmd::BankRegister reg{};
  bool registered = false;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!registered && std::chrono::steady_clock::now() < until) {
    manager->Update();
    if (manager->PopRegistration(&reg)) {
      manager->MarkRegistered(reg.bank);  // what the audio thread does at a block boundary
      registered = true;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  SND_REQUIRE_MSG(registered, "bank not registered: %s", manager->FailureReason(bank));
  SND_CHECK_EQ(manager->State(bank), SND_BANK_STATE_LOADED);
  const bk::PreparedBank* pb = bk::AsPrepared(reg.prepared);
  SND_REQUIRE(pb != nullptr);
  const bk::BankView& v = pb->view;
  SND_CHECK(io.Stats().filesCompleted >= 2u);  // size query + read went through the scheduler

  // ---- 3. Assets -> codec ----
  const std::uint32_t pcmRow = v.FindRow<bk::col::ASST_id>(HashId("pcm"));
  const std::uint32_t adRow = v.FindRow<bk::col::ASST_id>(HashId("adpcm"));
  SND_REQUIRE(pcmRow != bf::kNone && adRow != bf::kNone);
  const snd::CodecHeader pcmH = pipeline::AssetHeader(v, pcmRow);
  const snd::CodecHeader adH = pipeline::AssetHeader(v, adRow);
  SND_CHECK_EQ(pcmH.codec, SND_CODEC_PCM_S16);
  SND_CHECK_EQ(adH.codec, SND_CODEC_ADPCM);
  SND_CHECK_EQ(adH.sample_rate, 44100u);
  std::vector<std::vector<float>> pcm, ad;
  SND_REQUIRE(pipeline::DecodeAll(pcmH, v.WaveData(pcmRow).data, v.WaveData(pcmRow).bytes, 4096, &pcm));
  SND_REQUIRE(pipeline::DecodeAll(adH, v.WaveData(adRow).data, v.WaveData(adRow).bytes, 4096, &ad));

  // ---- 4. Bus effects: pool of exactly the runtime need ----
  const std::uint32_t music = v.FindRow<bk::col::BUSS_id>(HashId("Music"));
  SND_REQUIRE(music != bf::kNone);
  std::vector<std::vector<float>> params;
  params.reserve(8);
  const std::vector<snd::EffectDesc> descs = pipeline::BusEffectDescs(v, music, 2, kRate, kBlock, &params);
  SND_REQUIRE_EQ(descs.size(), 2u);
  const bf::DspPoolNeed need =
      dsp::ComputeDspPoolNeed(descs.data(), static_cast<std::uint32_t>(descs.size()), kRate, nullptr, 0);
  SND_CHECK_EQ(need.instanceBytes, pb->dspNeed.instanceBytes);  // == DSPN (what BankManagerDesc.dsp reserves)
  SND_CHECK_EQ(need.instanceCount, pb->dspNeed.instanceCount);
  std::vector<std::uint8_t> poolMem(static_cast<std::size_t>(need.instanceBytes) + 64);
  std::uint8_t* at = poolMem.data();
  while (reinterpret_cast<std::uintptr_t>(at) % 64 != 0) ++at;
  std::vector<dsp::EffectSlot> chain;
  for (const snd::EffectDesc& d : descs) {
    const std::uint32_t b = dsp::EffectInstanceBytes(d, kRate, nullptr, 0);
    SND_REQUIRE(dsp::EffectInit(at, b, d, kRate));  // normal worker at bank load
    chain.push_back(dsp::EffectSlot{d.effect_type, nullptr, at, nullptr, nullptr, 0u, 0u});
    at += b;
  }
  SND_CHECK_EQ(static_cast<std::uint64_t>(at - poolMem.data()) <= poolMem.size(), true);

  // ---- 5. The audio-thread part: resample, mix, effects (RT scope) ----
  const float maxRatio = 4.0f;  // Config.maxPitchRatio default (D-44)
  std::vector<std::uint8_t> rsMem(dsp::ResamplerStateBytes(dsp::ResamplerQuality::Sinc, 1, maxRatio) + 64);
  void* rs = rsMem.data();
  while (reinterpret_cast<std::uintptr_t>(rs) % 64 != 0) rs = static_cast<std::uint8_t*>(rs) + 1;
  dsp::ResamplerInit(rs, dsp::ResamplerQuality::Sinc, 1, maxRatio);
  const std::uint32_t pad = dsp::ResamplerPadFrames(dsp::ResamplerQuality::Sinc, maxRatio);
  Planar rsIn(1, pad + kBlock * 4 + 64);
  Planar rsOut(1, kBlock);
  Planar bus(2, kBlock);
  snd::test::AudioBuffer out(2, kFrames, kRate);
  const float ratio = static_cast<float>(adH.sample_rate) / static_cast<float>(kRate);
  std::uint64_t adPos = 0;
  snd::test::rt::Reset();
  {
    SND_RT_SCOPE();
    for (std::uint32_t blk = 0; blk < kBlocks; ++blk) {
      // ADPCM voice: source rate -> internal rate.
      const std::uint32_t inFrames = dsp::ResamplerInputFrames(rs, kBlock, ratio, ratio);
      float* in = rsIn.ptr[0] + pad;
      for (std::uint32_t i = 0; i < inFrames; ++i) {
        in[i] = adPos + i < ad[0].size() ? ad[0][static_cast<std::size_t>(adPos + i)] : 0.0f;
      }
      float* inPtr[1] = {rsIn.ptr[0]};
      const std::uint32_t used = dsp::ResamplerProcess(rs, inPtr, inFrames, rsOut.ptr.data(), kBlock, ratio, ratio);
      adPos += used;
      // Mix both voices onto the stereo bus (centre, gain 1).
      for (std::uint32_t c = 0; c < 2; ++c) {
        for (std::uint32_t i = 0; i < kBlock; ++i) {
          const std::size_t f = static_cast<std::size_t>(blk) * kBlock + i;
          bus.ptr[c][i] = pcm[0][f] + rsOut.ptr[0][i];
        }
      }
      dsp::ProcessEffectChain(chain.data(), static_cast<std::uint32_t>(chain.size()), bus.ptr.data(), 2, kBlock);
      for (std::uint32_t c = 0; c < 2; ++c) {
        float* dst = out.Channel(c) + static_cast<std::size_t>(blk) * kBlock;
        for (std::uint32_t i = 0; i < kBlock; ++i) dst[i] = bus.ptr[c][i];
      }
    }
  }
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  snd::test::Note("RT hooks: new/delete %s, libc %s", snd::test::rt::NewHooksActive() ? "on" : "off",
                  snd::test::rt::LibcHooksActive() ? "on" : "off");

  // ---- 6. Analytic checks (steady part, after the limiter look-ahead and filter settling) ----
  const std::size_t from = 8192, n = 16384;
  for (std::uint32_t c = 0; c < 2; ++c) {
    const float* y = out.Channel(c);
    const double a440 = ToneAmplitude(y, from, n, 440.0, kRate);
    const double a1k = ToneAmplitude(y, from, n, 1000.0, kRate);
    const double a12k = ToneAmplitude(y, from, n, 12000.0, kRate);
    SND_CHECK_MSG(std::fabs(Db(a440, 0.25)) < 0.3, "440 Hz %.4f (%.2f dB)", a440, Db(a440, 0.25));
    SND_CHECK_MSG(std::fabs(Db(a1k, 0.4)) < 0.5, "1 kHz (ADPCM, resampled) %.4f (%.2f dB)", a1k, Db(a1k, 0.4));
    SND_CHECK_MSG(Db(a12k, 0.15) < -15.0, "12 kHz above the 4 kHz lowpass: %.2f dB", Db(a12k, 0.15));
    double sum = 0, peak = 0;
    for (std::size_t i = from; i < from + n; ++i) {
      sum += static_cast<double>(y[i]) * y[i];
      peak = std::fmax(peak, std::fabs(y[i]));
    }
    const double rms = std::sqrt(sum / static_cast<double>(n));
    const double expect = std::sqrt(0.25 * 0.25 / 2 + 0.4 * 0.4 / 2);
    SND_CHECK_MSG(std::fabs(rms / expect - 1.0) < 0.05, "RMS %.4f, expected about %.4f", rms, expect);
    SND_CHECK_MSG(peak < 0.966, "peak %.4f above the limiter ceiling", peak);
    if (c == 0) {
      snd::test::Note("440 Hz %.2f dB, 1 kHz %.2f dB, 12 kHz %.2f dB, RMS %.4f (expected %.4f), peak %.4f",
                      Db(a440, 0.25), Db(a1k, 0.4), Db(a12k, 0.15), rms, expect, peak);
    }
  }
  // Both channels carry the same mix.
  for (std::size_t i = 0; i < kFrames; ++i) {
    if (out.Channel(0)[i] != out.Channel(1)[i]) {
      SND_CHECK_MSG(false, "channels differ at %zu", i);
      break;
    }
  }

  // ---- 7. Golden ----
  snd::test::GoldenTolerance tol;
  tol.maxAbs = 2e-4f;   // compilers / SIMD paths (resampler, filters)
  tol.maxRms = 2e-5f;
  SND_CHECK_GOLDEN("e2e_bus_mix", out, tol);

  // ---- 8. Teardown in the documented order ----
  SND_CHECK_EQ(manager->BeginUnload(bank, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(manager->Release(bank), SND_OK);
  io.Shutdown();
  workers.WaitIdle();
  bk::BankManager::Destroy(manager);
  io.Destroy();
  streamPool.Destroy();
  workers.Shutdown();
}
