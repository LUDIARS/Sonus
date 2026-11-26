// Replay of a recording: parse + filter (non-RT), then feed core's replay hook.
#include "snd/livelink/replayer.hpp"

#include <cstddef>
#include <cstring>

#include "prepare_effect.hpp"
#include <new>

#include "snd/core/core.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/livelink/command_check.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/livelink/wire.hpp"

namespace snd::livelink {

namespace bf = ::snd::bank_format;

namespace {

bool Skipped(std::uint16_t type) noexcept {
  switch (static_cast<cmd::CommandType>(type)) {
    case cmd::CommandType::BankRegister:
    case cmd::CommandType::BankUnload:
    case cmd::CommandType::PlaySynth:
    case cmd::CommandType::PlayInput:
    case cmd::CommandType::PlayUserBuffer:
    case cmd::CommandType::CreateUserBuffer:
    case cmd::CommandType::DestroyUserBuffer:
      return true;
    default:
      return false;
  }
}

std::uint64_t FallbackMemoryBytes(const FallbackCaps& k, std::uint32_t params) noexcept {
  const std::uint64_t words = std::uint64_t{BitWordsOf(k.playbacks)} + BitWordsOf(k.emitters) +
                              BitWordsOf(k.transports) + BitWordsOf(k.userBuffers) + BitWordsOf(k.banks);
  return ((words * 8u + std::uint64_t{params} * sizeof(cmd::OverflowParam)) + 63u) & ~std::uint64_t{63};
}

template <class P>
P LoadP(const void* p) noexcept {
  P v;
  std::memcpy(&v, p, sizeof(P));
  return v;
}

std::uint32_t Mix32(std::uint32_t h, std::uint32_t v) noexcept {
  h ^= v;
  h *= 0x01000193u;
  return h;
}

// Validates recorded LiveReplaceChunk data (the server's rules: a table chunk of a known
// table, or a structural edit with valid arguments).
bool ValidLiveData(const RecordLiveData& h, const std::uint8_t* data) noexcept {
  if (h.fourcc == kStructEditTag) {
    if (h.bytes < sizeof(StructEdit)) return false;
    const StructEdit m = LoadP<StructEdit>(data);
    if (CheckStructEdit(m) != SND_OK || std::uint64_t{m.argCount} * 8u != h.bytes - sizeof(StructEdit)) return false;
    // args are 8-aligned in the copy (data copies are 64-aligned), checked there
    return true;
  }
  std::uint32_t rows = 0;
  return CheckTableChunk(h.fourcc, data, h.bytes, &rows) == SND_OK;
}

}  // namespace

void OutputDigest::Add(const float* s, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    std::uint32_t b = 0;
    std::memcpy(&b, &s[i], 4);
    for (int k = 0; k < 4; ++k) {
      value ^= (b >> (8 * k)) & 0xFFu;
      value *= 0x100000001B3ull;
    }
  }
  samples += n;
}

int Replayer::Create(const ReplayDesc& desc, Replayer** out) noexcept {
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  *out = nullptr;
  if (desc.config == nullptr || desc.allocator == nullptr || (desc.bytes != 0u && desc.record == nullptr)) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  void* mem = desc.allocator->Allocate(sizeof(Replayer), 64, MemTag::LiveLink);
  if (mem == nullptr) return SND_ERR_OUT_OF_BUDGET;
  Replayer* r = new (mem) Replayer();
  r->alloc_ = desc.allocator;
  const int res = r->Init(desc);
  if (res != SND_OK) {
    Destroy(r);
    return res;
  }
  *out = r;
  return SND_OK;
}

void Replayer::Destroy(Replayer* r) noexcept {
  if (r == nullptr) return;
  IAllocator* a = r->alloc_;
  r->~Replayer();
  a->Free(r, MemTag::LiveLink);
}

Replayer::~Replayer() {
  if (alloc_ == nullptr) return;
  if (pluginPool_) {
    pluginPool_->~LivePluginPool();
    alloc_->Free(pluginPool_, MemTag::LiveLink);
  }
  alloc_->Free(commands_, MemTag::LiveLink);
  alloc_->Free(arenas_, MemTag::LiveLink);
  alloc_->Free(fallbacks_, MemTag::LiveLink);
  alloc_->Free(fallbackMemory_, MemTag::LiveLink);
  alloc_->Free(banks_, MemTag::LiveLink);
  if (chunks_ != nullptr) {
    for (std::uint32_t i = 0; i < chunkCount_; ++i) chunks_[i].~PendingChunk();
    alloc_->Free(chunks_, MemTag::LiveLink);
  }
  alloc_->Free(chunkMemory_, MemTag::LiveLink);
  alloc_->Free(startCells_, MemTag::LiveLink);
  alloc_->Free(decisions_, MemTag::LiveLink);
  alloc_->Free(voiceScratch_, MemTag::LiveLink);
}

