// Test harness of the source module: a project (JSON + WAV) built by
// buildtool into a bank and stream files, served by the memory FS through the
// IO scheduler, played by core with the source module's Sample / Stream kinds.
//
// Two drives:
//   Deterministic (default): no IO thread (IoScheduler::RunOnce before each
//     block), the deterministic job system in Immediate mode (jobs run inside
//     PumpServices, i.e. in Update before each block), no RT workers.
//   Threaded: IO thread, jobs::WorkerPool, RT workers (concurrency tests).
#ifndef SND_SOURCE_TEST_SOURCE_HARNESS_HPP
#define SND_SOURCE_TEST_SOURCE_HARNESS_HPP

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mini_project.hpp"
#include "snd/base/allocator.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/codec/codec.hpp"
#include "snd/core/core.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/source/runtime.hpp"
#include "snd/test/test.hpp"

// SND_REQUIRE with a message (the test kit has SND_CHECK_MSG only).
#ifndef SND_REQUIRE_MSG
#define SND_REQUIRE_MSG(cond, ...)      \
  do {                                  \
    if (!(cond)) {                      \
      SND_CHECK_MSG(cond, __VA_ARGS__); \
      return;                           \
    }                                   \
  } while (0)
#endif

namespace srctest {

namespace bt = snd::buildtool;

// --- signals ------------------------------------------------------------------------
// Values exactly representable in int16 (PC16 round trips without error).
inline float Q16(double v) {
  double q = std::round(v * 32768.0);
  if (q > 32767.0) q = 32767.0;
  if (q < -32768.0) q = -32768.0;
  return static_cast<float>(q / 32768.0);
}
inline bt::PcmData Signal(std::uint32_t channels, std::uint32_t rate, std::uint64_t frames,
                          double (*f)(std::uint32_t ch, std::uint64_t i)) {
  bt::PcmData p;
  p.channels = channels;
  p.sampleRate = rate;
  p.frames = frames;
  p.samples.resize(static_cast<std::size_t>(frames * channels));
  for (std::uint64_t i = 0; i < frames; ++i) {
    for (std::uint32_t c = 0; c < channels; ++c) p.samples[static_cast<std::size_t>(i * channels + c)] = Q16(f(c, i));
  }
  return p;
}
// Ramp that never touches 0: frame i -> 0.1 + 0.8 * (i mod period) / period.
inline double Ramp(std::uint32_t c, std::uint64_t i) {
  const double v = 0.1 + 0.8 * static_cast<double>(i % 4000u) / 4000.0;
  return c == 0 ? v : -v;
}
inline double Sine1k(std::uint32_t c, std::uint64_t i) {
  return (c == 0 ? 0.5 : 0.25) * std::sin(6.283185307179586 * 1000.0 * static_cast<double>(i) / 48000.0);
}

// --- project ------------------------------------------------------------------------
struct AssetSpec {
  std::string name;
  bt::PcmData pcm;
  std::string loadForm = "pcm";   // pcm / compressed / decompressCache / stream
  std::string codec = "PC16";     // PCMF / PC16 / ADPC / OPUS
  std::string extra;              // more JSON members of the asset
  std::string clip;               // more JSON members of the event's clip
  bool floatWav = true;
};

struct Built {
  bool ok = false;
  std::string diag;
  std::vector<std::uint8_t> bank;
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;  // stream files (relative paths)
};

// Bank "Main" with master bus "Master" (stereo) and, per asset, an event named like the asset.
inline Built Build(const std::string& tag, const std::vector<AssetSpec>& assets) {
  Built b;
  mini::Project proj("src-" + tag);
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true, \"channels\": 2");
  for (const AssetSpec& a : assets) {
    proj.Wav("audio/" + a.name + ".wav", a.pcm, a.floatWav ? bt::WavSampleFormat::Float32 : bt::WavSampleFormat::Int16);
    std::string body = "\"file\": \"audio/" + a.name + ".wav\", \"loadForm\": \"" + a.loadForm +
                       "\", \"convert\": {\"default\": {\"codec\": \"" + a.codec + "\"}}";
    if (!a.extra.empty()) body += ", " + a.extra;
    proj.Add("assets/" + a.name + ".json", "asset", a.name, body);
    std::string clip = "{\"asset\": \"" + a.name + "\"";
    if (!a.clip.empty()) clip += ", " + a.clip;
    clip += "}";
    proj.Add("events/" + a.name + ".json", "event", a.name,
             "\"bank\": \"Main\", \"bus\": \"Master\", \"tracks\": [{\"clips\": [" + clip + "]}]");
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
  for (const std::string& s : res.streams) b.files.emplace_back(s, mini::ReadBytes(proj.out() / s));
  b.ok = !b.bank.empty();
  return b;
}

// --- runtime ----------------------------------------------------------------------------
struct Options {
  bool threaded = false;
  std::uint32_t rtWorkers = 0;
  std::uint32_t maxStreams = 4;
  std::uint32_t maxDecoders = 8;
  std::uint32_t chunkBytes = 8u << 10;
  std::uint64_t poolBytes = 512u << 10;
  std::uint32_t ringChunks = 4;
  std::uint32_t fsLatency = 0;  // polls per read (memory FS)
  std::uint32_t maxVoiceChannels = 2;
  std::uint64_t pcmCacheBytes = 4u << 20;
  std::uint32_t pcmPageBytes = 64u << 10;
  snd::IAllocator* allocator = nullptr;
  bool loadLod = false;  // the load LOD (wall-clock budget, 2.4): off unless a test is about it
};

struct EndRec {
  std::uint64_t handle;
  snd_end_reason reason;
  snd_end_detail detail;
};

class Rig {
 public:
  explicit Rig(const Options& o = {}) : opt_(o) {
    alloc_ = o.allocator != nullptr ? o.allocator : &snd::base::DefaultAllocator();
    fs.SetDefaultLatency(o.fsLatency);
    if (o.threaded) {
      snd::jobs::WorkerPoolConfig wc;
      wc.workers = 2;
      ok_ = workers_.Init(*alloc_, wc);
    }
    ok_ = ok_ && pool.Init(*alloc_, o.poolBytes, o.chunkBytes, o.maxStreams);
    snd::io::IoSchedulerConfig ic;
    ic.maxStreams = o.maxStreams;
    ic.ringChunks = o.ringChunks;
    ic.bytesPerWindow = 0;
    ok_ = ok_ && io.Init(*alloc_, fs, pool, ic);
    if (o.threaded) ok_ = ok_ && io.Start();
    snd::source::RuntimeDesc rd;
    rd.allocator = alloc_;
    rd.io = &io;
    rd.streamPool = &pool;
    rd.maxVoiceChannels = o.maxVoiceChannels;
    ok_ = ok_ && runtime.Init(rd);
    snd_config_init(&cfg);
    cfg.max_streams = o.maxStreams;
    cfg.memory.max_decoders = o.maxDecoders;
    cfg.memory.stream_chunk_bytes = o.chunkBytes;
    cfg.memory.stream_pool_bytes = o.poolBytes;
    cfg.memory.pcm_cache_bytes = o.pcmCacheBytes;
    cfg.memory.pcm_cache_page_bytes = o.pcmPageBytes;
    cfg.jobs.realtime_workers = o.rtWorkers;
    cfg.max_voice_channels = o.maxVoiceChannels;
    cfg.max_real_voices = 160;
    if (!o.loadLod) cfg.tuning.lod_down_blocks = 0;  // a busy machine must not virtualise voices
    cfg.max_virtual_voices = 256;
    snd::core::CoreDesc d;
    d.config = &cfg;
    d.allocator = alloc_;
    d.sources = &runtime.Kinds();
    d.jobs = o.threaded ? static_cast<snd::IJobSystem*>(&workers_) : static_cast<snd::IJobSystem*>(&jobs);
    d.bankScheduler = &io;
    d.bankUnregistered = &snd::source::Runtime::BankUnregistered;
    d.bankUser = &runtime;
    d.rtWorkerRealtimePriority = false;
    created = ok_ ? snd::core::System::Create(d, &sys) : SND_ERR_OUT_OF_BUDGET;
    if (sys != nullptr) {
      snd_callbacks cb{};
      cb.user = this;
      cb.on_end = [](void* u, snd_playback_handle h, snd_end_reason r, snd_end_detail det) {
        static_cast<Rig*>(u)->ends.push_back(EndRec{h.value, r, det});
      };
      sys->SetCallbacks(&cb);
      out.resize(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
    }
  }
  ~Rig() { Teardown(); }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;

  void Teardown() {
    if (torn_) return;
    torn_ = true;
    // bank_manager.hpp order: audio stopped, IO scheduler, jobs, core, then memory.
    io.Shutdown();
    if (opt_.threaded) workers_.Shutdown();
    if (sys != nullptr) snd::core::System::Destroy(sys);
    sys = nullptr;
    runtime.Destroy();
    io.Destroy();
    pool.Destroy();
  }
  bool Ok() const { return ok_ && created == SND_OK && sys != nullptr; }

  void PumpIo() {
    if (!opt_.threaded) {
      for (int i = 0; i < 8 && io.RunOnce(); ++i) {
      }
    }
  }
  void Step() {
    PumpIo();
    sys->Update();
    PumpIo();
    sys->RenderBlock(out.data());
    all.insert(all.end(), out.begin(), out.end());
    if (opt_.threaded) std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  void Steps(std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) Step();
  }
  // Adds the bank and its stream files to the memory FS and loads the bank.
  snd_bank_handle Load(const Built& b) { return LoadAs(b, "Main.sndb"); }
  // Under another path: the IO scheduler keeps files open and the memory FS
  // serves the bytes of the open time, so a new bank needs a new path.
  snd_bank_handle LoadAs(const Built& b, const char* path) {
    fs.AddFile(path, b.bank);
    for (const auto& f : b.files) fs.AddFile(f.first, f.second);
    const snd_bank_handle h = sys->LoadBankAsync(path);
    for (int i = 0; i < 2000 && sys->GetBankState(h) != SND_BANK_STATE_LOADED; ++i) {
      Step();
      if (opt_.threaded) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sys->Update();
    all.clear();
    return h;
  }
  snd_playback_handle Play(const char* event) { return sys->Play(snd::HashId(event), snd_emitter_handle{0}, nullptr); }
  std::uint32_t Ch() const { return sys->OutputChannels(); }
  std::size_t Frames() const { return all.size() / Ch(); }
  float At(std::size_t frame, std::uint32_t c) const { return all[frame * Ch() + c]; }
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
  std::vector<float> out;
  std::vector<float> all;
  std::vector<EndRec> ends;

 private:
  Options opt_;
  snd::IAllocator* alloc_ = nullptr;
  snd::jobs::WorkerPool workers_;
  bool ok_ = true;
  bool torn_ = false;
};

// --- analysis -------------------------------------------------------------------------
// First frame of channel c whose |value| exceeds eps, or SIZE_MAX.
inline std::size_t FirstNonZero(const Rig& r, std::uint32_t c, float eps = 1e-6f, std::size_t from = 0) {
  for (std::size_t f = from; f < r.Frames(); ++f) {
    if (std::fabs(r.At(f, c)) > eps) return f;
  }
  return SIZE_MAX;
}
// Least-squares gain of out[c][at..at+n) against ref[0..n) and the largest residual.
struct Fit {
  double gain = 0;
  double maxErr = 0;
};
inline Fit FitGain(const Rig& r, std::uint32_t c, std::size_t at, const std::vector<float>& ref) {
  Fit f;
  double num = 0, den = 0;
  for (std::size_t i = 0; i < ref.size() && at + i < r.Frames(); ++i) {
    num += double(r.At(at + i, c)) * ref[i];
    den += double(ref[i]) * ref[i];
  }
  f.gain = den > 0 ? num / den : 0;
  for (std::size_t i = 0; i < ref.size(); ++i) {
    const double o = at + i < r.Frames() ? r.At(at + i, c) : 0.0;
    const double e = std::fabs(o - f.gain * ref[i]);
    if (e > f.maxErr) f.maxErr = e;
  }
  return f;
}
// Channel c of an interleaved PcmData.
inline std::vector<float> Chan(const bt::PcmData& p, std::uint32_t c) {
  std::vector<float> v(static_cast<std::size_t>(p.frames));
  for (std::uint64_t i = 0; i < p.frames; ++i) v[static_cast<std::size_t>(i)] = p.samples[static_cast<std::size_t>(i * p.channels + c)];
  return v;
}
// Played sequence of a loop: [0, le) then [lb, le) repeated, `n` frames.
inline std::vector<float> LoopModel(const std::vector<float>& x, std::uint64_t lb, std::uint64_t le, std::size_t n) {
  std::vector<float> v;
  v.reserve(n);
  std::uint64_t pos = 0;
  while (v.size() < n) {
    v.push_back(x[static_cast<std::size_t>(pos)]);
    if (++pos == le) pos = lb;
  }
  return v;
}
// Goertzel amplitude of `hz` on channel c over [from, from + n).
inline double Tone(const Rig& r, std::uint32_t c, std::size_t from, std::size_t n, double hz, double rate = 48000.0) {
  const double w = 6.283185307179586 * hz / rate;
  double re = 0, im = 0, ws = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(6.283185307179586 * double(i) / double(n - 1));
    const double v = r.At(from + i, c) * win;
    re += v * std::cos(w * double(i));
    im -= v * std::sin(w * double(i));
    ws += win;
  }
  return 2.0 * std::sqrt(re * re + im * im) / ws;
}

}  // namespace srctest

#endif  // SND_SOURCE_TEST_SOURCE_HARNESS_HPP
