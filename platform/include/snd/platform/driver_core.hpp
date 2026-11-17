// Device-independent audio-thread driving logic (design.md 2.2, 2.4, 2.6, CONTRACTS.md D-66).
//
// DriverCore owns the output ring between the device callback (reader) and
// the audio thread (writer) and decides how many blocks to render. It has no
// threads: AudioDriver (audio_driver.hpp) runs it on a real device, tests
// call the three entry points directly and get deterministic results.
//
//   device callback:  OnDeviceCallback(out, frames, channels)   copies out of the ring
//   audio thread:     Service(prefill)                           renders blocks
//   device listener:  OnDeviceChanged(rate, period)              CHANGED events
//
// Rules (2.2):
//   - Target fill = device period + renderAheadBlocks blocks. The period is
//     the larger of the device's PeriodFrames() (or the last CHANGED event)
//     and the largest `frames` a callback has asked for, so a device whose
//     callbacks vary in length is still covered. Clamped to the ring.
//   - Service renders fixed blocks of `blockFrames` while the fill is below
//     the target. Per wake at most ceil(period / block) + catchupBlocks
//     blocks (the catch-up limit after an underrun); a prefill (before the
//     device starts / after RESUMED) is limited to the target instead.
//   - Device periods need not be multiples of the block: the ring absorbs
//     the difference (any `frames` per callback, any block size).
//   - Underrun: the callback copies what is there, fades the last
//     `fadeFrames` of it to zero, outputs silence for the rest and counts
//     it; the first frames after the underrun fade in. The silent frames are
//     reported to the renderer as RenderInfo::lostFrames of the next block
//     so the shared clock advances by them (music and game time stay
//     aligned, 2.2).
//     The fade-in keeps its position across callbacks (always fadeFrames
//     long). An underrun that starts exactly at a callback boundary cannot
//     fade the previous output (already played): that click is accepted
//     (no extra latency for an abnormal case).
//   - Format change: `channels` in the callback is authoritative. When it
//     differs from the ring's format the callback outputs silence (counted
//     as lost frames) and asks the writer for the new count; the writer
//     acknowledges with the ring position where the new format starts and
//     calls FormatFn before rendering in it; the reader drops everything
//     before that position (those frames are added to the next block's
//     RenderInfo::lostFrames, counted as DriverStats::droppedFrames). The
//     reader asks again when the answer does not match the device any more. A sample-rate change (CHANGED event) is passed to
//     FormatFn the same way without dropping audio. The renderer always gets
//     the device's rate and channel count; resampling to an internal rate
//     and up/down-mixing are the caller's (core's final stage, 2.2, 2.6).
//   - Load (2.4): every block's render time is measured against
//     block duration x loadTargetPercent; the LoadMonitor level is passed in
//     RenderInfo::loadLevel.
//
// Threads: Init/Release with neither side running. OnDeviceCallback from
// one thread at a time (the device thread), Service from one thread at a
// time (the audio thread), OnDeviceChanged and the statistics from any
// thread. Everything except Init/Release is RT-safe (no allocation, no
// lock, no IO); the render function runs inside Service.
#ifndef SND_PLATFORM_DRIVER_CORE_HPP
#define SND_PLATFORM_DRIVER_CORE_HPP

#include <atomic>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/platform/load_monitor.hpp"
#include "snd/platform/output_ring.hpp"
#include "snd/types.h"

