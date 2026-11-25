// Recording format: arena record writer (audio thread) and the validating reader.
#include "snd/livelink/record_format.hpp"

#include <cstring>

#include "snd/core/core.hpp"

namespace snd::livelink {

FallbackCaps CapsOf(const snd_config& c) noexcept {
  FallbackCaps k;
  k.playbacks = c.max_playbacks;
  k.emitters = c.max_emitters;
  k.transports = c.max_transports;
  k.userBuffers = c.max_user_buffers;
  k.banks = c.max_banks;
  return k;
}

namespace {

std::uint32_t UsedParams(const cmd::OverflowFallback& fb) noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < fb.paramCapacity; ++i) n += fb.params[i].used != 0u ? 1u : 0u;
  return n;
}

std::uint32_t FallbackBytes(const cmd::OverflowFallback& fb, const FallbackCaps& k) noexcept {
  const std::uint32_t words = BitWordsOf(k.playbacks) + BitWordsOf(k.emitters) + BitWordsOf(k.transports) +
                              BitWordsOf(k.userBuffers) + BitWordsOf(k.banks);
  return static_cast<std::uint32_t>(sizeof(FallbackHeader) + words * 8u + UsedParams(fb) * sizeof(cmd::OverflowParam));
}

void PutBits(MessageRing& r, const std::uint64_t* words, std::uint32_t bits) noexcept {
  const std::uint32_t n = BitWordsOf(bits);
  if (words != nullptr) {
    r.Put(words, n * 8u);
  } else {
    const std::uint64_t zero = 0;
    for (std::uint32_t i = 0; i < n; ++i) r.Put(&zero, 8u);
  }
}

}  // namespace

std::uint32_t ArenaRecordBytes(const core::LiveArenaView& a, const FallbackCaps& caps) noexcept {
  std::uint64_t n = sizeof(RecordArena) + sizeof(ArenaSections) + std::uint64_t{a.normalBytes} + a.reserveBytes;
  if (a.fallback != nullptr) n += FallbackBytes(*a.fallback, caps);
  return n > 0xFFFFFFF0u ? 0xFFFFFFF0u : static_cast<std::uint32_t>(n);
}

