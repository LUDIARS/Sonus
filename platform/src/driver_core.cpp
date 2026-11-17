#include "snd/platform/driver_core.hpp"

#include <cassert>
#include <cstring>
#include <initializer_list>

#include "snd/base/atomic.hpp"
#include "snd/platform/clock.hpp"

namespace snd::platform {

namespace {
constexpr std::uint64_t kPosBits = 39;
constexpr std::uint64_t kPosMask = (1ull << kPosBits) - 1u;

constexpr std::uint64_t PackRequest(std::uint32_t gen, std::uint32_t channels, std::uint32_t rate) noexcept {
  return (static_cast<std::uint64_t>(gen & 0xFFFFu) << 48) | (static_cast<std::uint64_t>(channels & 0xFFu) << 32) |
         static_cast<std::uint64_t>(rate);
}
constexpr std::uint32_t ReqGen(std::uint64_t v) noexcept { return static_cast<std::uint32_t>(v >> 48); }
constexpr std::uint32_t ReqChannels(std::uint64_t v) noexcept { return static_cast<std::uint32_t>((v >> 32) & 0xFFu); }
constexpr std::uint32_t ReqRate(std::uint64_t v) noexcept { return static_cast<std::uint32_t>(v & 0xFFFFFFFFu); }

constexpr std::uint64_t PackAck(std::uint32_t gen, std::uint32_t channels, bool flush, std::uint64_t pos) noexcept {
  return (static_cast<std::uint64_t>(gen & 0xFFFFu) << 48) | (static_cast<std::uint64_t>(channels & 0xFFu) << 40) |
         (static_cast<std::uint64_t>(flush ? 1u : 0u) << 39) | (pos & kPosMask);
}
constexpr std::uint32_t AckGen(std::uint64_t v) noexcept { return static_cast<std::uint32_t>(v >> 48); }
constexpr std::uint32_t AckChannels(std::uint64_t v) noexcept { return static_cast<std::uint32_t>((v >> 40) & 0xFFu); }
constexpr std::uint64_t AckPos(std::uint64_t v) noexcept { return v & kPosMask; }

// Exponential average with weight 1/16 (single writer).
void UpdateAverage(std::atomic<std::uint64_t>& avg, std::uint64_t x) noexcept {
  const std::uint64_t a = avg.load(std::memory_order_relaxed);
  const std::uint64_t next = a == 0u ? x : (a > x ? a - (a - x) / 16u : a + (x - a) / 16u);
  avg.store(next, std::memory_order_relaxed);
}
void Bump(std::atomic<std::uint64_t>& c, std::uint64_t by = 1u) noexcept {
  c.store(c.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}
}  // namespace

bool DriverCore::Init(IAllocator& allocator, const DriverCoreDesc& desc, std::uint32_t sampleRate,
                      std::uint32_t channels, std::uint32_t periodFrames) noexcept {
  Release();
  if (desc.render == nullptr || desc.blockFrames == 0u || desc.maxChannels == 0u ||
      desc.maxChannels > 255u || channels == 0u || channels > desc.maxChannels || sampleRate == 0u) {
    return false;
  }
  desc_ = desc;
  const std::uint64_t period = periodFrames != 0u ? periodFrames : desc.blockFrames;
  const std::uint64_t maxPeriod = desc.maxPeriodFrames > period ? desc.maxPeriodFrames : period;
  const std::uint64_t frames =
      2u * (maxPeriod + (static_cast<std::uint64_t>(desc.renderAheadBlocks) + desc.catchupBlocks + 1u) * desc.blockFrames);
  const std::uint64_t samples = base::NextPowerOfTwo(frames * desc.maxChannels);
  if (samples == 0u || samples > (1ull << 32)) return false;
  std::size_t scratchSamples = 0, scratchBytes = 0;
  if (!base::CheckedMul(desc.blockFrames, desc.maxChannels, &scratchSamples) ||
      !base::CheckedMul(scratchSamples, sizeof(float), &scratchBytes)) {
    return false;
  }
  ringStorage_ = static_cast<float*>(allocator.Allocate(static_cast<std::size_t>(samples) * sizeof(float), 64, MemTag::Misc));
  scratch_ = static_cast<float*>(allocator.Allocate(scratchBytes, 64, MemTag::Misc));
  if (ringStorage_ == nullptr || scratch_ == nullptr) {
    allocator_ = &allocator;
    Release();
    return false;
  }
  std::memset(ringStorage_, 0, static_cast<std::size_t>(samples) * sizeof(float));
  std::memset(scratch_, 0, scratchBytes);
  allocator_ = &allocator;
  ring_.Bind(ringStorage_, samples);
  capSamples_.store(samples, std::memory_order_relaxed);
  cfgBlock_.store(desc.blockFrames, std::memory_order_relaxed);
  cfgAhead_.store(desc.renderAheadBlocks, std::memory_order_relaxed);
  cfgLoadPercent_.store(desc.loadTargetPercent, std::memory_order_relaxed);

  request_.store(PackRequest(0, channels, sampleRate), std::memory_order_relaxed);
  ack_.store(PackAck(0, channels, false, 0), std::memory_order_relaxed);
  devicePeriod_.store(static_cast<std::uint32_t>(period), std::memory_order_relaxed);
  maxFramesSeen_.store(0, std::memory_order_relaxed);
  writerGen_ = 0;
  channels_ = channels;
  rate_ = sampleRate;
  budgetNs_ = BlockBudgetNs(desc.blockFrames, sampleRate, desc.loadTargetPercent);
  blockIndex_ = 0;
  announced_ = false;
  lod_.Configure(desc.lod);
  readerAckGen_ = 0;
  readerChannels_ = channels;
  requestedChannels_ = channels;
  fadeInPending_ = false;
  fadePos_ = 0;
  lastRequestGen_ = 0;
  readerFloor_ = 0;
  lastCallbackNs_ = 0;
  pendingLost_.store(0, std::memory_order_relaxed);
  floor_.store(0, std::memory_order_relaxed);
  cancel_.store(false, std::memory_order_relaxed);
  for (auto* c : {&callbacks_, &consumed_, &underruns_, &lostTotal_, &blocks_, &formatChanges_, &intervalMin_,
                  &intervalMax_, &intervalAvg_, &renderLast_, &renderMax_, &renderAvg_, &droppedTotal_}) {
    c->store(0, std::memory_order_relaxed);
  }
  statRate_.store(sampleRate, std::memory_order_relaxed);
  statChannels_.store(channels, std::memory_order_relaxed);
  return true;
}

void DriverCore::Release() noexcept {
  if (allocator_ != nullptr) {
    if (ringStorage_ != nullptr) allocator_->Free(ringStorage_, MemTag::Misc);
    if (scratch_ != nullptr) allocator_->Free(scratch_, MemTag::Misc);
  }
  ringStorage_ = nullptr;
  scratch_ = nullptr;
  allocator_ = nullptr;
}

std::uint64_t DriverCore::QueuedSamples() const noexcept {
  // Samples below the last channel-change position are dropped by the
  // reader; they do not count towards the fill (nor occupy space for long).
  const std::uint64_t w = ring_.WriteIndex();
  const std::uint64_t r = ring_.ReadIndexAcquire();
  const std::uint64_t f = floor_.load(std::memory_order_relaxed);
  const std::uint64_t from = r > f ? r : f;
  return w > from ? w - from : 0u;
}

std::uint64_t DriverCore::CapacityFrames(std::uint32_t channels) const noexcept {
  return channels == 0u ? 0u : capSamples_.load(std::memory_order_relaxed) / channels;
}

std::uint32_t DriverCore::EffectivePeriod() const noexcept {
  const std::uint32_t p = devicePeriod_.load(std::memory_order_relaxed);
  const std::uint32_t seen = maxFramesSeen_.load(std::memory_order_relaxed);
  return p > seen ? p : seen;
}

std::uint32_t DriverCore::TargetFor(std::uint32_t channels) const noexcept {
  const std::uint64_t block = cfgBlock_.load(std::memory_order_relaxed);
  const std::uint64_t want = static_cast<std::uint64_t>(EffectivePeriod()) +
                             static_cast<std::uint64_t>(cfgAhead_.load(std::memory_order_relaxed)) * block;
  const std::uint64_t cap = CapacityFrames(channels);
  const std::uint64_t limit = cap > block ? cap - block : 0u;
  return static_cast<std::uint32_t>(want < limit ? want : limit);
}

std::uint32_t DriverCore::TargetFrames() const noexcept {
  return TargetFor(statChannels_.load(std::memory_order_relaxed));
}

std::uint32_t DriverCore::RequestFormat(std::uint32_t channels, std::uint32_t rate) noexcept {
  std::uint64_t cur = request_.load(std::memory_order_relaxed);
  for (;;) {
    const std::uint32_t ch = channels != 0u ? channels : ReqChannels(cur);
    const std::uint32_t r = rate != 0u ? rate : ReqRate(cur);
    const std::uint64_t next = PackRequest(ReqGen(cur) + 1u, ch, r);
    if (request_.compare_exchange_weak(cur, next, std::memory_order_release, std::memory_order_relaxed)) {
      return ReqGen(next);
    }
  }
}

void DriverCore::OnDeviceChanged(std::uint32_t sampleRate, std::uint32_t periodFrames) noexcept {
  if (periodFrames != 0u) devicePeriod_.store(periodFrames, std::memory_order_relaxed);
  if (sampleRate != 0u) RequestFormat(0u, sampleRate);
}

void DriverCore::ApplyFormatRequest() noexcept {
  const std::uint64_t req = request_.load(std::memory_order_acquire);
  const std::uint32_t gen = ReqGen(req);
  if (gen == writerGen_ && announced_) return;
  writerGen_ = gen;
  const std::uint32_t ch = ReqChannels(req);
  const std::uint32_t rate = ReqRate(req);
  const bool channelChange = ch != 0u && ch != channels_ && ch <= desc_.maxChannels;
  const bool rateChange = rate != 0u && rate != rate_;
  if (channelChange) channels_ = ch;
  if (rateChange) {
    rate_ = rate;
    budgetNs_ = BlockBudgetNs(desc_.blockFrames, rate_, desc_.loadTargetPercent);
  }
  if (channelChange || rateChange) Bump(formatChanges_);
  // The new format starts at the current write index; with a channel change
  // the reader drops everything before it (written in the old format).
  // The ack always carries the position of the LAST channel change, so a
  // reader that missed it (a later rate-only ack overwrote it) still drops
  // the data written before it in the old layout.
  if (channelChange) floor_.store(ring_.WriteIndex(), std::memory_order_relaxed);
  ack_.store(PackAck(gen, channels_, channelChange, floor_.load(std::memory_order_relaxed)), std::memory_order_release);
  statRate_.store(rate_, std::memory_order_relaxed);
  statChannels_.store(channels_, std::memory_order_relaxed);
  if (desc_.formatChanged != nullptr && (!announced_ || channelChange || rateChange)) {
    desc_.formatChanged(desc_.user, rate_, channels_);
  }
  announced_ = true;
}

std::uint32_t DriverCore::Service(bool prefill) noexcept {
  if (scratch_ == nullptr) return 0;
  ApplyFormatRequest();
  const std::uint32_t ch = channels_;
  const std::uint32_t block = desc_.blockFrames;
  const std::uint32_t target = TargetFor(ch);
  const std::uint32_t perPeriod = (EffectivePeriod() + block - 1u) / block;
  const std::uint32_t limit = prefill ? target / block + 1u : perPeriod + desc_.catchupBlocks;
  const std::uint64_t blockSamples = static_cast<std::uint64_t>(block) * ch;
  std::uint32_t n = 0;
  while (n < limit && !cancel_.load(std::memory_order_acquire)) {
    const std::uint64_t queued = QueuedSamples();
    if (queued / ch >= target) break;
    if (ring_.Capacity() - ring_.QueuedForWriter() < blockSamples) break;  // physical space
    RenderInfo info;
    info.sampleRate = rate_;
    info.loadLevel = lod_.Level();
    info.blockIndex = blockIndex_;
    info.lostFrames = pendingLost_.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t t0 = MonotonicNanoseconds();
    desc_.render(desc_.user, scratch_, block, ch, &info);
    const std::uint64_t dt = MonotonicNanoseconds() - t0;
    lod_.Record(dt, budgetNs_);
    renderLast_.store(dt, std::memory_order_relaxed);
    if (dt > renderMax_.load(std::memory_order_relaxed)) renderMax_.store(dt, std::memory_order_relaxed);
    UpdateAverage(renderAvg_, dt);
    ring_.Write(scratch_, blockSamples);
    ++blockIndex_;
    Bump(blocks_);
    ++n;
  }
  return n;
}

void DriverCore::OnDeviceCallback(float* out, std::uint32_t frames, std::uint32_t channels) noexcept {
  if (frames == 0u || out == nullptr) return;
  const std::uint64_t now = MonotonicNanoseconds();
  if (lastCallbackNs_ != 0u && now >= lastCallbackNs_) {
    const std::uint64_t dt = now - lastCallbackNs_;
    const std::uint64_t mn = intervalMin_.load(std::memory_order_relaxed);
    if (mn == 0u || dt < mn) intervalMin_.store(dt, std::memory_order_relaxed);
    if (dt > intervalMax_.load(std::memory_order_relaxed)) intervalMax_.store(dt, std::memory_order_relaxed);
    UpdateAverage(intervalAvg_, dt);
  }
  lastCallbackNs_ = now;
  Bump(callbacks_);
  Bump(consumed_, frames);
  if (frames > maxFramesSeen_.load(std::memory_order_relaxed)) maxFramesSeen_.store(frames, std::memory_order_relaxed);

  // Write index first, then the ack: if the write index includes data in a
  // new format, the ack published before it is visible too. After applying a
  // new ack the write index is read again (it is then >= the ack position)
  // and the ack re-checked, until both agree (the writer may have switched
  // again in between).
  std::uint64_t w = ring_.AcquireWriteIndex();
  std::uint64_t a = ack_.load(std::memory_order_acquire);
  bool settled = false;
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (AckGen(a) == readerAckGen_) {
      settled = true;
      break;
    }
    const std::uint32_t oldChannels = readerChannels_;
    readerAckGen_ = AckGen(a);
    readerChannels_ = AckChannels(a);
    if (AckPos(a) != (readerFloor_ & kPosMask)) {
      // A channel change happened at AckPos (>= the read index: the reader
      // never reads data published after an ack it has not seen).
      const std::uint64_t r = ring_.ReadIndex();
      const std::uint64_t pos = r + ((AckPos(a) - r) & kPosMask);
      readerFloor_ = pos;
      // Frames in the old layout that are dropped: reported to the renderer
      // with the next block like silence (D-66), counted separately.
      const std::uint64_t dropped = oldChannels != 0u ? (pos - r) / oldChannels : 0u;
      if (dropped > 0u) {
        pendingLost_.fetch_add(dropped, std::memory_order_acq_rel);
        Bump(droppedTotal_, dropped);
      }
      ring_.SkipTo(pos);
      fadeInPending_ = true;
      fadePos_ = 0;
    }
    w = ring_.AcquireWriteIndex();
    a = ack_.load(std::memory_order_acquire);
  }

