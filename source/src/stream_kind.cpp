// Stream kind (stream_kind.hpp).
#include "stream_kind.hpp"

#include <new>
#include <thread>

#include "snd/base/sparse_set.hpp"
#include "snd/format/bank_format.hpp"

namespace snd::source::detail {

namespace bf = snd::bank_format;
using schema::ColumnId;
using schema::SystemId;

namespace {

// Input of a stream's decoder: the preload first, then the channel's records.
class StreamInput final : public InputSource {
 public:
  StreamInput(StreamKind& k, StreamInst& s) noexcept : k_(k), s_(s), ch_(k.Channel(s.slot)) {}

  bool Peek(DecodeInput& in) noexcept override {
    if (s_.preActive != 0u) {
      if (s_.preOff < s_.a.preloadBytes) {
        const std::uint64_t left = s_.a.preloadBytes - s_.preOff;
        in.ptr = s_.a.preload + s_.preOff;
        in.bytes = left < kMaxInput ? static_cast<std::uint32_t>(left) : kMaxInput;
        return true;
      }
      s_.preActive = 0;
    }
    for (;;) {
      if (s_.curValid == 0u && !Next()) return false;
      if (s_.curOff < s_.cur.bytes) {
        in.ptr = k_.Pool().Data(s_.cur.chunk) + s_.curOff;
        in.bytes = s_.cur.bytes - s_.curOff;
        return true;
      }
      Finish();
    }
  }
  void Consume(std::uint32_t n) noexcept override {
    if (s_.preActive != 0u) s_.preOff += n;
    else s_.curOff += n;
  }
  bool Exhausted() const noexcept override { return s_.preActive == 0u && s_.curValid == 0u && s_.eof != 0u; }
  bool SkipToWrap(std::uint64_t byte) noexcept override {
    if (s_.skipToWrap == 0u) {  // drop the rest of the record that holds the loop end
      s_.preActive = 0;
      s_.skipToWrap = s_.wrapNoTag != 0u ? 0u : 1u;  // reads re-planned at the loop start carry no tag
      s_.wrapNoTag = 0;
      s_.wrapSkip = 0;
      if (s_.curValid != 0u) {
        ch_.Release(s_.cur);
        s_.curValid = 0;
      }
      if (s_.a.preload != nullptr && byte < s_.a.preloadBytes) {
        // The loop start lies in the preload: go on from memory at once; the
        // planner's wrapped read follows, its bytes up to the preload end are skipped.
        s_.preActive = 1;
        s_.preOff = byte;
        s_.wrapSkip = s_.a.preloadBytes - byte;
        s_.eof = 0;
        return true;
      }
    }
    if (s_.curValid == 0u && !Next()) return false;  // Next() drops records up to the wrapped one
    s_.eof = 0;
    return true;
  }
  std::uint32_t Failure() const noexcept override { return s_.error.load(std::memory_order_relaxed); }

 private:
  // Pops the next current data record into cur (header records are parsed here).
  bool Next() noexcept {
    io::ChunkRecord rec{};
    while (ch_.Pop(rec)) {
      if (rec.epoch != ch_.Epoch()) {  // stale (handoff: compare after Pop)
        ch_.Release(rec);
        continue;
      }
      if ((rec.tag & kTagHeader) != 0u) {
        k_.ParseHeader(s_, rec);
        ch_.Release(rec);
        continue;
      }
      if (rec.status != io::ChunkStatus::Ok) {
        s_.error.store(SND_END_DETAIL_STREAM_IO, std::memory_order_release);
        ch_.Release(rec);
        return false;
      }
      if (s_.skipToWrap != 0u) {  // waiting for the loop's wrapped read: drop the reads past the loop end
        if ((rec.tag & kTagWrapped) == 0u) {
          ch_.Release(rec);
          continue;
        }
        s_.skipToWrap = 0;
      }
      std::uint32_t skip = 0;
      if (s_.wrapSkip != 0u) {  // bytes the preload already gave
        skip = s_.wrapSkip < rec.bytes ? static_cast<std::uint32_t>(s_.wrapSkip) : rec.bytes;
        s_.wrapSkip -= skip;
      }
      s_.cur = rec;
      s_.curValid = 1;
      s_.curOff = skip;
      k_.MirrorReadPos(s_);
      return true;
    }
    return false;
  }
  void Finish() noexcept {
    const io::ChunkRecord rec = s_.cur;
    ch_.Release(rec);
    s_.curValid = 0;
    const std::uint64_t end = rec.offset + rec.bytes;
    const std::uint64_t dataEnd = s_.a.streamDataOffset + s_.dataBytes;
    if (rec.bytes < rec.requested) {
      // A short read inside the data: the file is shorter than its header says.
      if (end < dataEnd) s_.error.store(SND_END_DETAIL_STREAM_IO, std::memory_order_release);
      s_.eof = 1;
    }
    if (!s_.loop.WrapsAfter(s_.dec.decLoops) && s_.headerDone.load(std::memory_order_relaxed) != 0u &&
        end >= s_.a.streamDataOffset + s_.dataBytes) {
      s_.eof = 1;
    }
  }

  StreamKind& k_;
  StreamInst& s_;
  io::StreamChannel& ch_;
};

class StreamSink final : public OutputSink {
 public:
  StreamSink(StreamInst& s, std::uint32_t frames, std::uint64_t w, const std::atomic<std::uint64_t>& r,
             std::uint64_t limit) noexcept
      : s_(s), mask_(frames - 1u), frames_(frames), w_(w), r_(r), limit_(limit) {}
  std::uint32_t Room() const noexcept override {
    const std::uint64_t used = w_ - r_.load(std::memory_order_acquire);
    std::uint64_t room = used >= frames_ ? 0u : frames_ - used;
    if (room > limit_) room = limit_;
    return static_cast<std::uint32_t>(room);
  }
  void Put(const float* const* src, std::uint32_t offset, std::uint32_t n) noexcept override {
    for (std::uint32_t i = 0; i < n;) {
      const std::uint32_t at = static_cast<std::uint32_t>(w_ & mask_);
      std::uint32_t k = frames_ - at;
      if (k > n - i) k = n - i;
      for (std::uint32_t c = 0; c < s_.channels; ++c) std::memcpy(s_.ring[c] + at, src[c] + offset + i, sizeof(float) * k);
      w_ += k;
      i += k;
    }
    limit_ -= n;
  }
  std::uint64_t W() const noexcept { return w_; }

