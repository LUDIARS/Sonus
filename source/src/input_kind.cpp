// Input kind and input ports (input_kind.hpp, snd/source/input_port.hpp).
#include "input_kind.hpp"

#include <cstring>
#include <new>

namespace snd::source {

// ---------------------------------------------------------------------------
// InputPort
// ---------------------------------------------------------------------------
void InputPort::Bind(float* data, std::uint32_t channels, std::uint32_t frames) noexcept {
  data_ = data;
  channels_ = channels;
  frames_ = frames;
  sampleRate_.store(0u, std::memory_order_relaxed);
  maxLatency_.store(0u, std::memory_order_relaxed);
  writePos_.store(0u, std::memory_order_relaxed);
  readPos_.store(0u, std::memory_order_relaxed);
}

void InputPort::Configure(std::uint32_t sampleRate, std::uint32_t maxLatencyFrames) noexcept {
  sampleRate_.store(sampleRate, std::memory_order_relaxed);
  maxLatency_.store(maxLatencyFrames, std::memory_order_relaxed);
}

std::uint32_t InputPort::MaxLatencyFrames() const noexcept {
  std::uint32_t m = maxLatency_.load(std::memory_order_relaxed);
  if (m == 0u) m = frames_ / 2u;
  const std::uint32_t floor = 2u * blockFrames_.load(std::memory_order_relaxed);  // D-197
  if (m < floor) m = floor;
  if (m > frames_) m = frames_;
  return m == 0u ? 1u : m;
}

std::uint32_t InputPort::Buffered() const noexcept {
  const std::uint64_t w = writePos_.load(std::memory_order_acquire);
  const std::uint64_t r = readPos_.load(std::memory_order_acquire);
  return w > r ? static_cast<std::uint32_t>(w - r) : 0u;
}

InputPortStats InputPort::Stats() const noexcept {
  InputPortStats s;
  s.writtenFrames = written_.load(std::memory_order_relaxed);
  s.droppedFrames = dropped_.load(std::memory_order_relaxed);
  s.readFrames = read_.load(std::memory_order_relaxed);
  s.skippedFrames = skipped_.load(std::memory_order_relaxed);
  s.underruns = underruns_.load(std::memory_order_relaxed);
  return s;
}

std::uint32_t InputPort::Write(const float* const* channels, std::uint32_t channelCount, std::uint32_t frames) noexcept {
  if (data_ == nullptr || channels == nullptr || channelCount == 0u || frames == 0u) return 0u;
  // SPSC: the consumer only moves readPos forward, so the free space seen here can only grow.
  const std::uint64_t r = readPos_.load(std::memory_order_acquire);
  const std::uint64_t w = writePos_.load(std::memory_order_relaxed);
  const std::uint64_t used = w - r;
  const std::uint64_t space = used >= frames_ ? 0u : frames_ - used;
  const std::uint32_t n = static_cast<std::uint32_t>(space < frames ? space : frames);
  for (std::uint32_t f = 0; f < n;) {
    const std::uint32_t at = static_cast<std::uint32_t>((w + f) & (frames_ - 1u));
    std::uint32_t len = n - f;
    if (len > frames_ - at) len = frames_ - at;
    for (std::uint32_t c = 0; c < channels_; ++c) {
      const float* src = channels[c < channelCount ? c : channelCount - 1u];
      float* dst = Channel(c) + at;
      if (src == nullptr) {
        std::memset(dst, 0, sizeof(float) * len);
      } else {
        for (std::uint32_t k = 0; k < len; ++k) {  // D-197: not finite -> 0 (writer side)
          const float v = src[f + k];
          dst[k] = v - v == 0.0f ? v : 0.0f;
        }
      }
    }
    f += len;
  }
  writePos_.store(w + n, std::memory_order_release);
  written_.fetch_add(n, std::memory_order_relaxed);
  if (n < frames) dropped_.fetch_add(frames - n, std::memory_order_relaxed);
  return n;
}

void InputPort::SkipToLive(std::uint32_t keep) noexcept {
  const std::uint64_t w = writePos_.load(std::memory_order_acquire);
  const std::uint64_t r = readPos_.load(std::memory_order_relaxed);
  if (w <= r + keep) return;  // at most `keep` buffered: nothing to drop
  skipped_.fetch_add(w - keep - r, std::memory_order_relaxed);
  readPos_.store(w - keep, std::memory_order_release);
}

std::uint32_t InputPort::Read(float* const* out, std::uint32_t outChannels, std::uint32_t frames) noexcept {
  if (data_ == nullptr) return 0u;
  const std::uint64_t w = writePos_.load(std::memory_order_acquire);
  std::uint64_t r = readPos_.load(std::memory_order_relaxed);
  std::uint64_t avail = w > r ? w - r : 0u;
  const std::uint32_t lat = MaxLatencyFrames();
  if (avail > lat) {  // the writer ran ahead: drop the oldest down to half the bound
    const std::uint64_t keep = lat / 2u;
    const std::uint64_t skip = avail - keep;
    r += skip;
    avail = keep;
    skipped_.fetch_add(skip, std::memory_order_relaxed);
  }
  const std::uint32_t take = static_cast<std::uint32_t>(avail < frames ? avail : frames);
  if (out != nullptr) {
    for (std::uint32_t f = 0; f < take;) {
      const std::uint32_t at = static_cast<std::uint32_t>((r + f) & (frames_ - 1u));
      std::uint32_t len = take - f;
      if (len > frames_ - at) len = frames_ - at;
      for (std::uint32_t c = 0; c < outChannels; ++c) {
        if (out[c] != nullptr) std::memcpy(out[c] + f, Channel(c < channels_ ? c : channels_ - 1u) + at, sizeof(float) * len);
      }
      f += len;
    }
  }
  readPos_.store(r + take, std::memory_order_release);
  read_.fetch_add(take, std::memory_order_relaxed);
  return take;
}

namespace detail {

namespace {
constexpr std::size_t Align64(std::size_t n) noexcept { return (n + 63u) & ~std::size_t{63u}; }
std::uint32_t Cap(const SourceLimits& l) noexcept { return l.maxVoices == 0u ? 1u : l.maxVoices; }
}  // namespace

// ---------------------------------------------------------------------------
// InputKind
// ---------------------------------------------------------------------------
std::size_t InputKind::PoolBytes(const SourceLimits& limits) const noexcept {
  const std::uint32_t n = Cap(limits);
  return Align64(sizeof(InputInst) * n) + Align64(sizeof(std::uint32_t) * n) + sizeof(std::uint32_t) * (portCount_ + 1u);
}

bool InputKind::Init(void* pool, std::size_t bytes, const SourceLimits& limits, const SourceServices&) noexcept {
  const std::uint32_t n = Cap(limits);
  if (pool == nullptr || bytes < PoolBytes(limits)) return false;
  auto* base = static_cast<std::uint8_t*>(pool);
  cap_ = n;
  sampleRate_ = limits.sampleRate == 0u ? 48000u : limits.sampleRate;
  blockFrames_ = limits.blockFrames == 0u ? 256u : limits.blockFrames;
  for (std::uint32_t p = 0; p < portCount_; ++p) ports_[p].SetBlockFrames(blockFrames_);
  inst_ = reinterpret_cast<InputInst*>(base);
  freeInst_ = reinterpret_cast<std::uint32_t*>(base + Align64(sizeof(InputInst) * n));
  portOwner_ = reinterpret_cast<std::uint32_t*>(base + Align64(sizeof(InputInst) * n) + Align64(sizeof(std::uint32_t) * n));
  for (std::uint32_t i = 0; i < n; ++i) {
    new (&inst_[i]) InputInst();
    freeInst_[i] = n - 1u - i;
  }
  for (std::uint32_t p = 0; p < portCount_; ++p) portOwner_[p] = kInvalidSlot;
  freeCount_ = n;
  live_.store(0, std::memory_order_relaxed);
  return true;
}

void InputKind::Shutdown() noexcept {
  inst_ = nullptr;
  freeInst_ = nullptr;
  portOwner_ = nullptr;
  freeCount_ = 0;
  cap_ = 0;
  live_.store(0, std::memory_order_relaxed);
}

void InputKind::Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SourceStartRequest& rq = requests[k];
    SourceStartResult& res = results[k];
    res = SourceStartResult{kInvalidSlot, SND_END_DETAIL_NONE, 0u, 0u};
    const auto* p = static_cast<const InputParams*>(rq.params);
    if (p == nullptr || inst_ == nullptr || p->desc.device_index >= portCount_) continue;  // core ends it
    const std::uint32_t port = p->desc.device_index;
    if (portOwner_[port] != kInvalidSlot || freeCount_ == 0u) {  // one reader per port (SPSC)
      res.endDetail = SND_END_DETAIL_VOICE_LIMIT;
      continue;
    }
    const std::uint32_t i = freeInst_[--freeCount_];
    InputInst& in = inst_[i];
    in.port = port;
    in.fullLast = 0;  // D-197: the first block is never an underrun
    portOwner_[port] = i;
    ports_[port].SkipToLive(blockFrames_);
    const std::uint32_t rate = ports_[port].SampleRate();
    res.reserved = rate == sampleRate_ ? 0u : rate;  // D-194: the material rate (0 = internal)
    std::atomic_ref<std::uint32_t>(in.state).store(1u, std::memory_order_release);
    live_.fetch_add(1u, std::memory_order_relaxed);
    res.sourceSlot = i;
  }
}

