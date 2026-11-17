// Drives the source kinds directly (no core): a Stream table, services whose
// postJob queues jobs for the test to run (or runs them at once), the IO
// scheduler over the memory FS (RunOnce), and SourceAssets made from a built
// bank the way core makes them (core/src/bank_registry.cpp). RenderBatch is
// called with source-rate items, so the output is the source frames
// themselves (no resampler): exact comparison with the codec.
#ifndef SND_SOURCE_TEST_KIND_DRIVER_HPP
#define SND_SOURCE_TEST_KIND_DRIVER_HPP

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "snd/bank/bank_view.hpp"
#include "snd/bank/validate.hpp"
#include "snd/base/allocator.hpp"
#include "snd/base/sparse_set.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/source/runtime.hpp"
#include "snd/test/rt_guard.hpp"
#include "source_harness.hpp"

namespace srctest {

namespace bk = snd::bank;
namespace bfmt = snd::bank_format;

class KindDriver {
 public:
  struct Opt {
    std::uint32_t maxStreams = 2;
    std::uint32_t maxDecoders = 4;
    std::uint32_t maxVoices = 16;
    std::uint32_t chunkBytes = 4096;
    std::uint64_t poolBytes = 256u << 10;
    std::uint32_t blockFrames = 256;
    std::uint64_t pcmCacheBytes = 2u << 20;
    bool manualJobs = false;  // true: jobs wait in `queue` until RunJobs()
    snd::IAllocator* allocator = nullptr;
  };

  explicit KindDriver(const Built& b, const Opt& o) : opt_(o) {
    alloc_ = o.allocator != nullptr ? o.allocator : &snd::base::DefaultAllocator();
    // Bank.
    bankBytes_ = b.bank.size();
    bank_.reset(static_cast<std::uint8_t*>(std::aligned_alloc(64, (bankBytes_ + 127) / 64 * 64)));
    std::memcpy(bank_.get(), b.bank.data(), bankBytes_);
    bk::ParseOptions po;
    po.platform = bfmt::kPlatformAny;
    bk::ParseError err;
    ok_ = bk::ParseBank(bank_.get(), bankBytes_, po, &view_, &err) == SND_OK;
    for (const auto& f : b.files) fs.AddFile(f.first, f.second);
    // IO.
    ok_ = ok_ && pool.Init(*alloc_, o.poolBytes, o.chunkBytes, o.maxStreams);
    snd::io::IoSchedulerConfig ic;
    ic.maxStreams = o.maxStreams;
    ic.ringChunks = 4;
    ic.bytesPerWindow = 0;
    ok_ = ok_ && io.Init(*alloc_, fs, pool, ic);
    // Stream table (Direct).
    snd::schema::StreamTable& st = tables_.Stream;
    rowMem_.resize(snd::base::rows::StorageBytes(o.maxStreams, snd::schema::Indexing::Direct) + 64);
    snd::base::rows::Bind(st.rows, o.maxStreams, snd::schema::Indexing::Direct, rowMem_.data());
    fileId_.resize(o.maxStreams);
    sync_.resize(o.maxStreams);
    control_.resize(o.maxStreams);
    range_.resize(o.maxStreams);
    stall_.resize(o.maxStreams);
    readPos_.reset(new std::atomic<std::uint64_t>[o.maxStreams]);
    ringWrite_.reset(new std::atomic<std::uint64_t>[o.maxStreams]);
    ringRead_.reset(new std::atomic<std::uint64_t>[o.maxStreams]);
    st.col.fileId = fileId_.data();
    st.col.syncGroup = sync_.data();
    st.col.control = control_.data();
    st.col.ringRange = range_.data();
    st.col.stallCount = stall_.data();
    st.col.readPos = readPos_.get();
    st.col.ringWrite = ringWrite_.get();
    st.col.ringRead = ringRead_.get();
    // Kinds.
    snd::source::RuntimeDesc rd;
    rd.allocator = alloc_;
    rd.io = &io;
    rd.streamPool = &pool;
    ok_ = ok_ && runtime.Init(rd);
    lim_.maxVoices = o.maxVoices;
    lim_.maxRealVoices = o.maxVoices;
    lim_.maxStreams = o.maxStreams;
    lim_.streamChunkBytes = o.chunkBytes;
    lim_.streamPoolBytes = o.poolBytes;
    lim_.maxDecoders = o.maxDecoders;
    lim_.decoderSlotBytes = 64u << 10;
    lim_.voiceRingFrames = 4096;
    lim_.callbackAheadBlocks = 4;
    lim_.pcmCacheBytes = o.pcmCacheBytes;
    lim_.pcmCachePageBytes = 64u << 10;
    lim_.blockFrames = o.blockFrames;
    lim_.sampleRate = 48000;
    lim_.streamTargetAheadMs = 200;
    services_.allocator = alloc_;
    services_.tables = &tables_;
    services_.postJob = &PostJobThunk;
    services_.jobCtx = this;
    for (snd::SourceKind k : {snd::SourceKind::Sample, snd::SourceKind::Stream}) {
      snd::source::ISourceKind* kind = runtime.Kinds().kinds[static_cast<std::size_t>(k)];
      if (!ok_ || kind == nullptr) continue;
      const std::size_t bytes = kind->PoolBytes(lim_);
      pools_.emplace_back(static_cast<std::uint8_t*>(std::aligned_alloc(64, (bytes + 127) / 64 * 64)), &std::free);
      ok_ = kind->Init(pools_.back().get(), bytes, lim_, services_);
      if (ok_) inited_.push_back(kind);
    }
    // Assets as core prepares them.
    if (ok_) {
      const std::uint32_t nA = view_.Rows(bfmt::chunk::ASST);
      assets_.resize(nA);
      for (std::uint32_t a = 0; a < nA; ++a) assets_[a] = MakeAsset(a);
    }
  }
  ~KindDriver() {
    for (snd::source::ISourceKind* k : inited_) {
      RunJobs();
      k->Shutdown();
    }
    io.Shutdown();
    runtime.Destroy();
    io.Destroy();
    pool.Destroy();
  }
  KindDriver(const KindDriver&) = delete;
  KindDriver& operator=(const KindDriver&) = delete;
  bool Ok() const { return ok_; }