int Replayer::Init(const ReplayDesc& d) noexcept {
  config_ = *d.config;
  if (config_.plugins.effect_count) {
    auto* mem = alloc_->Allocate(sizeof(core::LivePluginPool), 64, MemTag::LiveLink);
    if (!mem) return SND_ERR_OUT_OF_BUDGET;
    pluginPool_ = new (mem) core::LivePluginPool();
  }
  const FallbackCaps caps = CapsOf(config_);
  const CommandLimits limits = LimitsOf(config_);
  if (!applier_.Init(*alloc_, d.maxEdits, config_.max_banks)) return SND_ERR_OUT_OF_BUDGET;
  structEditor_ = d.structEditor;
  applyStart_ = d.applyStartState;
  // Pass 1: validate, count.
  std::uint64_t cmdBytes = 0, fbBytes = 0, liveBytes = 0;
  bool haveBase = false;
  std::uint64_t lastBlock = 0;
  bool sawBegin = false;
  {
    RecordReader rr(d.record, d.bytes);
    RecordItem it;
    while (rr.Next(&it)) {
      const std::uint32_t k = static_cast<std::uint32_t>(it.kind);
      if (k == kRecordLiveData) {
        if (it.liveData.bytes > (64u << 20) || !ValidLiveData(it.liveData, it.data)) return SND_ERR_CORRUPT;
        ++info_.liveData;
        liveBytes += (std::uint64_t{it.liveData.bytes} + 63u) & ~std::uint64_t{63};
        continue;
      }
      if (k == kRecordBegin) {
        if (sawBegin || info_.arenas != 0u) return SND_ERR_CORRUPT;  // once, before the first arena
        sawBegin = true;
        info_.origin = it.begin.origin;
        info_.lostState = it.begin.lostState;
        continue;
      }
      if (k == kRecordCell || k == kRecordOverride) {
        if (!sawBegin || info_.arenas != 0u) return SND_ERR_CORRUPT;  // starting state: after Begin
        if (k == kRecordCell) {
          ++info_.startCells;
        } else {
          if (it.override_.overrideKind > static_cast<std::uint32_t>(cmd::LiveOverrideKind::Switch)) return SND_ERR_CORRUPT;
          ++info_.startOverrides;
        }
        continue;
      }
      switch (it.kind) {
        case RecordKind::Arena: {
          if (!haveBase) {
            info_.firstBlock = it.arena.appliedBlock;
            haveBase = true;
          }
          if (it.arena.appliedBlock < info_.firstBlock || it.arena.appliedBlock < lastBlock) return SND_ERR_CORRUPT;
          lastBlock = it.arena.appliedBlock;
          ++info_.arenas;
          cmdBytes += it.sections.normalBytes + std::uint64_t{it.sections.reserveBytes};
          if (it.fallback != nullptr) {
            FallbackHeader fh;
            std::memcpy(&fh, it.fallback, sizeof(fh));
            if (fh.playbackBits != caps.playbacks || fh.emitterBits != caps.emitters ||
                fh.transportBits != caps.transports || fh.userBufferBits != caps.userBuffers ||
                fh.bankBits != caps.banks) {
              return SND_ERR_VERSION_MISMATCH;
            }
            ++fallbackCount_;
            fbBytes += FallbackMemoryBytes(caps, fh.paramCount);
          }
          break;
        }
        case RecordKind::Lod:
          if (!haveBase) {  // the recording starts with the level in effect (record_format.hpp)
            info_.firstBlock = it.lod.block;
            lastBlock = it.lod.block;
            haveBase = true;
          }
          ++info_.lodRecords;
          break;
        case RecordKind::Stall:
          ++info_.stallRecords;
          break;
        case RecordKind::BankMap:
          ++info_.bankMaps;
          if (info_.arenas != 0u) ++info_.bankChanges;
          break;
        default:
          break;
      }
    }
    if (rr.Error() != RecordError::None) return SND_ERR_CORRUPT;
  }
  info_.lastBlock = lastBlock;
  // The starting overrides become LiveOverride commands of a first arena (relative block 0).
  const bool prologue = applyStart_ && info_.startOverrides != 0u;
  prologue_ = prologue;
  cmdBytes += std::uint64_t{info_.startOverrides} * cmd::kWireSize<cmd::LiveOverride>;
  if (cmdBytes > 0xFFFFFFFFull) return SND_ERR_OUT_OF_BUDGET;
  chunkCount_ = info_.liveData;
  // Allocate.
  auto A = [&](std::uint64_t bytes) -> void* {
    return alloc_->Allocate(static_cast<std::size_t>(bytes != 0u ? bytes : 64u), 64, MemTag::LiveLink);
  };
  commands_ = static_cast<std::uint8_t*>(A(cmdBytes));
  arenas_ = static_cast<Arena*>(A(sizeof(Arena) * (std::uint64_t{info_.arenas} + (prologue ? 1u : 0u))));
  fallbacks_ = static_cast<cmd::OverflowFallback*>(A(sizeof(cmd::OverflowFallback) * std::uint64_t{fallbackCount_}));
  fallbackMemory_ = static_cast<std::uint8_t*>(A(fbBytes));
  banks_ = static_cast<BankRef*>(A(sizeof(BankRef) * std::uint64_t{info_.bankMaps}));
  chunks_ = static_cast<PendingChunk*>(A(sizeof(PendingChunk) * std::uint64_t{chunkCount_}));
  if (chunks_) for (std::uint32_t i = 0; i < chunkCount_; ++i) new (&chunks_[i]) PendingChunk();
  chunkMemory_ = static_cast<std::uint8_t*>(A(liveBytes));
  startCells_ = static_cast<RecordCell*>(A(sizeof(RecordCell) * std::uint64_t{info_.startCells}));
  maxDecisions_ = d.maxDecisions;
  decisions_ = static_cast<ReplayDecision*>(A(sizeof(ReplayDecision) * std::uint64_t{maxDecisions_}));
  voiceCap_ = config_.max_virtual_voices != 0u ? config_.max_virtual_voices : 1u;
  voiceScratch_ = static_cast<VoiceInfo*>(A(sizeof(VoiceInfo) * std::uint64_t{voiceCap_}));
  if (commands_ == nullptr || arenas_ == nullptr || fallbacks_ == nullptr || fallbackMemory_ == nullptr ||
      banks_ == nullptr || chunks_ == nullptr || chunkMemory_ == nullptr || startCells_ == nullptr ||
      decisions_ == nullptr || voiceScratch_ == nullptr) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  std::memset(fallbackMemory_, 0, static_cast<std::size_t>(fbBytes != 0u ? fbBytes : 64u));
  // Pass 2: copy, filter.
  RecordReader rr(d.record, d.bytes);
  RecordItem it;
  std::uint32_t at = 0, fi = 0;
  std::uint64_t fbAt = 0, liveAt = 0;
  std::uint32_t chunkAt = 0, pendingFrom = 0;  // chunks_[pendingFrom, chunkAt): data for the next arena
  snd_result prepResult = SND_OK;
  std::uint32_t overrides = 0;
  if (prologue) {  // reserve the first arena for the starting overrides (filled below)
    Arena& a = arenas_[arenaCount_++];
    a = Arena{};
    a.fallback = ~0u;
  }
  auto keyOf = [&](std::uint32_t i) { return static_cast<std::uint64_t>(chunks_[i].reserved) | (std::uint64_t{chunks_[i].seq} << 32); };
  while (rr.Next(&it)) {
    const std::uint32_t k = static_cast<std::uint32_t>(it.kind);
    if (k == kRecordLiveData) {
      PendingChunk& c = chunks_[chunkAt++];
      c.data = chunkMemory_ + liveAt;
      std::memcpy(c.data, it.data, it.liveData.bytes);
      liveAt += (std::uint64_t{it.liveData.bytes} + 63u) & ~std::uint64_t{63};
      c.bytes = it.liveData.bytes;
      c.bankId = it.liveData.bankId;
      c.fourcc = it.liveData.fourcc;
      c.reserved = static_cast<std::uint32_t>(it.liveData.key);  // the pairing key (until paired)
      c.seq = static_cast<std::uint32_t>(it.liveData.key >> 32);
      c.state.store(PendingChunk::kFree, std::memory_order_relaxed);
      if (c.fourcc == kStructEditTag) {
        const StructEdit m = LoadP<StructEdit>(c.data);
        if (CheckStructArgs(m, reinterpret_cast<const std::uint64_t*>(c.data + sizeof(StructEdit))) != SND_OK) {
          return SND_ERR_CORRUPT;
        }
      } else {
        bf::TableChunkHeader th;
        std::memcpy(&th, c.data, sizeof(th));
        c.rows = th.rowCount;
      }
      continue;
    }
    if (k == kRecordCell) {
      startCells_[startCellCount_++] = it.cell;
      continue;
    }
    if (k == kRecordOverride) {
      if (!prologue) continue;
      cmd::LiveOverride o{};
      o.kind = it.override_.overrideKind;
      o.id = it.override_.id;
      o.target = HandleRef::FromValue(it.override_.target);
      o.value = it.override_.value;
      o.enable = 1u;
      const cmd::CommandHeader h{static_cast<std::uint16_t>(cmd::CommandType::LiveOverride),
                                 static_cast<std::uint16_t>(cmd::kWireSize<cmd::LiveOverride>), overrides};
      if (!CheckCommand(h, reinterpret_cast<const std::uint8_t*>(&o), limits)) {
        ++info_.invalidCommands;
        ++info_.invalidByType[h.type];
        continue;
      }
      std::memset(commands_ + at, 0, cmd::kWireSize<cmd::LiveOverride>);
      std::memcpy(commands_ + at, &h, sizeof(h));
      std::memcpy(commands_ + at + sizeof(h), &o, sizeof(o));
      at += cmd::kWireSize<cmd::LiveOverride>;
      arenas_[0].normalBytes += cmd::kWireSize<cmd::LiveOverride>;
      ++overrides;
      continue;
    }
    if (it.kind == RecordKind::BankMap) {
      banks_[bankCount_++] = BankRef{it.bankMap.bankId, 0u, it.bankMap.contentHash};
      continue;
    }
    if (it.kind != RecordKind::Arena) continue;
    Arena& a = arenas_[arenaCount_++];
    a = Arena{};
    a.relBlock = it.arena.appliedBlock - info_.firstBlock;
    a.producer = it.arena.producer;
    a.flags = it.sections.arenaFlags;
    a.fallback = ~0u;
    auto copy = [&](const std::uint8_t* b, std::uint32_t n, std::uint32_t* off, std::uint32_t* bytes) {
      *off = at;
      cmd::ForEachCommand(b, b + n, [&](const cmd::CommandHeader& h, const std::uint8_t* payload) {
        if (!applyStart_ && (it.sections.arenaFlags & kInitialRoutingArena)) return;
        if (h.type == static_cast<std::uint16_t>(cmd::CommandType::LiveReplaceChunk)) {
          // 1.1: replayed with the data recorded just before this arena (paired by pointer).
          const auto c = LoadP<cmd::LiveReplaceChunk>(payload);
          const std::uint64_t key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(c.prepared));
          std::uint32_t found = ~0u;
          for (std::uint32_t i = pendingFrom; i < chunkAt && found == ~0u; ++i) {
            if (chunks_[i].state.load(std::memory_order_relaxed) == PendingChunk::kFree && keyOf(i) == key &&
                chunks_[i].fourcc == c.fourcc) {
              found = i;
            }
          }
          if (found == ~0u) {
            ++info_.skippedCommands;  // a 1.0 recording, or the data did not fit when recording
            return;
          }
          PendingChunk& pc = chunks_[found];
          pc.state.store(PendingChunk::kQueued, std::memory_order_relaxed);
          pc.reserved = 0u;
          pc.seq = 0u;
          if (pc.fourcc == kStructEditTag) {
            if (structEditor_ == &CoreStructEditor() && prepResult == SND_OK) {
              const auto limit = d.pluginPreparationBytes ? d.pluginPreparationBytes : config_.dev_reserve.bytes;
              if (pluginPool_) prepResult = PrepareExternalEffect(*pluginPool_, *alloc_, config_, nullptr, pc, limit);
              else if (pc.bankId) prepResult = SND_ERR_UNSUPPORTED;
            }
            ++info_.structEdits;
          } else {
            ++info_.replacedChunks;
          }
          std::memcpy(commands_ + at, payload - sizeof(cmd::CommandHeader), h.sizeBytes);
          const void* mine = &pc;
          std::memcpy(commands_ + at + sizeof(cmd::CommandHeader) + offsetof(cmd::LiveReplaceChunk, prepared), &mine,
                      sizeof(mine));
          at += h.sizeBytes;
          ++info_.commands;
          return;
        }
        if (Skipped(h.type)) {
          ++info_.skippedCommands;
          return;
        }
        if (!CheckCommand(h, payload, limits)) {
          ++info_.invalidCommands;
          if (h.type < static_cast<std::uint16_t>(cmd::CommandType::Count)) ++info_.invalidByType[h.type];
          return;
        }
        std::memcpy(commands_ + at, payload - sizeof(cmd::CommandHeader), h.sizeBytes);
        at += h.sizeBytes;
        ++info_.commands;
      });
      *bytes = at - *off;
    };
    copy(it.normal, it.sections.normalBytes, &a.normalOffset, &a.normalBytes);
    copy(it.reserve, it.sections.reserveBytes, &a.reserveOffset, &a.reserveBytes);
    if (prepResult != SND_OK) return prepResult;
    if (it.fallback != nullptr) {
      FallbackHeader fh;
      std::memcpy(&fh, it.fallback, sizeof(fh));
      cmd::OverflowFallback& fb = fallbacks_[fi];
      std::uint8_t* m = fallbackMemory_ + fbAt;
      const std::uint8_t* src = it.fallback + sizeof(FallbackHeader);
      auto bits = [&](std::uint32_t count) {
        std::uint64_t* w = reinterpret_cast<std::uint64_t*>(m);
        const std::uint32_t n = BitWordsOf(count) * 8u;
        std::memcpy(w, src, n);
        m += n;
        src += n;
        return w;
      };
      fb.stopPlayback = bits(caps.playbacks);
      fb.destroyEmitter = bits(caps.emitters);
      fb.destroyTransport = bits(caps.transports);
      fb.destroyUserBuffer = bits(caps.userBuffers);
      fb.unloadBank = bits(caps.banks);
      std::memset(fb.unloadBank, 0, BitWordsOf(caps.banks) * 8u);  // bank slots differ in the replay
      fb.params = reinterpret_cast<cmd::OverflowParam*>(m);
      std::uint32_t kept = 0;
      for (std::uint32_t i = 0; i < fh.paramCount; ++i) {
        cmd::OverflowParam op;
        std::memcpy(&op, src + std::size_t{i} * sizeof(op), sizeof(op));
        if (op.used == 0u) continue;
        if (!CheckOverflowParam(op, limits)) {
          ++info_.invalidFallbackParams;
          continue;
        }
        fb.params[kept++] = op;
      }
      fb.paramCapacity = kept;
      fb.lostParams = fh.lostParams;
      fbAt += FallbackMemoryBytes(caps, fh.paramCount);
      a.fallback = fi++;
    }
    pendingFrom = chunkAt;  // data belongs to the arena that follows it only
  }
  info_.commands += overrides;
  if (applyStart_ && (info_.startCells != 0u || overrides != 0u)) info_.leadBlocks = 1u;
  // Conditions (2.5) known from the recording.
  std::uint32_t lodLevels = 0;
  {
    RecordReader lr(d.record, d.bytes);
    RecordItem li;
    std::uint32_t lods = 0;
    while (lr.Next(&li)) {
      if (li.kind != RecordKind::Lod) continue;
      ++lods;
      lodLevels |= li.lod.level != 0u ? 1u : 0u;
      lodLevels |= lods > 1u ? 1u : 0u;
    }
  }
  std::uint32_t& c = info_.conditions;
  if (lodLevels != 0u) c |= kReplayLodRecorded;
  if (info_.stallRecords != 0u) c |= kReplayStallRecorded;
  if (info_.origin == static_cast<std::uint32_t>(RecordOrigin::MidRun)) c |= kReplayMidRun;
  if (!sawBegin) c |= kReplayNoBegin;
  if (info_.lostState != 0u) c |= kReplayStateLost;
  if (info_.skippedCommands != 0u || info_.invalidCommands != 0u || info_.invalidFallbackParams != 0u) c |= kReplaySkipped;
  if (info_.structEdits != 0u && structEditor_ == nullptr) c |= kReplayStructEdits;
  if (info_.bankChanges != 0u) c |= kReplayBankChanges;
  return SND_OK;
}