void InputKind::Release(const SlotIndex* sourceSlots, std::uint32_t count) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SlotIndex i = sourceSlots[k];
    if (i >= cap_ || inst_[i].state != 1u) continue;
    InputInst& in = inst_[i];
    std::atomic_ref<std::uint32_t>(in.state).store(0u, std::memory_order_release);
    if (in.port < portCount_ && portOwner_[in.port] == i) portOwner_[in.port] = kInvalidSlot;
    freeInst_[freeCount_++] = i;
    live_.fetch_sub(1u, std::memory_order_relaxed);
  }
}

void InputKind::Control(const SourceControlOp* ops, std::uint32_t count) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SourceControlOp& op = ops[k];
    if (op.sourceSlot >= cap_ || inst_[op.sourceSlot].state != 1u) continue;
    // D-197: Devirtualize keeps reading on (the virtual voice consumed in time).
    if (op.op == ControlOp::Seek || op.op == ControlOp::Resume) {
      ports_[inst_[op.sourceSlot].port].SkipToLive(blockFrames_);
      inst_[op.sourceSlot].fullLast = 0;  // the block after a skip is not an underrun
    }
  }
}

void InputKind::QueryReady(const SlotIndex*, std::uint32_t count, std::uint8_t* ready) const noexcept {
  for (std::uint32_t k = 0; k < count; ++k) ready[k] = 1u;
}

