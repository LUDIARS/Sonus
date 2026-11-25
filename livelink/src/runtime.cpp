// Live link, audio-thread side: core's hooks -> SPSC channels.
#include "snd/livelink/runtime.hpp"

#include <cstring>

#include "prepare_effect.hpp"
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
    if (pluginPool_) {
      pluginPool_->~LivePluginPool();
      alloc_->Free(pluginPool_, MemTag::LiveLink);
    }
    alloc_->Free(knownBanks_, MemTag::LiveLink);
    alloc_->Free(scratchBanks_, MemTag::LiveLink);
    if (chunks_ != nullptr) {
      for (std::uint32_t i = 0; i < chunkCount_; ++i) chunks_[i].~PendingChunk();
      alloc_->Free(chunks_, MemTag::LiveLink);
    }
    alloc_->Free(rejectedStructs_, MemTag::LiveLink);
    alloc_->Free(chunkMemory_, MemTag::LiveLink);
    alloc_->Free(producerSession_, MemTag::LiveLink);
    alloc_->Free(revertOpen_, MemTag::LiveLink);
    alloc_->Free(auditions_, MemTag::LiveLink);
    alloc_->Free(structLog_, MemTag::LiveLink);
    alloc_->Free(streamStalls_, MemTag::LiveLink);
    alloc_->Free(editScratch_, MemTag::LiveLink);
  }
}

bool LiveRuntime::Init(const RuntimeDesc& d) noexcept {
  alloc_ = d.allocator;
  config_ = *d.config;
  if (config_.plugins.effect_count) {
    auto* mem = alloc_->Allocate(sizeof(core::LivePluginPool), 64, MemTag::LiveLink);
    if (!mem) return false;
    pluginPool_ = new (mem) core::LivePluginPool();
  }
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
    rejectedStructs_ = static_cast<const void**>(a.Allocate(sizeof(void*) * chunkCount_, 64, MemTag::LiveLink));
    if (!rejectedStructs_) return false;
    chunkMemory_ = static_cast<std::uint8_t*>(a.Allocate(std::size_t{chunkBytes_} * chunkCount_, 64, MemTag::LiveLink));
    if (!chunkMemory_) return false;
    chunks_ = static_cast<PendingChunk*>(a.Allocate(sizeof(PendingChunk) * chunkCount_, 64, MemTag::LiveLink));
    if (!chunks_) return false;
    for (std::uint32_t i = 0; i < chunkCount_; ++i) {
      new (&chunks_[i]) PendingChunk();
      chunks_[i].data = chunkMemory_ + std::size_t{chunkBytes_} * i;
    }
  }
  // 1.1
  maxAuditions_ = d.maxAuditions != 0u ? d.maxAuditions : 1u;
  auditions_ = static_cast<std::uint64_t*>(a.Allocate(sizeof(std::uint64_t) * maxAuditions_, 64, MemTag::LiveLink));
  structEditor_ = d.structEditor;
  structLogCap_ = d.maxStructLog != 0u ? d.maxStructLog : 1u;
  structLog_ = static_cast<StructLogEntry*>(a.Allocate(sizeof(StructLogEntry) * structLogCap_, 64, MemTag::LiveLink));
  maxStreams_ = config_.max_streams;
  streamStalls_ = static_cast<std::uint32_t*>(
      a.Allocate(sizeof(std::uint32_t) * (maxStreams_ != 0u ? maxStreams_ : 1u), 64, MemTag::LiveLink));
  editScratchCap_ = d.maxEdits != 0u ? d.maxEdits : 1u;
  editScratch_ = static_cast<EditView*>(a.Allocate(sizeof(EditView) * editScratchCap_, 64, MemTag::LiveLink));
  if (auditions_ == nullptr || structLog_ == nullptr || streamStalls_ == nullptr || editScratch_ == nullptr) return false;
  std::memset(auditions_, 0, sizeof(std::uint64_t) * maxAuditions_);
  std::memset(static_cast<void*>(structLog_), 0, sizeof(StructLogEntry) * structLogCap_);
  std::memset(streamStalls_, 0, sizeof(std::uint32_t) * (maxStreams_ != 0u ? maxStreams_ : 1u));
  if (!readings_.Init(a, d.resultEntries, MemTag::LiveLink) ||
      !auditionStops_.Init(a, maxAuditions_ * 4u, MemTag::LiveLink) ||
      !details_.Init(a, d.detailEntries != 0u ? d.detailEntries : 1u, MemTag::LiveLink) ||
      !streams_.Init(a, static_cast<std::uint32_t>(sizeof(StreamSnapshotHeader) +
                                                   sizeof(StreamInfo) * (maxStreams_ != 0u ? maxStreams_ : 1u)))) {
    return false;
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
  d->liveTap.arenaApplied = [](void* u, const core::LiveArenaView& a) noexcept {
    auto* rt = Self(u);
    if (rt->recording_.load(std::memory_order_relaxed) && !WriteArenaRecord(rt->records_, a, rt->caps_, rt->rejectedStructs_, rt->rejectedStructCount_)) {
      Bump(rt->dropRecords_);
      ++rt->recordDroppedSession_;
    }
  };
  d->liveTap.arena = [](void* u, const core::LiveArenaView& a) noexcept { Self(u)->OnArena(a); };
  d->liveTap.event = [](void* u, const notify::EventRecord& r) noexcept { Self(u)->OnEvent(r); };
  d->liveTap.ended = [](void* u, std::uint32_t s, std::uint32_t g, std::uint32_t r, std::uint32_t det) noexcept {
    Self(u)->OnEnded(s, g, r, det);
  };
  d->liveTap.stall = [](void* u, std::uint32_t s, Id ev, std::uint32_t n, BlockIndex b) noexcept {
    Self(u)->OnStall(s, ev, n, b);
  };
  d->liveTap.block = [](void* u, core::System& sys) noexcept { Self(u)->OnBlock(sys); };
  d->liveTap.admit = [](void* u, cmd::CommandType t, const void* p) noexcept { return Self(u)->Admit(t, p); };
}

