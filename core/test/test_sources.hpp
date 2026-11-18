// Source kinds for the core tests (the real Sample / Stream kinds are the
// source module's, stage 2 next group). Analytic output:
//   TestSampleKind (SourceKind::Sample, source rate): plays the asset's
//     resident PCMF data (interleaved float32) exactly, honours loops
//     (loopEnd != 0: [loopStart, loopEnd); kStartLoop without points: whole
//     asset); reports `ended` past the last frame.
//   TestOscKind (SourceKind::Oscillator, output rate): sine of
//     OscillatorParams::frequencyHz x the item's pitch ramp, amplitude 1.
// Both use the voice slot as their source slot. Optional per-render delay
// (late-chunk tests): RenderBatch spins `slowNs` when it is set.
// `holdOwnerForWorkerNs` (with slowNs) makes a late chunk certain: in a
// parallel batch (chunkGeneration != 0) the owner (audio) thread stays in
// its RenderBatch until an RT worker has entered a RenderBatch of the same
// batch (bounded by that many ns), and that worker's chunk also stays until
// the test has counted the block as rendered (`stepped` moved on, same bound),
// i.e. until the block's deadline has certainly passed: the chunk is late
// however long the audio thread was preempted.
#ifndef SND_CORE_TEST_TEST_SOURCES_HPP
#define SND_CORE_TEST_TEST_SOURCES_HPP

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>

#include "snd/internal/source_if.hpp"
#include "snd/platform/clock.hpp"