bool WriteArenaRecord(MessageRing& ring, const core::LiveArenaView& a, const FallbackCaps& caps,
                      const void* const* rejected, std::uint32_t rejectedCount) noexcept {
  // Pointers are captured from this arena, in application order (normal then reserve).
  // A refused edit changed no state. Keep the block/fallback, omit only that command.
  const auto normalEnd = reinterpret_cast<std::uintptr_t>(a.normal) + a.normalBytes;
  std::uint32_t normalRejected = 0;
  while (normalRejected < rejectedCount) {
    auto address = reinterpret_cast<std::uintptr_t>(rejected[normalRejected]);
    if (address < reinterpret_cast<std::uintptr_t>(a.normal) || address >= normalEnd) break;
    ++normalRejected;
  }
  constexpr auto wireBytes = cmd::kWireSize<cmd::LiveReplaceChunk>;
  const std::uint32_t total = ArenaRecordBytes(a, caps) - rejectedCount * wireBytes;
  if (!ring.Begin(total)) return false;
  const std::uint32_t fbBytes = a.fallback != nullptr ? FallbackBytes(*a.fallback, caps) : 0u;
  RecordArena h{};
  h.kind = static_cast<std::uint32_t>(RecordKind::Arena);
  h.producer = a.producer;
  h.appliedBlock = a.appliedBlock;
  h.bytes = total - static_cast<std::uint32_t>(sizeof(RecordArena));
  h.reserved = 0;
  ring.Put(&h, sizeof(h));
  const ArenaSections s{a.normalBytes - normalRejected * wireBytes,
                        a.reserveBytes - (rejectedCount - normalRejected) * wireBytes, fbBytes,
                        a.fallback != nullptr ? (a.flags | cmd::kArenaHasFallback) : (a.flags & ~cmd::kArenaHasFallback)};
  ring.Put(&s, sizeof(s));
  auto putRegion = [&](const std::uint8_t* data, std::uint32_t bytes, std::uint32_t first, std::uint32_t end) {
    if (!bytes) return;
    const auto* cursor = data;
    for (auto i = first; i < end; ++i) {
      const auto* header = static_cast<const std::uint8_t*>(rejected[i]) - sizeof(cmd::CommandHeader);
      if (header != cursor) ring.Put(cursor, static_cast<std::uint32_t>(header - cursor));
      cursor = header + wireBytes;
    }
    if (cursor != data + bytes) ring.Put(cursor, static_cast<std::uint32_t>(data + bytes - cursor));
  };
  putRegion(a.normal, a.normalBytes, 0, normalRejected);
  putRegion(a.reserve, a.reserveBytes, normalRejected, rejectedCount);
  if (a.fallback != nullptr) {
    const cmd::OverflowFallback& fb = *a.fallback;
    const FallbackHeader fh{caps.playbacks, caps.emitters, caps.transports, caps.userBuffers, caps.banks,
                            UsedParams(fb), fb.lostParams, 0u};
    ring.Put(&fh, sizeof(fh));
    PutBits(ring, fb.stopPlayback, caps.playbacks);
    PutBits(ring, fb.destroyEmitter, caps.emitters);
    PutBits(ring, fb.destroyTransport, caps.transports);
    PutBits(ring, fb.destroyUserBuffer, caps.userBuffers);
    PutBits(ring, fb.unloadBank, caps.banks);
    for (std::uint32_t i = 0; i < fb.paramCapacity; ++i) {
      if (fb.params[i].used != 0u) ring.Put(&fb.params[i], sizeof(cmd::OverflowParam));
    }
  }
  ring.Commit();
  return true;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
bool CheckFallback(const std::uint8_t* p, std::uint32_t bytes) noexcept {
  if (bytes < sizeof(FallbackHeader)) return false;
  FallbackHeader fh;
  std::memcpy(&fh, p, sizeof(fh));
  constexpr std::uint32_t kMaxBits = 1u << 24;
  if (fh.playbackBits > kMaxBits || fh.emitterBits > kMaxBits || fh.transportBits > kMaxBits ||
      fh.userBufferBits > kMaxBits || fh.bankBits > kMaxBits || fh.paramCount > (1u << 20)) {
    return false;
  }
  const std::uint64_t words = std::uint64_t{BitWordsOf(fh.playbackBits)} + BitWordsOf(fh.emitterBits) +
                              BitWordsOf(fh.transportBits) + BitWordsOf(fh.userBufferBits) + BitWordsOf(fh.bankBits);
  const std::uint64_t need = sizeof(FallbackHeader) + words * 8u + std::uint64_t{fh.paramCount} * sizeof(cmd::OverflowParam);
  return need == bytes;
}

bool RecordReader::Next(RecordItem* out) noexcept {
  if (error_ != RecordError::None || at_ >= bytes_) return false;
  auto fail = [&](RecordError e) {
    error_ = e;
    return false;
  };
  const std::uint64_t left = bytes_ - at_;
  if (left < 16u) return fail(RecordError::Truncated);
  const std::uint8_t* p = data_ + at_;
  std::uint32_t kind = 0;
  std::memcpy(&kind, p, 4);
  *out = RecordItem{};
  out->offset = at_;
  switch (static_cast<RecordKind>(kind)) {
    case RecordKind::Lod:
      out->kind = RecordKind::Lod;
      std::memcpy(&out->lod, p, sizeof(RecordLod));
      at_ += sizeof(RecordLod);
      return true;
    case RecordKind::Stall:
      out->kind = RecordKind::Stall;
      std::memcpy(&out->stall, p, sizeof(RecordStall));
      at_ += sizeof(RecordStall);
      return true;
    case RecordKind::BankMap:
      out->kind = RecordKind::BankMap;
      std::memcpy(&out->bankMap, p, sizeof(RecordBankMap));
      at_ += sizeof(RecordBankMap);
      return true;
    default:
      break;
    case RecordKind::Arena: {
      if (left < sizeof(RecordArena)) return fail(RecordError::Truncated);
      out->kind = RecordKind::Arena;
      std::memcpy(&out->arena, p, sizeof(RecordArena));
      const std::uint64_t body = out->arena.bytes;
      if ((body & 7u) != 0u || body < sizeof(ArenaSections)) return fail(RecordError::BadSections);
      if (body > left - sizeof(RecordArena)) return fail(RecordError::Truncated);
      const std::uint8_t* b = p + sizeof(RecordArena);
      std::memcpy(&out->sections, b, sizeof(ArenaSections));
      const ArenaSections& s = out->sections;
      const std::uint64_t sum = sizeof(ArenaSections) + std::uint64_t{s.normalBytes} + s.reserveBytes + s.fallbackBytes;
      if (sum != body || ((s.normalBytes | s.reserveBytes | s.fallbackBytes) & 7u) != 0u) {
        return fail(RecordError::BadSections);
      }
      const bool hasFb = (s.arenaFlags & cmd::kArenaHasFallback) != 0u;
      if (hasFb != (s.fallbackBytes != 0u)) return fail(RecordError::BadSections);
      out->normal = b + sizeof(ArenaSections);
      out->reserve = out->normal + s.normalBytes;
      out->fallback = hasFb ? out->reserve + s.reserveBytes : nullptr;
      auto ignore = [](const cmd::CommandHeader&, const std::uint8_t*) {};
      if (!cmd::ForEachCommand(out->normal, out->normal + s.normalBytes, ignore) ||
          !cmd::ForEachCommand(out->reserve, out->reserve + s.reserveBytes, ignore)) {
        return fail(RecordError::BadCommands);
      }
      if (hasFb && !CheckFallback(out->fallback, s.fallbackBytes)) return fail(RecordError::BadFallback);
      at_ += sizeof(RecordArena) + body;
      return true;
    }
  }
  // 1.1 kinds (constants, see livelink_protocol.hpp).
  out->kind = static_cast<RecordKind>(kind);
  switch (kind) {
    case kRecordLiveData: {
      if (left < sizeof(RecordLiveData)) return fail(RecordError::Truncated);
      std::memcpy(&out->liveData, p, sizeof(RecordLiveData));
      const std::uint64_t padded = (std::uint64_t{out->liveData.bytes} + 7u) & ~std::uint64_t{7};
      if (padded > left - sizeof(RecordLiveData)) return fail(RecordError::Truncated);
      out->data = p + sizeof(RecordLiveData);
      at_ += sizeof(RecordLiveData) + padded;
      return true;
    }
    case kRecordBegin:
      if (left < sizeof(RecordBegin)) return fail(RecordError::Truncated);
      std::memcpy(&out->begin, p, sizeof(RecordBegin));
      if (out->begin.origin != static_cast<std::uint16_t>(RecordOrigin::FromCreation) &&
          out->begin.origin != static_cast<std::uint16_t>(RecordOrigin::MidRun)) {
        return fail(RecordError::BadSections);
      }
      at_ += sizeof(RecordBegin);
      return true;
    case kRecordCell:
      if (left < sizeof(RecordCell)) return fail(RecordError::Truncated);
      std::memcpy(&out->cell, p, sizeof(RecordCell));
      at_ += sizeof(RecordCell);
      return true;
    case kRecordOverride:
      if (left < sizeof(RecordOverride)) return fail(RecordError::Truncated);
      std::memcpy(&out->override_, p, sizeof(RecordOverride));
      at_ += sizeof(RecordOverride);
      return true;
    default:
      break;
  }
  return fail(RecordError::UnknownKind);
}

const char* RecordErrorName(RecordError e) noexcept {
  switch (e) {
    case RecordError::None: return "none";
    case RecordError::Truncated: return "truncated";
    case RecordError::UnknownKind: return "unknown record kind";
    case RecordError::BadSections: return "bad arena sections";
    case RecordError::BadCommands: return "malformed command stream";
    case RecordError::BadFallback: return "bad fallback section";
  }
  return "?";
}

}  // namespace snd::livelink