void Replayer::Install(core::CoreDesc* d) noexcept {
  d->liveUser = this;
  d->liveCommand = [](void* u, cmd::CommandType t, const void* p) noexcept { static_cast<Replayer*>(u)->OnCommand(t, p); };
  d->liveTap.user = this;
  d->liveTap.replay = [](void* u, BlockIndex b, core::LiveArenaView* out) noexcept {
    return static_cast<Replayer*>(u)->Next(b, out);
  };
  d->liveTap.event = [](void* u, const notify::EventRecord& r) noexcept { static_cast<Replayer*>(u)->OnEvent(r); };
  d->liveTap.ended = [](void* u, std::uint32_t s, std::uint32_t g, std::uint32_t r, std::uint32_t det) noexcept {
    static_cast<Replayer*>(u)->OnEnded(s, g, r, det);
  };
  d->liveTap.stall = [](void* u, std::uint32_t s, Id, std::uint32_t, BlockIndex) noexcept {
    static_cast<Replayer*>(u)->OnStall(s);
  };
  d->liveTap.block = [](void* u, core::System& sys) noexcept { static_cast<Replayer*>(u)->OnBlock(sys); };
}

ReplayInfo Replayer::Info() const noexcept {
  ReplayInfo i = info_;
  i.bankMismatches = bankMismatches_.load(std::memory_order_relaxed);
  i.replayLodChanges = replayLod_.load(std::memory_order_relaxed);
  i.replayStalls = replayStalls_.load(std::memory_order_relaxed);
  i.structEditsNotApplied = structNotApplied_.load(std::memory_order_relaxed);
  i.startStateFailures = startFailures_.load(std::memory_order_relaxed);
  i.decisions = decisionCount_.load(std::memory_order_acquire);
  i.decisionsLost = decisionsLost_.load(std::memory_order_relaxed);
  if (i.replayLodChanges != 0u) i.conditions |= kReplayLodInReplay;
  if (i.replayStalls != 0u) i.conditions |= kReplayStallInReplay;
  if (i.structEditsNotApplied != 0u) i.conditions |= kReplayStructEdits;
  if (i.startStateFailures != 0u) i.conditions |= kReplayStartStateFailed;
  return i;
}