  snd::source::ISourceKind& Sample() { return *runtime.Kinds().kinds[0]; }
  snd::source::ISourceKind& Stream() { return *runtime.Kinds().kinds[1]; }
  const snd::source::SourceAsset* Asset(const std::string& name) const {
    for (std::size_t i = 0; i < assets_.size(); ++i) {
      if (assets_[i].assetId == snd::HashId(name.c_str())) return &assets_[i];
    }
    return nullptr;
  }

  // Starts one voice; returns the source slot (kInvalidSlot on refusal; *detail).
  snd::SlotIndex Start(snd::source::ISourceKind& k, const snd::source::SourceAsset* a, std::uint64_t startFrame,
                       std::uint32_t flags, std::uint32_t* detail = nullptr, std::uint32_t* priming = nullptr) {
    snd::source::SourceStartRequest rq{};
    rq.voiceSlot = nextVoice_++;
    rq.startFrame = startFrame;
    rq.asset = a;
    rq.channels = a != nullptr ? a->codec.channels : 1u;
    rq.flags = flags;
    snd::source::SourceStartResult res{};
    {
      SND_RT_SCOPE();
      k.Start(&rq, 1, &res);
    }
    if (detail != nullptr) *detail = res.endDetail;
    if (priming != nullptr) *priming = res.needsPriming;
    return res.sourceSlot;
  }