void LiveRuntime::SetProfiler(std::uint32_t mask, std::uint32_t meterHz) noexcept {
  if (meterHz == 0u) meterHz = config_.livelink.meter_hz != 0u ? config_.livelink.meter_hz : 30u;
  std::uint32_t every = (blocksPerSecond_ + meterHz / 2u) / meterHz;
  meterEvery_.store(every < 1u ? 1u : every, std::memory_order_relaxed);
  mask_.store(mask, std::memory_order_release);
}

snd_result LiveRuntime::PreparePlugin(PendingChunk& p) noexcept {
  if (!pluginPool_ || structEditor_ != &CoreStructEditor()) return SND_OK;
  auto* sys = system();
  return sys ? PrepareExternalEffect(*pluginPool_, *alloc_, config_, sys, p, config_.dev_reserve.bytes) : SND_ERR_NOT_READY;
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
  d.readings = dropReadings_.load(std::memory_order_relaxed);
  d.auditions = dropAuditions_.load(std::memory_order_relaxed);
  d.details = dropDetails_.load(std::memory_order_relaxed);
  d.structLog = dropStructLog_.load(std::memory_order_relaxed);
  d.structUndo = dropStructUndo_.load(std::memory_order_relaxed);
  return d;
}

// --- 1.1: auditions and the dev reserve (live link thread side) -----------------------------
bool LiveRuntime::ReserveAudition() noexcept {
  std::uint32_t v = auditionsActive_.load(std::memory_order_relaxed);
  do {
    if (v >= maxAuditions_) return false;
  } while (!auditionsActive_.compare_exchange_weak(v, v + 1u, std::memory_order_acq_rel));
  return true;
}

void LiveRuntime::ReleaseAudition() noexcept {
  std::uint32_t v = auditionsActive_.load(std::memory_order_relaxed);
  do {
    if (v == 0u) return;
  } while (!auditionsActive_.compare_exchange_weak(v, v - 1u, std::memory_order_acq_rel));
}

StructCost StructCostOf(StructOp op, std::uint32_t argCount) noexcept {
  StructCost c;
  switch (op) {
    case StructOp::AddBus: c.buses = 1u; break;
    case StructOp::AddEffect: c.effects = 1u; break;
    case StructOp::AddSend: break;
    case StructOp::RemoveBus:
    case StructOp::RemoveEffect:
      return c;  // removes take nothing
  }
  c.bytes = kStructEditBaseBytes + std::uint64_t{argCount} * 8u;
  return c;
}

// One reserving thread (the live link thread); the audio thread only gives back, so a
// check-then-add cannot oversubscribe.
snd_result LiveRuntime::ReserveStruct(const StructCost& c) noexcept {
  const snd_dev_reserve_config& r = config_.dev_reserve;
  if (usedBuses_.load(std::memory_order_acquire) + c.buses > r.buses ||
      usedEffects_.load(std::memory_order_acquire) + c.effects > r.effects ||
      usedBytes_.load(std::memory_order_acquire) + c.bytes > r.bytes) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  usedBuses_.fetch_add(c.buses, std::memory_order_acq_rel);
  usedEffects_.fetch_add(c.effects, std::memory_order_acq_rel);
  usedBytes_.fetch_add(c.bytes, std::memory_order_acq_rel);
  return SND_OK;
}

