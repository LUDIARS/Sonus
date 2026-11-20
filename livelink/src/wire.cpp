// Live link wire codec: headers, fixed payloads, lists, trailing data and the
// validation of every tool -> game message (protocol §2-§5).
#include "snd/livelink/wire.hpp"

#include <cmath>

#include "snd/format/bank_check.hpp"
#include "snd/format/bank_format.hpp"

namespace snd::livelink {

namespace bf = ::snd::bank_format;

HeaderStatus DecodeHeader(const std::uint8_t* p, FrameHeader* out) noexcept {
  FrameHeader h;
  std::memcpy(&h, p, sizeof(h));
  *out = h;
  if (h.magic != kMagic) return HeaderStatus::BadMagic;
  if (h.payloadBytes > kMaxPayloadBytes) return HeaderStatus::TooLarge;
  return HeaderStatus::Ok;
}

void EncodeHeader(std::uint8_t* out, std::uint16_t type, std::uint16_t flags, std::uint32_t payloadBytes,
                  std::uint32_t seq) noexcept {
  const FrameHeader h{kMagic, type, flags, payloadBytes, seq};
  std::memcpy(out, &h, sizeof(h));
}

bool ParseList(const std::uint8_t* payload, std::uint32_t payloadBytes, std::uint32_t maxCount, ListView* out) noexcept {
  if (payloadBytes < sizeof(ListHeader)) return false;
  ListHeader lh;
  std::memcpy(&lh, payload, sizeof(lh));
  if (lh.count > maxCount) return false;
  if (lh.count != 0u && lh.elemBytes == 0u) return false;
  const std::uint64_t need = std::uint64_t{lh.count} * lh.elemBytes;
  if (need > payloadBytes - sizeof(ListHeader)) return false;
  out->elements = payload + sizeof(ListHeader);
  out->count = lh.count;
  out->elemBytes = lh.elemBytes;
  return true;
}

bool TrailingData(const std::uint8_t* payload, std::uint32_t payloadBytes, std::uint32_t fixedBytes,
                  std::uint64_t bytes, const std::uint8_t** data) noexcept {
  if (payloadBytes < fixedBytes) return bytes == 0u ? (*data = payload + payloadBytes, true) : false;
  if (bytes > payloadBytes - fixedBytes) return false;
  *data = payload + (payloadBytes - bytes);
  return true;
}

namespace {
bool Finite(float f) noexcept { return std::isfinite(f); }
bool Bool01(std::uint32_t v) noexcept { return v <= 1u; }
}  // namespace

snd_result CheckHello(const Hello& m) noexcept {
  return m.role == static_cast<std::uint32_t>(Role::Tool) ? SND_OK : SND_ERR_INVALID_ARGUMENT;
}

snd_result CheckSyncDecision(const SyncDecision& m) noexcept {
  return m.mode <= static_cast<std::uint32_t>(SyncMode::Disconnect) ? SND_OK : SND_ERR_INVALID_ARGUMENT;
}

snd_result CheckSetValue(const SetValue& m) noexcept {
  if (m.valueType > static_cast<std::uint32_t>(ValueType::Id)) return SND_ERR_INVALID_ARGUMENT;
  const int ci = bf::check::KnownChunkIndex(m.chunk);
  if (ci < 0 || bf::kChunks[ci].kind != bf::ChunkKind::Table) return SND_ERR_NOT_FOUND;
  const bf::ColumnInfo* info = bf::check::FindColumnInfo(m.chunk, m.column);
  if (info == nullptr) return SND_ERR_NOT_FOUND;
  // The value type must match the column (f32 column <- F32; u32 column <- U32 / I32 / Id).
  const bool f32 = info->type == bf::ColumnType::F32;
  const bool isFloat = m.valueType == static_cast<std::uint32_t>(ValueType::F32);
  if (f32 != isFloat) return SND_ERR_INVALID_ARGUMENT;
  if (info->type != bf::ColumnType::F32 && info->type != bf::ColumnType::U32) return SND_ERR_INVALID_ARGUMENT;
  if ((m.value >> 32) != 0u) return SND_ERR_INVALID_ARGUMENT;  // 32-bit columns only
  if (isFloat) {
    float f = 0;
    const std::uint32_t lo = static_cast<std::uint32_t>(m.value);
    std::memcpy(&f, &lo, 4);
    if (!Finite(f)) return SND_ERR_INVALID_ARGUMENT;
  }
  return SND_OK;
}

snd_result CheckAudition(const Audition& m) noexcept {
  if ((m.flags & ~(kAuditionStopAll | kAuditionAtPosition)) != 0u) return SND_ERR_INVALID_ARGUMENT;
  if ((m.flags & kAuditionAtPosition) != 0u &&
      !(Finite(m.position[0]) && Finite(m.position[1]) && Finite(m.position[2]))) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  if ((m.flags & kAuditionStopAll) == 0u && m.eventId == 0u) return SND_ERR_INVALID_ARGUMENT;
  return SND_OK;
}

snd_result CheckOverrideParam(const OverrideParam& m) noexcept {
  if (m.paramId == 0u || !Bool01(m.enable)) return SND_ERR_INVALID_ARGUMENT;
  if (m.enable != 0u && !Finite(m.value)) return SND_ERR_INVALID_ARGUMENT;
  return SND_OK;
}

snd_result CheckOverrideState(const OverrideState& m) noexcept {
  if (m.musicEventId == 0u || !Bool01(m.enable)) return SND_ERR_INVALID_ARGUMENT;
  if (m.enable != 0u && m.stateId == 0u) return SND_ERR_INVALID_ARGUMENT;
  return SND_OK;
}

snd_result CheckSetProfilerMask(const SetProfilerMask& m) noexcept { return CheckSetProfilerMask(m, 0u); }

snd_result CheckSetProfilerMask(const SetProfilerMask& m, std::uint16_t minor) noexcept {
  if ((m.mask & ~ProfilerMaskOf(minor)) != 0u) return SND_ERR_INVALID_ARGUMENT;
  if (m.meterHz > 1000u) return SND_ERR_INVALID_ARGUMENT;
  return SND_OK;
}

snd_result CheckRecordControl(const RecordControl& m) noexcept {
  return Bool01(m.start) ? SND_OK : SND_ERR_INVALID_ARGUMENT;
}

snd_result CheckStructEdit(const StructEdit& m) noexcept {
  if (m.op > static_cast<std::uint32_t>(StructOp::AddSend)) return SND_ERR_INVALID_ARGUMENT;
  if (m.argCount > 64u) return SND_ERR_INVALID_ARGUMENT;
  return SND_OK;
}

namespace {
bool F32Arg(std::uint64_t a) noexcept {
  if ((a >> 32) != 0u) return false;
  const std::uint32_t lo = static_cast<std::uint32_t>(a);
  float f = 0;
  std::memcpy(&f, &lo, 4);
  return Finite(f);
}
}  // namespace

snd_result CheckStructArgs(const StructEdit& m, const std::uint64_t* args) noexcept {
  const snd_result r = CheckStructEdit(m);
  if (r != SND_OK) return r;
  if (m.targetId == 0u || (m.argCount != 0u && args == nullptr)) return SND_ERR_INVALID_ARGUMENT;
  auto id = [](std::uint64_t a) { return a != 0u && a <= 0xFFFFFFFFull; };
  switch (static_cast<StructOp>(m.op)) {
    case StructOp::AddBus:
      if (m.argCount < 2u || m.argCount > 3u || !id(args[0]) || args[0] == m.targetId) return SND_ERR_INVALID_ARGUMENT;
      if (args[1] < 1u || args[1] > SND_MAX_CHANNELS) return SND_ERR_INVALID_ARGUMENT;
      if (m.argCount == 3u && !F32Arg(args[2])) return SND_ERR_INVALID_ARGUMENT;
      return SND_OK;
    case StructOp::RemoveBus:
      return m.argCount == 0u ? SND_OK : SND_ERR_INVALID_ARGUMENT;
    case StructOp::AddEffect:
      if (m.argCount < 2u || m.argCount > kMaxStructEffectParams + 2u || !id(args[0])) return SND_ERR_INVALID_ARGUMENT;
      if (args[1] > 0xFFFFFFFFull) return SND_ERR_INVALID_ARGUMENT;  // insert position (~0u: append)
      for (std::uint32_t i = 2; i < m.argCount; ++i) {
        if (!F32Arg(args[i])) return SND_ERR_INVALID_ARGUMENT;
      }
      return SND_OK;
    case StructOp::RemoveEffect:
      return m.argCount == 1u && args[0] <= 0xFFFFu ? SND_OK : SND_ERR_INVALID_ARGUMENT;
    case StructOp::AddSend:
      if (m.argCount < 2u || m.argCount > 3u || !id(args[0]) || args[0] == m.targetId || !F32Arg(args[1])) {
        return SND_ERR_INVALID_ARGUMENT;
      }
      if (m.argCount == 3u && args[2] > 1u) return SND_ERR_INVALID_ARGUMENT;  // 1 = pre-fader
      return SND_OK;
  }
  return SND_ERR_INVALID_ARGUMENT;
}

snd_result CheckReloadBank(const ReloadBank& m, const std::uint8_t* path) noexcept {
  if (m.bankId == 0u || (m.flags & ~kReloadLoadIfAbsent) != 0u) return SND_ERR_INVALID_ARGUMENT;
  if (m.pathBytes == 0u || m.pathBytes > kMaxReloadPathBytes || path == nullptr) return SND_ERR_INVALID_ARGUMENT;
  for (std::uint32_t i = 0; i < m.pathBytes; ++i) {
    if (path[i] == 0u) return SND_ERR_INVALID_ARGUMENT;
  }
  return SND_OK;
}

snd_result CheckCellAddress(const SetValue& m) noexcept {
  const int ci = bf::check::KnownChunkIndex(m.chunk);
  if (ci < 0 || bf::kChunks[ci].kind != bf::ChunkKind::Table) return SND_ERR_NOT_FOUND;
  if (bf::check::FindColumnInfo(m.chunk, m.column) == nullptr) return SND_ERR_NOT_FOUND;
  return SND_OK;
}

snd_result CheckTableChunk(std::uint32_t fourcc, const std::uint8_t* data, std::uint64_t bytes,
                           std::uint32_t* rows) noexcept {
  const int ci = bf::check::KnownChunkIndex(fourcc);
  if (ci < 0) return SND_ERR_NOT_FOUND;
  if (bf::kChunks[ci].kind != bf::ChunkKind::Table) return SND_ERR_UNSUPPORTED;
  if (bytes < sizeof(bf::TableChunkHeader)) return SND_ERR_INVALID_ARGUMENT;
  bf::TableChunkHeader th;
  std::memcpy(&th, data, sizeof(th));
  if (th.columnCount > 256u) return SND_ERR_INVALID_ARGUMENT;
  const std::uint64_t dir = sizeof(bf::TableChunkHeader) + std::uint64_t{th.columnCount} * sizeof(bf::ColumnEntry);
  if (dir > bytes) return SND_ERR_INVALID_ARGUMENT;
  for (std::uint32_t i = 0; i < th.columnCount; ++i) {
    bf::ColumnEntry ce;
    std::memcpy(&ce, data + sizeof(bf::TableChunkHeader) + std::size_t{i} * sizeof(ce), sizeof(ce));
    const bf::ColumnInfo* info = bf::check::FindColumnInfo(fourcc, ce.columnId);
    if (info == nullptr) continue;  // unknown column: ignored
    if (ce.elemBytes != bf::ColumnTypeBytes(info->type)) return SND_ERR_INVALID_ARGUMENT;
    if ((ce.offset % 64u) != 0u || ce.offset < dir) return SND_ERR_INVALID_ARGUMENT;
    const std::uint64_t len = std::uint64_t{th.rowCount} * ce.elemBytes;
    if (ce.offset > bytes || len > bytes - ce.offset) return SND_ERR_INVALID_ARGUMENT;
    for (std::uint32_t j = 0; j < i; ++j) {  // a column id appears once
      bf::ColumnEntry pe;
      std::memcpy(&pe, data + sizeof(bf::TableChunkHeader) + std::size_t{j} * sizeof(pe), sizeof(pe));
      if (pe.columnId == ce.columnId) return SND_ERR_INVALID_ARGUMENT;
    }
  }
  *rows = th.rowCount;
  return SND_OK;
}

std::uint32_t EncodeField(std::uint32_t chunkFourcc, std::uint32_t column) noexcept {
  const int ci = bf::check::KnownChunkIndex(chunkFourcc);
  if (ci < 0 || column > 0xFFFFu || bf::check::FindColumnInfo(chunkFourcc, column) == nullptr) return kFieldRevertAll;
  return (static_cast<std::uint32_t>(ci) << 16) | column;
}

bool IsMarkerField(std::uint32_t field) noexcept {
  return field == kFieldRevertAll || field == kFieldSession || field == kFieldRevertBegin || field == kFieldAuditionStop;
}

bool DecodeField(std::uint32_t field, std::uint32_t* chunkFourcc, std::uint32_t* column) noexcept {
  const std::uint32_t ci = field >> 16;
  if (IsMarkerField(field) || ci >= bf::check::kKnownChunkCount) return false;
  *chunkFourcc = bf::kChunks[ci].fourcc;
  *column = field & 0xFFFFu;
  return bf::check::FindColumnInfo(*chunkFourcc, *column) != nullptr;
}

void CopyName(char* dst, std::size_t dstBytes, const char* src, std::size_t srcBytes) noexcept {
  if (dstBytes == 0u) return;
  std::size_t n = 0;
  while (n < srcBytes && n + 1u < dstBytes && src[n] != '\0') {
    dst[n] = src[n];
    ++n;
  }
  for (std::size_t i = n; i < dstBytes; ++i) dst[i] = '\0';
}

const char* MessageTypeName(std::uint16_t type) noexcept {
  switch (static_cast<MessageType>(type)) {
    case MessageType::Hello: return "Hello";
    case MessageType::HelloReply: return "HelloReply";
    case MessageType::SyncDecision: return "SyncDecision";
    case MessageType::Ping: return "Ping";
    case MessageType::Pong: return "Pong";
    case MessageType::Ack: return "Ack";
    case MessageType::Error: return "Error";
    case MessageType::SetValue: return "SetValue";
    case MessageType::ReplaceChunk: return "ReplaceChunk";
    case MessageType::Audition: return "Audition";
    case MessageType::OverrideParam: return "OverrideParam";
    case MessageType::OverrideState: return "OverrideState";
    case MessageType::ClearOverrides: return "ClearOverrides";
    case MessageType::SetProfilerMask: return "SetProfilerMask";
    case MessageType::RecordControl: return "RecordControl";
    case MessageType::StructEdit: return "StructEdit";
    case MessageType::VoiceList: return "VoiceList";
    case MessageType::BlockTiming: return "BlockTiming";
    case MessageType::MemoryUsage: return "MemoryUsage";
    case MessageType::StreamStall: return "StreamStall";
    case MessageType::LodChange: return "LodChange";
    case MessageType::EventLog: return "EventLog";
    case MessageType::BusMeters: return "BusMeters";
    case MessageType::RecordData: return "RecordData";
    case MessageType::RevertValue: return "RevertValue";
    case MessageType::ReadCell: return "ReadCell";
    case MessageType::QueryBanks: return "QueryBanks";
    case MessageType::ReloadBank: return "ReloadBank";
    case MessageType::CellValue: return "CellValue";
    case MessageType::BankList: return "BankList";
    case MessageType::StreamList: return "StreamList";
    case MessageType::BlockDetail: return "BlockDetail";
    case MessageType::ReloadStatus: return "ReloadStatus";
  }
  return "Unknown";
}

}  // namespace snd::livelink