 private:
  StreamInst& s_;
  std::uint32_t mask_;
  std::uint32_t frames_;
  std::uint64_t w_;
  const std::atomic<std::uint64_t>& r_;
  std::uint64_t limit_;
};

// Data byte just past the packet holding frame `end - 1` (loop end for the planner).
std::uint64_t EndByte(const AssetCopy& a, std::uint64_t end, std::uint64_t dataBytes) noexcept {
  const CodecHeader& h = a.h;
  if (IsPcm(h.codec)) {
    const std::uint64_t b = end * PcmFrameBytes(h);
    return b < dataBytes ? b : dataBytes;
  }
  const std::uint32_t pf = codec::PacketFrames(h);
  const std::uint32_t pb = codec::PacketBytes(h);
  if (pf != 0u && pb != 0u) {
    const std::uint64_t packets = (end + h.pre_skip + pf - 1u) / pf;
    const std::uint64_t b = packets * pb;
    return b < dataBytes ? b : dataBytes;
  }
  for (std::uint32_t i = 0; i < a.seekCount; ++i) {  // variable packets: first seek point at or after the end
    if (a.seekFrames[i] >= end) return a.seekOffsets[i] < dataBytes ? a.seekOffsets[i] : dataBytes;
  }
  return dataBytes;
}


// dataBytes the header may give for the asset (bank-format.md 6.3): exact for
// PCM and ADPCM (whole packets for preSkip + totalFrames), bounded for Opus
// (packets of 3 .. 2 + streams x 1277 bytes), at least 1 for other codecs.
bool DataBytesFit(const AssetCopy& a, std::uint64_t bytes) noexcept {
  const CodecHeader& h = a.h;
  if (IsPcm(h.codec)) return bytes == h.total_frames * PcmFrameBytes(h);
  const std::uint32_t pf = codec::PacketFrames(h);
  const std::uint32_t pb = codec::PacketBytes(h);
  if (pf == 0u) return bytes != 0u;
  const std::uint64_t packets = (h.total_frames + h.pre_skip + pf - 1u) / pf;
  if (pb != 0u) return bytes == packets * pb;
  if (h.codec == SND_CODEC_OPUS) {
    const std::uint64_t maxPacket = codec::OpusMaxPacketBytes(h) + codec::kOpusLengthPrefixBytes;
    return bytes >= packets * (codec::kOpusLengthPrefixBytes + 1u) && (maxPacket <= 2u || bytes <= packets * maxPacket);
  }
  return bytes != 0u;
}

}  // namespace

// ---------------------------------------------------------------------------
// Columns (D-45): ringWrite / ringRead are the PCM ring positions.
// ---------------------------------------------------------------------------
std::uint64_t StreamKind::RingWrite(const StreamInst& s) const noexcept {
  return schema::ReadColumn<SystemId::Render, ColumnId::Stream_ringWrite>(*sv_.tables)[s.slot].load(std::memory_order_acquire);
}
std::uint64_t StreamKind::RingRead(const StreamInst& s) const noexcept {
  return schema::ReadColumn<SystemId::Render, ColumnId::Stream_ringRead>(*sv_.tables)[s.slot].load(std::memory_order_acquire);
}
void StreamKind::SetRingRead(StreamInst& s, std::uint64_t v) noexcept {
  schema::WriteColumn<SystemId::Render, ColumnId::Stream_ringRead>(*sv_.tables)[s.slot].store(v, std::memory_order_release);
}
void StreamKind::SetRingWrite(StreamInst& s, std::uint64_t v) noexcept {
  schema::WriteColumn<SystemId::ExternalDecode, ColumnId::Stream_ringWrite>(*sv_.tables)[s.slot].store(
      v, std::memory_order_release);
}
void StreamKind::MirrorReadPos(StreamInst& s) noexcept {
  // The IO side's published read position (StreamChannel::ReadPos), copied by the
  // channel's consumer into the column the schema gives to the IO side.
  const std::uint64_t p = Channel(s.slot).ReadPos();
  schema::WriteColumn<SystemId::ExternalIo, ColumnId::Stream_readPos>(*sv_.tables)[s.slot].store(
      p == io::StreamChannel::kNoReadPos ? 0u : p, std::memory_order_release);
}

bool StreamKind::ParseHeader(StreamInst& s, const io::ChunkRecord& rec) noexcept {
  bf::StreamFileHeader h{};
  bool ok = rec.status == io::ChunkStatus::Ok && rec.bytes >= sizeof(h);
  if (ok) std::memcpy(&h, Pool().Data(rec.chunk), sizeof(h));
  ok = ok && h.magic == bf::kStreamMagic && h.version == bf::kStreamVersion && h.codec == s.a.h.codec &&
       h.channels == s.a.h.channels && h.sampleRate == s.a.h.sample_rate && h.assetId == s.a.assetId &&
       h.totalFrames == s.a.h.total_frames && (s.a.streamDataOffset == 0u || h.dataOffset == s.a.streamDataOffset) &&
       DataBytesFit(s.a, h.dataBytes);
  if (!ok) {
    s.error.store(SND_END_DETAIL_STREAM_IO, std::memory_order_release);
    return false;
  }
  if (s.a.streamDataOffset == 0u) s.a.streamDataOffset = h.dataOffset;
  s.dataBytes = h.dataBytes;
  s.headerDone.store(1u, std::memory_order_release);
  return true;
}

// ---------------------------------------------------------------------------
// Layout and lifetime
// ---------------------------------------------------------------------------
void StreamKind::Layout(Carver& c, const SourceLimits& l) noexcept {
  const std::uint32_t rate = l.sampleRate == 0u ? 48000u : l.sampleRate;
  const std::uint32_t block = l.blockFrames == 0u ? 256u : l.blockFrames;
  const std::uint64_t target = std::uint64_t{l.streamTargetAheadMs} * rate / 1000u;
  const std::uint32_t frames = NextPow2(static_cast<std::uint32_t>(target + 4u * block > 1024u ? target + 4u * block : 1024u));
  const std::uint32_t ringCount = l.maxStreams * sh_.maxVoiceChannels;  // every stream at maxVoiceChannels
  const std::uint32_t n = l.maxStreams;
  if (c.base == nullptr) {
    c.Take<StreamInst>(n);
    c.Take<std::uint32_t>(n);
    RingPool::Measure(c, ringCount, frames);
    MeasureCarry(c, n);
    return;
  }
  inst_ = c.Take<StreamInst>(n);
  std::uint32_t* fl = c.Take<std::uint32_t>(n);
  if (n != 0u && (inst_ == nullptr || fl == nullptr || !rings_.Bind(c, ringCount, frames))) {
    c.ok = false;
    return;
  }
  float* carry = c.Take<float>(std::size_t{n} * kMaxChannels * kCarryFrames);
  if (n != 0u && carry == nullptr) {
    c.ok = false;
    return;
  }
  cap_ = n;
  if (n != 0u) free_.Bind(fl, n);
  for (std::uint32_t i = 0; i < n; ++i) {
    StreamInst* s = new (&inst_[i]) StreamInst();
    s->state = 0;
    s->slot = i;
    s->kind = this;
    s->jobCounter = 0;
    s->cancel = 0;
    for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
      s->rings[ch] = kNone;
      s->ring[ch] = nullptr;
      s->dec.carry[ch] = carry + (std::size_t{i} * kMaxChannels + ch) * kCarryFrames;
    }
  }
  readyFrames_ = frames / 4u < 8u * block ? frames / 4u : 8u * block;
  fadeFrames_ = rate * kFadeMs / 1000u;
  if (fadeFrames_ == 0u) fadeFrames_ = 1;
}

