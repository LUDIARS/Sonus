// Replay of a recording: parse + filter (non-RT), then feed core's replay hook.
#include "snd/livelink/replayer.hpp"

#include <cstring>
#include <new>

#include "snd/core/core.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/livelink/wire.hpp"

namespace snd::livelink {

namespace {

bool Skipped(std::uint16_t type) noexcept {
  switch (static_cast<cmd::CommandType>(type)) {
    case cmd::CommandType::BankRegister:
    case cmd::CommandType::BankUnload:
    case cmd::CommandType::LiveReplaceChunk:
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

}  // namespace

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
  alloc_->Free(commands_, MemTag::LiveLink);
  alloc_->Free(arenas_, MemTag::LiveLink);
  alloc_->Free(fallbacks_, MemTag::LiveLink);
  alloc_->Free(fallbackMemory_, MemTag::LiveLink);
  alloc_->Free(banks_, MemTag::LiveLink);
}

int Replayer::Init(const ReplayDesc& d) noexcept {
  config_ = *d.config;
  const FallbackCaps caps = CapsOf(config_);
  if (!applier_.Init(*alloc_, d.maxEdits, config_.max_banks)) return SND_ERR_OUT_OF_BUDGET;
  // Pass 1: validate, count.
  std::uint64_t cmdBytes = 0, fbBytes = 0;
  bool haveBase = false;
  std::uint64_t lastBlock = 0;
  {
    RecordReader rr(d.record, d.bytes);
    RecordItem it;
    while (rr.Next(&it)) {
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
          break;
      }
    }
    if (rr.Error() != RecordError::None) return SND_ERR_CORRUPT;
  }
  info_.lastBlock = lastBlock;
  if (cmdBytes > 0xFFFFFFFFull) return SND_ERR_OUT_OF_BUDGET;
  // Allocate.
  auto A = [&](std::uint64_t bytes) -> void* {
    return alloc_->Allocate(static_cast<std::size_t>(bytes != 0u ? bytes : 64u), 64, MemTag::LiveLink);
  };
  commands_ = static_cast<std::uint8_t*>(A(cmdBytes));
  arenas_ = static_cast<Arena*>(A(sizeof(Arena) * std::uint64_t{info_.arenas}));
  fallbacks_ = static_cast<cmd::OverflowFallback*>(A(sizeof(cmd::OverflowFallback) * std::uint64_t{fallbackCount_}));
  fallbackMemory_ = static_cast<std::uint8_t*>(A(fbBytes));
  banks_ = static_cast<BankRef*>(A(sizeof(BankRef) * std::uint64_t{info_.bankMaps}));
  if (commands_ == nullptr || arenas_ == nullptr || fallbacks_ == nullptr || fallbackMemory_ == nullptr ||
      banks_ == nullptr) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  std::memset(fallbackMemory_, 0, static_cast<std::size_t>(fbBytes != 0u ? fbBytes : 64u));
  // Pass 2: copy, filter.
  RecordReader rr(d.record, d.bytes);
  RecordItem it;
  std::uint32_t at = 0, fi = 0;
  std::uint64_t fbAt = 0;
  while (rr.Next(&it)) {
    if (it.kind == RecordKind::BankMap) {
      banks_[bankCount_++] = BankRef{it.bankMap.bankId, 0u, it.bankMap.contentHash};
      continue;
    }
    if (it.kind != RecordKind::Arena) continue;
    Arena& a = arenas_[arenaCount_++];
    a = Arena{};
    a.relBlock = it.arena.appliedBlock - info_.firstBlock;
    a.flags = it.sections.arenaFlags;
    a.fallback = ~0u;
    auto copy = [&](const std::uint8_t* b, std::uint32_t n, std::uint32_t* off, std::uint32_t* bytes) {
      *off = at;
      cmd::ForEachCommand(b, b + n, [&](const cmd::CommandHeader& h, const std::uint8_t* payload) {
        if (Skipped(h.type)) {
          ++info_.skippedCommands;
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
      std::memcpy(fb.params, src, std::size_t{fh.paramCount} * sizeof(cmd::OverflowParam));
      fb.paramCapacity = fh.paramCount;
      fb.lostParams = fh.lostParams;
      fbAt += FallbackMemoryBytes(caps, fh.paramCount);
      a.fallback = fi++;
    }
  }
  return SND_OK;
}

void Replayer::Install(core::CoreDesc* d) noexcept {
  d->liveUser = this;
  d->liveCommand = [](void* u, cmd::CommandType t, const void* p) noexcept { static_cast<Replayer*>(u)->OnCommand(t, p); };
  d->liveTap.user = this;
  d->liveTap.replay = [](void* u, BlockIndex b, core::LiveArenaView* out) noexcept {
    return static_cast<Replayer*>(u)->Next(b, out);
  };
}

ReplayInfo Replayer::Info() const noexcept {
  ReplayInfo i = info_;
  i.bankMismatches = bankMismatches_.load(std::memory_order_relaxed);
  return i;
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
  if (!begun_) {
    begun_ = true;
    base_ = block;
    core::System* sys = system_.load(std::memory_order_acquire);
    if (sys != nullptr) CheckBanks(*sys);
  }
  const std::uint64_t rel = block - base_;
  while (cursor_ < arenaCount_ && arenas_[cursor_].relBlock < rel) ++cursor_;  // cannot happen (ordered)
  if (cursor_ < arenaCount_ && arenas_[cursor_].relBlock == rel) {
    const Arena& a = arenas_[cursor_++];
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
  if (cursor_ >= arenaCount_ && rel >= Blocks()) finished_.store(1u, std::memory_order_release);
  return false;
}

void Replayer::OnCommand(cmd::CommandType type, const void* payload) noexcept {
  core::System* sys = system_.load(std::memory_order_acquire);
  if (sys == nullptr || type != cmd::CommandType::LiveSetValue) return;
  cmd::LiveSetValue c;
  std::memcpy(&c, payload, sizeof(c));
  applier_.SetValue(*sys, c);
}

}  // namespace snd::livelink
