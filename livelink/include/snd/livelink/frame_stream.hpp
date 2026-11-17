// Framing over a byte stream (protocol §2): an incremental reader that cuts
// frames out of whatever the transport delivers, and a sender with two frame
// queues (control first, then bulk) that never interleaves frames.
//
// Single-threaded: each object belongs to one thread (the live link thread or
// the tool's network thread). Memory comes from the IAllocator at Init.
#ifndef SND_LIVELINK_FRAME_STREAM_HPP
#define SND_LIVELINK_FRAME_STREAM_HPP

#include <cstddef>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/livelink/transport.hpp"
#include "snd/livelink/wire.hpp"

namespace snd::livelink {

class FrameReader {
 public:
  class Handler {
   public:
    virtual ~Handler() = default;
    // A complete frame. `payload` is valid during the call.
    virtual void OnFrame(const FrameHeader& header, const std::uint8_t* payload) noexcept = 0;
    // A frame larger than the reader's buffer (but within kMaxPayloadBytes): its
    // payload is skipped. The handler usually answers with Error.
    virtual void OnOversized(const FrameHeader& header) noexcept = 0;
  };

  FrameReader() noexcept = default;
  ~FrameReader();
  FrameReader(const FrameReader&) = delete;
  FrameReader& operator=(const FrameReader&) = delete;

  bool Init(IAllocator& allocator, std::uint32_t maxPayloadBytes) noexcept;
  void Destroy() noexcept;
  void Reset() noexcept;  // new connection

  // Consumes `n` bytes. Returns Ok, or the status of a bad header (the stream is
  // then unusable: send Error and disconnect). Stops at the first bad header.
  HeaderStatus Feed(const std::uint8_t* data, std::size_t n, Handler& handler) noexcept;
  // false while a frame is half received (for diagnostics).
  bool AtBoundary() const noexcept { return state_ == State::Header && have_ == 0u; }
  std::uint32_t MaxPayload() const noexcept { return maxPayload_; }

 private:
  enum class State : std::uint8_t { Header, Payload, Skip };
  IAllocator* alloc_ = nullptr;
  std::uint8_t* buffer_ = nullptr;
  std::uint32_t maxPayload_ = 0;
  State state_ = State::Header;
  std::uint8_t header_[kFrameHeaderBytes] = {};
  FrameHeader current_{};
  std::uint32_t have_ = 0;  // bytes of the header / payload / skipped part received
};

// FIFO of complete frames in a byte ring (single thread).
class FrameQueue {
 public:
  FrameQueue() noexcept = default;
  ~FrameQueue();
  FrameQueue(const FrameQueue&) = delete;
  FrameQueue& operator=(const FrameQueue&) = delete;

  bool Init(IAllocator& allocator, std::uint32_t capacityBytes) noexcept;
  void Destroy() noexcept;
  void Clear() noexcept { head_ = tail_ = 0; }

  // Appends one frame (payload = a then b) if all of it fits; false otherwise.
  bool Push(std::uint16_t type, std::uint16_t flags, std::uint32_t seq, const void* a, std::uint32_t na,
            const void* b = nullptr, std::uint32_t nb = 0) noexcept;
  std::uint64_t Used() const noexcept { return tail_ - head_; }
  std::uint32_t Capacity() const noexcept { return capacity_; }
  std::uint64_t Free() const noexcept { return capacity_ - Used(); }
  bool Empty() const noexcept { return head_ == tail_; }

  // Bytes of the frame at the head (header + payload); 0 if empty.
  std::uint64_t HeadFrameBytes() const noexcept;
  // Contiguous bytes at the head (up to `max`).
  const std::uint8_t* Peek(std::uint64_t max, std::size_t* n) const noexcept;
  void Pop(std::size_t n) noexcept { head_ += n; }

 private:
  void Write(const void* src, std::uint32_t n) noexcept;
  IAllocator* alloc_ = nullptr;
  std::uint8_t* data_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::uint64_t head_ = 0, tail_ = 0;
};

// Sends the queued frames: finishes the frame in flight, then control frames
// before bulk frames.
class FrameSender {
 public:
  bool Init(IAllocator& allocator, std::uint32_t controlBytes, std::uint32_t bulkBytes) noexcept;
  void Destroy() noexcept;
  void Clear() noexcept;
  FrameQueue& control() noexcept { return control_; }
  FrameQueue& bulk() noexcept { return bulk_; }
  bool Idle() const noexcept { return inFlight_ == 0u && control_.Empty() && bulk_.Empty(); }
  // Sends as much as the transport takes. Ok / WouldBlock, or Closed / Error.
  IoStatus Pump(ITransport& t, std::uint64_t* sentBytes) noexcept;

 private:
  FrameQueue control_, bulk_;
  FrameQueue* current_ = nullptr;
  std::uint64_t inFlight_ = 0;  // bytes of the current frame still to send
};

}  // namespace snd::livelink

#endif  // SND_LIVELINK_FRAME_STREAM_HPP
