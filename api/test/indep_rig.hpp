// Independent tests of the api stage (spec/handoff/README.md 3): a test rig like
// api_rig.hpp but with a counting game allocator (the snd_allocator table), so the
// tests can check "every byte back" and "no allocation after snd_create" (14).
// The rig drives the null device period by period and waits for the audio
// thread's refill, so captured frame i is common-clock sample i.
#ifndef SND_API_TEST_INDEP_RIG_HPP
#define SND_API_TEST_INDEP_RIG_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include <thread>
#include <vector>

#include "api_rig.hpp"

namespace indep {

// Aligned allocation that builds on POSIX and on Windows (MinGW / MSVC).
inline void* AlignedAlloc(std::size_t bytes, std::size_t align) {
#if defined(_WIN32)
  return _aligned_malloc(bytes, align);
#else
  void* p = nullptr;
  return posix_memalign(&p, align, bytes) == 0 ? p : nullptr;
#endif
}
inline void AlignedFree(void* p) {
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}

// Counting allocator through the C table. `failAt` > 0: the failAt-th call (1-based)
// returns NULL (fault injection for the roll-back of snd_create).
struct CountingAllocator {
  std::atomic<std::int64_t> live{0};
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> failAt{0};
  std::atomic<std::uint64_t> failures{0};
  static void* SND_CALL Alloc(void* u, size_t n, size_t align, snd_mem_tag) {
    auto* c = static_cast<CountingAllocator*>(u);
    const std::uint64_t k = c->calls.fetch_add(1) + 1u;
    const std::uint64_t f = c->failAt.load();
    if (f != 0u && k == f) {
      c->failures.fetch_add(1);
      return nullptr;
    }
    if (align < 16) align = 16;
    void* p = AlignedAlloc(n == 0 ? 1 : n, align);
    if (p == nullptr) return nullptr;
    c->live.fetch_add(1);
    return p;
  }
  static void SND_CALL Free(void* u, void* p, snd_mem_tag) {
    if (p == nullptr) return;
    static_cast<CountingAllocator*>(u)->live.fetch_sub(1);
    AlignedFree(p);
  }
  snd_allocator Table() { return snd_allocator{this, &Alloc, &Free}; }
};

struct Events {
  std::vector<apitest::End> ends;
  std::vector<apitest::Error> errors;
  std::vector<apitest::Beat> beats;
};

class Rig {
 public:
  explicit Rig(snd_config cfg = apitest::SmallConfig(), snd::mock::NullDeviceConfig dc = snd::mock::NullDeviceConfig{},
               bool installCallbacks = true)
      : device(dc) {
    devTable_ = snd::ToC(device);
    fsTable_ = snd::ToC(fs);
    allocTable_ = alloc.Table();
    cfg.output.device = &devTable_;
    created = snd_create(&cfg, &allocTable_, &fsTable_, &sys);
    if (sys != nullptr && installCallbacks) InstallCallbacks();
  }
  ~Rig() { snd_destroy(sys); }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;

  void InstallCallbacks() {
    snd_callbacks cb{};
    cb.user = &ev;
    cb.on_end = [](void* u, snd_playback_handle h, snd_end_reason r, snd_end_detail d) {
      static_cast<Events*>(u)->ends.push_back(apitest::End{h.value, r, d});
    };
    cb.on_error = [](void* u, snd_result r, std::uint32_t d, std::uint64_t o) {
      static_cast<Events*>(u)->errors.push_back(apitest::Error{r, d, o});
    };
    cb.on_beat = [](void* u, snd_transport_handle t, const snd_musical_time* p, const snd_timing* tm) {
      static_cast<Events*>(u)->beats.push_back(apitest::Beat{t.value, p->bar, p->beat, tm->clock_sample});
    };
    snd_set_callbacks(sys, &cb);
  }

  snd::platform::AudioDriver* Driver() { return snd::compose::PartsOf(sys).driver; }

  bool WaitRefill() {
    snd::platform::AudioDriver* d = Driver();
    if (d == nullptr) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
      const snd::platform::AudioDriverStats s = d->Stats();
      // The ring fill (blocks rendered minus frames consumed is not the fill after an
      // underrun: the consumed count includes the silence the device played).
      if (s.core.queuedFrames >= s.core.targetFrames && s.core.targetFrames != 0u) return true;
      if (std::chrono::steady_clock::now() > until) return false;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  void Settle() {
    const snd::compose::Parts p = snd::compose::PartsOf(sys);
    snd_update(sys);
    if (p.workers != nullptr) p.workers->WaitIdle(2000000000ull);
  }
  void Step(std::uint32_t periods = 1) {
    for (std::uint32_t i = 0; i < periods; ++i) {
      snd_update(sys);
      if (!WaitRefill()) ++refillTimeouts;
      if (settle) Settle();
      device.Pump(1);
    }
  }
  std::uint64_t RenderClock() { return Driver()->Stats().core.blocksRendered * 256u; }
  // Captured sample of channel `ch` at common clock `clock` (raw, not normalised).
  float Raw(std::uint64_t clock, std::uint32_t ch = 0) const {
    const std::vector<float>& c = device.Captured();
    const std::size_t i = static_cast<std::size_t>(clock) * device.Channels() + ch;
    return i < c.size() ? c[i] : -100.0f;
  }
  std::uint64_t Frames() const { return device.CapturedFrames(); }
  std::uint32_t EndsOf(std::uint64_t h) const {
    std::uint32_t n = 0;
    for (const apitest::End& e : ev.ends) n += e.handle == h ? 1u : 0u;
    return n;
  }

  CountingAllocator alloc;
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
  snd_allocator allocTable_{};
};

// LowLatency mono synth writing a constant (user points at the float).
inline void SND_CALL DcSynth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                             const snd_render_context*) {
  const float v = *static_cast<const float*>(user);
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = v;
  }
}

inline snd_synth_desc DcDesc(const float* value, snd_synth_mode mode = SND_SYNTH_LOW_LATENCY, std::uint32_t ch = 1) {
  snd_synth_desc d{};
  d.callback = &DcSynth;
  d.user = const_cast<float*>(value);
  d.channels = ch;
  d.mode = mode;
  d.volume = 1.0f;
  return d;
}

}  // namespace indep

#endif  // SND_API_TEST_INDEP_RIG_HPP
