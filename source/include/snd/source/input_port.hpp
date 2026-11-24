// Input ports of the Input source kind (design.md 5 "Input"; CONTRACTS.md D-194).
//
// The platform has no input device interface yet (platform is outside the
// source module), so an input "device" is a port the game (or a test) feeds
// with PCM, the same idea as platform::ManualOutputDevice for the output: the
// game captures from its microphone / another system and writes the frames
// here; an Input voice (snd_play_input, snd_input_desc::device_index = port
// index) plays them.
//
// One port = one lock-free single-producer / single-consumer ring of
// `Capacity()` frames (a power of two) x `Channels()` non-interleaved float
// channels, allocated by Runtime::Init (no allocation afterwards). D-200: the capacity
// is at least 4 blocks (4 x Config.blockSize, rounded up to a power of two): a port
// smaller than that moves, empty, to memory in the Input kind's pool when core creates
// the kind (and back when core destroys it), so do not write a port during core's
// Create / Destroy. Capacity() is valid again after them.
//   Producer  Write(): any thread, ONE writer per port at a time. It copies
//             what fits and drops the rest (the newest frames: overrun, counted
//             in InputPortStats::droppedFrames). Samples that are not finite
//             (NaN, +-Inf) are written as 0 (D-197; checked here, on the writer's
//             thread, never on the render side). Channels beyond the port's are
//             ignored; port channels beyond the given ones repeat the last one.
//   Consumer  the Input kind (one playing voice per port at a time). A voice
//             starts "live" (D-197): of what was written before its start it
//             keeps only the newest block (blockFrames, or all if less). When
//             more than MaxLatencyFrames() are buffered (the writer ran ahead, a
//             burst), the consumer drops the oldest frames down to half of it
//             (InputPortStats::skippedFrames). Missing frames are silence. A voice
//             with more channels than the port repeats the port's last channel;
//             one with fewer reads the port's first ones (the rest is ignored).
// Rate: the port's sample rate (Configure, 0 = the internal rate). The Input
// kind is a source-rate kind (D-22): core resamples. Core takes a rate other
// than the internal one from SourceStartResult::reserved (D-194; until core
// reads it, requests/D-03, write at the internal rate).
#ifndef SND_SOURCE_INPUT_PORT_HPP
#define SND_SOURCE_INPUT_PORT_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace snd::source {

struct InputPortStats {
  std::uint64_t writtenFrames = 0;  // accepted by Write
  std::uint64_t droppedFrames = 0;  // refused by Write (ring full: overrun on the writer side)
  std::uint64_t readFrames = 0;     // delivered to voices (heard or advanced virtually)
  std::uint64_t skippedFrames = 0;  // dropped by the consumer (start, resume, latency bound)
  // D-200 (D-197 (3)): shortages that began, i.e. blocks that came short right after a
  // block that got every requested frame. The following short blocks of the same shortage
  // are not counted again (nor are a voice's first block or the block after a skip).
  std::uint64_t underruns = 0;
};

class InputPort {
 public:
  InputPort() noexcept = default;
  InputPort(const InputPort&) = delete;
  InputPort& operator=(const InputPort&) = delete;

  // Any thread, one writer at a time. `channels[c]` holds `frames` floats. Returns the
  // frames accepted (0 for no channels). Lock-free, allocation-free.
  std::uint32_t Write(const float* const* channels, std::uint32_t channelCount, std::uint32_t frames) noexcept;

  // Any thread. sampleRate: the rate the writer delivers (0 = internal rate); taken by
  // a voice at its start. maxLatencyFrames: buffered frames a playing voice tolerates
  // before it drops the oldest (0 = Capacity() / 2; clamped to [2 blocks, Capacity()],
  // D-197).
  void Configure(std::uint32_t sampleRate, std::uint32_t maxLatencyFrames) noexcept;

  std::uint32_t Channels() const noexcept { return channels_; }
  std::uint32_t Capacity() const noexcept { return frames_; }
  std::uint32_t SampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
  std::uint32_t MaxLatencyFrames() const noexcept;
  // Frames written and not consumed yet (approximate from another thread).
  std::uint32_t Buffered() const noexcept;
  InputPortStats Stats() const noexcept;

  // --- the Input kind (consumer side; audio thread / RT worker of the reading voice) ---
  void Bind(float* data, std::uint32_t channels, std::uint32_t frames) noexcept;
  // Owner thread, while nothing writes or reads the port (the Input kind's Init / Shutdown,
  // D-200): moves the port to other memory of `frames` (power of two) frames per channel,
  // emptied (positions 0). Configure's values and the statistics are kept.
  void Rebind(float* data, std::uint32_t frames) noexcept;
  float* Data() const noexcept { return data_; }
  // Owner thread (the Input kind's Init): the block size, for the latency floor (D-197).
  void SetBlockFrames(std::uint32_t frames) noexcept { blockFrames_.store(frames, std::memory_order_relaxed); }
  // A voice starts / resumes live: drops what was written so far except the newest
  // `keep` frames (D-197: one block).
  void SkipToLive(std::uint32_t keep) noexcept;
  // Reads up to `frames` frames into out[0..outChannels) (out may be nullptr: consumed
  // without output). Returns the frames read.
  std::uint32_t Read(float* const* out, std::uint32_t outChannels, std::uint32_t frames) noexcept;
  void CountUnderrun() noexcept { underruns_.fetch_add(1u, std::memory_order_relaxed); }

 private:
  float* Channel(std::uint32_t c) const noexcept { return data_ + std::size_t{c} * frames_; }

  float* data_ = nullptr;
  std::uint32_t channels_ = 0;
  std::uint32_t frames_ = 0;  // power of two
  std::atomic<std::uint32_t> sampleRate_{0};
  std::atomic<std::uint32_t> maxLatency_{0};
  std::atomic<std::uint32_t> blockFrames_{0};
  alignas(64) std::atomic<std::uint64_t> writePos_{0};  // producer
  alignas(64) std::atomic<std::uint64_t> readPos_{0};   // consumer
  std::atomic<std::uint64_t> written_{0}, dropped_{0};
  alignas(64) std::atomic<std::uint64_t> read_{0}, skipped_{0}, underruns_{0};
};

}  // namespace snd::source

#endif  // SND_SOURCE_INPUT_PORT_HPP