  const std::size_t samplesOut = static_cast<std::size_t>(frames) * (channels == 0u ? 1u : channels);
  if (!settled || channels != readerChannels_ || channels == 0u) {
    // Ask (again) for the device layout when it is not the one requested
    // last, or when the writer has already answered the last request with a
    // different layout (the device flipped back and forth before it ran).
    const bool answered = static_cast<std::int16_t>(static_cast<std::uint16_t>(readerAckGen_ - lastRequestGen_)) >= 0;
    if (settled && channels != 0u && channels <= desc_.maxChannels && (channels != requestedChannels_ || answered)) {
      requestedChannels_ = channels;
      lastRequestGen_ = RequestFormat(channels, 0u);
    }
    std::memset(out, 0, samplesOut * sizeof(float));
    pendingLost_.fetch_add(frames, std::memory_order_acq_rel);
    Bump(lostTotal_, frames);
    return;
  }
  requestedChannels_ = channels;

  const std::uint64_t r = ring_.ReadIndex();
#ifndef NDEBUG
  assert(r <= w && "DriverCore: the read index ran past the write index");
#endif
  // Clamp: never read beyond the published data or more than the ring holds.
  const std::uint64_t availSamples = w > r ? w - r : 0u;
  const std::uint64_t cap = ring_.Capacity();
  const std::uint64_t availFrames = (availSamples < cap ? availSamples : cap) / channels;
  const std::uint32_t n = availFrames < frames ? static_cast<std::uint32_t>(availFrames) : frames;
  if (n > 0u) ring_.Read(out, static_cast<std::uint64_t>(n) * channels);
  const std::uint32_t fade = desc_.fadeFrames;
  if (fadeInPending_ && n > 0u) {
    // The ramp runs over `fade` frames whatever the callback lengths are:
    // its position is kept across callbacks.
    std::uint32_t i = 0;
    for (; i < n && fadePos_ < fade; ++i, ++fadePos_) {
      const float g = static_cast<float>(fadePos_ + 1u) / static_cast<float>(fade + 1u);
      for (std::uint32_t c = 0; c < channels; ++c) out[static_cast<std::size_t>(i) * channels + c] *= g;
    }
    if (fadePos_ >= fade) fadeInPending_ = false;
  }
  if (n < frames) {
    if (n > 0u && fade > 0u) {
      const std::uint32_t len = fade < n ? fade : n;
      const std::uint32_t start = n - len;
      for (std::uint32_t i = 0; i < len; ++i) {
        const float g = static_cast<float>(len - 1u - i) / static_cast<float>(len);
        for (std::uint32_t c = 0; c < channels; ++c) out[static_cast<std::size_t>(start + i) * channels + c] *= g;
      }
    }
    std::memset(out + static_cast<std::size_t>(n) * channels, 0,
                static_cast<std::size_t>(frames - n) * channels * sizeof(float));
    Bump(underruns_);
    pendingLost_.fetch_add(frames - n, std::memory_order_acq_rel);
    Bump(lostTotal_, frames - n);
    fadeInPending_ = fade > 0u;
    fadePos_ = 0;
  }
}

DriverStats DriverCore::Stats() const noexcept {
  DriverStats s;
  s.callbacks = callbacks_.load(std::memory_order_relaxed);
  s.framesConsumed = consumed_.load(std::memory_order_relaxed);
  s.underruns = underruns_.load(std::memory_order_relaxed);
  s.lostFrames = lostTotal_.load(std::memory_order_relaxed);
  s.blocksRendered = blocks_.load(std::memory_order_relaxed);
  s.formatChanges = formatChanges_.load(std::memory_order_relaxed);
  s.intervalMinNs = intervalMin_.load(std::memory_order_relaxed);
  s.intervalMaxNs = intervalMax_.load(std::memory_order_relaxed);
  s.intervalAvgNs = intervalAvg_.load(std::memory_order_relaxed);
  s.renderLastNs = renderLast_.load(std::memory_order_relaxed);
  s.renderMaxNs = renderMax_.load(std::memory_order_relaxed);
  s.renderAvgNs = renderAvg_.load(std::memory_order_relaxed);
  s.sampleRate = statRate_.load(std::memory_order_relaxed);
  s.channels = statChannels_.load(std::memory_order_relaxed);
  s.budgetNs = BlockBudgetNs(cfgBlock_.load(std::memory_order_relaxed), s.sampleRate,
                             cfgLoadPercent_.load(std::memory_order_relaxed));
  s.droppedFrames = droppedTotal_.load(std::memory_order_relaxed);
  s.overBudgetBlocks = lod_.OverBudgetBlocks();
  s.loadLevel = lod_.Level();
  s.targetFrames = TargetFor(s.channels);
  const std::uint64_t q = s.channels != 0u ? QueuedSamples() / s.channels : 0u;
  s.queuedFrames = static_cast<std::uint32_t>(q);
  return s;
}

}  // namespace snd::platform