  struct Voice {
    snd::SlotIndex slot = snd::kInvalidSlot;
    snd::Playhead ph{};
    std::uint32_t channels = 1;
    std::vector<std::vector<float>> out;  // all frames rendered so far, per channel
    std::uint32_t ended = 0, starved = 0;
    std::uint64_t silentBlocks = 0;
  };
  // One block for the voices of kind k: IO, background, jobs, render `frames` source frames each.
  void Block(snd::source::ISourceKind& k, std::vector<Voice*> voices, std::uint32_t frames) {
    for (int i = 0; i < 8 && io.RunOnce(); ++i) {
    }
    std::vector<std::vector<float>> buf;
    std::vector<snd::source::RenderItem> items;
    std::vector<std::vector<float*>> ptrs(voices.size());
    for (std::size_t v = 0; v < voices.size(); ++v) {
      Voice& vc = *voices[v];
      for (std::uint32_t c = 0; c < vc.channels; ++c) buf.emplace_back(frames, 0.0f);
    }
    std::size_t bi = 0;
    for (std::size_t v = 0; v < voices.size(); ++v) {
      Voice& vc = *voices[v];
      for (std::uint32_t c = 0; c < vc.channels; ++c) ptrs[v].push_back(buf[bi++].data());
      snd::source::RenderItem it{};
      it.voiceSlot = static_cast<snd::SlotIndex>(v);
      it.sourceSlot = vc.slot;
      it.realSlot = static_cast<snd::SlotIndex>(v);
      it.framesRequested = frames;
      it.channels = vc.channels;
      it.pitch = snd::Ramp{1.0f, 1.0f};
      it.playhead = &vc.ph;
      it.out = ptrs[v].data();
      items.push_back(it);
    }
    std::vector<snd::VoiceRenderStatus> st(voices.size());
    std::vector<std::uint8_t> scratch(1024);
    snd::source::Scratch sc{scratch.data(), scratch.size(), 0};
    snd::source::RenderBatch b{};
    b.kind = k.Kind();
    b.count = static_cast<std::uint32_t>(items.size());
    b.items = items.data();
    b.block = snd::source::BlockInfo{block_, block_ * opt_.blockFrames, opt_.blockFrames, 48000};
    b.scratch = &sc;
    {
      SND_RT_SCOPE();
      if (!items.empty()) k.RenderBatch(b, st.data());
      k.ScheduleBackground(b.block);
    }
    ++block_;
    if (!opt_.manualJobs) RunJobs();
    bi = 0;
    for (std::size_t v = 0; v < voices.size(); ++v) {
      Voice& vc = *voices[v];
      vc.out.resize(vc.channels);
      for (std::uint32_t c = 0; c < vc.channels; ++c) {
        const std::vector<float>& src = buf[bi++];
        vc.out[c].insert(vc.out[c].end(), src.begin(), src.begin() + st[v].framesRendered);
      }
      vc.ended |= st[v].ended;
      vc.starved += st[v].starved;
      if (st[v].framesRendered == 0u) ++vc.silentBlocks;
    }
  }
  void Control(snd::source::ISourceKind& k, snd::SlotIndex s, snd::source::ControlOp op, std::uint64_t f = 0,
               std::uint64_t f2 = 0) {
    snd::source::SourceControlOp c{};
    c.sourceSlot = s;
    c.op = op;
    c.frame = f;
    c.frame2 = f2;
    SND_RT_SCOPE();
    k.Control(&c, 1);
  }
  void Release(snd::source::ISourceKind& k, snd::SlotIndex s) {
    SND_RT_SCOPE();
    k.Release(&s, 1);
  }
  bool Ready(snd::source::ISourceKind& k, snd::SlotIndex s) {
    std::uint8_t r = 0;
    k.QueryReady(&s, 1, &r);
    return r != 0u;
  }
  std::size_t RunJobs() {
    std::size_t n = 0;
    while (!queue.empty()) {
      const snd::JobDesc j = queue.front();
      queue.pop_front();
      snd::RunJob(j);
      ++n;
    }
    return n;
  }

  snd::mock::MemoryFileSystem fs;
  snd::io::StreamPool pool;
  snd::io::IoScheduler io;
  snd::source::Runtime runtime;
  std::deque<snd::JobDesc> queue;