std::size_t StreamKind::PoolBytes(const SourceLimits& limits) const noexcept {
  if (sh_.io == nullptr || sh_.streamPool == nullptr) return 0;
  Carver c;
  const_cast<StreamKind*>(this)->Layout(c, limits);
  return c.used;
}

bool StreamKind::Init(void* pool, std::size_t bytes, const SourceLimits& limits, const SourceServices& services) noexcept {
  sv_ = services;
  lim_ = limits;
  enabled_ = false;
  if (sh_.io == nullptr || sh_.streamPool == nullptr || services.tables == nullptr) return false;
  if (sh_.io->ChannelCount() < limits.maxStreams) return false;  // the IO scheduler must have a channel per stream slot
  Carver c{static_cast<std::uint8_t*>(pool), 0, bytes, true};
  Layout(c, limits);
  if (!c.ok) return false;
  if (limits.maxDecoders != 0u) {
    if (!sh_.EnsureDecoders(limits, services.allocator)) return false;
    decoders_ = true;
  }
  chunkBytes_ = sh_.streamPool->ChunkBytes();
  if (limits.streamChunkBytes != 0u && limits.streamChunkBytes < chunkBytes_) chunkBytes_ = limits.streamChunkBytes;
  live_ = 0;
  enabled_ = true;
  return true;
}

void StreamKind::Shutdown() noexcept {
  for (std::uint32_t i = 0; i < cap_; ++i) std::atomic_ref<std::uint32_t>(inst_[i].cancel).store(1u, std::memory_order_release);
  for (std::uint32_t i = 0; i < cap_; ++i) {
    StreamInst& s = inst_[i];
    while (JobBusy(s.jobCounter)) std::this_thread::yield();
    if (s.state != 0u) {
      Channel(i).Cancel();
      if (s.curValid != 0u) Channel(i).Release(s.cur);
      s.curValid = 0;
      Channel(i).Reclaim();  // what is still in flight comes back at IoScheduler::Shutdown
      if (sh_.decodersReady) sh_.decoders.Release(s.dslot);
      rings_.Release(s.channels, s.rings);
      s.state = 0;
    }
    s.~StreamInst();
  }
  cap_ = 0;
  live_ = 0;
  if (decoders_) sh_.ReleaseDecoders();
  decoders_ = false;
  enabled_ = false;
}

// ---------------------------------------------------------------------------
// Decode (job, or audio thread while no job runs)
// ---------------------------------------------------------------------------
void StreamKind::Decode(StreamInst& s, std::uint64_t limit) noexcept {
  if (s.error.load(std::memory_order_relaxed) != 0u) return;
  StreamInput in(*this, s);
  const auto& r = schema::ReadColumn<SystemId::Render, ColumnId::Stream_ringRead>(*sv_.tables)[s.slot];
  StreamSink sink(s, rings_.frames, RingWrite(s), r, limit);
  s.dec.Run(in, sink);
  SetRingWrite(s, sink.W());
  if (s.dec.error != 0u && s.error.load(std::memory_order_relaxed) == 0u) s.error.store(s.dec.error, std::memory_order_release);
  if (s.dec.ended) s.ended.store(1u, std::memory_order_release);
}

void SND_CALL StreamKind::DecodeJob(void* data) {
  StreamInst& s = *static_cast<StreamInst*>(data);
  if (std::atomic_ref<std::uint32_t>(s.cancel).load(std::memory_order_acquire) != 0u) return;
  s.kind->Decode(s);
}

// ---------------------------------------------------------------------------
// Audio thread helpers (no job of `s` in flight)
// ---------------------------------------------------------------------------
void StreamKind::DropCurrent(StreamInst& s) noexcept {
  if (s.curValid != 0u) Channel(s.slot).Release(s.cur);
  s.curValid = 0;
  s.curOff = 0;
  s.skipToWrap = 0;
  s.wrapSkip = 0;
  s.wrapNoTag = 0;
}

void StreamKind::SetupPlanner(StreamInst& s) noexcept {
  const std::uint64_t base = s.a.streamDataOffset;
  std::uint64_t lb = 0, le = 0;
  if (s.loop.Looping()) {
    SeekPoint sp{};
    if (ResolveSeek(s.a, s.loop.lb, &sp)) {
      lb = base + sp.byte;
      le = base + EndByte(s.a, s.loop.le, s.dataBytes);
      if (le <= lb) lb = le = 0;
    }
  }
  s.planner.Reset(base, base + s.dataBytes, lb, le, chunkBytes_);
  s.planner.Seek(base + s.ioStart);
  s.lastReadEnd = base + s.ioStart;
  s.wrapsPosted = 0;
  s.plannerSet = 1;
  s.planEnded = 0;
}

void StreamKind::ResetForSeek(StreamInst& s, std::uint64_t target, std::uint32_t loops) noexcept {
  // IO and decoder side only (no job of s runs). The PCM ring is not touched:
  // the new data is appended after the current write position and Render
  // adopts it (Execute / RenderOne), so nothing Render reads changes here.
  io::StreamChannel& ch = Channel(s.slot);
  if (s.hasPendingLoop != 0u) {
    s.loop = s.pendingLoop;
    s.hasPendingLoop = 0;
  }
  s.dec.loop = s.loop;
  ch.Cancel();
  DropCurrent(s);
  s.ended.store(0u, std::memory_order_relaxed);
  if (s.error.load(std::memory_order_relaxed) != SND_END_DETAIL_STREAM_IO) s.error.store(0u, std::memory_order_relaxed);
  s.dec.error = 0;
  s.eof = 0;
  std::uint64_t byte = 0;
  if (!s.dec.SeekTo(target, &byte)) {
    s.ended.store(1u, std::memory_order_release);
    s.preActive = 0;
    s.ioStart = 0;
  } else if (s.a.preload != nullptr && byte < s.a.preloadBytes) {
    s.preActive = 1;
    s.preOff = byte;
    s.ioStart = s.a.preloadBytes;
  } else {
    s.preActive = 0;
    s.ioStart = byte;
  }
  s.dec.decLoops = loops;
  if (s.headerDone.load(std::memory_order_acquire) != 0u) {
    SetupPlanner(s);
  } else {
    s.plannerSet = 0;
    s.headerPosted = 0;  // the cancelled header read is posted again
  }
  if (s.ended.load(std::memory_order_relaxed) != 0u) s.planEnded = 1;
}