void LiveRuntime::ReleaseStruct(const StructCost& c) noexcept {
  usedBuses_.fetch_sub(c.buses, std::memory_order_acq_rel);
  usedEffects_.fetch_sub(c.effects, std::memory_order_acq_rel);
  usedBytes_.fetch_sub(c.bytes, std::memory_order_acq_rel);
}

bool LiveRuntime::ReserveStructLog() noexcept {
  std::uint32_t v = structLogTaken_.load(std::memory_order_relaxed);
  do {
    if (v >= structLogCap_) return false;
  } while (!structLogTaken_.compare_exchange_weak(v, v + 1u, std::memory_order_acq_rel));
  return true;
}

void LiveRuntime::ReleaseStructLog(std::uint32_t n) noexcept {
  std::uint32_t v = structLogTaken_.load(std::memory_order_relaxed);
  do {
    if (v < n) {
      Bump(dropStructLog_);  // more given back than taken: a bug
      n = v;
    }
  } while (!structLogTaken_.compare_exchange_weak(v, v - n, std::memory_order_acq_rel));
}

DevReserveUsage LiveRuntime::ReserveUsage() const noexcept {
  DevReserveUsage u;
  u.buses = usedBuses_.load(std::memory_order_acquire);
  u.effects = usedEffects_.load(std::memory_order_acquire);
  u.bytes = usedBytes_.load(std::memory_order_acquire);
  u.busesLimit = config_.dev_reserve.buses;
  u.effectsLimit = config_.dev_reserve.effects;
  u.bytesLimit = config_.dev_reserve.bytes;
  return u;
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
// LiveClearOverrides of a disconnect revert (the first one after kFieldRevertBegin of the
// same producer): that one removes only the overrides its session (or an older one) set
// last; the newest session's ClearOverrides clears everything (core).
bool LiveRuntime::Admit(cmd::CommandType type, const void* payload) noexcept {
  const bool known = currentProducer_ < producerCount_;
  const std::uint32_t session = known ? producerSession_[currentProducer_] : 0u;
  const bool revert = type == cmd::CommandType::LiveClearOverrides && known && revertOpen_[currentProducer_] != 0u;
  if (revert) revertOpen_[currentProducer_] = 0u;  // one ClearOverrides per revert
  if (!StaleProducer()) {
    if (type == cmd::CommandType::LiveClearOverrides) {
      for (OverrideOwner& o : overrideOwners_) o = OverrideOwner{};
    } else if (type == cmd::CommandType::LiveOverride) {
      const auto c = Load<cmd::LiveOverride>(payload);
      OverrideOwner* slot = nullptr;
      for (OverrideOwner& o : overrideOwners_) {
        if (o.used != 0u && o.kind == c.kind && o.id == c.id) slot = &o;
      }
      if (c.enable == 0u) {
        if (slot != nullptr) *slot = OverrideOwner{};
      } else {
        for (std::uint32_t i = 0; i < kMaxOverrideOwners && slot == nullptr; ++i) {
          if (overrideOwners_[i].used == 0u) slot = &overrideOwners_[i];
        }
        if (slot != nullptr) *slot = OverrideOwner{c.kind, c.id, session, 1u, c.value, 0u, c.target.Value()};
      }
    }
    return true;
  }
  if (revert) {  // an older session's revert: its own overrides only
    core::System* sys = system();
    for (OverrideOwner& o : overrideOwners_) {
      if (o.used == 0u || o.session > session) continue;
      if (sys != nullptr) sys->LiveRemoveOverride(o.kind, o.id);
      o = OverrideOwner{};
    }
    return false;  // not core's clear-all
  }
  Bump(dropStale_);
  return false;
}

void LiveRuntime::OnCommand(core::System& sys, cmd::CommandType type, const void* payload) noexcept {
  const bool known = currentProducer_ < producerCount_;
  ApplyResult r{static_cast<std::uint32_t>(type), SND_OK, 0u, 0u, known ? producerSession_[currentProducer_] : 0u, 0u};
  if (type == cmd::CommandType::LiveSetValue) {
    const auto c = Load<cmd::LiveSetValue>(payload);
    if (c.field == kFieldSession && c.objectId == 0u) {  // the commands that follow belong to this session
      if (known) {
        producerSession_[currentProducer_] = static_cast<std::uint32_t>(c.value);
        revertOpen_[currentProducer_] = 0u;  // a revert left unfinished never carries over
      }
      return;
    }
    if (c.field == kFieldRevertBegin && c.objectId == 0u) {
      if (known) revertOpen_[currentProducer_] = 1u;
      return;
    }
    if (c.field == kFieldAuditionStop && c.objectId == 0u) {  // 1.1: the stops went out in ScanArena
      if (!results_.TryPush(r)) Bump(dropResults_);
      return;
    }
    if (c.valueType == kValueTypeReadCell) {  // 1.1: a read changes nothing; a stale one is not answered
      if (StaleProducer()) {
        r.result = SND_ERR_INVALID_HANDLE;
      } else {
        CellReading rd;
        applier_.ReadCell(sys, c, &rd);
        rd.session = r.session;
        if (!readings_.TryPush(rd)) Bump(dropReadings_);
        r.result = rd.result;
        r.cells = rd.cells;
      }
      if (!results_.TryPush(r)) Bump(dropResults_);
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
    if (c.field == kFieldRevertAll) UndoStructs(sys, r.session);  // 1.1: newest first
  } else if (type == cmd::CommandType::LiveReplaceChunk) {
    const auto c = Load<cmd::LiveReplaceChunk>(payload);
    PendingChunk* p = static_cast<PendingChunk*>(const_cast<void*>(c.prepared));
    // Only slots of this runtime's pool are ever dereferenced.
    bool ours = false;
    for (std::uint32_t i = 0; i < chunkCount_ && !ours; ++i) ours = p == &chunks_[i];
    const std::uint32_t st = ours ? p->state.load(std::memory_order_acquire) : PendingChunk::kFree;
    // 1.1: a structural edit that is not applied gives its dev reserve back.
    const bool pending = st == PendingChunk::kQueued || st == PendingChunk::kCancelled;
    const bool structEdit = pending && p->fourcc == kStructEditTag;
    const StructCost cost = structEdit ? StructCost{p->costBuses, p->costEffects, p->costBytes} : StructCost{};
    const std::uint32_t logPlace = structEdit ? p->structLogPlace : 0u;
    if (ours && StaleProducer() && (st == PendingChunk::kQueued || st == PendingChunk::kCancelled)) {
      Bump(dropStale_);  // D-87: not applied; the slot goes back to the pool
      if (structEdit) { CancelPreparedEffect(*p); ReleaseStruct(cost); }
      if (logPlace != 0u) ReleaseStructLog();
      p->state.store(PendingChunk::kFree, std::memory_order_release);
      r.result = SND_ERR_INVALID_HANDLE;
    } else if (st == PendingChunk::kCancelled) {
      if (structEdit) { CancelPreparedEffect(*p); ReleaseStruct(cost); }
      if (logPlace != 0u) ReleaseStructLog();
      p->state.store(PendingChunk::kFree, std::memory_order_release);  // its Server is gone
      r.result = SND_ERR_INVALID_HANDLE;
    } else if (st != PendingChunk::kQueued) {
      r.result = SND_ERR_INVALID_ARGUMENT;
    } else {
      ApplyOutcome o;
      if (structEdit) {
        ApplyStruct(sys, *p, r.session, &r);
        if (r.result != SND_OK) CancelPreparedEffect(*p);
        else p->prepared = nullptr;  // the mixer/tail now owns this lease
        o.result = r.result;
        o.cells = r.cells;
      } else {
        o = applier_.ReplaceChunk(sys, *p, r.session);
      }
      r.result = o.result;
      r.cells = o.cells;
      if (recording_.load(std::memory_order_relaxed) && (!structEdit || o.result == SND_OK)) WriteLiveData(c);
      p->result = o.result;
      p->cells = o.cells;
      std::uint32_t q = PendingChunk::kQueued;
      if (!p->state.compare_exchange_strong(q, PendingChunk::kDone, std::memory_order_acq_rel)) {
        p->state.store(PendingChunk::kFree, std::memory_order_release);  // cancelled while applying
      }
    }
    if (structEdit && r.result != SND_OK && recording_.load(std::memory_order_relaxed) &&
        rejectedStructCount_ < chunkCount_) rejectedStructs_[rejectedStructCount_++] = payload;
  } else {
    return;
  }
  if (!results_.TryPush(r)) Bump(dropResults_);
}

void LiveRuntime::OnArena(const core::LiveArenaView& a) noexcept {
  rejectedStructCount_ = 0;
  block_ = a.appliedBlock;
  currentProducer_ = a.producer;
  ScanArena(a);  // audition tracking before ApplyCommands
  if (a.normalBytes != 0u || a.reserveBytes != 0u || a.fallback != nullptr) ++arenasSeen_;

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

void LiveRuntime::OnEnded(std::uint32_t slot, std::uint32_t generation, std::uint32_t reason, std::uint32_t detail) noexcept {
  const std::uint64_t v = HandleRef{slot, generation}.Value();
  for (std::uint32_t i = 0; i < maxAuditions_; ++i) {  // 1.1: a tracked audition ended
    if (auditions_[i] == v) {
      auditions_[i] = 0u;
      ReleaseAudition();
      break;
    }
  }
  if ((mask_.load(std::memory_order_relaxed) & kProfEventLog) == 0u) return;
  PushLog(kLogPlaybackEnded, 0u, slot, (reason << 16) | (detail & 0xFFFFu));
}

void LiveRuntime::OnStall(std::uint32_t slot, Id eventId, std::uint32_t count, BlockIndex block) noexcept {
  if (slot < maxStreams_) streamStalls_[slot] += count;
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
  // A reload invalidates entries of the old bank generation, including the reservations.
  std::uint32_t kept = 0;
  for (std::uint32_t j = 0; j < structLogCount_; ++j) {
    const auto& e = structLog_[j];
    auto present = [&](std::uint64_t handle) {
      if (!handle) return true;
      for (std::uint32_t i = 0; i < n; ++i) if (scratchBanks_[i].handle.Value() == handle) return true;
      return false;
    };
    if (present(e.bankHandle) && present(e.targetBankHandle)) structLog_[kept++] = e;
    else ReleaseStruct(StructCost{e.buses, e.effects, e.bytes});
  }
  if (kept < structLogCount_) ReleaseStructLog(structLogCount_ - kept);
  structLogCount_ = kept;
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
  if ((mask & kProfBlockDetail) != 0u) {  // 1.1
    const BlockDetail bd{bp.block,
                         static_cast<float>(static_cast<double>(bp.totalNs) / 1000.0),
                         static_cast<float>(static_cast<double>(bp.logicNs) / 1000.0),
                         static_cast<float>(static_cast<double>(bp.renderNs) / 1000.0),
                         ss.stats.block_budget_us,
                         bp.renderedVoices,
                         bp.chunks,
                         bp.lateChunks,
                         bp.heldVoices,
                         bp.parallel,
                         ss.stats.lod_level};
    if (!details_.TryPush(bd)) Bump(dropDetails_);
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
    if ((mask & kProfStreamList) != 0u) PublishStreams(sys, bp.block);  // 1.1
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
    RecordStartState(sys);  // 1.1: after the first Lod and the BankMap records
    recording_.store(1u, std::memory_order_release);
  } else if (!want && on) {
    const RecordEnd e{kRecordEndMarker, recordDroppedSession_};
    if (records_.Push(&e, sizeof(e))) recording_.store(0u, std::memory_order_release);
    // else: retried at the next block (the end marker is never lost)
  }
}

// ---------------------------------------------------------------------------
// 1.1 (audio thread)
// ---------------------------------------------------------------------------
void LiveRuntime::TrackAudition(HandleRef h) noexcept {
  for (std::uint32_t i = 0; i < maxAuditions_; ++i) {
    if (auditions_[i] == 0u) {
      auditions_[i] = h.Value();
      return;
    }
  }
  Bump(dropAuditions_);  // not stoppable by StopAll (the place reservation should prevent it)
}

void LiveRuntime::WriteLiveData(const cmd::LiveReplaceChunk& c) noexcept {
  PendingChunk* p = static_cast<PendingChunk*>(const_cast<void*>(c.prepared));
  bool ours = false;
  for (std::uint32_t i = 0; i < chunkCount_ && !ours; ++i) ours = p == &chunks_[i];
  if (!ours || p->bytes > chunkBytes_) return;
  const std::uint32_t bytes = static_cast<std::uint32_t>(p->bytes);
  const std::uint32_t padded = (bytes + 7u) & ~7u;
  const RecordLiveData h{kRecordLiveData, p->fourcc, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)),
                         p->bankId, bytes};
  if (!records_.Begin(static_cast<std::uint32_t>(sizeof(h)) + padded)) {
    Bump(dropRecords_);
    ++recordDroppedSession_;
    return;
  }
  records_.Put(&h, sizeof(h));
  if (bytes != 0u) records_.Put(p->data, bytes);
  const std::uint64_t zero = 0;
  if (padded != bytes) records_.Put(&zero, padded - bytes);
  records_.Commit();
}

void LiveRuntime::ScanArena(const core::LiveArenaView& a) noexcept {
  auto scan = [&](const cmd::CommandHeader& h, const std::uint8_t* p) {
    switch (static_cast<cmd::CommandType>(h.type)) {
      case cmd::CommandType::LiveAudition:
        TrackAudition(Load<cmd::LiveAudition>(p).playback);
        break;
      case cmd::CommandType::LiveSetValue: {
        const auto c = Load<cmd::LiveSetValue>(p);
        if (c.objectId != 0u || c.field != kFieldAuditionStop) break;
        for (std::uint32_t i = 0; i < maxAuditions_; ++i) {  // the auditions started before the marker
          if (auditions_[i] != 0u && !auditionStops_.TryPush(auditions_[i])) Bump(dropAuditions_);
        }
        break;
      }
      default:
        break;
    }
  };
  cmd::ForEachCommand(a.normal, a.normal + a.normalBytes, scan);
  cmd::ForEachCommand(a.reserve, a.reserve + a.reserveBytes, scan);
}

void LiveRuntime::ApplyStruct(core::System& sys, PendingChunk& p, std::uint32_t session, ApplyResult* r) noexcept {
  const StructCost cost{p.costBuses, p.costEffects, p.costBytes};
  std::uint32_t logPlace = p.structLogPlace;
  StructEdit h{};
  if (p.bytes < sizeof(h)) {
    ReleaseStruct(cost);
    if (logPlace != 0u) ReleaseStructLog();
    r->result = SND_ERR_INVALID_ARGUMENT;
    return;
  }
  std::memcpy(&h, p.data, sizeof(h));
  const std::uint64_t* args = reinterpret_cast<const std::uint64_t*>(p.data + sizeof(StructEdit));
  const StructEditRequest q{static_cast<StructOp>(h.op), h.targetId, h.argCount, session, args, ++nextStructToken_, p.prepared};
  const bool removing = q.op == StructOp::RemoveBus || q.op == StructOp::RemoveEffect;
  const auto origin = removing && structEditor_ ? structEditor_->RemovalOrigin(sys, q) : ~std::uint64_t{0};
  auto sameObject = [&](const StructLogEntry& e, StructOp op) {
    return e.op == static_cast<std::uint32_t>(op) && e.targetId == q.targetId &&
           (origin == ~std::uint64_t{0} || origin == e.token);
  };
  // D-172: a remove accepted without a log place (the log was full) is applied only when it
  // cancels a live add (no place needed); otherwise it needs a place now, or it is refused
  // without asking the editor.
  if (logPlace == 0u && (q.op == StructOp::RemoveBus || q.op == StructOp::RemoveEffect)) {
    const auto addOp = q.op == StructOp::RemoveBus ? StructOp::AddBus : StructOp::AddEffect;
    bool cancels = false;
    for (std::uint32_t i = 0; i < structLogCount_ && !cancels; ++i) {
      cancels = sameObject(structLog_[i], addOp);
    }
    if (!cancels) {
      if (!ReserveStructLog()) {
        ReleaseStruct(cost);
        r->result = SND_ERR_LIMIT_REACHED;
        r->cells = 0u;
        return;
      }
      logPlace = 1u;
    }
  }
  const auto owner = structEditor_ ? structEditor_->OwnerBank(sys, q) : 0;
  const snd_result res = structEditor_ != nullptr ? structEditor_->Apply(sys, q) : SND_ERR_UNSUPPORTED;
  r->result = res;
  r->cells = res == SND_OK ? 1u : 0u;
  if (res != SND_OK) {
    ReleaseStruct(cost);
    if (logPlace != 0u) ReleaseStructLog();
    return;
  }
  // A remove of what a live edit added cancels both (the whole cost of the add comes back,
  // nothing to undo, D-172). Removing a live bus also takes away the live effects on it and
  // the live sends from it (they go with the bus: their cost comes back, no undo is asked).
  const bool removeBus = q.op == StructOp::RemoveBus, removeFx = q.op == StructOp::RemoveEffect;
  if (removeBus || removeFx) {
    std::uint32_t found = ~0u;
    for (std::uint32_t i = structLogCount_; i-- > 0u && found == ~0u;) {
      const StructLogEntry& e = structLog_[i];
      const bool match = sameObject(e, removeBus ? StructOp::AddBus : StructOp::AddEffect);
      if (match) found = i;
    }
    if (found != ~0u) {
      std::uint32_t freed = 0;
      for (std::uint32_t i = 0; i < structLogCount_; ++i) {
        StructLogEntry& e = structLog_[i];
        const bool onBus = removeBus && i > found && e.targetId == q.targetId &&
                           (e.op == static_cast<std::uint32_t>(StructOp::AddEffect) ||
                            e.op == static_cast<std::uint32_t>(StructOp::AddSend));
        if (i != found && !onBus) continue;
        ReleaseStruct(StructCost{e.buses, e.effects, e.bytes});
        e.used = 0u;
        ++freed;
      }
      std::uint32_t kept = 0;
      for (std::uint32_t i = 0; i < structLogCount_; ++i) {
        if (structLog_[i].used != 0u) structLog_[kept++] = structLog_[i];
      }
      structLogCount_ = kept;
      ReleaseStructLog(freed + (logPlace != 0u ? 1u : 0u));  // the entries and the remove's own place
      return;
    }
  }
  if (logPlace == 0u || structLogCount_ >= structLogCap_) {
    Bump(dropStructLog_);  // cannot happen: a place was reserved at acceptance (D-172)
    return;
  }
  StructLogEntry& e = structLog_[structLogCount_++];
  e = StructLogEntry{};
  e.used = 1u;
  e.bankHandle = owner;
  e.targetBankHandle = owner && q.op == StructOp::AddSend ? sys.LiveBusBank(static_cast<Id>(args[0])) : 0;
  e.token = q.token;
  e.op = h.op;
  e.targetId = h.targetId;
  e.argCount = h.argCount;
  e.session = session;
  e.producer = currentProducer_;
  e.buses = cost.buses;
  e.effects = cost.effects;
  e.bytes = cost.bytes;
  for (std::uint32_t k = 0; k < h.argCount && k < 4u; ++k) e.args[k] = args[k];
}

void LiveRuntime::UndoStructs(core::System& sys, std::uint32_t session) noexcept {
  std::uint32_t kept = 0;
  // Newest first; the kept ones (a newer session's) stay in order.
  for (std::uint32_t i = structLogCount_; i-- > 0u;) {
    StructLogEntry& e = structLog_[i];
    if (session != 0u && e.session > session) continue;
    const std::uint32_t logged = e.argCount < 4u ? e.argCount : 4u;
    const StructEditRequest q{static_cast<StructOp>(e.op), e.targetId, logged, e.session, e.args, e.token};
    const snd_result res = structEditor_ != nullptr ? structEditor_->Undo(sys, q) : SND_ERR_UNSUPPORTED;
    if (res != SND_OK) {
      Bump(dropStructUndo_);
      continue;  // a newer session can still depend on this object; retry on its disconnect
    }
    ReleaseStruct(StructCost{e.buses, e.effects, e.bytes});
    e.used = 0u;
  }
  for (std::uint32_t i = 0; i < structLogCount_; ++i) {
    if (structLog_[i].used != 0u) structLog_[kept++] = structLog_[i];
  }
  if (structLogCount_ > kept) ReleaseStructLog(structLogCount_ - kept);
  structLogCount_ = kept;
}

void LiveRuntime::PublishStreams(core::System& sys, std::uint64_t block) noexcept {
  std::uint8_t* b = streams_.Back();
  const StreamSnapshotHeader h{maxStreams_, 0u, block};
  std::memcpy(b, &h, sizeof(h));
  for (std::uint32_t s = 0; s < maxStreams_; ++s) {
    const Id ev = sys.LiveStreamEvent(s);
    const StreamInfo si{s, ev, static_cast<std::uint32_t>(ev != 0u ? StreamState::Active : StreamState::Idle),
                        streamStalls_[s]};
    std::memcpy(b + sizeof(h) + std::size_t{s} * sizeof(StreamInfo), &si, sizeof(si));
  }
  streams_.Publish();
}

// The retained AddBus/AddSend operations reconstruct a static routing topology by ID.
// Encode them with ordinary 1.1 records, including ownership for later disconnect undo.
void LiveRuntime::WriteInitialRoutes() noexcept {
  for (std::uint32_t i = 0; i < structLogCount_; ++i) {
    const auto& e = structLog_[i];
    const auto bytes = static_cast<std::uint32_t>(sizeof(StructEdit) + 8u * e.argCount);
    const auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&e));
    const RecordLiveData data{kRecordLiveData, kStructEditTag, key, 0, bytes};
    if (!records_.Begin(sizeof(data) + bytes)) {
      Bump(dropRecords_); ++recordDroppedSession_; return;
    }
    const StructEdit edit{e.op, e.targetId, e.argCount, 0};
    records_.Put(&data, sizeof(data));
    records_.Put(&edit, sizeof(edit));
    records_.Put(e.args, e.argCount * 8u);
    records_.Commit();
    constexpr auto markSize = cmd::kWireSize<cmd::LiveSetValue>;
    constexpr auto editSize = cmd::kWireSize<cmd::LiveReplaceChunk>;
    struct Commands {
      cmd::CommandHeader setHeader;
      cmd::LiveSetValue set;
      cmd::CommandHeader editHeader;
      cmd::LiveReplaceChunk replace;
      cmd::CommandHeader restoreHeader;
      cmd::LiveSetValue restore;
    };
    static_assert(sizeof(Commands) == markSize * 2 + editSize);
    const auto current = e.producer < producerCount_ ? producerSession_[e.producer] : 0u;
    const auto marker = static_cast<std::uint16_t>(cmd::CommandType::LiveSetValue);
    const Commands cmds{{marker, markSize, 0}, {0, kFieldSession, 0, 0, e.session},
                        {static_cast<std::uint16_t>(cmd::CommandType::LiveReplaceChunk), editSize, 1},
                        {0, kStructEditTag, &e}, {marker, markSize, 2}, {0, kFieldSession, 0, 0, current}};
    core::LiveArenaView arena{};
    arena.flags = kInitialRoutingArena;
    arena.producer = e.producer;
    arena.appliedBlock = block_;
    arena.normal = reinterpret_cast<const std::uint8_t*>(&cmds);
    arena.normalBytes = sizeof(cmds);
    if (!WriteArenaRecord(records_, arena, caps_)) {
      Bump(dropRecords_); ++recordDroppedSession_; return;
    }
  }
}

