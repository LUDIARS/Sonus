// Live link, audio-thread side: core's hooks -> SPSC channels.
#include "snd/livelink/runtime.hpp"

#include <cstring>
#include <new>

#include "snd/core/core.hpp"
#include "snd/livelink/wire.hpp"

namespace snd::livelink {

namespace {

template <class P>
P Load(const void* p) noexcept {
  P v;
  std::memcpy(&v, p, sizeof(P));
  return v;
}

LiveRuntime* Self(void* u) noexcept { return static_cast<LiveRuntime*>(u); }
inline void Bump(std::atomic<std::uint32_t>& c) noexcept { c.fetch_add(1u, std::memory_order_relaxed); }

}  // namespace

int LiveRuntime::Create(const RuntimeDesc& desc, LiveRuntime** out) noexcept {
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  *out = nullptr;
  if (desc.config == nullptr || desc.allocator == nullptr) return SND_ERR_INVALID_ARGUMENT;
  void* mem = desc.allocator->Allocate(sizeof(LiveRuntime), alignof(LiveRuntime) < 16 ? 16 : alignof(LiveRuntime),
                                       MemTag::LiveLink);
  if (mem == nullptr) return SND_ERR_OUT_OF_BUDGET;
  LiveRuntime* rt = new (mem) LiveRuntime();
  if (!rt->Init(desc)) {
    IAllocator* a = desc.allocator;
    rt->~LiveRuntime();
    a->Free(mem, MemTag::LiveLink);
    return SND_ERR_OUT_OF_BUDGET;
  }
  *out = rt;
  return SND_OK;
}

void LiveRuntime::Destroy(LiveRuntime* rt) noexcept {
  if (rt == nullptr) return;
  IAllocator* a = rt->alloc_;
  rt->~LiveRuntime();
  a->Free(rt, MemTag::LiveLink);
}

LiveRuntime::~LiveRuntime() {
  if (alloc_ != nullptr) {
    alloc_->Free(knownBanks_, MemTag::LiveLink);
    alloc_->Free(scratchBanks_, MemTag::LiveLink);
    if (chunks_ != nullptr) {
      for (std::uint32_t i = 0; i < chunkCount_; ++i) chunks_[i].~PendingChunk();
      alloc_->Free(chunks_, MemTag::LiveLink);
    }
    alloc_->Free(chunkMemory_, MemTag::LiveLink);
    alloc_->Free(producerSession_, MemTag::LiveLink);
    alloc_->Free(revertOpen_, MemTag::LiveLink);
  }
}

bool LiveRuntime::Init(const RuntimeDesc& d) noexcept {
  alloc_ = d.allocator;
  config_ = *d.config;
  caps_ = CapsOf(config_);
  maxVoices_ = d.maxVoices != 0u ? d.maxVoices : (config_.max_virtual_voices != 0u ? config_.max_virtual_voices : 1u);
  maxMeters_ = d.maxMeters != 0u ? d.maxMeters : 256u;
  maxBanks_ = config_.max_banks != 0u ? config_.max_banks : 1u;
  const std::uint32_t bs = config_.block_size != 0u ? config_.block_size : 256u;
  blocksPerSecond_ = config_.sample_rate / bs != 0u ? config_.sample_rate / bs : 1u;
  IAllocator& a = *alloc_;
  producerCount_ = config_.max_registered_threads != 0u ? config_.max_registered_threads : 1u;
  producerSession_ = static_cast<std::uint32_t*>(a.Allocate(sizeof(std::uint32_t) * producerCount_, 64, MemTag::LiveLink));
  revertOpen_ = static_cast<std::uint8_t*>(a.Allocate(producerCount_, 64, MemTag::LiveLink));
  if (producerSession_ == nullptr || revertOpen_ == nullptr) return false;
  for (std::uint32_t i = 0; i < producerCount_; ++i) {
    producerSession_[i] = 0u;
    revertOpen_[i] = 0u;
  }
  chunkCount_ = d.chunkSlots;
  chunkBytes_ = (d.chunkBytes + 63u) & ~63u;
  if (chunkCount_ != 0u) {
    chunks_ = static_cast<PendingChunk*>(a.Allocate(sizeof(PendingChunk) * chunkCount_, 64, MemTag::LiveLink));
    chunkMemory_ = static_cast<std::uint8_t*>(a.Allocate(std::size_t{chunkBytes_} * chunkCount_, 64, MemTag::LiveLink));
    if (chunks_ == nullptr || chunkMemory_ == nullptr) return false;
    for (std::uint32_t i = 0; i < chunkCount_; ++i) {
      new (&chunks_[i]) PendingChunk();
      chunks_[i].data = chunkMemory_ + std::size_t{chunkBytes_} * i;
    }
  }
  knownBanks_ = static_cast<core::LiveBankInfo*>(a.Allocate(sizeof(core::LiveBankInfo) * maxBanks_, 64, MemTag::LiveLink));
  scratchBanks_ =
      static_cast<core::LiveBankInfo*>(a.Allocate(sizeof(core::LiveBankInfo) * maxBanks_, 64, MemTag::LiveLink));
  return knownBanks_ != nullptr && scratchBanks_ != nullptr && applier_.Init(a, d.maxEdits, maxBanks_) &&
         log_.Init(a, d.logEntries, MemTag::LiveLink) && timing_.Init(a, d.timingEntries, MemTag::LiveLink) &&
         stalls_.Init(a, d.stallEntries, MemTag::LiveLink) && lods_.Init(a, d.lodEntries, MemTag::LiveLink) &&
         results_.Init(a, d.resultEntries, MemTag::LiveLink) && records_.Init(a, d.recordRingBytes) &&
         voices_.Init(a, static_cast<std::uint32_t>(sizeof(VoiceSnapshotHeader) + sizeof(VoiceInfo) * maxVoices_)) &&
         meters_.Init(a, static_cast<std::uint32_t>(sizeof(VoiceSnapshotHeader) + sizeof(MeterEntry) * maxMeters_)) &&
         stats_.Init(a, sizeof(StatsSnapshot)) &&
         banks_.Init(a, static_cast<std::uint32_t>(sizeof(BankSnapshotHeader) + sizeof(core::LiveBankInfo) * maxBanks_));
}

void LiveRuntime::Install(core::CoreDesc* d) noexcept {
  d->liveUser = this;
  d->liveCommand = [](void* u, cmd::CommandType t, const void* p) noexcept {
    LiveRuntime* rt = Self(u);
    core::System* sys = rt->system();
    if (sys != nullptr) rt->OnCommand(*sys, t, p);
  };
  d->liveTap.user = this;
  d->liveTap.arena = [](void* u, const core::LiveArenaView& a) noexcept { Self(u)->OnArena(a); };
  d->liveTap.event = [](void* u, const notify::EventRecord& r) noexcept { Self(u)->OnEvent(r); };
  d->liveTap.ended = [](void* u, std::uint32_t s, std::uint32_t g, std::uint32_t r, std::uint32_t det) noexcept {
    Self(u)->OnEnded(s, g, r, det);
  };
  d->liveTap.stall = [](void* u, std::uint32_t s, Id ev, std::uint32_t n, BlockIndex b) noexcept {
    Self(u)->OnStall(s, ev, n, b);
  };
  d->liveTap.block = [](void* u, core::System& sys) noexcept { Self(u)->OnBlock(sys); };
  d->liveTap.admit = [](void* u, cmd::CommandType t) noexcept { return Self(u)->Admit(t); };
}

void LiveRuntime::SetProfiler(std::uint32_t mask, std::uint32_t meterHz) noexcept {
  if (meterHz == 0u) meterHz = config_.livelink.meter_hz != 0u ? config_.livelink.meter_hz : 30u;
  std::uint32_t every = (blocksPerSecond_ + meterHz / 2u) / meterHz;
  meterEvery_.store(every < 1u ? 1u : every, std::memory_order_relaxed);
  mask_.store(mask, std::memory_order_release);
}

PendingChunk* LiveRuntime::AcquireChunk() noexcept {
  for (std::uint32_t i = 0; i < chunkCount_; ++i) {
    std::uint32_t expected = PendingChunk::kFree;
    // Free -> Queued happens later (after the data is copied): reserve with Done here.
    if (chunks_[i].state.compare_exchange_strong(expected, PendingChunk::kDone, std::memory_order_acq_rel)) {
      return &chunks_[i];
    }
  }
  return nullptr;
}

void LiveRuntime::FinishChunk(PendingChunk* c) noexcept {
  if (c == nullptr) return;
  std::uint32_t expected = PendingChunk::kDone;
  c->state.compare_exchange_strong(expected, PendingChunk::kFree, std::memory_order_acq_rel);
}

void LiveRuntime::CancelAllChunks() noexcept {
  for (std::uint32_t i = 0; i < chunkCount_; ++i) {
    std::uint32_t q = PendingChunk::kQueued;
    if (!chunks_[i].state.compare_exchange_strong(q, PendingChunk::kCancelled, std::memory_order_acq_rel)) {
      std::uint32_t d = PendingChunk::kDone;
      chunks_[i].state.compare_exchange_strong(d, PendingChunk::kFree, std::memory_order_acq_rel);
    }
  }
}

RuntimeDrops LiveRuntime::Drops() const noexcept {
  RuntimeDrops d;
  d.log = dropLog_.load(std::memory_order_relaxed);
  d.timing = dropTiming_.load(std::memory_order_relaxed);
  d.stalls = dropStalls_.load(std::memory_order_relaxed);
  d.lod = dropLod_.load(std::memory_order_relaxed);
  d.results = dropResults_.load(std::memory_order_relaxed);
  d.records = dropRecords_.load(std::memory_order_relaxed);
  d.staleEdits = dropStale_.load(std::memory_order_relaxed);
  return d;
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------

void LiveRuntime::PushLog(std::uint32_t kind, std::uint32_t id, std::uint32_t handleSlot, std::uint32_t detail) noexcept {
  if (!log_.TryPush(LogEntry{clock_, kind, id, handleSlot, detail})) Bump(dropLog_);
}

void LiveRuntime::PushRecord(const void* rec, std::uint32_t bytes) noexcept {
  if (!records_.Push(rec, bytes)) {
    Bump(dropRecords_);
    ++recordDroppedSession_;
  }
}

bool LiveRuntime::StaleProducer() const noexcept {
  if (currentProducer_ >= producerCount_) return false;
  const std::uint32_t s = producerSession_[currentProducer_];
  return s != 0u && s < latestSession_.load(std::memory_order_acquire);
}

// D-87: an older session's LiveOverride / LiveClearOverrides is dropped, except the
// LiveClearOverrides of a disconnect revert (after kFieldRevertBegin of the same producer).
bool LiveRuntime::Admit(cmd::CommandType type) noexcept {
  if (!StaleProducer()) return true;
  if (type == cmd::CommandType::LiveClearOverrides && revertOpen_[currentProducer_] != 0u) return true;
  Bump(dropStale_);
  return false;
}

void LiveRuntime::OnCommand(core::System& sys, cmd::CommandType type, const void* payload) noexcept {
  const bool known = currentProducer_ < producerCount_;
  ApplyResult r{static_cast<std::uint32_t>(type), SND_OK, 0u, 0u, known ? producerSession_[currentProducer_] : 0u, 0u};
  if (type == cmd::CommandType::LiveSetValue) {
    const auto c = Load<cmd::LiveSetValue>(payload);
    if (c.field == kFieldSession && c.objectId == 0u) {  // the commands that follow belong to this session
      if (known) producerSession_[currentProducer_] = static_cast<std::uint32_t>(c.value);
      return;
    }
    if (c.field == kFieldRevertBegin && c.objectId == 0u) {
      if (known) revertOpen_[currentProducer_] = 1u;
      return;
    }
    // D-87: an earlier Server's edit is not applied (its result is dropped there); its
    // disconnect revert is (it only restores the cells that session wrote last).
    if (c.field != kFieldRevertAll && StaleProducer()) {
      Bump(dropStale_);
      r.result = SND_ERR_INVALID_HANDLE;
      if (!results_.TryPush(r)) Bump(dropResults_);
      return;
    }
    if (c.field == kFieldRevertAll && known) revertOpen_[currentProducer_] = 0u;
    const ApplyOutcome o = applier_.SetValue(sys, c, r.session);
    r.result = o.result;
    r.cells = o.cells;
    r.revert = c.field == kFieldRevertAll ? 1u : 0u;
  } else if (type == cmd::CommandType::LiveReplaceChunk) {
    const auto c = Load<cmd::LiveReplaceChunk>(payload);
    PendingChunk* p = static_cast<PendingChunk*>(const_cast<void*>(c.prepared));
    // Only slots of this runtime's pool are ever dereferenced.
    bool ours = false;
    for (std::uint32_t i = 0; i < chunkCount_ && !ours; ++i) ours = p == &chunks_[i];
    const std::uint32_t st = ours ? p->state.load(std::memory_order_acquire) : PendingChunk::kFree;
    if (ours && StaleProducer() && (st == PendingChunk::kQueued || st == PendingChunk::kCancelled)) {
      Bump(dropStale_);  // D-87: not applied; the slot goes back to the pool
      p->state.store(PendingChunk::kFree, std::memory_order_release);
      r.result = SND_ERR_INVALID_HANDLE;
    } else if (st == PendingChunk::kCancelled) {
      p->state.store(PendingChunk::kFree, std::memory_order_release);  // its Server is gone
      r.result = SND_ERR_INVALID_HANDLE;
    } else if (st != PendingChunk::kQueued) {
      r.result = SND_ERR_INVALID_ARGUMENT;
    } else {
      const ApplyOutcome o = applier_.ReplaceChunk(sys, *p, r.session);
      r.result = o.result;
      r.cells = o.cells;
      p->result = o.result;
      p->cells = o.cells;
      std::uint32_t q = PendingChunk::kQueued;
      if (!p->state.compare_exchange_strong(q, PendingChunk::kDone, std::memory_order_acq_rel)) {
        p->state.store(PendingChunk::kFree, std::memory_order_release);  // cancelled while applying
      }
    }
  } else {
    return;
  }
  if (!results_.TryPush(r)) Bump(dropResults_);
}

void LiveRuntime::OnArena(const core::LiveArenaView& a) noexcept {
  block_ = a.appliedBlock;
  currentProducer_ = a.producer;
  if (recording_.load(std::memory_order_relaxed) != 0u && !WriteArenaRecord(records_, a, caps_)) {
    Bump(dropRecords_);
    ++recordDroppedSession_;
  }
  if ((mask_.load(std::memory_order_relaxed) & kProfEventLog) == 0u) return;
  auto scan = [&](const cmd::CommandHeader& h, const std::uint8_t* p) {
    switch (static_cast<cmd::CommandType>(h.type)) {
      case cmd::CommandType::Play: {
        const auto c = Load<cmd::Play>(p);
        PushLog(kLogCommandBit | h.type, c.event, c.playback.slot, c.flags);
        break;
      }
      case cmd::CommandType::LiveAudition: {
        const auto c = Load<cmd::LiveAudition>(p);
        PushLog(kLogCommandBit | h.type, c.event, c.playback.slot, c.flags);
        break;
      }
      case cmd::CommandType::Stop: {
        const auto c = Load<cmd::Stop>(p);
        PushLog(kLogCommandBit | h.type, 0u, c.playback.slot, 0u);
        break;
      }
      default:
        break;
    }
  };
  cmd::ForEachCommand(a.normal, a.normal + a.normalBytes, scan);
  cmd::ForEachCommand(a.reserve, a.reserve + a.reserveBytes, scan);
}

void LiveRuntime::OnEvent(const notify::EventRecord& rec) noexcept {
  const std::uint32_t mask = mask_.load(std::memory_order_relaxed);
  if (rec.type == static_cast<std::uint16_t>(notify::EventType::LodChange)) {
    lod_ = rec.a;
    if ((mask & kProfLod) != 0u && !lods_.TryPush(LodChange{rec.a, rec.b, block_})) Bump(dropLod_);
    if (recording_.load(std::memory_order_relaxed) != 0u) {
      const RecordLod l{static_cast<std::uint32_t>(RecordKind::Lod), rec.a, block_};
      PushRecord(&l, sizeof(l));
    }
  }
  if ((mask & kProfEventLog) == 0u) return;
  const bool problem = rec.type == static_cast<std::uint16_t>(notify::EventType::Error) ||
                       rec.type == static_cast<std::uint16_t>(notify::EventType::Warning);
  PushLog(rec.type, rec.a, static_cast<std::uint32_t>(rec.object), problem ? rec.detail : rec.b);
}

void LiveRuntime::OnEnded(std::uint32_t slot, std::uint32_t, std::uint32_t reason, std::uint32_t detail) noexcept {
  if ((mask_.load(std::memory_order_relaxed) & kProfEventLog) == 0u) return;
  PushLog(kLogPlaybackEnded, 0u, slot, (reason << 16) | (detail & 0xFFFFu));
}

void LiveRuntime::OnStall(std::uint32_t slot, Id eventId, std::uint32_t count, BlockIndex block) noexcept {
  if ((mask_.load(std::memory_order_relaxed) & kProfStreams) != 0u &&
      !stalls_.TryPush(StreamStall{eventId, slot, clock_})) {
    Bump(dropStalls_);
  }
  if (recording_.load(std::memory_order_relaxed) != 0u) {
    const RecordStall s{static_cast<std::uint32_t>(RecordKind::Stall), slot, block};
    for (std::uint32_t i = 0; i < count && i < 64u; ++i) PushRecord(&s, sizeof(s));
  }
}

void LiveRuntime::UpdateBanks(core::System& sys) noexcept {
  std::uint32_t n = sys.LiveBanks(scratchBanks_, maxBanks_);
  if (n > maxBanks_) n = maxBanks_;
  bool changed = n != knownBankCount_;
  for (std::uint32_t i = 0; i < n && !changed; ++i) {
    changed = scratchBanks_[i].handle.slot != knownBanks_[i].handle.slot ||
              scratchBanks_[i].handle.generation != knownBanks_[i].handle.generation;
  }
  if (!changed) return;
  if (recording_.load(std::memory_order_relaxed) != 0u) {
    for (std::uint32_t i = 0; i < n; ++i) {
      bool known = false;
      for (std::uint32_t k = 0; k < knownBankCount_ && !known; ++k) {
        known = knownBanks_[k].handle.slot == scratchBanks_[i].handle.slot &&
                knownBanks_[k].handle.generation == scratchBanks_[i].handle.generation;
      }
      if (!known) {
        const RecordBankMap m{static_cast<std::uint32_t>(RecordKind::BankMap), scratchBanks_[i].bankId,
                              scratchBanks_[i].contentHash};
        PushRecord(&m, sizeof(m));
      }
    }
  }
  std::memcpy(static_cast<void*>(knownBanks_), scratchBanks_, sizeof(core::LiveBankInfo) * n);
  knownBankCount_ = n;
  std::uint8_t* b = banks_.Back();
  const BankSnapshotHeader h{n, 0u};
  std::memcpy(b, &h, sizeof(h));
  std::memcpy(b + sizeof(h), knownBanks_, sizeof(core::LiveBankInfo) * n);
  banks_.Publish();
}

void LiveRuntime::OnBlock(core::System& sys) noexcept {
  const core::BlockProfile& bp = sys.LastBlock();
  StatsSnapshot ss{};
  sys.LiveStats(&ss.stats);
  ss.block = bp.block;
  block_ = ss.stats.block_index;  // the next ApplyCommands
  clock_ = ss.stats.clock_sample;
  const std::uint32_t mask = mask_.load(std::memory_order_relaxed);
  if ((mask & kProfTiming) != 0u) {
    const BlockTiming t{bp.block,
                        static_cast<float>(static_cast<double>(bp.logicNs) / 1000.0),
                        static_cast<float>(static_cast<double>(bp.renderNs) / 1000.0),
                        ss.stats.block_budget_us,
                        ss.stats.lod_level,
                        bp.lateChunks,
                        ss.stats.underruns};
    if (!timing_.TryPush(t)) Bump(dropTiming_);
  }
  if (++blocksSinceMeters_ >= meterEvery_.load(std::memory_order_relaxed)) {
    blocksSinceMeters_ = 0;
    if ((mask & kProfVoices) != 0u) {
      std::uint8_t* b = voices_.Back();
      const std::uint32_t total = sys.LiveVoices(reinterpret_cast<VoiceInfo*>(b + sizeof(VoiceSnapshotHeader)), maxVoices_);
      const VoiceSnapshotHeader h{total < maxVoices_ ? total : maxVoices_, total, bp.block};
      std::memcpy(b, &h, sizeof(h));
      voices_.Publish();
    }
    if ((mask & kProfMeters) != 0u) {
      std::uint8_t* b = meters_.Back();
      const std::uint32_t total = sys.LiveMeters(reinterpret_cast<MeterEntry*>(b + sizeof(VoiceSnapshotHeader)), maxMeters_);
      const VoiceSnapshotHeader h{total < maxMeters_ ? total : maxMeters_, total, bp.block};
      std::memcpy(b, &h, sizeof(h));
      meters_.Publish();
    }
  }
  if (++blocksSinceStats_ >= 8u) {
    blocksSinceStats_ = 0;
    std::memcpy(stats_.Back(), &ss, sizeof(ss));
    stats_.Publish();
  }
  UpdateBanks(sys);
  // Recording start / end at this block boundary (11.3).
  const bool want = recordRequest_.load(std::memory_order_acquire) != 0u;
  const bool on = recording_.load(std::memory_order_relaxed) != 0u;
  if (want && !on) {
    recordDroppedSession_ = 0;
    const RecordLod l{static_cast<std::uint32_t>(RecordKind::Lod), lod_, block_};
    PushRecord(&l, sizeof(l));
    for (std::uint32_t i = 0; i < knownBankCount_; ++i) {
      const RecordBankMap m{static_cast<std::uint32_t>(RecordKind::BankMap), knownBanks_[i].bankId,
                            knownBanks_[i].contentHash};
      PushRecord(&m, sizeof(m));
    }
    recording_.store(1u, std::memory_order_release);
  } else if (!want && on) {
    const RecordEnd e{kRecordEndMarker, recordDroppedSession_};
    if (records_.Push(&e, sizeof(e))) recording_.store(0u, std::memory_order_release);
    // else: retried at the next block (the end marker is never lost)
  }
}

}  // namespace snd::livelink
