// Test rig of the api module: one snd_system made through the C API (snd_create)
// on the null output device and an in-memory file system, driven by pumping the
// device period by period. The audio thread (platform::AudioDriver) renders
// ahead into its ring; Step() waits until the ring is back at its target before
// the next period, so the device never underruns and captured frame i is the
// common-clock sample i (the first captured frame is the first block core rendered).
#ifndef SND_API_TEST_API_RIG_HPP
#define SND_API_TEST_API_RIG_HPP

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "snd/api/compose.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/mocks/null_output_device.hpp"
#include "snd/platform/audio_driver.hpp"
#include "snd/source/runtime.hpp"
#include "snd/sonus.h"
#include "snd/sonus.hpp"
#include "snd/test/test.hpp"

namespace apitest {

inline constexpr float kCenter = 0.70710678f;  // mono voice on the stereo master (constant-power centre)

// A small configuration (memory) for the tests; LOD off (CONTRACTS.md 4).
inline snd_config SmallConfig() {
  snd_config c;
  snd_config_init(&c);
  c.max_real_voices = 32;
  c.max_virtual_voices = 64;
  c.fade_reserve_voices = 8;
  c.max_playbacks = 64;
  c.max_emitters = 16;
  c.max_transports = 8;
  c.max_reservations = 64;
  c.max_streams = 4;
  c.max_banks = 8;
  c.max_user_buffers = 4;
  c.max_global_params = 32;
  c.param_overflow_blocks = 16;
  c.event_ring_capacity = 4096;
  c.max_registered_threads = 4;
  c.commands.arena_bytes = 32u << 10;
  c.commands.reserve_bytes = 4u << 10;
  c.commands.overflow_param_slots = 32;
  c.memory.stream_pool_bytes = 1u << 20;
  c.memory.pcm_cache_bytes = 1u << 20;
  c.memory.max_decoders = 8;
  c.memory.frame_arena_bytes = 128u << 10;
  c.memory.thread_arena_bytes = 128u << 10;
  c.jobs.realtime_workers = 0;
  c.jobs.normal_workers = 2;
  c.max_voice_channels = 2;
  c.max_buses = 16;
  c.max_categories = 8;
  c.max_snapshots = 8;
  c.max_limit_groups = 8;
  c.max_param_defs = 32;
  c.max_duck_rules = 8;
  c.tuning.lod_down_blocks = 0;
  return c;
}

struct Beat {
  std::uint64_t transport;
  std::int32_t bar, beat;
  std::uint64_t clock;
};
struct End {
  std::uint64_t handle;
  snd_end_reason reason;
  snd_end_detail detail;
};
struct Error {
  snd_result code;
  std::uint32_t detail;
  std::uint64_t object;
};
struct Events {
  std::vector<Beat> beats;
  std::vector<End> ends;
  std::vector<Error> errors;
  std::vector<std::pair<std::uint64_t, snd_result>> banks;
};

class Rig {
 public:
  explicit Rig(snd_config cfg = SmallConfig(), snd::mock::NullDeviceConfig dc = snd::mock::NullDeviceConfig{})
      : device(dc) {
    devTable_ = snd::ToC(device);
    fsTable_ = snd::ToC(fs);
    cfg.output.device = &devTable_;
    created = snd_create(&cfg, nullptr, &fsTable_, &sys);
    if (sys == nullptr) return;
    snd_callbacks cb{};
    cb.user = &ev;
    cb.on_beat = [](void* u, snd_transport_handle t, const snd_musical_time* p, const snd_timing* tm) {
      static_cast<Events*>(u)->beats.push_back(Beat{t.value, p->bar, p->beat, tm->clock_sample});
    };
    cb.on_end = [](void* u, snd_playback_handle h, snd_end_reason r, snd_end_detail d) {
      static_cast<Events*>(u)->ends.push_back(End{h.value, r, d});
    };
    cb.on_error = [](void* u, snd_result r, std::uint32_t d, std::uint64_t o) {
      static_cast<Events*>(u)->errors.push_back(Error{r, d, o});
    };
    cb.on_bank_loaded = [](void* u, snd_bank_handle b, snd_result r) {
      static_cast<Events*>(u)->banks.emplace_back(b.value, r);
    };
    snd_set_callbacks(sys, &cb);
  }
  ~Rig() { snd_destroy(sys); }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;