void LiveRuntime::RecordStartState(core::System& sys) noexcept {
  std::uint32_t edits = applier_.CopyEdits(sys, editScratch_, editScratchCap_);
  bool routesOnly = structLogCount_ != 0 && structEditor_ == &CoreStructEditor() && edits == 0;
  for (std::uint32_t i = 0; routesOnly && i < structLogCount_; ++i) {
    const auto op = static_cast<StructOp>(structLog_[i].op);
    routesOnly = op == StructOp::AddBus || op == StructOp::AddSend;
  }
  std::uint32_t lost = routesOnly ? 0u : structLogCount_;
  if (edits > editScratchCap_) {
    lost += edits - editScratchCap_;
    edits = editScratchCap_;
  }
  std::uint32_t physicalEffects;
  sys.LiveReserveLeft(nullptr, &physicalEffects, nullptr, nullptr);
  if (physicalEffects < config_.dev_reserve.effects && lost == 0) ++lost;
  auto bankOf = [&](std::uint32_t slot, Id* id) {
    for (std::uint32_t k = 0; k < knownBankCount_; ++k) {
      if (knownBanks_[k].handle.slot == slot) {
        *id = knownBanks_[k].bankId;
        return true;
      }
    }
    return false;
  };
  std::uint32_t cells = 0;
  for (std::uint32_t i = 0; i < edits; ++i) {
    Id id = 0;
    cells += bankOf(editScratch_[i].bankSlot, &id) ? 1u : 0u;
  }
  lost += edits - cells;
  {  // D-175: playbacks alive in core at the start are state the recording does not carry (C-04)
    snd_stats st{};
    sys.LiveStats(&st);
    lost += st.playbacks;
  }
  const RecordBegin b{kRecordBegin,
                      static_cast<std::uint16_t>(arenasSeen_ == 0u ? RecordOrigin::FromCreation : RecordOrigin::MidRun),
                      static_cast<std::uint16_t>(lost < 0xFFFFu ? lost : 0xFFFFu), block_};
  PushRecord(&b, sizeof(b));
  for (std::uint32_t i = 0; i < edits; ++i) {
    const EditView& e = editScratch_[i];
    Id id = 0;
    if (!bankOf(e.bankSlot, &id)) continue;
    std::uint64_t now = e.original;
    ValueApplier::ReadBits(sys, core::LiveCell{e.bankSlot, e.row}, e.fourcc, e.column, &now);
    const RecordCell c{kRecordCell, id, e.fourcc, e.column, e.row, 0u, now, e.original};
    PushRecord(&c, sizeof(c));
  }
  for (const OverrideOwner& o : overrideOwners_) {
    if (o.used == 0u) continue;
    const RecordOverride r{kRecordOverride, o.kind, o.id, o.value, o.target};
    PushRecord(&r, sizeof(r));
  }
  if (routesOnly) WriteInitialRoutes();
}

}  // namespace snd::livelink