void InputKind::RenderBatch(const source::RenderBatch& batch, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t k = 0; k < batch.count; ++k) {
    const RenderItem& it = batch.items[k];
    VoiceRenderStatus& st = status[k];
    st = VoiceRenderStatus{};
    if (it.sourceSlot >= cap_ || !Live(inst_[it.sourceSlot])) {
      st.ended = 1;
      continue;
    }
    InputInst& in = inst_[it.sourceSlot];
    InputPort& port = ports_[in.port];
    float* outs[kMaxChannels];
    const std::uint32_t ch = it.channels < kMaxChannels ? it.channels : kMaxChannels;
    for (std::uint32_t c = 0; c < ch; ++c) outs[c] = it.out[c] + it.frameOffset;
    const std::uint32_t n = it.framesRequested;
    const std::uint32_t got = port.Read(outs, ch, n);  // the rest stays silent (out is zeroed)
    const bool wasFull = in.fullLast != 0u;
    in.fullLast = got == n ? 1u : 0u;
    if (got < n && wasFull) {  // D-197: an underrun only after a block that got everything
      st.starved = 1;
      port.CountUnderrun();
      underruns_.fetch_add(1u, std::memory_order_relaxed);
    }
    it.playhead->frame += n;
    st.framesRendered = n;
  }
}

void InputKind::AdvanceVirtual(const AdvanceItem* items, std::uint32_t count, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const AdvanceItem& a = items[k];
    status[k] = VoiceRenderStatus{};
    if (a.sourceSlot >= cap_ || inst_[a.sourceSlot].state != 1u) {
      status[k].ended = 1;
      continue;
    }
    const float pitch = a.pitch > 0.0f ? a.pitch : 1.0f;
    const std::uint32_t frames = static_cast<std::uint32_t>(static_cast<float>(a.outputFrames) * pitch + 0.5f);
    const std::uint32_t got = ports_[inst_[a.sourceSlot].port].Read(nullptr, 0u, frames);  // consumed, not heard
    inst_[a.sourceSlot].fullLast = got == frames ? 1u : 0u;
    a.playhead->frame += frames;
  }
}

}  // namespace detail
}  // namespace snd::source