  snd::platform::AudioDriver* Driver() { return snd::compose::PartsOf(sys).driver; }

  // Waits until the audio thread has refilled its ring to the target (render ahead).
  bool WaitRefill() {
    snd::platform::AudioDriver* d = Driver();
    if (d == nullptr) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
      const snd::platform::AudioDriverStats s = d->Stats();
      const std::uint64_t ahead = s.core.blocksRendered * BlockFrames() - s.core.framesConsumed;
      if (ahead >= s.core.targetFrames && s.core.targetFrames != 0u) return true;
      if (std::chrono::steady_clock::now() > until) return false;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  // The decode jobs and stream reads posted so far finish before the next block,
  // whatever the machine's load (TSan): analytic checks then measure the data, not the
  // scheduling (as source_end_to_end_test). Off: the runtime's own timing.
  void Settle() {
    const snd::compose::Parts p = snd::compose::PartsOf(sys);
    snd_update(sys);  // pumps core's service queues into the workers / IO
    if (p.workers != nullptr) p.workers->WaitIdle(2000000000ull);
    if (p.io != nullptr) {
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      const std::uint64_t e0 = p.io->DrainEpoch();
      for (;;) {
        const snd::io::IoStats is = p.io->Stats();
        if ((p.io->DrainEpoch() >= e0 + 2u && is.pending == 0u && is.inFlight == 0u) ||
            std::chrono::steady_clock::now() > until) {
          break;
        }
        p.io->Wake();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    }
  }
  // One device period per step (= one block with the default config), after snd_update.
  void Step(std::uint32_t periods = 1) {
    for (std::uint32_t i = 0; i < periods; ++i) {
      snd_update(sys);
      if (!WaitRefill()) ++refillTimeouts;
      if (settle) Settle();
      device.Pump(1);
    }
  }
  // Steps until `frames` frames are captured in total.
  void RunTo(std::uint64_t frames) {
    while (device.CapturedFrames() < frames) Step();
  }
  std::uint32_t BlockFrames() const { return 256u; }
  // Captured channel 0 at common clock `clock`, normalised by the centre pan of a mono voice.
  float At(std::uint64_t clock) const {
    const std::vector<float>& c = device.Captured();
    const std::size_t ch = device.Channels();
    const std::size_t i = static_cast<std::size_t>(clock) * ch;
    return i < c.size() ? c[i] / kCenter : 0.0f;
  }
  std::uint64_t Frames() const { return device.CapturedFrames(); }

  bool LoadBank(const std::string& path, const std::vector<std::uint8_t>& bytes, snd_bank_handle* out = nullptr) {
    fs.AddFile(path, bytes);
    const snd_bank_handle h = snd_load_bank_async(sys, path.c_str());
    if (out != nullptr) *out = h;
    for (int i = 0; i < 2000 && snd_get_bank_state(sys, h) != SND_BANK_STATE_LOADED; ++i) {
      Step();
      if (snd_get_bank_state(sys, h) == SND_BANK_STATE_FAILED) break;
    }
    snd_update(sys);
    return snd_get_bank_state(sys, h) == SND_BANK_STATE_LOADED;
  }

  snd::mock::NullOutputDevice device;
  snd::mock::MemoryFileSystem fs;
  snd_system* sys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  Events ev;
  std::uint32_t refillTimeouts = 0;
  bool settle = true;

 private:
  snd_output_device devTable_{};
  snd_file_system fsTable_{};
};

// Amplitude of a sine of `hz` in channel 0 over [from, from + n) (Hann window, normalised).
inline double Level(const Rig& r, std::uint64_t from, std::size_t n, double hz) {
  const double w = 6.283185307179586 * hz / 48000.0;
  double re = 0, im = 0, ws = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(6.283185307179586 * double(i) / double(n - 1));
    const double v = r.At(from + i) * win;
    re += v * std::cos(w * double(i));
    im -= v * std::sin(w * double(i));
    ws += win;
  }
  return 2.0 * std::sqrt(re * re + im * im) / ws;
}

}  // namespace apitest

#endif  // SND_API_TEST_API_RIG_HPP