void StreamKind::Execute(StreamInst& s, std::uint32_t gen, std::uint32_t kind, std::uint64_t target, std::uint32_t loops,
                         std::uint32_t& inlineBudget) noexcept {
  const std::uint64_t base = RingWrite(s);
  // A loop (a pending SetLoop included) that ends before the target: the voice
  // goes to the loop start, and the playhead with it.
  const LoopRange& lp = s.hasPendingLoop != 0u ? s.pendingLoop : s.loop;
  if (lp.WrapsAfter(loops) && target >= lp.le) target = lp.lb;
  ResetForSeek(s, target, loops);
  s.execBase = base;
  s.execTarget = target;
  s.execLoops = loops;
  s.execKind = kind;
  s.execLoop = s.loop;
  s.execGen.store(gen, std::memory_order_release);
  if (s.preActive != 0u && inlineBudget != 0u) {  // bounded: the room the ring has now
    --inlineBudget;
    Decode(s);
  }
}

// SetLoop without a re-read: the decoder has not wrapped since what Render
// reads now (ring = decPos - ringPos frames), every posted read is linear
// (no wrap posted that the decoder has not consumed) and the reads do not go
// past where the old and the new loop differ.
bool StreamKind::LoopChangeFits(const StreamInst& s, const LoopRange& to) const noexcept {
  if (s.plannerSet == 0u || s.dec.awaitWrap || s.skipToWrap != 0u || s.wrapsPosted != s.dec.decLoops ||
      s.reqGen.load(std::memory_order_relaxed) != s.adoptGen.load(std::memory_order_acquire)) {
    return false;
  }
  const std::uint64_t total = s.a.h.total_frames;
  const std::uint32_t loops = Ld(s.pubRingLoops);
  const std::uint64_t ringPos = Ld(s.pubRingPos);
  const std::uint64_t oldEnd = s.loop.WrapsAfter(loops) ? s.loop.le : total;
  const std::uint64_t newEnd = to.WrapsAfter(loops) ? to.le : total;
  const std::uint64_t agree = oldEnd < newEnd ? oldEnd : newEnd;
  const std::uint64_t held = RingWrite(s) - RingRead(s);
  if (ringPos >= newEnd || s.dec.decPos < ringPos || s.dec.decPos - ringPos != held || s.dec.decPos > agree) {
    return false;
  }
  if (to.WrapsAfter(loops)) {  // the reads must not pass the new loop end
    const std::uint64_t endByte = s.a.streamDataOffset + EndByte(s.a, to.le, s.dataBytes);
    if (s.lastReadEnd > endByte) return false;
  }
  return true;
}

void StreamKind::PostReads(StreamInst& s) noexcept {
  io::StreamChannel& ch = Channel(s.slot);
  if (s.headerPosted == 0u) {
    source::IoReadRequest rq{};
    rq.streamSlot = s.slot;
    rq.chunkIndex = kTagHeader | (s.seq++ & 0x3FFFFFFFu);
    rq.path = s.path;
    rq.offset = 0;
    rq.dst = nullptr;
    rq.bytes = sizeof(bf::StreamFileHeader);
    rq.priority = s.a.ioPriority;
    if (sh_.io->Post(rq)) {
      s.headerPosted = 1;
      Count(sh_, kIoPosted);
    } else {
      Count(sh_, kIoRejected);
      return;
    }
  }
  if (s.plannerSet == 0u || s.planEnded != 0u) return;
  while (ch.Outstanding() < ch.Capacity()) {
    const io::StreamReadPlanner save = s.planner;
    io::StreamRead rd{};
    if (!s.planner.Next(rd)) {
      s.planEnded = 1;
      return;
    }
    if (rd.wrapped) ++s.wrapsPosted;
    s.lastReadEnd = rd.offset + rd.bytes;
    if (rd.loopEnd && s.loop.count != 0u && s.wrapsPosted + 1u >= s.loop.count) {
      // Finite loop count: after the last loop the reads run on to the data end.
      const std::uint64_t base = s.a.streamDataOffset;
      s.planner.Reset(base, base + s.dataBytes, 0, 0, chunkBytes_);
      s.planner.Seek(rd.offset + rd.bytes);
    }
    source::IoReadRequest rq{};
    rq.streamSlot = s.slot;
    rq.chunkIndex = (rd.wrapped ? kTagWrapped : 0u) | (s.seq++ & 0x3FFFFFFFu);
    rq.path = s.path;
    rq.offset = rd.offset;
    rq.dst = nullptr;
    rq.bytes = rd.bytes;
    rq.priority = s.a.ioPriority;
    if (!sh_.io->Post(rq)) {
      s.planner = save;
      Count(sh_, kIoRejected);
      return;
    }
    Count(sh_, kIoPosted);
  }
}

bool StreamKind::TryFree(StreamInst& s) noexcept {
  DropCurrent(s);
  if (!Channel(s.slot).Reclaim()) return false;  // reads still in flight: next block
  sh_.decoders.Release(s.dslot);  // generation-checked (A1-15)
  s.dslot = codec::DecoderSlot{};
  rings_.Release(s.channels, s.rings);
  for (std::uint32_t c = 0; c < kMaxChannels; ++c) s.ring[c] = nullptr;
  s.state = 0;
  free_.Push(s.slot);
  if (live_ > 0u) --live_;
  return true;
}

