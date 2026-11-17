// Framing over a byte stream: reader state machine, frame queues, sender.
#include "snd/livelink/frame_stream.hpp"

namespace snd::livelink {

// ---------------------------------------------------------------------------
// FrameReader
// ---------------------------------------------------------------------------
FrameReader::~FrameReader() { Destroy(); }

bool FrameReader::Init(IAllocator& allocator, std::uint32_t maxPayloadBytes) noexcept {
  Destroy();
  if (maxPayloadBytes > kMaxPayloadBytes) maxPayloadBytes = kMaxPayloadBytes;
  alloc_ = &allocator;
  maxPayload_ = maxPayloadBytes;
  if (maxPayloadBytes != 0u) {
    buffer_ = static_cast<std::uint8_t*>(allocator.Allocate(maxPayloadBytes, 64, MemTag::LiveLink));
    if (buffer_ == nullptr) return false;
  }
  Reset();
  return true;
}

void FrameReader::Destroy() noexcept {
  if (alloc_ != nullptr && buffer_ != nullptr) alloc_->Free(buffer_, MemTag::LiveLink);
  buffer_ = nullptr;
  maxPayload_ = 0;
}

void FrameReader::Reset() noexcept {
  state_ = State::Header;
  have_ = 0;
  current_ = FrameHeader{};
}

HeaderStatus FrameReader::Feed(const std::uint8_t* data, std::size_t n, Handler& handler) noexcept {
  std::size_t i = 0;
  while (i < n) {
    const std::size_t left = n - i;
    if (state_ == State::Header) {
      const std::size_t take = left < kFrameHeaderBytes - have_ ? left : kFrameHeaderBytes - have_;
      std::memcpy(header_ + have_, data + i, take);
      have_ += static_cast<std::uint32_t>(take);
      i += take;
      if (have_ < kFrameHeaderBytes) break;
      const HeaderStatus st = DecodeHeader(header_, &current_);
      have_ = 0;
      if (st != HeaderStatus::Ok) return st;
      if (current_.payloadBytes == 0u) {
        handler.OnFrame(current_, buffer_);
      } else if (current_.payloadBytes > maxPayload_) {
        state_ = State::Skip;
        handler.OnOversized(current_);
      } else {
        state_ = State::Payload;
      }
    } else {
      const std::uint32_t need = current_.payloadBytes - have_;
      const std::uint32_t take = left < need ? static_cast<std::uint32_t>(left) : need;
      if (state_ == State::Payload) std::memcpy(buffer_ + have_, data + i, take);
      have_ += take;
      i += take;
      if (have_ == current_.payloadBytes) {
        if (state_ == State::Payload) handler.OnFrame(current_, buffer_);
        state_ = State::Header;
        have_ = 0;
      }
    }
  }
  return HeaderStatus::Ok;
}

// ---------------------------------------------------------------------------
// FrameQueue
// ---------------------------------------------------------------------------
FrameQueue::~FrameQueue() { Destroy(); }

bool FrameQueue::Init(IAllocator& allocator, std::uint32_t capacityBytes) noexcept {
  Destroy();
  alloc_ = &allocator;
  data_ = static_cast<std::uint8_t*>(allocator.Allocate(capacityBytes, 64, MemTag::LiveLink));
  if (data_ == nullptr) return false;
  capacity_ = capacityBytes;
  head_ = tail_ = 0;
  return true;
}

void FrameQueue::Destroy() noexcept {
  if (alloc_ != nullptr && data_ != nullptr) alloc_->Free(data_, MemTag::LiveLink);
  data_ = nullptr;
  capacity_ = 0;
  head_ = tail_ = 0;
}

void FrameQueue::Write(const void* src, std::uint32_t n) noexcept {
  const std::uint8_t* s = static_cast<const std::uint8_t*>(src);
  while (n != 0u) {
    const std::uint32_t at = static_cast<std::uint32_t>(tail_ % capacity_);
    const std::uint32_t run = capacity_ - at < n ? capacity_ - at : n;
    std::memcpy(data_ + at, s, run);
    s += run;
    n -= run;
    tail_ += run;
  }
}

bool FrameQueue::Push(std::uint16_t type, std::uint16_t flags, std::uint32_t seq, const void* a, std::uint32_t na,
                      const void* b, std::uint32_t nb) noexcept {
  const std::uint64_t payload = std::uint64_t{na} + nb;
  if (data_ == nullptr || payload > kMaxPayloadBytes || kFrameHeaderBytes + payload > Free()) return false;
  std::uint8_t h[kFrameHeaderBytes];
  EncodeHeader(h, type, flags, static_cast<std::uint32_t>(payload), seq);
  Write(h, kFrameHeaderBytes);
  if (na != 0u) Write(a, na);
  if (nb != 0u) Write(b, nb);
  return true;
}

std::uint64_t FrameQueue::HeadFrameBytes() const noexcept {
  if (Used() < kFrameHeaderBytes) return 0;
  std::uint8_t h[kFrameHeaderBytes];
  for (std::uint32_t k = 0; k < kFrameHeaderBytes; ++k) h[k] = data_[(head_ + k) % capacity_];
  FrameHeader fh;
  std::memcpy(&fh, h, sizeof(fh));
  return kFrameHeaderBytes + std::uint64_t{fh.payloadBytes};
}

const std::uint8_t* FrameQueue::Peek(std::uint64_t max, std::size_t* n) const noexcept {
  const std::uint32_t at = static_cast<std::uint32_t>(head_ % capacity_);
  std::uint64_t run = capacity_ - at;
  if (run > Used()) run = Used();
  if (run > max) run = max;
  *n = static_cast<std::size_t>(run);
  return data_ + at;
}

// ---------------------------------------------------------------------------
// FrameSender
// ---------------------------------------------------------------------------
bool FrameSender::Init(IAllocator& allocator, std::uint32_t controlBytes, std::uint32_t bulkBytes) noexcept {
  current_ = nullptr;
  inFlight_ = 0;
  return control_.Init(allocator, controlBytes) && bulk_.Init(allocator, bulkBytes);
}

void FrameSender::Destroy() noexcept {
  control_.Destroy();
  bulk_.Destroy();
  current_ = nullptr;
  inFlight_ = 0;
}

void FrameSender::Clear() noexcept {
  control_.Clear();
  bulk_.Clear();
  current_ = nullptr;
  inFlight_ = 0;
}

IoStatus FrameSender::Pump(ITransport& t, std::uint64_t* sentBytes) noexcept {
  for (;;) {
    if (inFlight_ == 0u) {
      current_ = !control_.Empty() ? &control_ : (!bulk_.Empty() ? &bulk_ : nullptr);
      if (current_ == nullptr) return IoStatus::Ok;
      inFlight_ = current_->HeadFrameBytes();
      if (inFlight_ == 0u) {  // cannot happen: queues hold whole frames
        current_->Clear();
        continue;
      }
    }
    std::size_t n = 0;
    const std::uint8_t* p = current_->Peek(inFlight_, &n);
    std::size_t sent = 0;
    const IoStatus st = t.Send(p, n, &sent);
    if (st == IoStatus::Ok) {
      current_->Pop(sent);
      inFlight_ -= sent;
      if (sentBytes != nullptr) *sentBytes += sent;
      continue;
    }
    return st;
  }
}

}  // namespace snd::livelink
