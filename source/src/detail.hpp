// Shared parts of the Sample and Stream kinds: asset copies, loop arithmetic,
// seek resolution (D-23), PCM reading, carving of the kind pools, free lists,
// PCM rings and the decode engine.
#ifndef SND_SOURCE_DETAIL_HPP
#define SND_SOURCE_DETAIL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "snd/codec/codec.hpp"
#include "snd/codec/decoder_pool.hpp"
#include "snd/internal/source_if.hpp"
#include "snd/source/runtime.hpp"

namespace snd::source::detail {

inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;
inline constexpr std::uint32_t kCarryFrames = 256;   // decode granularity (frames per Decode call)
inline constexpr std::uint32_t kMaxInput = codec::kHoldInputBytes;  // bytes per Decode call (D-51)
inline constexpr std::uint32_t kFadeMs = 5;         // short fade (D-15, Config.tuning.shortFadeMs default)

inline std::uint64_t AlignUp(std::uint64_t v, std::uint64_t a) noexcept { return (v + a - 1u) & ~(a - 1u); }
inline std::uint32_t NextPow2(std::uint32_t v) noexcept {
  std::uint32_t p = 1;
  while (p < v && p < 0x80000000u) p <<= 1u;
  return p;
}

// Bump allocator over the pool core gives a kind (or a measuring pass with base == nullptr).
struct Carver {
  std::uint8_t* base = nullptr;
  std::size_t used = 0;
  std::size_t cap = 0;
  bool ok = true;
  template <class T>
  T* Take(std::size_t n, std::size_t align = 64) noexcept {
    const std::size_t p = static_cast<std::size_t>(AlignUp(used, align));
    const std::size_t bytes = sizeof(T) * n;
    used = p + bytes;
    if (base == nullptr) return nullptr;
    if (used > cap) {
      ok = false;
      return nullptr;
    }
    return reinterpret_cast<T*>(base + p);
  }
};

// LIFO of indices (single thread: the audio thread).
struct FreeStack {
  std::uint32_t* items = nullptr;
  std::uint32_t size = 0;
  std::uint32_t cap = 0;
  void Bind(std::uint32_t* mem, std::uint32_t n) noexcept {
    items = mem;
    cap = n;
    size = n;
    for (std::uint32_t i = 0; i < n; ++i) items[i] = n - 1u - i;
  }
  std::uint32_t Pop() noexcept { return size == 0u ? kNone : items[--size]; }
  void Push(std::uint32_t i) noexcept {
    if (size < cap) items[size++] = i;
  }
};

// Fixed pool of single-channel float rings of `frames` (power of two) each.
struct RingPool {
  float* base = nullptr;
  std::uint32_t frames = 0;
  std::uint32_t count = 0;
  FreeStack free;
  static void Measure(Carver& c, std::uint32_t count, std::uint32_t frames) noexcept {
    c.Take<float>(std::size_t{count} * frames);
    c.Take<std::uint32_t>(count);
  }
  bool Bind(Carver& c, std::uint32_t n, std::uint32_t f) noexcept {
    count = n;
    frames = f;
    base = c.Take<float>(std::size_t{n} * f);
    std::uint32_t* st = c.Take<std::uint32_t>(n);
    if (base == nullptr || st == nullptr) return false;
    free.Bind(st, n);
    return true;
  }
  float* Ring(std::uint32_t i) const noexcept { return base + std::size_t{i} * frames; }
  // Takes `channels` rings into idx[]; false (nothing taken) if not enough are free.
  bool Acquire(std::uint32_t channels, std::uint32_t* idx) noexcept {
    if (free.size < channels) return false;
    for (std::uint32_t c = 0; c < channels; ++c) idx[c] = free.Pop();
    return true;
  }
  void Release(std::uint32_t channels, std::uint32_t* idx) noexcept {
    for (std::uint32_t c = channels; c > 0u; --c) {
      if (idx[c - 1u] != kNone) free.Push(idx[c - 1u]);
      idx[c - 1u] = kNone;
    }
  }
};

// What a kind keeps of a SourceAsset (core's copy lives in the bank record,
// which may be freed before the last decode job of a released voice ends; the
// bank memory itself is covered by core's job epoch, 3.3).
struct AssetCopy {
  CodecHeader h;
  const std::uint8_t* data;
  std::uint64_t dataBytes;
  const std::uint8_t* preload;
  std::uint32_t preloadBytes;
  std::uint32_t preloadFrames;
  const std::uint64_t* seekFrames;
  const std::uint64_t* seekOffsets;
  std::uint32_t seekCount;
  std::uint32_t streamFile;
  std::uint64_t streamDataOffset;
  std::uint64_t loopStart;
  std::uint64_t loopEnd;
  Id assetId;
  LoadForm form;
  std::uint8_t stallPolicy;
  std::uint8_t virtualReturn;
  std::uint8_t ioPriority;
  std::uint16_t bankSlot;
  std::uint16_t loadGen;  // SourceAsset::reserved: bank load generation (pcm_cache.hpp)
};
inline AssetCopy CopyAsset(const SourceAsset& a) noexcept {
  AssetCopy c{};
  c.h = a.codec;
  c.data = a.data;
  c.dataBytes = a.dataBytes;
  c.preload = a.preload;
  c.preloadBytes = a.preload != nullptr ? a.preloadBytes : 0u;
  c.preloadFrames = a.preload != nullptr ? a.preloadFrames : 0u;
  c.seekFrames = a.seekFrames;
  c.seekOffsets = a.seekOffsets;
  c.seekCount = (a.seekFrames != nullptr && a.seekOffsets != nullptr) ? a.seekCount : 0u;
  c.streamFile = a.streamFile;
  c.streamDataOffset = a.streamDataOffset;
  c.loopStart = a.loopStart;
  c.loopEnd = a.loopEnd;
  c.assetId = a.assetId;
  c.form = a.form;
  c.stallPolicy = a.stallPolicy;
  c.virtualReturn = a.virtualReturn;
  c.ioPriority = a.ioPriority;
  c.bankSlot = a.bankSlot;
  c.loadGen = a.reserved;
  return c;
}

// Loop region of a voice: [lb, le), le == 0 = no loop. kStartLoop without
// valid loop points loops the whole asset; loop points without kStartLoop do
// not loop (core sets kStartLoop whenever the asset has points).
struct LoopRange {
  std::uint64_t lb = 0;
  std::uint64_t le = 0;
  std::uint32_t count = 0;  // loops to play in total (0 = infinite); the last one runs on to the end
  bool Looping() const noexcept { return le > lb && count != 1u; }
  // True if the material wraps at le after `done` completed loops.
  bool WrapsAfter(std::uint32_t done) const noexcept { return Looping() && (count == 0u || done + 1u < count); }
};
// Proposed (stage-2 report): SourceStartRequest::flags bits 16..31 = loop count
// (CLIP.loopCount, 0 = infinite). Core does not set them yet, so 0 = infinite.
inline constexpr std::uint32_t kStartLoopCountShift = 16;
inline LoopRange MakeLoop(const AssetCopy& a, bool loop, std::uint64_t start, std::uint64_t end,
                          std::uint32_t count = 0) noexcept {
  LoopRange r;
  r.count = count;
  if (!loop) return r;
  const std::uint64_t total = a.h.total_frames;
  if (end != 0u && end <= total && start < end) {
    r.lb = start;
    r.le = end;
  } else if (total > 0u) {
    r.lb = 0;
    r.le = total;
  }
  return r;
}
// Maps a material position into the loop (start positions past the loop end).
inline std::uint64_t WrapInto(const LoopRange& l, std::uint64_t pos) noexcept {
  if (!l.Looping() || pos < l.le) return pos;
  return l.lb + (pos - l.le) % (l.le - l.lb);
}

// Seek point (D-23): decode from `frame` (first playable frame of a packet)
// reading data bytes from `byte`, dropping target - frame frames.
struct SeekPoint {
  std::uint64_t frame;
  std::uint64_t byte;
};
inline bool ResolveSeek(const AssetCopy& a, std::uint64_t target, SeekPoint* out) noexcept {
  if (target >= a.h.total_frames) return false;
  if (target == 0u) {
    *out = SeekPoint{0, 0};
    return true;
  }
  if (a.seekCount != 0u) {
    const std::uint32_t e = codec::FindSeekPoint(a.seekFrames, a.seekCount, target, a.h.pre_roll, a.h.total_frames);
    if (e == codec::kNoSeekPoint) return false;
    *out = SeekPoint{a.seekFrames[e], a.seekOffsets[e]};
    return true;
  }
  const std::uint64_t t = target > a.h.pre_roll ? target - a.h.pre_roll : 0u;
  codec::PacketPosition p{};
  if (codec::PacketForFrame(a.h, t, &p)) {
    *out = SeekPoint{p.frame, p.byteOffset};
    return true;
  }
  *out = SeekPoint{0, 0};  // variable packets without a seek table: decode from the start
  return true;
}

inline bool IsPcm(std::uint32_t c) noexcept { return c == SND_CODEC_PCM_F32 || c == SND_CODEC_PCM_S16; }
inline std::uint32_t PcmFrameBytes(const CodecHeader& h) noexcept {
  return (h.codec == SND_CODEC_PCM_F32 ? 4u : 2u) * h.channels;
}

// Planar float out of interleaved PCMF / PC16 (bank-format.md 6.5).
// NaN / Inf of PCMF become 0 (same as the decoder).
inline void ReadPcm(const CodecHeader& h, const std::uint8_t* data, std::uint64_t frame, std::uint32_t n,
                    float* const* out, std::uint32_t outOffset, std::uint32_t channels) noexcept {
  const std::uint32_t ch = h.channels;
  if (h.codec == SND_CODEC_PCM_F32) {
    const std::uint8_t* p = data + frame * 4u * ch;
    for (std::uint32_t f = 0; f < n; ++f) {
      for (std::uint32_t c = 0; c < channels; ++c) {
        float v;
        std::memcpy(&v, p + (std::size_t{f} * ch + c) * 4u, 4);
        if (!(v - v == 0.0f)) v = 0.0f;
        out[c][outOffset + f] = v;
      }
    }
  } else {
    const std::uint8_t* p = data + frame * 2u * ch;
    for (std::uint32_t f = 0; f < n; ++f) {
      for (std::uint32_t c = 0; c < channels; ++c) {
        const std::uint8_t* s = p + (std::size_t{f} * ch + c) * 2u;
        const std::int16_t v = static_cast<std::int16_t>(static_cast<std::uint16_t>(s[0] | (s[1] << 8)));
        out[c][outOffset + f] = static_cast<float>(v) * (1.0f / 32768.0f);
      }
    }
  }
}

// Advances a material position by n played frames (loop wrap, loopCount).
inline void AdvancePos(const LoopRange& l, std::uint64_t& pos, std::uint32_t& loops, std::uint64_t n) noexcept {
  pos += n;
  while (l.WrapsAfter(loops) && pos >= l.le) {
    const std::uint64_t len = l.le - l.lb;
    if (l.count == 0u) {
      const std::uint64_t over = pos - l.le;
      loops += static_cast<std::uint32_t>(1u + over / len);
      pos = l.lb + over % len;
      break;
    }
    pos -= len;
    ++loops;
  }
}

// Virtual voice: playhead += outputFrames * pitch (32.32 fixed point).
inline bool AdvanceVirtualPos(const LoopRange& l, std::uint64_t total, Playhead& ph, std::uint32_t outFrames,
                              float pitch) noexcept {
  const double adv = static_cast<double>(outFrames) * static_cast<double>(pitch > 0.0f ? pitch : 0.0f);
  const std::uint64_t fixed = static_cast<std::uint64_t>(adv * 4294967296.0) + ph.fraction;
  ph.fraction = static_cast<std::uint32_t>(fixed);
  std::uint64_t pos = ph.frame;
  AdvancePos(l, pos, ph.loopCount, fixed >> 32);
  ph.frame = pos;
  return !l.WrapsAfter(ph.loopCount) && pos >= total;
}

// Short fades (D-15) applied in place.
inline void FadeOutTail(float* const* out, std::uint32_t channels, std::uint32_t offset, std::uint32_t n,
                        std::uint32_t fadeFrames) noexcept {
  const std::uint32_t f = n < fadeFrames ? n : fadeFrames;
  if (f == 0u) return;
  for (std::uint32_t c = 0; c < channels; ++c) {
    float* p = out[c] + offset + (n - f);
    for (std::uint32_t i = 0; i < f; ++i) p[i] *= static_cast<float>(f - i - 1u) / static_cast<float>(f);
  }
}
// Fades in from `done` of `total` fade frames; returns frames of fade applied.
inline std::uint32_t FadeIn(float* const* out, std::uint32_t channels, std::uint32_t offset, std::uint32_t n,
                            std::uint32_t done, std::uint32_t total) noexcept {
  if (done >= total) return 0;
  const std::uint32_t k = (total - done) < n ? total - done : n;
  for (std::uint32_t c = 0; c < channels; ++c) {
    float* p = out[c] + offset;
    for (std::uint32_t i = 0; i < k; ++i) p[i] *= static_cast<float>(done + i + 1u) / static_cast<float>(total + 1u);
  }
  return k;
}

// ---------------------------------------------------------------------------
// Decode engine (prefetch-lane jobs; also on the audio thread while no job runs)
// ---------------------------------------------------------------------------
struct DecodeInput {
  const std::uint8_t* ptr;
  std::uint32_t bytes;
};
class InputSource {
 public:
  virtual ~InputSource() = default;
  // Next bytes at the current input position; false if none are available now.
  virtual bool Peek(DecodeInput& in) noexcept = 0;
  virtual void Consume(std::uint32_t bytes) noexcept = 0;
  // True when no more input will ever come for the current position (end of data).
  virtual bool Exhausted() const noexcept = 0;
  // Loop wrap: position the input at data byte `byte` (the loop start's seek
  // point). Streams drop input until the planner's wrapped read. false = not yet.
  virtual bool SkipToWrap(std::uint64_t byte) noexcept = 0;
  // Input failed (IO error); the decoder stops with `detail`.
  virtual std::uint32_t Failure() const noexcept { return 0; }
};
class OutputSink {
 public:
  virtual ~OutputSink() = default;
  virtual std::uint32_t Room() const noexcept = 0;
  virtual void Put(const float* const* src, std::uint32_t offset, std::uint32_t n) noexcept = 0;
};

// Resident data of the bank as decoder input.
class ResidentInput final : public InputSource {
 public:
  ResidentInput(const AssetCopy& a, std::uint64_t& off) noexcept : a_(a), off_(off) {}
  bool Peek(DecodeInput& in) noexcept override {
    if (a_.data == nullptr || off_ >= a_.dataBytes) return false;
    const std::uint64_t left = a_.dataBytes - off_;
    in.ptr = a_.data + off_;
    in.bytes = left < kMaxInput ? static_cast<std::uint32_t>(left) : kMaxInput;
    return true;
  }
  void Consume(std::uint32_t n) noexcept override { off_ += n; }
  bool Exhausted() const noexcept override { return a_.data == nullptr || off_ >= a_.dataBytes; }
  bool SkipToWrap(std::uint64_t byte) noexcept override {
    off_ = byte;
    return true;
  }