// ---------------------------------------------------------------------------
// Start / Release / Control (audio thread, RunEventLogic)
// ---------------------------------------------------------------------------
bool StreamKind::StartOne(const SourceStartRequest& r, SourceStartResult& res, std::uint32_t& inlineBudget) noexcept {
  res = SourceStartResult{kInvalidSlot, SND_END_DETAIL_NONE, 0, 0};
  const SourceAsset* a = r.asset;
  if (!enabled_ || a == nullptr || a->form != LoadForm::Stream || a->streamPath == nullptr || a->codec.channels == 0u ||
      a->codec.channels > kMaxChannels) {
    res.endDetail = SND_END_DETAIL_STREAM_IO;
    Count(sh_, kRejectedOther);
    return false;
  }
  const std::size_t plen = std::strlen(a->streamPath);
  ICodec* codec = FindCodec(sv_, a->codec.codec);
  if (plen == 0u || plen >= io::kMaxIoPathBytes || codec == nullptr ||
      (codec::IsBuiltinCodec(a->codec.codec) && codec::ValidateHeader(a->codec) != codec::HeaderError::None)) {
    res.endDetail = (plen == 0u || plen >= io::kMaxIoPathBytes) ? SND_END_DETAIL_STREAM_IO : SND_END_DETAIL_DECODE;
    Count(sh_, kRejectedOther);
    return false;
  }
  const std::uint32_t slot = free_.Pop();
  if (slot == kNone) {
    res.endDetail = SND_END_DETAIL_NO_STREAM_SLOT;
    Count(sh_, kRejectedStreamSlot);
    return false;
  }
  StreamInst& s = inst_[slot];
  Channel(slot).ReopenFile();  // a new playback in this slot: the stream file may have been rebuilt
  if (!Channel(slot).Reclaim()) {  // cannot happen: slots are freed only once clean
    free_.Push(slot);
    res.endDetail = SND_END_DETAIL_NO_STREAM_SLOT;
    Count(sh_, kRejectedStreamSlot);
    return false;
  }
  const std::uint32_t assetCh = a->codec.channels;
  // The ring keeps the rendered channels (<= Config.maxVoiceChannels; the pool guarantees maxStreams of them).
  std::uint32_t ch = r.channels == 0u || r.channels > assetCh ? assetCh : r.channels;
  if (ch > sh_.maxVoiceChannels) ch = sh_.maxVoiceChannels;
  if (!rings_.Acquire(ch, s.rings)) {
    free_.Push(slot);
    res.endDetail = SND_END_DETAIL_NO_STREAM_SLOT;
    Count(sh_, kRejectedStreamSlot);
    return false;
  }
  codec::DecoderSlot ds;
  if (!sh_.decodersReady || sh_.decoders.Acquire(codec->StateBytes(a->codec), &ds) != codec::DecoderPoolStatus::Ok) {
    rings_.Release(ch, s.rings);
    free_.Push(slot);
    res.endDetail = SND_END_DETAIL_DECODER_POOL_FULL;  // D-14
    Count(sh_, kRejectedDecoder);
    return false;
  }
  // Instance.
  s.a = CopyAsset(*a);
  std::memcpy(s.path, a->streamPath, plen + 1u);
  s.loop = MakeLoop(s.a, (r.flags & kStartLoop) != 0u, s.a.loopStart, s.a.loopEnd, r.flags >> kStartLoopCountShift);
  s.channels = ch;
  for (std::uint32_t c = 0; c < ch; ++c) s.ring[c] = rings_.Ring(s.rings[c]);
  s.dslot = ds;
  s.dec.codec = codec;
  s.dec.state = ds.state;
  s.dec.asset = &s.a;
  s.dec.loop = s.loop;
  s.dec.channels = ch;
  s.dec.error = 0;
  s.curValid = 0;
  s.curOff = 0;
  s.skipToWrap = 0;
  s.dataBytes = 0;
  s.eof = 0;
  s.headerDone.store(0u, std::memory_order_relaxed);
  s.ended.store(0u, std::memory_order_relaxed);
  s.error.store(0u, std::memory_order_relaxed);
  std::atomic_ref<std::uint32_t>(s.cancel).store(0u, std::memory_order_relaxed);
  s.plannerSet = 0;
  s.planEnded = 0;
  s.headerPosted = 0;
  s.pendingStop = 0;
  s.hasPendingLoop = 0;
  s.virt = (r.flags & kStartVirtual) != 0u ? 1u : 0u;
  s.errorCounted = 0;
  s.primed = 0;
  s.stalled = 0;
  s.kill = 0;
  s.fadeDone = fadeFrames_;
  s.lag = 0;
  s.reseek.store(0u, std::memory_order_relaxed);
  const std::uint64_t start = WrapInto(s.loop, r.startFrame);
  s.rloop = s.loop;
  s.ringPos = start;
  s.ringLoops = 0;
  s.vpos = start;
  s.vloops = 0;
  s.renderPos = start;
  s.renderLoops = 0;
  s.pubRingPos = start;
  s.pubRingLoops = 0;
  s.loopReq = 0;
  {  // nothing pending: every generation equal
    const std::uint32_t g = s.reqGen.load(std::memory_order_relaxed);
    s.execGen.store(g, std::memory_order_relaxed);
    s.adoptGen.store(g, std::memory_order_relaxed);
  }
  s.hasOverride = start != r.startFrame ? 1u : 0u;
  s.overridePos = start;
  // Stream row (RunEventLogic creates it, D-45: slot = row).
  schema::TableRows& rows = schema::CreatorRows<SystemId::RunEventLogic, schema::TableId::Stream>(*sv_.tables);
  base::rows::InsertNext(rows, slot);
  schema::WriteColumn<SystemId::RunEventLogic, ColumnId::Stream_fileId>(*sv_.tables)[slot] = s.a.streamFile;
  schema::WriteColumn<SystemId::RunEventLogic, ColumnId::Stream_syncGroup>(*sv_.tables)[slot] = r.syncGroup;
  StreamControl ctl{};
  ctl.state = static_cast<std::uint8_t>(StreamStateId::Priming);
  ctl.priority = s.a.ioPriority;
  ctl.stallPolicy = s.a.stallPolicy;
  ctl.chunkBytes = chunkBytes_;
  ctl.loopStartFrame = s.loop.lb;
  ctl.loopEndFrame = s.loop.le;
  schema::WriteColumn<SystemId::RunEventLogic, ColumnId::Stream_control>(*sv_.tables)[slot] = ctl;
  schema::WriteColumn<SystemId::RunEventLogic, ColumnId::Stream_ringRange>(*sv_.tables)[slot] = RangeU32{s.rings[0], ch};
  schema::WriteColumn<SystemId::Render, ColumnId::Stream_stallCount>(*sv_.tables)[slot] = 0u;
  schema::WriteColumn<SystemId::ExternalIo, ColumnId::Stream_readPos>(*sv_.tables)[slot].store(0u, std::memory_order_relaxed);
  SetRingRead(s, 0);
  SetRingWrite(s, 0);
  codec->Init(ds.state, s.a.h);
  s.state = 1;
  ++live_;
  ResetForSeek(s, start, 0);
  if (Ld(s.virt) == 0u) {
    PostReads(s);
    if (s.preActive != 0u && inlineBudget != 0u) {  // decode the head of the preload now (6.1)
      --inlineBudget;
      Count(sh_, kInlinePrimes);
      StreamInput in(*this, s);
      const auto& rr = schema::ReadColumn<SystemId::Render, ColumnId::Stream_ringRead>(*sv_.tables)[s.slot];
      StreamSink sink(s, rings_.frames, RingWrite(s), rr, readyFrames_);
      s.dec.Run(in, sink);
      SetRingWrite(s, sink.W());
      if (s.dec.error != 0u) s.error.store(s.dec.error, std::memory_order_release);
      if (s.dec.ended) s.ended.store(1u, std::memory_order_release);
    }
  }
  res.sourceSlot = slot;
  res.needsPriming = Ready(s) ? 0u : 1u;
  return true;
}

void StreamKind::Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept {
  std::uint32_t inlineBudget = 8;
  for (std::uint32_t i = 0; i < count; ++i) StartOne(requests[i], results[i], inlineBudget);
}