namespace snd::platform {

struct RenderInfo {
  std::uint32_t sampleRate = 0;   // device rate (frames per second of `interleaved`)
  std::uint32_t loadLevel = 0;    // LoadMonitor level (2.4)
  std::uint64_t blockIndex = 0;   // blocks rendered since Init
  // Frames of silence the device played since the previous block (underrun,
  // format switch). The renderer advances its clock by them (2.2).
  std::uint64_t lostFrames = 0;
};

// Audio thread: write `frames` interleaved frames of `channels` channels.
using RenderFn = void (*)(void* user, float* interleaved, std::uint32_t frames, std::uint32_t channels,
                          const RenderInfo* info);
// Audio thread, before the first block in a new format (and once at the start).
using FormatFn = void (*)(void* user, std::uint32_t sampleRate, std::uint32_t channels);

struct DriverCoreDesc {
  std::uint32_t blockFrames = 256;        // Config.blockSize
  std::uint32_t renderAheadBlocks = 2;    // Config.renderAheadBlocks
  std::uint32_t catchupBlocks = 2;        // Config.tuning.underrunCatchupBlocks
  std::uint32_t maxChannels = SND_MAX_CHANNELS;  // largest device channel count to accept
  std::uint32_t maxPeriodFrames = 4096;   // ring sizing: largest device period expected
  std::uint32_t fadeFrames = 64;          // underrun fade-out / fade-in length
  double loadTargetPercent = 60.0;        // Config.tuning.loadTargetPercent
  LoadMonitorDesc lod{};                  // Config.tuning.lodDownBlocks / lodUpBlocks
  RenderFn render = nullptr;
  FormatFn formatChanged = nullptr;       // optional
  void* user = nullptr;
};

struct DriverStats {
  std::uint64_t callbacks = 0;
  std::uint64_t framesConsumed = 0;     // frames handed to the device (incl. silence)
  std::uint64_t underruns = 0;          // callbacks that found the ring short
  std::uint64_t lostFrames = 0;         // silence inserted (underrun + format switch), total
  std::uint64_t droppedFrames = 0;      // old-layout frames dropped at channel switches (also in RenderInfo::lostFrames)
  std::uint64_t blocksRendered = 0;
  std::uint64_t formatChanges = 0;      // format switches applied by the writer
  std::uint64_t intervalMinNs = 0;      // callback-to-callback interval
  std::uint64_t intervalMaxNs = 0;
  std::uint64_t intervalAvgNs = 0;      // exponential average (1/16)
  std::uint64_t renderLastNs = 0;       // one block
  std::uint64_t renderMaxNs = 0;
  std::uint64_t renderAvgNs = 0;        // exponential average (1/16)
  std::uint64_t budgetNs = 0;           // per block (2.4)
  std::uint64_t overBudgetBlocks = 0;
  std::uint32_t loadLevel = 0;
  std::uint32_t targetFrames = 0;
  std::uint32_t queuedFrames = 0;       // approximate ring fill
  std::uint32_t sampleRate = 0;
  std::uint32_t channels = 0;
};

class DriverCore {
 public:
  DriverCore() noexcept = default;
  ~DriverCore() { Release(); }
  DriverCore(const DriverCore&) = delete;
  DriverCore& operator=(const DriverCore&) = delete;

  // Non-RT. Allocates the ring and the block buffer (MemTag::Misc). Fails on
  // invalid values (no render function, block 0, channels outside
  // 1..maxChannels, rate 0) or allocation failure.
  bool Init(IAllocator& allocator, const DriverCoreDesc& desc, std::uint32_t sampleRate, std::uint32_t channels,
            std::uint32_t periodFrames) noexcept;
  void Release() noexcept;
  bool Initialized() const noexcept { return scratch_ != nullptr; }