std::uint32_t Replayer::CopyDecisions(ReplayDecision* out, std::uint32_t capacity) const noexcept {
  const std::uint32_t n = decisionCount_.load(std::memory_order_acquire);
  const std::uint32_t m = n < capacity ? n : capacity;
  for (std::uint32_t i = 0; i < m; ++i) out[i] = decisions_[i];
  return m;
}

void Replayer::Log(std::uint32_t kind, std::uint32_t id, std::uint32_t slot, std::uint32_t detail) noexcept {
  if (!begun_ || finished_.load(std::memory_order_relaxed) != 0u) return;
  const std::uint32_t n = decisionCount_.load(std::memory_order_relaxed);
  if (n >= maxDecisions_) {
    decisionsLost_.fetch_add(1u, std::memory_order_relaxed);
    return;
  }
  decisions_[n] = ReplayDecision{rel_, kind, id, slot, detail};
  decisionCount_.store(n + 1u, std::memory_order_release);
}

void Replayer::LogArena(const Arena& a) noexcept {
  auto scan = [&](const cmd::CommandHeader& h, const std::uint8_t* p) {
    switch (static_cast<cmd::CommandType>(h.type)) {
      case cmd::CommandType::Play: {
        const auto c = LoadP<cmd::Play>(p);
        Log(0x8000u | h.type, c.event, c.playback.slot, c.flags);
        break;
      }
      case cmd::CommandType::LiveAudition: {
        const auto c = LoadP<cmd::LiveAudition>(p);
        Log(0x8000u | h.type, c.event, c.playback.slot, c.flags);
        break;
      }
      case cmd::CommandType::Stop: {
        const auto c = LoadP<cmd::Stop>(p);
        Log(0x8000u | h.type, 0u, c.playback.slot, 0u);
        break;
      }
      default:
        break;
    }
  };
  cmd::ForEachCommand(commands_ + a.normalOffset, commands_ + a.normalOffset + a.normalBytes, scan);
  cmd::ForEachCommand(commands_ + a.reserveOffset, commands_ + a.reserveOffset + a.reserveBytes, scan);
}