 private:
  const AssetCopy& a_;
  std::uint64_t& off_;
};

struct Decoder {
  ICodec* codec = nullptr;
  void* state = nullptr;
  AssetCopy* asset = nullptr;
  LoopRange loop;
  std::uint32_t channels = 0;  // decoded channels (= asset channels)
  std::uint64_t decPos = 0;    // material frame of the next kept frame
  std::uint32_t decLoops = 0;  // loops completed by the decoder
  std::uint64_t discard = 0;   // decoded frames to drop first (pre-roll, D-23)
  float* carry[kMaxChannels] = {};
  std::uint32_t carryHead = 0;
  std::uint32_t carryCount = 0;
  bool drain = false;
  bool awaitWrap = false;
  bool ended = false;
  std::uint32_t error = 0;     // snd_end_detail (STREAM_IO, DECODE)

  // Positions the codec for material frame `target`; returns the data byte to
  // read from. false = past the end (ended).
  bool SeekTo(std::uint64_t target, std::uint64_t* byte) noexcept;
  // Decodes until the sink is full, input is missing, or the material ended.
  // Returns frames put into the sink.
  std::uint64_t Run(InputSource& in, OutputSink& out, std::uint32_t maxIterations = 1u << 16) noexcept;
};

// Carry storage (kCarryFrames x channels).
inline void MeasureCarry(Carver& c, std::uint32_t contexts) noexcept {
  c.Take<float>(std::size_t{contexts} * kMaxChannels * kCarryFrames);
}

// Shared state of the kinds (one per Runtime).
struct Shared {
  IAllocator* allocator = nullptr;
  io::IoScheduler* io = nullptr;
  io::StreamPool* streamPool = nullptr;
  std::uint32_t maxVoiceChannels = kMaxChannels;  // RuntimeDesc (Config.maxVoiceChannels)
  codec::DecoderPool decoders;
  std::uint32_t decoderUsers = 0;  // kinds that called EnsureDecoders
  bool decodersReady = false;
  std::atomic<std::uint64_t> counters[32] = {};
  bool EnsureDecoders(const SourceLimits& l, IAllocator* a) noexcept;
  void ReleaseDecoders() noexcept;
};
enum Counter : std::uint32_t {
  kCacheHits, kCacheMisses, kCacheEvictions, kCacheFallbacks, kRejectedDecoder, kRejectedStreamSlot,
  kRejectedOther, kSampleUnderruns, kStreamStalls, kStreamStallBlocks, kStreamIoErrors, kDecodeErrors,
  kJobsPosted, kJobsRejected, kIoPosted, kIoRejected, kInlinePrimes, kCounterCount
};
inline void Count(Shared& s, Counter c, std::uint64_t n = 1) noexcept {
  s.counters[c].fetch_add(n, std::memory_order_relaxed);
}

// Codec lookup: services first (custom codecs override built-ins), else built-in.
inline ICodec* FindCodec(const SourceServices& sv, std::uint32_t fourcc) noexcept {
  ICodec* c = sv.findCodec != nullptr ? sv.findCodec(sv.codecCtx, fourcc) : nullptr;
  return c != nullptr ? c : codec::BuiltinCodec(fourcc);
}

// Job helpers: done counter doubles as the "job in flight" flag.
inline bool JobBusy(const std::uint32_t& counter) noexcept {
  return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(counter)).load(std::memory_order_acquire) != 0u;
}
inline bool PostJob(const SourceServices& sv, Shared& sh, snd_job_fn fn, void* data, std::uint32_t* counter,
                    const std::uint32_t* cancel, std::uint32_t priority) noexcept {
  if (sv.postJob == nullptr) return false;
  std::atomic_ref<std::uint32_t>(*counter).store(1u, std::memory_order_release);
  JobDesc j{};
  j.fn = fn;
  j.data = data;
  j.done_counter = counter;
  j.cancel_flag = cancel;
  j.priority = priority;
  if (!sv.postJob(sv.jobCtx, j, JobLane::Prefetch)) {
    std::atomic_ref<std::uint32_t>(*counter).store(0u, std::memory_order_release);
    Count(sh, kJobsRejected);
    return false;
  }
  Count(sh, kJobsPosted);
  return true;
}

}  // namespace snd::source::detail

#endif  // SND_SOURCE_DETAIL_HPP