 private:
  static bool PostJobThunk(void* ctx, const snd::JobDesc& job, snd::JobLane) noexcept {
    KindDriver& d = *static_cast<KindDriver*>(ctx);
    snd::test::rt::ExpectViolations allow;  // the test's queue allocates; the real one (core) does not
    d.queue.push_back(job);
    return true;
  }
  snd::source::SourceAsset MakeAsset(std::uint32_t a) const {
    const bk::BankView& v = view_;
    snd::source::SourceAsset s{};
    s.codec.codec = v.Column<bk::col::ASST_codec>()[a];
    s.codec.channels = v.Column<bk::col::ASST_channels>()[a];
    s.codec.sample_rate = v.Column<bk::col::ASST_sampleRate>()[a];
    s.codec.frames_per_packet = v.Column<bk::col::ASST_framesPerPacket>()[a];
    s.codec.total_frames = v.Column<bk::col::ASST_totalFrames>()[a];
    s.codec.pre_skip = v.Column<bk::col::ASST_preSkip>()[a];
    s.codec.pre_roll = v.Column<bk::col::ASST_preRoll>()[a];
    s.codec.block_align = v.Column<bk::col::ASST_blockAlign>()[a];
    const bk::ByteSpan extra = v.AssetExtra(a);
    s.codec.extra = extra.data;
    s.codec.extra_bytes = static_cast<std::uint32_t>(extra.bytes);
    const std::uint32_t form = v.Column<bk::col::ASST_loadForm>()[a];
    s.form = static_cast<snd::source::LoadForm>(form);
    if (s.form != snd::source::LoadForm::Stream) {
      const bk::ByteSpan d = v.WaveData(a);
      s.data = d.data;
      s.dataBytes = d.bytes;
    }
    const bk::ByteSpan pre = v.Preload(a);
    s.preload = pre.data;
    s.preloadBytes = static_cast<std::uint32_t>(pre.bytes);
    s.preloadFrames = v.Column<bk::col::ASST_preloadFrames>()[a];
    const snd::RangeU32 seek = v.SeekRange(a);
    if (seek.count != 0u) {
      s.seekFrames = v.Column<bk::col::SEEK_frame>().data() + seek.first;
      s.seekOffsets = v.Column<bk::col::SEEK_byteOffset>().data() + seek.first;
      s.seekCount = seek.count;
    }
    const std::uint32_t strm = v.Column<bk::col::ASST_stream>()[a];
    s.streamFile = strm;
    if (strm != bfmt::kNone) {
      s.streamPath = v.StreamPath(strm);
      s.streamDataOffset = v.Column<bk::col::STRM_dataOffset>()[strm];
    }
    s.loopStart = v.Column<bk::col::ASST_loopStart>()[a];
    s.loopEnd = v.Column<bk::col::ASST_loopEnd>()[a];
    s.assetId = v.Column<bk::col::ASST_id>()[a];
    const std::uint32_t fl = v.Column<bk::col::ASST_flags>()[a];
    s.stallPolicy = (fl & bfmt::kAssetStallSoundPriority) != 0u ? 1u : 0u;
    s.virtualReturn = (fl & bfmt::kAssetVirtualKill) != 0u ? 2u : ((fl & bfmt::kAssetVirtualRestart) != 0u ? 1u : 0u);
    s.ioPriority = 0;
    s.bankSlot = 0;
    return s;
  }

  Opt opt_;
  snd::IAllocator* alloc_ = nullptr;
  bool ok_ = true;
  std::unique_ptr<std::uint8_t, void (*)(void*)> bank_{nullptr, &std::free};
  std::size_t bankBytes_ = 0;
  bk::BankView view_;
  std::vector<snd::source::SourceAsset> assets_;
  snd::schema::Tables tables_{};
  std::vector<std::uint8_t> rowMem_;
  std::vector<snd::FileId> fileId_;
  std::vector<std::uint32_t> sync_;
  std::vector<snd::StreamControl> control_;
  std::vector<snd::RangeU32> range_;
  std::vector<std::uint32_t> stall_;
  std::unique_ptr<std::atomic<std::uint64_t>[]> readPos_, ringWrite_, ringRead_;
  snd::source::SourceLimits lim_{};
  snd::source::SourceServices services_{};
  std::vector<std::unique_ptr<std::uint8_t, void (*)(void*)>> pools_;
  std::vector<snd::source::ISourceKind*> inited_;
  std::uint64_t block_ = 0;
  std::uint32_t nextVoice_ = 0;
};

}  // namespace srctest

#endif  // SND_SOURCE_TEST_KIND_DRIVER_HPP