namespace coretest {

struct SampleInstance {
  const snd::source::SourceAsset* asset;
  std::uint32_t loop;
  std::uint32_t live;
};

class TestSampleKind final : public snd::source::ISourceKind {
 public:
  snd::SourceKind Kind() const noexcept override { return snd::SourceKind::Sample; }
  bool RendersAtOutputRate() const noexcept override { return false; }
  std::size_t PoolBytes(const snd::source::SourceLimits& l) const noexcept override {
    return sizeof(SampleInstance) * l.maxVoices;
  }
  bool Init(void* pool, std::size_t, const snd::source::SourceLimits& l,
            const snd::source::SourceServices&) noexcept override {
    inst_ = static_cast<SampleInstance*>(pool);
    cap_ = l.maxVoices;
    for (std::uint32_t i = 0; i < cap_; ++i) inst_[i] = SampleInstance{nullptr, 0, 0};
    return true;
  }
  void Shutdown() noexcept override {}
  void Start(const snd::source::SourceStartRequest* r, std::uint32_t n,
             snd::source::SourceStartResult* res) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      res[i] = snd::source::SourceStartResult{snd::kInvalidSlot, 0, 0, 0};
      if (r[i].asset == nullptr || r[i].voiceSlot >= cap_) continue;
      inst_[r[i].voiceSlot] = SampleInstance{r[i].asset, (r[i].flags & snd::source::kStartLoop) != 0u ? 1u : 0u, 1u};
      res[i].sourceSlot = r[i].voiceSlot;
      starts.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void Release(const snd::SlotIndex* s, std::uint32_t n) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      if (s[i] < cap_) inst_[s[i]].live = 0;
      releases.fetch_add(1, std::memory_order_relaxed);
    }
    releaseCalls.fetch_add(1, std::memory_order_relaxed);
  }
  void Control(const snd::source::SourceControlOp*, std::uint32_t n) noexcept override {
    controls.fetch_add(n, std::memory_order_relaxed);
  }
  void QueryReady(const snd::SlotIndex*, std::uint32_t n, std::uint8_t* ready) const noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) ready[i] = 1;
  }
  void RenderBatch(const snd::source::RenderBatch& b, snd::VoiceRenderStatus* st) noexcept override {
    batches.fetch_add(1, std::memory_order_relaxed);
    // Slow on RT workers; the thread that created the kind (the audio thread of the
    // tests) only a little, so that the workers get chunks even on a loaded machine.
    if (slowNs.load(std::memory_order_relaxed) != 0u) {
      const std::uint64_t ns = slowNs.load(std::memory_order_relaxed);
      const bool owner = std::this_thread::get_id() == owner_;
      const std::uint64_t hold = holdOwnerForWorkerNs.load(std::memory_order_relaxed);
      std::uint32_t entryStep = 0;
      if (!owner) {
        entryStep = stepped.load(std::memory_order_acquire);
        // Announce the batch this worker runs (max: a straggler of an older batch never lowers it).
        std::uint32_t seen = workerGeneration.load(std::memory_order_relaxed);
        while (seen < b.chunkGeneration &&
               !workerGeneration.compare_exchange_weak(seen, b.chunkGeneration, std::memory_order_relaxed)) {
        }
      } else if (hold != 0u && b.chunkGeneration != 0u) {
        const std::uint64_t limit = snd::platform::MonotonicNanoseconds() + hold;
        while (workerGeneration.load(std::memory_order_relaxed) < b.chunkGeneration &&
               snd::platform::MonotonicNanoseconds() < limit) {
          std::this_thread::yield();
        }
        if (workerGeneration.load(std::memory_order_relaxed) < b.chunkGeneration) ownerHoldTimeouts.fetch_add(1);
      }
      const std::uint64_t start = snd::platform::MonotonicNanoseconds();
      const std::uint64_t until = start + (owner ? ns / 100u : ns);
      while (snd::platform::MonotonicNanoseconds() < until) {
      }
      if (!owner && hold != 0u) {
        while (stepped.load(std::memory_order_acquire) == entryStep &&
               snd::platform::MonotonicNanoseconds() < start + hold) {
          std::this_thread::yield();
        }
      }
    }
    for (std::uint32_t i = 0; i < b.count; ++i) {
      const snd::source::RenderItem& it = b.items[i];
      st[i] = snd::VoiceRenderStatus{};
      if (it.sourceSlot >= cap_ || inst_[it.sourceSlot].live == 0u) continue;
      const SampleInstance& s = inst_[it.sourceSlot];
      const snd::source::SourceAsset& a = *s.asset;
      const std::uint32_t ch = a.codec.channels;
      const std::uint64_t total = a.codec.total_frames;
      const float* data = reinterpret_cast<const float*>(a.data);
      std::uint64_t lb = a.loopStart, le = a.loopEnd;
      if (s.loop != 0u && le == 0u) {
        lb = 0;
        le = total;
      }
      std::uint64_t pos = it.playhead->frame;
      std::uint32_t written = 0;
      for (std::uint32_t f = 0; f < it.framesRequested; ++f) {
        if (s.loop != 0u && le > lb && pos >= le) {
          pos = lb + (pos - le);
          ++it.playhead->loopCount;
        }
        if (pos >= total) {
          st[i].ended = 1;
          break;
        }
        for (std::uint32_t c = 0; c < it.channels; ++c) {
          it.out[c][it.frameOffset + f] = data[pos * ch + (c < ch ? c : ch - 1u)];
        }
        ++pos;
        ++written;
      }
      if (s.loop == 0u && pos >= total) st[i].ended = 1;
      it.playhead->frame = pos;
      st[i].framesRendered = written;
    }
  }
  void AdvanceVirtual(const snd::source::AdvanceItem* items, std::uint32_t n,
                      snd::VoiceRenderStatus* st) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      st[i] = snd::VoiceRenderStatus{};
      const SampleInstance& s = inst_[items[i].sourceSlot];
      const std::uint64_t adv = static_cast<std::uint64_t>(items[i].outputFrames * items[i].pitch);
      items[i].playhead->frame += adv;
      if (s.loop == 0u && items[i].playhead->frame >= s.asset->codec.total_frames) st[i].ended = 1;
      if (s.loop != 0u && s.asset->codec.total_frames != 0u) items[i].playhead->frame %= s.asset->codec.total_frames;
      advances.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void ScheduleBackground(const snd::source::BlockInfo&) noexcept override {}
  std::uint32_t PrimeFrames(const snd::source::SourceAsset&, std::uint64_t) const noexcept override { return 0; }

  std::atomic<std::uint32_t> starts{0}, releases{0}, releaseCalls{0}, controls{0}, batches{0}, advances{0};
  std::atomic<std::uint64_t> slowNs{0};
  std::atomic<std::uint64_t> holdOwnerForWorkerNs{0};
  std::atomic<std::uint32_t> workerGeneration{0};   // newest batch a worker rendered in (slow mode)
  std::atomic<std::uint32_t> ownerHoldTimeouts{0};  // parallel batches no worker joined in time
  std::atomic<std::uint32_t> stepped{0};            // the test counts rendered blocks (hold mode)

 private:
  std::thread::id owner_ = std::this_thread::get_id();
  SampleInstance* inst_ = nullptr;
  std::uint32_t cap_ = 0;
};