  // Device thread (inside the DeviceCallback).
  void OnDeviceCallback(float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept;
  // Any thread (event listener). 0 = unchanged.
  void OnDeviceChanged(std::uint32_t sampleRate, std::uint32_t periodFrames) noexcept;
  // Audio thread. Returns the number of blocks rendered.
  std::uint32_t Service(bool prefill) noexcept;
  // Any thread. The running (and every later) Service returns before its next
  // block; the block being rendered finishes. Cleared by Init.
  void CancelService() noexcept { cancel_.store(true, std::memory_order_release); }

  // Any thread.
  DriverStats Stats() const noexcept;
  std::uint32_t TargetFrames() const noexcept;
  std::uint32_t LoadLevel() const noexcept { return lod_.Level(); }
  std::uint32_t BlockFrames() const noexcept { return cfgBlock_.load(std::memory_order_relaxed); }
  std::uint64_t CapacityFrames(std::uint32_t channels) const noexcept;

 private:
  std::uint32_t RequestFormat(std::uint32_t channels, std::uint32_t rate) noexcept;  // returns the new gen
  void ApplyFormatRequest() noexcept;
  std::uint32_t EffectivePeriod() const noexcept;
  std::uint32_t TargetFor(std::uint32_t channels) const noexcept;
  std::uint64_t QueuedSamples() const noexcept;

  IAllocator* allocator_ = nullptr;
  DriverCoreDesc desc_{};
  float* ringStorage_ = nullptr;
  float* scratch_ = nullptr;
  OutputRing ring_;

  // Format handshake. request_: gen(16) | channels(8) | rate(32), absolute
  // desired values, 0 = keep. ack_: gen(16) | channels(8) | changed(1) | pos(39), pos = write index of the last channel change.
  std::atomic<std::uint64_t> request_{0};
  std::atomic<std::uint64_t> ack_{0};
  std::atomic<std::uint32_t> devicePeriod_{0};
  std::atomic<std::uint32_t> maxFramesSeen_{0};
  std::atomic<bool> cancel_{false};
  std::atomic<std::uint64_t> floor_{0};  // write index of the last channel change
  // Configuration read by Stats() from any thread (atomic: Init may run concurrently).
  std::atomic<std::uint64_t> capSamples_{0};
  std::atomic<std::uint32_t> cfgBlock_{0};
  std::atomic<std::uint32_t> cfgAhead_{0};
  std::atomic<double> cfgLoadPercent_{0.0};

  // Writer (audio thread) state.
  std::uint32_t writerGen_ = 0;
  std::uint32_t channels_ = 0;
  std::uint32_t rate_ = 0;
  std::uint64_t budgetNs_ = 0;
  std::uint64_t blockIndex_ = 0;
  bool announced_ = false;
  LoadMonitor lod_;

  // Reader (device thread) state.
  std::uint32_t readerAckGen_ = 0;
  std::uint32_t readerChannels_ = 0;
  std::uint32_t requestedChannels_ = 0;
  std::uint32_t lastRequestGen_ = 0;  // gen of the reader's last channel request
  std::uint64_t readerFloor_ = 0;     // channel-change position the reader last applied
  bool fadeInPending_ = false;
  std::uint32_t fadePos_ = 0;         // frames of the fade-in already applied
  std::uint64_t lastCallbackNs_ = 0;

  // Reader -> writer.
  std::atomic<std::uint64_t> pendingLost_{0};

  // Statistics (relaxed; one writer each).
  std::atomic<std::uint64_t> callbacks_{0};
  std::atomic<std::uint64_t> consumed_{0};
  std::atomic<std::uint64_t> underruns_{0};
  std::atomic<std::uint64_t> lostTotal_{0};
  std::atomic<std::uint64_t> blocks_{0};
  std::atomic<std::uint64_t> formatChanges_{0};
  std::atomic<std::uint64_t> droppedTotal_{0};
  std::atomic<std::uint64_t> intervalMin_{0};
  std::atomic<std::uint64_t> intervalMax_{0};
  std::atomic<std::uint64_t> intervalAvg_{0};
  std::atomic<std::uint64_t> renderLast_{0};
  std::atomic<std::uint64_t> renderMax_{0};
  std::atomic<std::uint64_t> renderAvg_{0};
  std::atomic<std::uint32_t> statRate_{0};
  std::atomic<std::uint32_t> statChannels_{0};
};

}  // namespace snd::platform

#endif  // SND_PLATFORM_DRIVER_CORE_HPP
