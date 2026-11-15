// Read sequence of one stream, with loop wrap-around (design.md 6.1, 6.3).
//
// The Stream source keeps a planner per stream and asks it for the next read
// whenever it has room to post one (IoScheduler::Post). Reads are at most
// chunkBytes long and never cross the loop end: the read that reaches the loop
// end is shortened, and the next one starts at the loop start with
// `wrapped = true` (the decoder then seeks its codec to the loop start). This
// is how "the IO reads the head of the loop ahead when the end approaches":
// reading ahead by the target amount automatically crosses into the loop head.
//
// Offsets are file byte offsets. Loop boundaries are byte offsets of codec
// packet boundaries (the seek table, 6.3), resolved by the source.
//
// Header-only, no allocation, no state outside the object: RT-safe on any
// thread; one object is used by one thread at a time.
#ifndef SND_IO_STREAM_READ_PLANNER_HPP
#define SND_IO_STREAM_READ_PLANNER_HPP

#include <cstdint>

namespace snd::io {

struct StreamRead {
  std::uint64_t offset;  // file offset
  std::uint32_t bytes;   // 1..chunkBytes
  bool wrapped;          // first read after jumping from the loop end to the loop start
  bool loopEnd;          // this read ends exactly at the loop end (next one wraps)
};

class StreamReadPlanner {
 public:
  // dataBegin/dataEnd: byte range of the encoded stream in the file.
  // loopBegin/loopEnd: byte range looped forever; loopEnd == 0 = no loop.
  // Invalid loop ranges (empty, outside the data) disable the loop.
  void Reset(std::uint64_t dataBegin, std::uint64_t dataEnd, std::uint64_t loopBegin,
             std::uint64_t loopEnd, std::uint32_t chunkBytes) noexcept {
    dataBegin_ = dataBegin;
    dataEnd_ = dataEnd > dataBegin ? dataEnd : dataBegin;
    chunk_ = chunkBytes == 0u ? 1u : chunkBytes;
    looping_ = loopEnd != 0u && loopBegin < loopEnd && loopBegin >= dataBegin_ && loopEnd <= dataEnd_;
    loopBegin_ = looping_ ? loopBegin : 0u;
    loopEnd_ = looping_ ? loopEnd : 0u;
    cursor_ = dataBegin_;
    wrapPending_ = false;
  }

  // Next read position (seek, devirtualise). Clamped to the data range. A
  // position at or past the loop end of a looping stream wraps to the loop start.
  void Seek(std::uint64_t offset) noexcept {
    if (offset < dataBegin_) offset = dataBegin_;
    if (offset > dataEnd_) offset = dataEnd_;
    cursor_ = offset;
    wrapPending_ = false;
    if (looping_ && cursor_ >= loopEnd_) cursor_ = loopBegin_;
  }

  // Produces the next read. false when a non-looping stream reached the end.
  bool Next(StreamRead& out) noexcept {
    const std::uint64_t limit = looping_ ? loopEnd_ : dataEnd_;
    bool wrapped = wrapPending_;
    if (cursor_ >= limit) {
      if (!looping_) return false;
      cursor_ = loopBegin_;
      wrapped = true;
    }
    const std::uint64_t left = limit - cursor_;
    const std::uint32_t n = left < chunk_ ? static_cast<std::uint32_t>(left) : chunk_;
    out.offset = cursor_;
    out.bytes = n;
    out.wrapped = wrapped;
    cursor_ += n;
    out.loopEnd = looping_ && cursor_ == loopEnd_;
    wrapPending_ = false;
    if (out.loopEnd) {
      cursor_ = loopBegin_;
      wrapPending_ = true;
    }
    return true;
  }

  std::uint64_t Cursor() const noexcept { return cursor_; }
  bool Looping() const noexcept { return looping_; }
  // Bytes left before the loop end (looping) or the data end; 0 at the end.
  std::uint64_t BytesToBoundary() const noexcept {
    if (wrapPending_) return 0u;
    const std::uint64_t limit = looping_ ? loopEnd_ : dataEnd_;
    return cursor_ < limit ? limit - cursor_ : 0u;
  }

 private:
  std::uint64_t dataBegin_ = 0;
  std::uint64_t dataEnd_ = 0;
  std::uint64_t loopBegin_ = 0;
  std::uint64_t loopEnd_ = 0;
  std::uint64_t cursor_ = 0;
  std::uint32_t chunk_ = 1;
  bool looping_ = false;
  bool wrapPending_ = false;
};

}  // namespace snd::io

#endif  // SND_IO_STREAM_READ_PLANNER_HPP
