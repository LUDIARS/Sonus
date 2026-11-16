// Test harness: one core System with the test source kinds, a memory file
// system for banks (DirectFileSystem mode) and the deterministic job system.
#ifndef SND_CORE_TEST_HARNESS_HPP
#define SND_CORE_TEST_HARNESS_HPP

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/core/core.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"
#include "test_bank.hpp"
#include "test_sources.hpp"

namespace coretest {

inline snd_config SmallConfig() {
  snd_config c;
  snd_config_init(&c);
  c.max_real_voices = 16;
  c.max_virtual_voices = 64;
  c.fade_reserve_voices = 4;
  c.max_playbacks = 64;
  c.max_emitters = 16;
  c.max_transports = 4;
  c.max_reservations = 32;
  c.max_streams = 4;
  c.max_banks = 8;
  c.max_user_buffers = 4;
  c.max_global_params = 32;
  c.param_overflow_blocks = 8;
  c.event_ring_capacity = 256;
  c.max_registered_threads = 4;
  c.commands.arena_bytes = 16u << 10;
  c.commands.reserve_bytes = 2u << 10;
  c.commands.overflow_param_slots = 16;
  c.memory.frame_arena_bytes = 128u << 10;
  c.memory.thread_arena_bytes = 256u << 10;
  c.jobs.realtime_workers = 0;
  c.jobs.voices_per_chunk = 4;
  c.max_voice_channels = 2;
  return c;
}

struct Ends {
  struct E {
    std::uint64_t handle;
    snd_end_reason reason;
    snd_end_detail detail;
  };
  std::vector<E> list;
  std::uint32_t loaded = 0;
  std::vector<snd_result> errors;
  bool Has(snd_playback_handle h, snd_end_reason r, snd_end_detail d = 0xFFFFFFFFu) const {
    for (const E& e : list) {
      if (e.handle == h.value && e.reason == r && (d == 0xFFFFFFFFu || e.detail == d)) return true;
    }
    return false;
  }
  std::uint32_t CountOf(snd_playback_handle h) const {
    std::uint32_t n = 0;
    for (const E& e : list) n += e.handle == h.value ? 1u : 0u;
    return n;
  }
};

class Harness {
 public:
  explicit Harness(const snd_config& cfg = SmallConfig(), snd::core::CoreLimits limits = {},
                   snd::music::IMusicSystem* music = nullptr, std::uint32_t claimDelayUs = 0) {
    kinds_.kinds[static_cast<int>(snd::SourceKind::Sample)] = &sample;
    kinds_.kinds[static_cast<int>(snd::SourceKind::Oscillator)] = &osc;
    snd::core::CoreDesc d;
    d.config = &cfg;
    d.allocator = &snd::base::DefaultAllocator();
    d.sources = &kinds_;
    d.music = music;
    d.jobs = &jobs;
    d.bankFileSystem = &fs;
    d.rtWorkerRealtimePriority = false;
    d.rtDebugClaimDelayMicroseconds = claimDelayUs;
    d.limits = limits;
    created = snd::core::System::Create(d, &sys);
    if (sys != nullptr) {
      snd_callbacks cb{};
      cb.user = &ends;
      cb.on_end = [](void* u, snd_playback_handle h, snd_end_reason r, snd_end_detail det) {
        static_cast<Ends*>(u)->list.push_back(Ends::E{h.value, r, det});
      };
      cb.on_bank_loaded = [](void* u, snd_bank_handle, snd_result r) {
        if (r == SND_OK) ++static_cast<Ends*>(u)->loaded;
      };
      cb.on_error = [](void* u, snd_result r, std::uint32_t, std::uint64_t) {
        static_cast<Ends*>(u)->errors.push_back(r);
      };
      sys->SetCallbacks(&cb);
      out.resize(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
    }
  }
  ~Harness() { snd::core::System::Destroy(sys); }
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  // Update (publish + callbacks) then one block; appends the block to `all`.
  void Step() {
    sys->Update();
    sys->RenderBlock(out.data());
    all.insert(all.end(), out.begin(), out.end());
  }
  void Steps(std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) Step();
  }
  // Loads a bank from memory and renders until it is registered.
  snd_bank_handle Load(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    fs.AddFile(path, bytes);
    const snd_bank_handle h = sys->LoadBankAsync(path.c_str());
    for (int i = 0; i < 64 && sys->GetBankState(h) != SND_BANK_STATE_LOADED; ++i) Step();
    sys->Update();
    return h;
  }
  std::uint32_t Ch() const { return sys->OutputChannels(); }
  std::uint32_t Frames() const { return sys->BlockFrames(); }
  // Sample `frame` of channel `c` in `all`.
  float At(std::size_t frame, std::uint32_t c) const { return all[frame * Ch() + c]; }
  std::size_t TotalFrames() const { return all.size() / Ch(); }
  void Clear() { all.clear(); }

  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  TestSampleKind sample;
  TestOscKind osc;
  snd::core::System* sys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  Ends ends;
  std::vector<float> out;
  std::vector<float> all;

 private:
  snd::source::SourceKindSet kinds_{};
};

// Standard test bank: master bus (row 1, stereo) <- "Sfx" (row 0), and the
// given assets / events.
inline TestBank BasicBank() {
  TestBank b;
  b.buses.push_back(TBus{snd::HashId("Sfx"), 1u, 0, 0, 0.0f, {}, {}});
  b.buses.push_back(TBus{snd::HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  return b;
}
inline TEvent SimpleEvent(const char* name, std::uint32_t asset, std::uint32_t bus = 0, float volumeDb = 0) {
  TEvent e;
  e.id = snd::HashId(name);
  e.bus = bus;
  TTrack t;
  TClip c;
  c.kind = 0;
  c.target = asset;
  c.volumeDb = volumeDb;
  t.clips.push_back(c);
  e.tracks.push_back(t);
  return e;
}

// Amplitude of `hz` in x[from, from+n) (Goertzel, Hann window).
inline double ToneAmplitude(const std::vector<float>& x, std::size_t stride, std::size_t ch, std::size_t from,
                            std::size_t n, double hz, double rate) {
  const double w = 6.283185307179586 * hz / rate;
  double re = 0, im = 0, ws = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(6.283185307179586 * double(i) / double(n - 1));
    const double v = x[(from + i) * stride + ch] * win;
    re += v * std::cos(w * double(i));
    im -= v * std::sin(w * double(i));
    ws += win;
  }
  return 2.0 * std::sqrt(re * re + im * im) / ws;
}

}  // namespace coretest

#endif  // SND_CORE_TEST_HARNESS_HPP