void Replayer::ApplyStartState(core::System& sys) noexcept {
  if (!applyStart_) return;
  for (std::uint32_t i = 0; i < startCellCount_; ++i) {
    const RecordCell& c = startCells_[i];
    if (applier_.WriteCell(sys, c.bankId, c.fourcc, c.column, c.row, c.value) != SND_OK) {
      startFailures_.fetch_add(1u, std::memory_order_relaxed);
    }
  }
}

void Replayer::OnEvent(const notify::EventRecord& r) noexcept {
  if (r.type == static_cast<std::uint16_t>(notify::EventType::LodChange)) replayLod_.fetch_add(1u, std::memory_order_relaxed);
  if (r.type == static_cast<std::uint16_t>(notify::EventType::Log)) return;  // format pointers: not a decision
  const bool problem = r.type == static_cast<std::uint16_t>(notify::EventType::Error) ||
                       r.type == static_cast<std::uint16_t>(notify::EventType::Warning);
  Log(r.type, r.a, static_cast<std::uint32_t>(r.object), problem ? r.detail : r.b);
}

void Replayer::OnEnded(std::uint32_t slot, std::uint32_t, std::uint32_t reason, std::uint32_t detail) noexcept {
  Log(0x4000u, 0u, slot, (reason << 16) | (detail & 0xFFFFu));
}