void StreamKind::Release(const SlotIndex* slots, std::uint32_t count) noexcept {
  for (std::uint32_t i = 0; i < count; ++i) {
    const SlotIndex sl = slots[i];
    if (sl >= cap_ || inst_[sl].state != 1u) continue;
    StreamInst& s = inst_[sl];
    std::atomic_ref<std::uint32_t>(s.cancel).store(1u, std::memory_order_release);
    Channel(sl).Cancel();  // stop the IO now; the records come back through Reclaim
    schema::TableRows& rows = schema::CreatorRows<SystemId::RunEventLogic, schema::TableId::Stream>(*sv_.tables);
    base::rows::Remove(rows, sl);
    s.state = 2;
    if (!JobBusy(s.jobCounter)) TryFree(s);
  }
}

// SetLoop that cannot keep the read plan: keep the decoded PCM up to where the
// old and the new loop differ (or to what is decoded), drop what lies past it,
// and decode / read again from there under the new loop. Render keeps reading
// the ring without a break. false when the ring is not one linear piece from
// the playhead.
bool StreamKind::KeepAndRedecode(StreamInst& s, const LoopRange& to) noexcept {
  if (s.dec.awaitWrap || s.skipToWrap != 0u) return false;
  const std::uint64_t total = s.a.h.total_frames;
  const std::uint32_t loops = Ld(s.pubRingLoops);
  const std::uint64_t ringPos = Ld(s.pubRingPos);
  const std::uint64_t oldEnd = s.loop.WrapsAfter(loops) ? s.loop.le : total;
  const std::uint64_t newEnd = to.WrapsAfter(loops) ? to.le : total;
  const std::uint64_t agree = oldEnd < newEnd ? oldEnd : newEnd;
  const std::uint64_t r = RingRead(s);
  const std::uint64_t held = RingWrite(s) - r;
  if (s.dec.decPos < ringPos || s.dec.decPos - ringPos != held || ringPos >= agree) return false;
  const std::uint64_t keepEnd = s.dec.decPos < agree ? s.dec.decPos : agree;
  s.loop = to;
  s.newLoop = to;  // Render follows it from its next RenderBatch
  St(s.loopReq, 1u);
  SetRingWrite(s, r + (keepEnd - ringPos));
  ResetForSeek(s, keepEnd, loops);  // IO and decoder from keepEnd (the ring is not touched)
  if (keepEnd == newEnd && to.WrapsAfter(loops) && s.headerDone.load(std::memory_order_relaxed) != 0u) {
    // The ring holds everything up to the new loop end: the decoder goes on at
    // the loop start (from the preload when it holds it, else the first reads,
    // planned from the loop start) and decodes the head now (bounded).
    s.dec.carryHead = s.dec.carryCount = 0;
    s.dec.drain = false;
    s.dec.discard = 0;
    s.dec.decPos = to.le;
    s.dec.awaitWrap = true;
    s.dec.ended = false;
    s.ended.store(0u, std::memory_order_relaxed);
    s.preActive = 0;
    s.wrapNoTag = 1;
    s.ioStart = EndByte(s.a, to.le, s.dataBytes);
    SetupPlanner(s);  // the cursor at the loop end wraps to the loop start
    Decode(s, readyFrames_);
  }
  return true;
}