struct OscInstance {
  double phase;
  float freq;
  std::uint32_t live;
};

class TestOscKind final : public snd::source::ISourceKind {
 public:
  snd::SourceKind Kind() const noexcept override { return snd::SourceKind::Oscillator; }
  bool RendersAtOutputRate() const noexcept override { return true; }
  std::size_t PoolBytes(const snd::source::SourceLimits& l) const noexcept override {
    return sizeof(OscInstance) * l.maxVoices;
  }
  bool Init(void* pool, std::size_t, const snd::source::SourceLimits& l,
            const snd::source::SourceServices&) noexcept override {
    inst_ = static_cast<OscInstance*>(pool);
    cap_ = l.maxVoices;
    rate_ = l.sampleRate;
    return true;
  }
  void Shutdown() noexcept override {}
  void Start(const snd::source::SourceStartRequest* r, std::uint32_t n,
             snd::source::SourceStartResult* res) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      res[i] = snd::source::SourceStartResult{snd::kInvalidSlot, 0, 0, 0};
      const auto* p = static_cast<const snd::source::OscillatorParams*>(r[i].params);
      if (r[i].voiceSlot >= cap_) continue;
      inst_[r[i].voiceSlot] = OscInstance{0.0, p != nullptr ? p->frequencyHz : 440.0f, 1u};
      res[i].sourceSlot = r[i].voiceSlot;
    }
  }
  void Release(const snd::SlotIndex* s, std::uint32_t n) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      if (s[i] < cap_) inst_[s[i]].live = 0;
    }
  }
  void Control(const snd::source::SourceControlOp*, std::uint32_t) noexcept override {}
  void QueryReady(const snd::SlotIndex*, std::uint32_t n, std::uint8_t* ready) const noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) ready[i] = 1;
  }
  void RenderBatch(const snd::source::RenderBatch& b, snd::VoiceRenderStatus* st) noexcept override {
    for (std::uint32_t i = 0; i < b.count; ++i) {
      const snd::source::RenderItem& it = b.items[i];
      st[i] = snd::VoiceRenderStatus{};
      OscInstance& o = inst_[it.sourceSlot];
      const std::uint32_t n = it.framesRequested;
      for (std::uint32_t f = 0; f < n; ++f) {
        const double ratio = it.pitch.start + (it.pitch.end - it.pitch.start) * double(f) / double(n);
        const float v = static_cast<float>(std::sin(o.phase));
        for (std::uint32_t c = 0; c < it.channels; ++c) it.out[c][it.frameOffset + f] = v;
        o.phase += 6.283185307179586 * o.freq * ratio / rate_;
        if (o.phase > 6.283185307179586) o.phase -= 6.283185307179586;
      }
      st[i].framesRendered = n;
    }
  }
  void AdvanceVirtual(const snd::source::AdvanceItem* items, std::uint32_t n,
                      snd::VoiceRenderStatus* st) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      st[i] = snd::VoiceRenderStatus{};
      OscInstance& o = inst_[items[i].sourceSlot];
      o.phase = std::fmod(o.phase + 6.283185307179586 * o.freq * items[i].pitch * items[i].outputFrames / rate_,
                          6.283185307179586);
    }
  }
  void ScheduleBackground(const snd::source::BlockInfo&) noexcept override {}
  std::uint32_t PrimeFrames(const snd::source::SourceAsset&, std::uint64_t) const noexcept override { return 0; }

 private:
  OscInstance* inst_ = nullptr;
  std::uint32_t cap_ = 0;
  double rate_ = 48000;
};

}  // namespace coretest

#endif  // SND_CORE_TEST_TEST_SOURCES_HPP