void Replayer::OnStall(std::uint32_t) noexcept { replayStalls_.fetch_add(1u, std::memory_order_relaxed); }

void Replayer::OnBlock(core::System& sys) noexcept {
  if (!begun_ || finished_.load(std::memory_order_relaxed) != 0u) return;
  LogVoices(sys);
  // D-175: finished at the end of the last recorded block.
  const std::uint64_t span = Blocks() - info_.leadBlocks;
  if (cursor_ >= arenaCount_ && block_ + 1u >= base_ + span) finished_.store(1u, std::memory_order_release);
}

void Replayer::LogVoices(core::System& sys) noexcept {
  if (maxDecisions_ == 0u) return;
  std::uint32_t n = sys.LiveVoices(voiceScratch_, voiceCap_);
  if (n > voiceCap_) n = voiceCap_;
  std::uint32_t h = 0x811C9DC5u;
  for (std::uint32_t i = 0; i < n; ++i) {
    const VoiceInfo& v = voiceScratch_[i];
    h = Mix32(h, v.eventId);
    h = Mix32(h, v.playbackSlot);
    h = Mix32(h, v.playbackState);
    h = Mix32(h, v.isVirtual);
    h = Mix32(h, v.busId);
    h = Mix32(h, v.sourceKind);
  }
  if (n != lastVoiceCount_ || h != lastVoiceDigest_) {
    lastVoiceCount_ = n;
    lastVoiceDigest_ = h;
    Log(kDecisionVoices, n, 0u, h);
  }
}