void StreamKind::Control(const SourceControlOp* ops, std::uint32_t count) noexcept {
  for (std::uint32_t i = 0; i < count; ++i) {
    const SourceControlOp& op = ops[i];
    if (op.sourceSlot >= cap_ || inst_[op.sourceSlot].state != 1u) continue;
    StreamInst& s = inst_[op.sourceSlot];
    StreamControl* ctl = schema::WriteColumn<SystemId::RunEventLogic, ColumnId::Stream_control>(*sv_.tables) + op.sourceSlot;
    std::uint64_t target = kNone;
    switch (op.op) {
      case ControlOp::Seek:
        target = op.frame;
        break;
      case ControlOp::Virtualize:
        St(s.virt, 1u);
        s.pendingStop = 1;  // IO stopped, position kept (6.3)
        ctl->state = static_cast<std::uint8_t>(StreamStateId::Paused);
        break;
      case ControlOp::Devirtualize:
        St(s.virt, 0u);
        s.pendingStop = 0;
        if (s.a.virtualReturn == 2u) {
          St(s.kill, 1u);
        } else {
          target = s.a.virtualReturn == 1u ? 0u : op.frame;
        }
        break;
      case ControlOp::SetLoop: {
        const bool loop = op.frame2 != 0u || op.frame != 0u;
        const LoopRange to = MakeLoop(s.a, loop, op.frame, op.frame2, s.loop.count);
        ctl->loopStartFrame = to.lb;
        ctl->loopEndFrame = to.le;
        if (!JobBusy(s.jobCounter) && s.hasPendingLoop == 0u && LoopChangeFits(s, to)) {
          // The data read and decoded so far is the same under the new loop:
          // keep the PCM ring, change the read plan from where it is.
          s.loop = to;
          s.dec.loop = to;
          s.newLoop = to;  // Render takes it at the head of its next RenderBatch
          St(s.loopReq, 1u);
          const std::uint64_t base = s.a.streamDataOffset;
          std::uint64_t lb = 0, le = 0;
          SeekPoint sp{};
          if (to.Looping() && ResolveSeek(s.a, to.lb, &sp)) {
            lb = base + sp.byte;
            le = base + EndByte(s.a, to.le, s.dataBytes);
            if (le <= lb) lb = le = 0;
          }
          s.planner.Reset(base, base + s.dataBytes, lb, le, chunkBytes_);
          s.planner.Seek(s.lastReadEnd);
          s.planEnded = 0;
          break;
        }
        const std::uint32_t want = s.reqGen.load(std::memory_order_relaxed);
        if (want != s.adoptGen.load(std::memory_order_acquire)) {
          // A seek is pending: its target stays, the voice follows the new loop from there.
          s.pendingLoop = to;
          s.hasPendingLoop = 1;
          if (s.execGen.load(std::memory_order_relaxed) == want) {  // executed already: once more with the loop
            s.reqGen.store(want + 1u, std::memory_order_release);
          }
          break;
        }
        const std::uint64_t rpos = Ld(s.renderPos);
        const bool behind = to.WrapsAfter(Ld(s.pubRingLoops)) && rpos >= to.le;
        if (!behind && !JobBusy(s.jobCounter) && s.hasPendingLoop == 0u && KeepAndRedecode(s, to)) break;
        s.pendingLoop = to;  // otherwise: re-read with the new loop, from the loop start when the
        s.hasPendingLoop = 1;  // loop end is behind the playhead (all forms), else from the position
        target = behind ? to.lb : rpos;
        break;
      }
      case ControlOp::Pause:
        ctl->state = static_cast<std::uint8_t>(StreamStateId::Paused);
        break;
      case ControlOp::Resume:
        ctl->state = static_cast<std::uint8_t>(StreamStateId::Running);
        break;
    }
    if (target == kNone) continue;
    s.seekTarget = WrapInto(s.hasPendingLoop != 0u ? s.pendingLoop : s.loop, target);
    s.seekLoops = op.op == ControlOp::SetLoop ? Ld(s.renderLoops) : 0u;
    s.reqGen.store(s.reqGen.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
    ctl->state = static_cast<std::uint8_t>(StreamStateId::Priming);
  }
}

bool StreamKind::Ready(const StreamInst& s) const noexcept {
  const std::uint32_t want = s.reqGen.load(std::memory_order_acquire);
  if (s.execGen.load(std::memory_order_acquire) != want) return false;  // not executed yet
  if (s.ended.load(std::memory_order_acquire) != 0u || s.error.load(std::memory_order_acquire) != 0u) return true;
  const std::uint64_t from = s.adoptGen.load(std::memory_order_acquire) == want ? RingRead(s) : s.execBase;
  return RingWrite(s) - from >= readyFrames_;
}

void StreamKind::QueryReady(const SlotIndex* slots, std::uint32_t count, std::uint8_t* ready) const noexcept {
  for (std::uint32_t i = 0; i < count; ++i) {
    const SlotIndex s = slots[i];
    ready[i] = (s >= cap_ || inst_[s].state != 1u || Ld(inst_[s].virt) != 0u || Ready(inst_[s])) ? 1u : 0u;
  }
}

// ---------------------------------------------------------------------------
// Render (RT worker / audio thread): copies out of the PCM ring only.
// ---------------------------------------------------------------------------
void StreamKind::RenderOne(StreamInst& s, const RenderItem& it, VoiceRenderStatus& st) noexcept {
  if (Ld(s.loopReq) != 0u && Xchg(s.loopReq, 0u) != 0u) s.rloop = s.newLoop;  // SetLoop in place (Control)
  if (Ld(s.kill) != 0u) {
    st.ended = 1;
    return;
  }
  if (s.hasOverride != 0u) {  // start position mapped into the loop (Start, before the first render)
    it.playhead->frame = s.overridePos;
    it.playhead->fraction = 0;
    s.hasOverride = 0;
  }
  if (Ld(s.virt) != 0u) return;  // virtual: silence (never stale data)
  // Adopt a re-positioned ring (seek, devirtualise, re-seek) once Publish executed it.
  const std::uint32_t want = s.reqGen.load(std::memory_order_acquire);
  if (want != s.adoptGen.load(std::memory_order_relaxed)) {
    if (s.execGen.load(std::memory_order_acquire) != want) return;  // asked, not executed yet: silence
    SetRingRead(s, s.execBase);
    s.rloop = s.execLoop;
    if (s.execKind == 2u) {  // time priority re-seek: the voice runs on by the clock
      s.lag -= s.reseekAhead;
      s.ringPos = s.execTarget;
      s.ringLoops = s.execLoops;
    } else {
      s.ringPos = s.execTarget;
      s.ringLoops = s.execLoops;
      s.vpos = s.execTarget;
      s.vloops = s.execLoops;
      s.lag = 0;
      s.primed = 0;
      s.stalled = 0;
      s.fadeDone = fadeFrames_;  // no fade: a seek is sample accurate (core fades devirtualised voices)
      it.playhead->frame = s.execTarget;
      it.playhead->fraction = 0;
      it.playhead->loopCount = s.execLoops;
    }
    s.adoptGen.store(want, std::memory_order_release);
  }
  const std::uint32_t err = s.error.load(std::memory_order_acquire);
  const std::uint32_t ended = s.ended.load(std::memory_order_acquire);
  const std::uint64_t w = RingWrite(s);
  std::uint64_t r = RingRead(s);
  std::uint64_t avail = w - r;
  const bool timePriority = s.a.stallPolicy == 0u;
  const std::uint32_t req = it.framesRequested;
  const std::uint32_t mask = rings_.frames - 1u;
  auto drop = [&](std::uint64_t n) noexcept {
    r += n;
    avail -= n;
    AdvancePos(s.rloop, s.ringPos, s.ringLoops, n);
  };
  auto publish = [&]() noexcept {
    SetRingRead(s, r);
    it.playhead->frame = s.vpos;
    it.playhead->loopCount = s.vloops;
  };
  if (s.primed == 0u) {
    if (avail < readyFrames_ && ended == 0u && err == 0u) return;  // Priming: silence, position kept
    s.primed = 1;
  }
  if (s.stalled != 0u) {
    // Resume once the ready amount (plus the frames to skip) is buffered.
    const std::uint64_t skip = s.lag > 0 ? static_cast<std::uint64_t>(s.lag) : 0u;
    const bool waitAhead = s.lag < 0;  // re-seeked ahead: the voice has not reached the ring yet
    if (!waitAhead && (avail >= readyFrames_ + skip || ended != 0u || err != 0u)) {
      drop(skip < avail ? skip : avail);
      s.lag = 0;
      s.stalled = 0;
      s.fadeDone = 0;
      s.vpos = s.ringPos;
      s.vloops = s.ringLoops;
    } else {
      st.starved = 1;
      Count(sh_, kStreamStallBlocks);
      if (timePriority) {
        s.lag += static_cast<std::int64_t>(req);
        AdvancePos(s.rloop, s.vpos, s.vloops, req);  // the playhead follows the clock
        const std::uint64_t room = rings_.frames > readyFrames_ + req ? rings_.frames - readyFrames_ - req : 0u;
        if (s.lag > 0 && static_cast<std::uint64_t>(s.lag) > room && s.reseek.load(std::memory_order_relaxed) == 0u) {
          // The gap no longer fits the ring: read again from where the voice will be.
          const std::uint64_t lead = readyFrames_ + 2u * req;
          std::uint64_t pos = s.vpos;
          std::uint32_t loops = s.vloops;
          AdvancePos(s.rloop, pos, loops, lead);
          s.reseekTarget = pos;
          s.reseekLoops = loops;
          s.reseekAhead = s.lag + static_cast<std::int64_t>(lead);  // target - ringPos
          s.reseek.store(1u, std::memory_order_release);
        }
      }
      publish();
      return;
    }
  }
  std::uint32_t n = req;
  if (avail < n) n = static_cast<std::uint32_t>(avail);
  for (std::uint32_t done = 0; done < n;) {
    const std::uint32_t at = static_cast<std::uint32_t>((r + done) & mask);
    std::uint32_t k = rings_.frames - at;
    if (k > n - done) k = n - done;
    for (std::uint32_t c = 0; c < it.channels && c < s.channels; ++c) {
      std::memcpy(it.out[c] + it.frameOffset + done, s.ring[c] + at, sizeof(float) * k);
    }
    done += k;
  }
  s.fadeDone += FadeIn(it.out, it.channels, it.frameOffset, n, s.fadeDone, fadeFrames_);
  drop(n);
  s.vpos = s.ringPos;
  s.vloops = s.ringLoops;
  st.framesRendered = n;
  if (n < req) {
    if (err != 0u || ended != 0u) {
      if (err != 0u && n != 0u) FadeOutTail(it.out, it.channels, it.frameOffset, n, fadeFrames_);
      if (n == 0u) st.ended = 1;
    } else {
      // Underrun (6.3): fade out what we have, count the stall.
      FadeOutTail(it.out, it.channels, it.frameOffset, n, fadeFrames_);
      s.stalled = 1;
      st.starved = 1;
      ++schema::WriteColumn<SystemId::Render, ColumnId::Stream_stallCount>(*sv_.tables)[s.slot];
      Count(sh_, kStreamStalls);
      if (timePriority) {
        s.lag = static_cast<std::int64_t>(req - n);
        AdvancePos(s.rloop, s.vpos, s.vloops, req - n);
      }
    }
  }
  publish();
}

void StreamKind::RenderBatch(const snd::source::RenderBatch& b, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t i = 0; i < b.count; ++i) {
    const RenderItem& it = b.items[i];
    status[i] = VoiceRenderStatus{};
    if (it.sourceSlot >= cap_ || inst_[it.sourceSlot].state != 1u) {
      status[i].ended = 1;
      continue;
    }
    StreamInst& s = inst_[it.sourceSlot];
    RenderOne(s, it, status[i]);
    St(s.renderPos, it.playhead->frame);
    St(s.renderLoops, it.playhead->loopCount);
    St(s.pubRingPos, s.ringPos);
    St(s.pubRingLoops, s.ringLoops);
  }
}

