// Decode engine shared by the per-voice rings, the PCM cache fill and the
// streams (D-23, D-51). Runs on a prefetch-lane worker, or on the audio thread
// while no job of the instance is in flight. No allocation, lock or IO.
#include "detail.hpp"

namespace snd::source::detail {

bool Decoder::SeekTo(std::uint64_t target, std::uint64_t* byte) noexcept {
  carryHead = carryCount = 0;
  drain = false;
  awaitWrap = false;
  ended = false;
  decLoops = 0;
  SeekPoint sp{};
  if (codec == nullptr || state == nullptr || asset == nullptr || !ResolveSeek(*asset, target, &sp)) {
    ended = true;
    decPos = target;
    discard = 0;
    *byte = 0;
    return false;
  }
  codec->Seek(state, sp.frame);
  decPos = target;
  discard = target - sp.frame;
  *byte = sp.byte;
  return true;
}

std::uint64_t Decoder::Run(InputSource& in, OutputSink& out, std::uint32_t maxIterations) noexcept {
  std::uint64_t produced = 0;
  const std::uint64_t total = asset->h.total_frames;
  for (std::uint32_t iter = 0; iter < maxIterations; ++iter) {
    // 1. Frames already decoded.
    while (carryHead < carryCount) {
      const std::uint32_t avail = carryCount - carryHead;
      if (discard != 0u) {
        const std::uint32_t k = discard < avail ? static_cast<std::uint32_t>(discard) : avail;
        carryHead += k;
        discard -= k;
        continue;
      }
      const std::uint64_t regionEnd = loop.WrapsAfter(decLoops) ? loop.le : total;
      if (awaitWrap || decPos >= regionEnd) {  // past the loop end / the end: dropped
        if (loop.WrapsAfter(decLoops)) awaitWrap = true;
        else ended = true;
        carryHead = carryCount;
        break;
      }
      std::uint32_t k = avail;
      if (regionEnd - decPos < k) k = static_cast<std::uint32_t>(regionEnd - decPos);
      const std::uint32_t room = out.Room();
      if (room == 0u) return produced;
      if (room < k) k = room;
      out.Put(carry, carryHead, k);
      carryHead += k;
      decPos += k;
      produced += k;
      if (decPos >= regionEnd) {
        if (loop.WrapsAfter(decLoops)) awaitWrap = true;
        else ended = true;
      }
    }
    if (ended || error != 0u) return produced;
    if (const std::uint32_t f = in.Failure(); f != 0u) {
      error = f;
      return produced;
    }
    // 2. Loop wrap: seek to the loop start (6.3, sample accurate).
    if (awaitWrap) {
      SeekPoint sp{};
      if (!ResolveSeek(*asset, loop.lb, &sp)) {
        ended = true;
        return produced;
      }
      if (!in.SkipToWrap(sp.byte)) return produced;
      codec->Seek(state, sp.frame);
      decPos = loop.lb;
      ++decLoops;
      discard = loop.lb - sp.frame;
      carryHead = carryCount = 0;
      drain = false;
      awaitWrap = false;
      continue;
    }
    if (out.Room() == 0u) return produced;
    // 3. Drain what the codec holds (D-51: until fewer than max frames come back).
    std::uint32_t n = 0;
    if (drain) {
      n = codec->Decode(state, nullptr, 0, carry, kCarryFrames);
    } else {
      DecodeInput ip{};
      if (!in.Peek(ip)) {
        if (in.Failure() != 0u) {
          error = in.Failure();
          return produced;
        }
        if (!in.Exhausted()) return produced;  // waiting for input
        if (loop.WrapsAfter(decLoops)) awaitWrap = true;  // truncated loop: wrap anyway
        else ended = true;
        continue;
      }
      const std::uint32_t len = ip.bytes < kMaxInput ? ip.bytes : kMaxInput;
      n = codec->Decode(state, ip.ptr, len, carry, kCarryFrames);
      in.Consume(len);
    }
    if (n == SND_DECODE_ERROR) {
      error = SND_END_DETAIL_DECODE;
      return produced;
    }
    drain = n == kCarryFrames;
    carryHead = 0;
    carryCount = n;
    if (n == 0u && !drain && decPos >= total && !loop.WrapsAfter(decLoops)) ended = true;
  }
  return produced;
}

bool Shared::EnsureDecoders(const SourceLimits& l, IAllocator* a) noexcept {
  if (decodersReady) {
    ++decoderUsers;
    return true;
  }
  IAllocator* al = a != nullptr ? a : allocator;
  if (al == nullptr) return false;
  if (!decoders.Init(*al, l.maxDecoders, l.decoderSlotBytes)) return false;
  decodersReady = true;
  decoderUsers = 1;
  return true;
}

void Shared::ReleaseDecoders() noexcept {
  if (decoderUsers > 0u) --decoderUsers;
  if (decoderUsers == 0u && decodersReady) {
    decoders.Shutdown();
    decodersReady = false;
  }
}

}  // namespace snd::source::detail
