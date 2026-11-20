// Independent tests of the stage 7-D kinds (Oscillator, Noise, Input; D-190 .. D-196)
// through core: SYNP banks built by buildtool, played by core::System with the source
// Runtime (deterministic drive, no IO thread), and a threaded drive (RT workers, a writer
// thread on an input port) for ThreadSanitizer:
//   ctest -j1 --repeat until-fail:20 -R indep_src1_core_test
// Analytic checks: harmonic ratios of the waveforms on the master bus, the phase of an
// oscillator that comes back from virtual, incoherence of two noise playbacks, the
// determinism of a recorded command sequence (design 2.5), an input playback that stays
// live while virtual (design 7 "動的生成・入力").
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "indep_src1_util.hpp"
#include "snd/id.h"
#include "source_harness.hpp"

using namespace srctest;
using isrc::kPi;
using isrc::kTwoPi;

namespace {

constexpr double kRate = 48000.0;

std::string Synth(const std::string& name, const std::string& synth, int priority = 128) {
  (void)name;
  return "\"bank\": \"Main\", \"bus\": \"Master\", \"priority\": " + std::to_string(priority) +
         ", \"tracks\": [{\"clips\": [{\"synth\": {" + synth + "}}]}]";
}

Built BuildBank(const std::string& tag) {
  Built b;
  mini::Project proj("isrc1-" + tag);
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true, \"channels\": 2");
  const char* waves[] = {"sine", "saw", "square", "triangle", "pulse"};
  for (const char* w : waves) {
    proj.Add(std::string("events/W_") + w + ".json", "event", std::string("W_") + w,
             Synth(w, std::string("\"kind\": \"oscillator\", \"waveform\": \"") + w +
                          "\", \"frequencyHz\": 375, \"duty\": 0.25"));
  }
  proj.Add("events/Hi1000.json", "event", "Hi1000",
           Synth("Hi1000", "\"kind\": \"oscillator\", \"waveform\": \"sine\", \"frequencyHz\": 1000", 200));
  proj.Add("events/Hi1500.json", "event", "Hi1500",
           Synth("Hi1500", "\"kind\": \"oscillator\", \"waveform\": \"sine\", \"frequencyHz\": 1500", 200));
  proj.Add("events/Lo1000.json", "event", "Lo1000",
           Synth("Lo1000", "\"kind\": \"oscillator\", \"waveform\": \"sine\", \"frequencyHz\": 1000", 10));
  for (const char* c : {"white", "pink", "brown"}) {
    proj.Add(std::string("events/N_") + c + ".json", "event", std::string("N_") + c,
             Synth(c, std::string("\"kind\": \"noise\", \"noiseColor\": \"") + c + "\", \"channels\": 1"));
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

// srctest::Rig with a configurable voice budget (deterministic drive).
class CoreRig {
 public:
  explicit CoreRig(std::uint32_t realVoices = 160, std::uint32_t virtualVoices = 256, bool threaded = false,
                   std::uint32_t rtWorkers = 0)
      : threaded_(threaded) {
    alloc_ = &snd::base::DefaultAllocator();
    if (threaded) {
      snd::jobs::WorkerPoolConfig wc;
      wc.workers = 2;
      ok_ = workers_.Init(*alloc_, wc);
    }
    ok_ = ok_ && pool.Init(*alloc_, 512u << 10, 8u << 10, 4);
    snd::io::IoSchedulerConfig ic;
    ic.maxStreams = 4;
    ic.ringChunks = 4;
    ic.bytesPerWindow = 0;
    ok_ = ok_ && io.Init(*alloc_, fs, pool, ic);
    if (threaded) ok_ = ok_ && io.Start();
    snd::source::RuntimeDesc rd;
    rd.allocator = alloc_;
    rd.io = &io;
    rd.streamPool = &pool;
    rd.maxVoiceChannels = 2;
    ok_ = ok_ && runtime.Init(rd);
    snd_config_init(&cfg);
    cfg.max_streams = 4;
    cfg.memory.stream_chunk_bytes = 8u << 10;
    cfg.memory.stream_pool_bytes = 512u << 10;
    cfg.memory.pcm_cache_bytes = 1u << 20;
    cfg.memory.pcm_cache_page_bytes = 64u << 10;
    cfg.jobs.realtime_workers = rtWorkers;
    cfg.max_voice_channels = 2;
    cfg.max_real_voices = realVoices;
    cfg.max_virtual_voices = virtualVoices;
    cfg.tuning.lod_down_blocks = 0;
    snd::core::CoreDesc d;
    d.config = &cfg;
    d.allocator = alloc_;
    d.sources = &runtime.Kinds();
    d.jobs = threaded ? static_cast<snd::IJobSystem*>(&workers_) : static_cast<snd::IJobSystem*>(&jobs);
    d.bankScheduler = &io;
    d.bankUnregistered = &snd::source::Runtime::BankUnregistered;
    d.bankUser = &runtime;
    d.rtWorkerRealtimePriority = false;
    created = ok_ ? snd::core::System::Create(d, &sys) : SND_ERR_OUT_OF_BUDGET;
    if (sys != nullptr) {
      snd_callbacks cb{};
      cb.user = this;
      cb.on_end = [](void* u, snd_playback_handle h, snd_end_reason r, snd_end_detail det) {
        static_cast<CoreRig*>(u)->ends.push_back(EndRec{h.value, r, det});
      };
      sys->SetCallbacks(&cb);
      out.resize(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
    }
  }
  ~CoreRig() {
    io.Shutdown();
    if (threaded_) workers_.Shutdown();
    if (sys != nullptr) snd::core::System::Destroy(sys);
    runtime.Destroy();
    io.Destroy();
    pool.Destroy();
  }
  CoreRig(const CoreRig&) = delete;
  CoreRig& operator=(const CoreRig&) = delete;
  bool Ok() const { return ok_ && created == SND_OK && sys != nullptr; }
  void Step() {
    if (!threaded_) {
      for (int i = 0; i < 8 && io.RunOnce(); ++i) {
      }
    }
    sys->Update();
    const auto t0 = std::chrono::steady_clock::now();
    sys->RenderBlock(out.data());
    renderUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    ++blocks;
    all.insert(all.end(), out.begin(), out.end());
  }
  void Steps(int n) {
    for (int i = 0; i < n; ++i) Step();
  }
  bool Load(const Built& b) {
    fs.AddFile("Main.sndb", b.bank);
    const snd_bank_handle h = sys->LoadBankAsync("Main.sndb");
    for (int i = 0; i < 2000 && sys->GetBankState(h) != SND_BANK_STATE_LOADED; ++i) {
      Step();
      if (threaded_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sys->Update();
    all.clear();
    renderUs = 0;
    blocks = 0;
    return sys->GetBankState(h) == SND_BANK_STATE_LOADED;
  }
  snd_playback_handle Play(const char* ev) { return sys->Play(snd::HashId(ev), snd_emitter_handle{0}, nullptr); }
  std::vector<float> Channel(std::uint32_t c, std::size_t from = 0) const {
    std::vector<float> x;
    const std::size_t ch = sys->OutputChannels();
    for (std::size_t f = from; f < all.size() / ch; ++f) x.push_back(all[f * ch + c]);
    return x;
  }
  const EndRec* EndOf(snd_playback_handle h) const {
    for (const EndRec& e : ends) {
      if (e.handle == h.value) return &e;
    }
    return nullptr;
  }

  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  snd::io::StreamPool pool;
  snd::io::IoScheduler io;
  snd::source::Runtime runtime;
  snd_config cfg{};
  snd::core::System* sys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  std::vector<float> out, all;
  std::vector<EndRec> ends;
  double renderUs = 0;
  int blocks = 0;

 private:
  bool threaded_ = false;
  snd::IAllocator* alloc_ = nullptr;
  snd::jobs::WorkerPool workers_;
  bool ok_ = true;
};

}  // namespace

// E1: each waveform through core: harmonic ratios on the master (exact-period window of
// 64 periods of 375 Hz) follow the Fourier series (the pan gain cancels in the ratio).
SND_TEST(IndepSrc1Core, WaveformHarmonicsOnTheMaster) {
  Built b = BuildBank("harm");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  struct W {
    const char* ev;
    double h[4];  // |c_k| / |c_1| for k = 1..4 (pulse: duty 0.25)
  };
  const double s25 = std::sin(kPi * 0.25), s50 = std::sin(kPi * 0.5), s75 = std::sin(kPi * 0.75), s100 = 0.0;
  const W ws[] = {{"W_sine", {1, 0, 0, 0}},
                  {"W_saw", {1, 1.0 / 2, 1.0 / 3, 1.0 / 4}},
                  {"W_square", {1, 0, 1.0 / 3, 0}},
                  {"W_triangle", {1, 0, 1.0 / 9, 0}},
                  {"W_pulse", {1, s50 / s25 / 2, s75 / s25 / 3, s100}}};
  for (const W& w : ws) {
    CoreRig r;
    SND_REQUIRE(r.Ok());
    SND_REQUIRE(r.Load(b));
    const snd_playback_handle h = r.Play(w.ev);
    SND_REQUIRE(h.value != 0u);
    r.Steps(60);
    const std::vector<float> x = r.Channel(0);
    const std::size_t n = 128 * 64, from = x.size() - n;
    const double a1 = std::abs(isrc::Coef(x, from, n, 64.0));
    SND_CHECK_MSG(a1 > 0.3, "%s: fundamental %f", w.ev, a1);
    for (int k = 2; k <= 4; ++k) {
      const double ak = std::abs(isrc::Coef(x, from, n, 64.0 * k)) / a1;
      SND_CHECK_MSG(std::fabs(ak - w.h[k - 1]) < 0.02 * w.h[k - 1] + 3e-3, "%s: h%d/h1 %.4f want %.4f", w.ev, k, ak,
                    w.h[k - 1]);
    }
    SND_CHECK(r.EndOf(h) == nullptr);
    SND_CHECK_EQ(r.runtime.Stats().oscillatorInstances, 1u);
  }
}

// E4 / O10 through core: with 2 real voices, the low-priority oscillator is virtual while
// two high-priority ones play; when one of those stops, it becomes real with the phase it
// would have had playing all along (the same as the stopped one, started at the same clock).
SND_TEST(IndepSrc1Core, OscillatorComesBackFromVirtualInPhase) {
  Built b = BuildBank("virt");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  CoreRig r(2, 16);
  SND_REQUIRE(r.Ok());
  SND_REQUIRE(r.Load(b));
  const snd_playback_handle hi = r.Play("Hi1000");
  const snd_playback_handle hi2 = r.Play("Hi1500");
  const snd_playback_handle lo = r.Play("Lo1000");
  r.Steps(40);
  snd_stats st{};
  SND_REQUIRE_EQ(r.sys->GetStats(&st), SND_OK);
  SND_CHECK_EQ(st.real_voices, 2u);
  SND_CHECK_EQ(st.virtual_voices, 1u);
  const std::size_t n = 4800;  // 100 periods of 1000 Hz, 150 of 1500 Hz
  std::vector<float> x = r.Channel(0);
  std::size_t from = x.size() - n;
  const std::complex<double> c1 = isrc::Coef(x, from, n, 100.0);
  const double a1 = std::abs(c1);
  SND_CHECK_NEAR(std::abs(isrc::Coef(x, from, n, 150.0)), a1, 0.01 * a1);  // both high ones, the same gain
  const double phase1 = std::arg(c1) - kTwoPi * 1000.0 * double(from) / kRate;
  r.sys->Stop(hi, 0.0f, nullptr);
  r.Steps(40);
  x = r.Channel(0);
  from = x.size() - n;
  const std::complex<double> c2 = isrc::Coef(x, from, n, 100.0);
  SND_CHECK_NEAR(std::abs(c2), a1, 0.02 * a1);  // the low one now sounds, at the same level
  const double phase2 = std::arg(c2) - kTwoPi * 1000.0 * double(from) / kRate;
  double dphi = std::remainder(phase2 - phase1, kTwoPi);
  SND_CHECK_MSG(std::fabs(dphi) < 0.02, "phase after devirtualisation off by %.4f rad", dphi);
  SND_CHECK(r.EndOf(hi) != nullptr);
  SND_CHECK(r.EndOf(lo) == nullptr);
  SND_CHECK(r.EndOf(hi2) == nullptr);
  SND_CHECK_EQ(r.runtime.Stats().oscillatorInstances, 2u);
}

// Design 2.5: the same command sequence on the same bank gives the same bits (the noise
// seeds come from the playbacks); two playbacks of one noise event are different noises
// (incoherent: the power adds, sqrt(2) in RMS, not 2).
SND_TEST(IndepSrc1Core, NoiseIsDeterministicPerPlaybackAndIncoherentAcrossPlaybacks) {
  Built b = BuildBank("det");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  std::vector<float> runs[2];
  for (int k = 0; k < 2; ++k) {
    CoreRig r;
    SND_REQUIRE(r.Ok());
    SND_REQUIRE(r.Load(b));
    r.Play("N_white");
    r.Steps(3);
    r.Play("N_pink");
    r.Play("W_saw");
    r.Steps(5);
    r.Play("N_brown");
    r.Steps(30);
    runs[k] = r.all;
  }
  SND_CHECK(runs[0].size() > 10000u);
  SND_CHECK_MSG(runs[0] == runs[1], "two runs of the same commands differ");
  double rms1 = 0, rms2 = 0;
  for (int k = 0; k < 2; ++k) {
    CoreRig r;
    SND_REQUIRE(r.Ok());
    SND_REQUIRE(r.Load(b));
    r.Play("N_white");
    if (k == 1) r.Play("N_white");
    r.Steps(200);
    const double v = isrc::RmsOf(r.Channel(0), 2048);
    (k == 0 ? rms1 : rms2) = v;
  }
  std::printf("noise through core: rms one %.4f, two playbacks %.4f (ratio %.3f)\n", rms1, rms2, rms2 / rms1);
  SND_CHECK_NEAR(rms2 / rms1, std::sqrt(2.0), 0.08);
}

// D-196 / RT (design 14): many synth voices of each kind at once through core's render
// (an RT scope: allocations / locks there fail the test), with stops and fades; the block
// time is printed. Stop with a fade ends the playbacks; the instances return.
SND_TEST(IndepSrc1Core, ManySynthVoicesStopsAndFades) {
  Built b = BuildBank("many");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  CoreRig r(160, 512);
  SND_REQUIRE(r.Ok());
  SND_REQUIRE(r.Load(b));
  std::vector<snd_playback_handle> hs;
  const char* evs[] = {"W_sine", "W_saw", "W_square", "W_triangle", "W_pulse", "N_white", "N_pink", "N_brown"};
  for (int i = 0; i < 150; ++i) hs.push_back(r.Play(evs[i % 8]));
  r.Steps(100);
  std::printf("150 synth voices through core: %.1f us per block of 256 frames (budget 5333 us)\n",
              r.renderUs / r.blocks);
  const snd::source::RuntimeStats s = r.runtime.Stats();
  SND_CHECK_EQ(s.oscillatorInstances + s.noiseInstances, 150u);
  SND_CHECK(isrc::AllFinite(r.all));
  for (std::size_t i = 0; i < hs.size(); ++i) r.sys->Stop(hs[i], i % 2 == 0 ? 0.0f : 0.05f, nullptr);
  r.Steps(30);
  for (const snd_playback_handle h : hs) SND_CHECK(r.EndOf(h) != nullptr);
  const snd::source::RuntimeStats s2 = r.runtime.Stats();
  SND_CHECK_EQ(s2.oscillatorInstances + s2.noiseInstances, 0u);
  const std::vector<float> tail = r.Channel(0, r.all.size() / 2 - 256);
  for (float v : tail) SND_CHECK_EQ(v, 0.0f);
}

// Design 7 "動的生成・入力" / I7 through core: an input playback plays live; when it is
// virtual (2 real voices taken by high-priority oscillators) it keeps consuming its port
// (the backlog does not grow, the writer does not overrun) and comes back live.
SND_TEST(IndepSrc1Core, InputPlaybackStaysLiveThroughVirtualisation) {
  Built b = BuildBank("inp");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  CoreRig r(2, 16);
  SND_REQUIRE(r.Ok());
  SND_REQUIRE(r.Load(b));
  snd::source::InputPort* port = r.runtime.InputPortOf(2);
  SND_REQUIRE(port != nullptr);
  snd_input_desc d{};
  d.device_index = 2;
  d.channels = 1;
  const snd_playback_handle in = r.sys->PlayInput(&d, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(in.value != 0u);
  std::uint64_t n = 0;
  std::vector<float> chunk(256);
  const float* ch[1] = {chunk.data()};
  auto feed = [&]() {
    for (float& v : chunk) v = static_cast<float>(0.5 * std::sin(kTwoPi * 600.0 * double(n++) / kRate));
    port->Write(ch, 1, 256);
  };
  for (int i = 0; i < 40; ++i) {
    feed();
    r.Step();
  }
  std::vector<float> x = r.Channel(0);
  const double a = std::abs(isrc::Coef(x, x.size() - 4800, 4800, 60.0));
  SND_CHECK_MSG(a > 0.2, "input before virtual: %f", a);
  // Two high-priority oscillators take the real voices.
  const snd_playback_handle h1 = r.Play("Hi1000");
  const snd_playback_handle h2 = r.Play("Hi1500");
  const std::uint64_t read0 = port->Stats().readFrames;
  for (int i = 0; i < 100; ++i) {
    feed();
    r.Step();
    SND_CHECK(port->Buffered() < 2048u);
  }
  snd_stats st{};
  r.sys->GetStats(&st);
  SND_CHECK_EQ(st.virtual_voices, 1u);
  x = r.Channel(0);
  SND_CHECK(std::abs(isrc::Coef(x, x.size() - 4800, 4800, 60.0)) < 0.01 * a);  // not heard
  SND_CHECK(port->Stats().readFrames - read0 > 90u * 256u);                     // but consumed
  SND_CHECK_EQ(port->Stats().droppedFrames, std::uint64_t{0});
  r.sys->Stop(h1, 0.0f, nullptr);
  for (int i = 0; i < 40; ++i) {
    feed();
    r.Step();
  }
  x = r.Channel(0);
  SND_CHECK_NEAR(std::abs(isrc::Coef(x, x.size() - 4800, 4800, 60.0)), a, 0.03 * a);
  SND_CHECK(r.EndOf(in) == nullptr);
  r.sys->Stop(h2, 0.0f, nullptr);
  r.sys->Stop(in, 0.0f, nullptr);
  r.Steps(4);
  SND_CHECK(r.EndOf(in) != nullptr);
  SND_CHECK_EQ(r.runtime.Stats().inputInstances, 0u);
  // The port can be played again.
  const snd_playback_handle again = r.sys->PlayInput(&d, snd_emitter_handle{0}, nullptr);
  r.Steps(4);
  SND_CHECK(r.EndOf(again) == nullptr);
  SND_CHECK_EQ(r.runtime.Stats().inputInstances, 1u);
  // An unknown port ends its playback (the start fails), without touching the others.
  snd_input_desc bad = d;
  bad.device_index = 4;  // RuntimeDesc default: 4 ports
  const snd_playback_handle hb = r.sys->PlayInput(&bad, snd_emitter_handle{0}, nullptr);
  r.Steps(4);
  SND_CHECK(r.EndOf(hb) != nullptr);
  SND_CHECK(r.EndOf(again) == nullptr);
}

// Threads (TSan): RT workers render the chunks of many oscillator / noise voices, a writer
// thread feeds an input port, playbacks start and stop while the audio side runs.
SND_TEST(IndepSrc1Core, ThreadedRenderWithAWriterThread) {
  Built b = BuildBank("thr");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  CoreRig r(64, 256, true, 2);
  SND_REQUIRE(r.Ok());
  SND_REQUIRE(r.Load(b));
  snd::source::InputPort* port = r.runtime.InputPortOf(0);
  SND_REQUIRE(port != nullptr);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::vector<float> c(97, 0.25f);
    const float* ch[1] = {c.data()};
    while (!stop.load(std::memory_order_acquire)) {
      port->Write(ch, 1, 97);
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  });
  snd_input_desc d{};
  d.device_index = 0;
  d.channels = 1;
  const snd_playback_handle in = r.sys->PlayInput(&d, snd_emitter_handle{0}, nullptr);
  const char* evs[] = {"W_sine", "W_saw", "N_white", "N_pink", "N_brown", "W_pulse"};
  std::vector<snd_playback_handle> hs;
  for (int blk = 0; blk < 300; ++blk) {
    if (blk % 3 == 0 && hs.size() < 80) hs.push_back(r.Play(evs[blk % 6]));
    if (blk % 7 == 0 && hs.size() > 10) {
      r.sys->Stop(hs.front(), 0.01f, nullptr);
      hs.erase(hs.begin());
    }
    r.Step();
  }
  stop.store(true, std::memory_order_release);
  writer.join();
  SND_CHECK(isrc::AllFinite(r.all));
  SND_CHECK(r.EndOf(in) == nullptr);
  const snd::source::InputPortStats s = port->Stats();
  SND_CHECK_EQ(s.writtenFrames, s.readFrames + s.skippedFrames + port->Buffered());
}