void StreamKind::AdvanceVirtual(const AdvanceItem* items, std::uint32_t count, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t i = 0; i < count; ++i) {
    status[i] = VoiceRenderStatus{};
    const AdvanceItem& it = items[i];
    if (it.sourceSlot >= cap_ || inst_[it.sourceSlot].state != 1u) {
      status[i].ended = 1;
      continue;
    }
    StreamInst& s = inst_[it.sourceSlot];
    if (s.hasOverride != 0u) {
      it.playhead->frame = s.overridePos;
      it.playhead->fraction = 0;
      s.hasOverride = 0;
    }
    if (AdvanceVirtualPos(s.rloop, s.a.h.total_frames, *it.playhead, it.outputFrames, it.pitch)) status[i].ended = 1;
    St(s.renderPos, it.playhead->frame);
  }
}

// ---------------------------------------------------------------------------
// Background (audio thread, Publish): IO, decode jobs, deferred releases
// ---------------------------------------------------------------------------
void StreamKind::ScheduleBackground(const BlockInfo&) noexcept {
  std::uint32_t inlineBudget = 4;
  for (std::uint32_t i = 0; i < cap_; ++i) {
    StreamInst& s = inst_[i];
    if (s.state == 0u || JobBusy(s.jobCounter)) continue;
    if (s.state == 2u) {
      TryFree(s);
      continue;
    }
    if (s.error.load(std::memory_order_acquire) != 0u) {
      if (s.errorCounted == 0u) {
        s.errorCounted = 1;
        Count(sh_, s.error.load(std::memory_order_relaxed) == SND_END_DETAIL_DECODE ? kDecodeErrors : kStreamIoErrors);
      }
      continue;
    }
    const std::uint32_t want = s.reqGen.load(std::memory_order_relaxed);
    if (s.reseek.load(std::memory_order_acquire) != 0u && s.execGen.load(std::memory_order_relaxed) == want &&
        s.adoptGen.load(std::memory_order_acquire) == want) {
      // Time priority: the voice keeps moving; the ring restarts ahead of it (Render adopts it).
      s.reseek.store(0u, std::memory_order_relaxed);
      s.reqGen.store(want + 1u, std::memory_order_release);
      Execute(s, want + 1u, 2u, s.reseekTarget, s.reseekLoops, inlineBudget);
    }
    if (s.pendingStop != 0u) {
      Channel(i).Cancel();
      DropCurrent(s);
      s.pendingStop = 0;
      s.plannerSet = 0;  // a devirtualise seeks and plans again
      s.headerPosted = s.headerDone.load(std::memory_order_relaxed) != 0u ? 1u : 0u;
    }
    if (Ld(s.virt) != 0u) continue;
    {
      const std::uint32_t g = s.reqGen.load(std::memory_order_relaxed);
      if (g != s.execGen.load(std::memory_order_relaxed)) Execute(s, g, 1u, s.seekTarget, s.seekLoops, inlineBudget);
    }
    // Header (the audio thread is the channel's consumer while no job runs).
    if (s.headerDone.load(std::memory_order_relaxed) == 0u && s.curValid == 0u) {
      if (const io::ChunkRecord* p = Channel(i).Peek(); p != nullptr && (p->tag & kTagHeader) != 0u) {
        io::ChunkRecord rec{};
        if (Channel(i).Pop(rec)) {
          ParseHeader(s, rec);
          Channel(i).Release(rec);
        }
      }
    }
    if (s.headerDone.load(std::memory_order_acquire) != 0u && s.plannerSet == 0u && s.reqGen.load(std::memory_order_relaxed) == s.execGen.load(std::memory_order_relaxed)) SetupPlanner(s);
    if (s.error.load(std::memory_order_relaxed) != 0u) continue;
    PostReads(s);
    // Decode job when the PCM ring has room and there is input.
    const std::uint64_t used = RingWrite(s) - RingRead(s);
    if (s.ended.load(std::memory_order_relaxed) != 0u || rings_.frames - used < rings_.frames / 4u) continue;
    const bool input = s.preActive != 0u || s.curValid != 0u || Channel(i).Ready() != 0u || s.dec.drain || s.dec.awaitWrap ||
                       s.dec.carryHead < s.dec.carryCount || (s.eof != 0u && !s.dec.ended);
    if (!input) continue;
    if (!PostJob(sv_, sh_, &StreamKind::DecodeJob, &s, &s.jobCounter, &s.cancel, 2u) && sv_.postJob == nullptr) {
      Decode(s);  // no job service: decode here (tests without services)
    }
  }
}

std::uint32_t StreamKind::PrimeFrames(const SourceAsset& a, std::uint64_t startFrame) const noexcept {
  if (a.preload != nullptr && startFrame < a.preloadFrames) return 0;
  // Head seek + first reads + decode: about two IO rounds of the target lead.
  const std::uint32_t rate = lim_.sampleRate == 0u ? 48000u : lim_.sampleRate;
  return rate / 10u + a.codec.pre_roll;
}

}  // namespace snd::source::detail