void Replayer::CheckBanks(core::System& sys) noexcept {
  core::LiveBankInfo b[64];
  const std::uint32_t n = sys.LiveBanks(b, 64);
  std::uint32_t miss = 0;
  for (std::uint32_t i = 0; i < bankCount_; ++i) {
    bool found = false;
    for (std::uint32_t k = 0; k < n && k < 64u && !found; ++k) {
      found = b[k].bankId == banks_[i].bankId && b[k].contentHash == banks_[i].contentHash;
    }
    miss += found ? 0u : 1u;
  }
  bankMismatches_.store(miss, std::memory_order_relaxed);
}

bool Replayer::Next(std::uint64_t block, core::LiveArenaView* out) noexcept {
  if (armed_.load(std::memory_order_acquire) == 0u || finished_.load(std::memory_order_relaxed) != 0u) return false;
  block_ = block;
  if (!begun_) {
    begun_ = true;
    base_ = block + info_.leadBlocks;
    core::System* sys = system_.load(std::memory_order_acquire);
    if (sys != nullptr) {
      CheckBanks(*sys);
      ApplyStartState(*sys);  // 1.1: override-layer cells (in the lead block, if any)
    }
  }
  if (block < base_) {  // 1.1 lead block: the starting overrides (the prologue arena), nothing else
    if (cursor_ == 0u && prologue_) {
      const Arena& a = arenas_[cursor_++];
      *out = core::LiveArenaView{};
      out->producer = kInvalidSlot;
      out->appliedBlock = block;
      out->normal = commands_ + a.normalOffset;
      out->normalBytes = a.normalBytes;
      out->reserve = commands_ + a.reserveOffset;
      out->reserveBytes = a.reserveBytes;
      out->flags = 0u;
      return true;
    }
    return false;
  }
  const std::uint64_t rel = block - base_;
  rel_ = rel;
  while (cursor_ < arenaCount_ && arenas_[cursor_].relBlock < rel) ++cursor_;  // cannot happen (ordered)
  if (cursor_ < arenaCount_ && arenas_[cursor_].relBlock == rel) {
    const Arena& a = arenas_[cursor_++];
    producer_ = a.producer;
    LogArena(a);
    *out = core::LiveArenaView{};
    out->producer = kInvalidSlot;
    out->appliedBlock = block;
    out->normal = commands_ + a.normalOffset;
    out->normalBytes = a.normalBytes;
    out->reserve = commands_ + a.reserveOffset;
    out->reserveBytes = a.reserveBytes;
    out->fallback = a.fallback != ~0u ? &fallbacks_[a.fallback] : nullptr;
    out->flags = a.fallback != ~0u ? (a.flags | cmd::kArenaHasFallback) : (a.flags & ~cmd::kArenaHasFallback);
    return true;
  }
  if (cursor_ >= arenaCount_ && rel + info_.leadBlocks >= Blocks()) finished_.store(1u, std::memory_order_release);
  return false;
}

