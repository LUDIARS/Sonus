// Live link wire codec (design.md 11.2, spec/interface/livelink-protocol.md).
//
// Every message is a 16-byte FrameHeader followed by `payloadBytes` of
// payload (snd/format/livelink_protocol.hpp). The input comes from the
// network: every function here validates sizes, counts and enumerations and
// never reads outside [payload, payload + payloadBytes).
//
// Compatibility rules (protocol §3):
//   - a known fixed payload is read as min(payloadBytes, sizeof(T)) bytes, the
//     rest is zero (a newer minor may append fields, an older peer may send
//     fewer);
//   - lists (ListHeader) are read element by element with
//     min(elemBytes, sizeof(T)) bytes per element;
//   - trailing data (ReplaceChunk data, StructEdit args, RecordData) is the
//     LAST `bytes` bytes of the payload, so a newer minor may grow the fixed
//     part in front of it (decision of this implementation, see the report).
// The wire is little endian; the structs are copied byte for byte, so this
// module requires a little-endian host (every supported target is).
#ifndef SND_LIVELINK_WIRE_HPP
#define SND_LIVELINK_WIRE_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "snd/format/livelink_protocol.hpp"
#include "snd/types.h"

namespace snd::livelink {

static_assert(std::endian::native == std::endian::little, "the live link wire format is little endian");

inline constexpr std::uint32_t kFrameHeaderBytes = sizeof(FrameHeader);

enum class HeaderStatus : std::uint8_t {
  Ok = 0,
  BadMagic = 1,  // not a live link stream: send Error, disconnect
  TooLarge = 2,  // payloadBytes > kMaxPayloadBytes: send Error, disconnect
};

// Header (16 bytes at `p`) -> *out. Validates magic and the payload limit.
HeaderStatus DecodeHeader(const std::uint8_t* p, FrameHeader* out) noexcept;
void EncodeHeader(std::uint8_t* out, std::uint16_t type, std::uint16_t flags, std::uint32_t payloadBytes,
                  std::uint32_t seq) noexcept;

// Fixed part of a payload (protocol §3): zero-filled, min(bytes, sizeof(T)) copied.
template <class T>
T ReadFixed(const std::uint8_t* payload, std::uint32_t payloadBytes) noexcept {
  T v{};
  std::memcpy(&v, payload, payloadBytes < sizeof(T) ? payloadBytes : sizeof(T));
  return v;
}

// A list payload: ListHeader then `count` elements of `elemBytes`.
struct ListView {
  const std::uint8_t* elements = nullptr;
  std::uint32_t count = 0;
  std::uint32_t elemBytes = 0;
  template <class T>
  T At(std::uint32_t i) const noexcept {
    T v{};
    std::memcpy(&v, elements + std::size_t{i} * elemBytes, elemBytes < sizeof(T) ? elemBytes : sizeof(T));
    return v;
  }
};
// false: the payload is shorter than the list it announces, or elemBytes == 0 with
// count != 0, or the list exceeds `maxCount`.
bool ParseList(const std::uint8_t* payload, std::uint32_t payloadBytes, std::uint32_t maxCount, ListView* out) noexcept;

// Trailing data of `bytes` bytes at the end of the payload, after a fixed part of at
// least `fixedBytes` (the part this version knows). false if it does not fit.
bool TrailingData(const std::uint8_t* payload, std::uint32_t payloadBytes, std::uint32_t fixedBytes,
                  std::uint64_t bytes, const std::uint8_t** data) noexcept;

// --- validation of tool -> game messages (enumerations, floats) -----------------------
// Each returns SND_OK or SND_ERR_INVALID_ARGUMENT / SND_ERR_NOT_FOUND (unknown table or
// column) for a message whose fixed part was read with ReadFixed.
snd_result CheckHello(const Hello& m) noexcept;
snd_result CheckSyncDecision(const SyncDecision& m) noexcept;
snd_result CheckSetValue(const SetValue& m) noexcept;
snd_result CheckAudition(const Audition& m) noexcept;
snd_result CheckOverrideParam(const OverrideParam& m) noexcept;
snd_result CheckOverrideState(const OverrideState& m) noexcept;
snd_result CheckSetProfilerMask(const SetProfilerMask& m) noexcept;
snd_result CheckRecordControl(const RecordControl& m) noexcept;
snd_result CheckStructEdit(const StructEdit& m) noexcept;
// ReplaceChunk data: a table chunk (TableChunkHeader + ColumnEntry[]) of a known table
// whose columns lie inside `bytes` with the element size of their type. Unknown column
// ids are allowed (ignored, protocol §3 / bank-format.md). *rows = its row count.
snd_result CheckTableChunk(std::uint32_t fourcc, const std::uint8_t* data, std::uint64_t bytes,
                           std::uint32_t* rows) noexcept;

// LiveSetValue::field (commands.hpp) for a bank column: (index of the chunk in
// bank_format::kChunks << 16) | column id. kFieldRevertAll is the live link's own
// "put every edited cell back" marker (objectId 0), never sent by a tool.
inline constexpr std::uint32_t kFieldRevertAll = 0xFFFFFFFFu;
std::uint32_t EncodeField(std::uint32_t chunkFourcc, std::uint32_t column) noexcept;  // kFieldRevertAll if unknown
bool DecodeField(std::uint32_t field, std::uint32_t* chunkFourcc, std::uint32_t* column) noexcept;

// Copies a NUL-padded name (Hello.name, Error.message) and terminates it.
void CopyName(char* dst, std::size_t dstBytes, const char* src, std::size_t srcBytes) noexcept;

const char* MessageTypeName(std::uint16_t type) noexcept;

}  // namespace snd::livelink

#endif  // SND_LIVELINK_WIRE_HPP