void Replayer::OnCommand(cmd::CommandType type, const void* payload) noexcept {
  core::System* sys = system_.load(std::memory_order_acquire);
  if (sys == nullptr) return;
  if (type == cmd::CommandType::LiveSetValue) {
    cmd::LiveSetValue c;
    std::memcpy(&c, payload, sizeof(c));
    if (c.valueType == kValueTypeReadCell) return;  // a read changes nothing
    if (c.field == kFieldSession && c.objectId == 0u && producer_ < 64u) {
      sessions_[producer_] = static_cast<std::uint32_t>(c.value);
      return;
    }
    const auto session = producer_ < 64u ? sessions_[producer_] : 0u;
    if (c.field == kFieldRevertAll && structEditor_) {
      for (std::uint32_t i = chunkCount_; i-- > 0;) {
        auto& p = chunks_[i];
        if (p.fourcc != kStructEditTag || !p.replayApplied || (session && p.replaySession > session)) continue;
        const auto h = LoadP<StructEdit>(p.data);
        StructEditRequest q{static_cast<StructOp>(h.op), h.targetId, h.argCount, p.replaySession,
                            reinterpret_cast<const std::uint64_t*>(p.data + sizeof(h)), std::uint64_t{i} + 1};
        if (structEditor_->Undo(*sys, q) == SND_OK) p.replayApplied = false;
        else structNotApplied_.fetch_add(1u, std::memory_order_relaxed);
      }
    }
    applier_.SetValue(*sys, c, session);
    return;
  }
  if (type != cmd::CommandType::LiveReplaceChunk) return;
  const auto c = LoadP<cmd::LiveReplaceChunk>(payload);
  PendingChunk* p = static_cast<PendingChunk*>(const_cast<void*>(c.prepared));
  bool ours = false;
  for (std::uint32_t i = 0; i < chunkCount_ && !ours; ++i) ours = p == &chunks_[i];
  if (!ours) return;  // only the replay's own copies are dereferenced
  if (p->fourcc == kStructEditTag) {
    StructEdit h{};
    std::memcpy(&h, p->data, sizeof(h));
    p->replaySession = producer_ < 64u ? sessions_[producer_] : 0u;
    const StructEditRequest q{static_cast<StructOp>(h.op), h.targetId, h.argCount, p->replaySession,
                              reinterpret_cast<const std::uint64_t*>(p->data + sizeof(h)),
                              static_cast<std::uint64_t>(p - chunks_) + 1, p->prepared};
    const snd_result r = structEditor_ != nullptr ? structEditor_->Apply(*sys, q) : SND_ERR_UNSUPPORTED;
    if (r != SND_OK) CancelPreparedEffect(*p);
    else p->prepared = nullptr;
    p->replayApplied = r == SND_OK;
    if (r != SND_OK) structNotApplied_.fetch_add(1u, std::memory_order_relaxed);
    return;
  }
  applier_.ReplaceChunk(*sys, *p);
}

}  // namespace snd::livelink
