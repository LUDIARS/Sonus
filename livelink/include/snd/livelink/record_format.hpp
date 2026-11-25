// Recording format (design.md 2.5, 11.3; protocol §7). The content of
// RecordData messages and of record files: records one after the other, each
// 8-byte aligned, no file header.
//
//   Arena    RecordArena{kind=1, producer, appliedBlock, bytes, reserved=0}
//            followed by `bytes` bytes:
//              ArenaSections{normalBytes, reserveBytes, fallbackBytes, arenaFlags}
//              normal region   (normalBytes, commands of commands.hpp)
//              reserve region  (reserveBytes)
//              fallback        (fallbackBytes; present iff arenaFlags has
//                               kArenaHasFallback): FallbackHeader, then the
//                               five bitsets as u64 words (BitWords(bits) each,
//                               in the order playback, emitter, transport,
//                               user buffer, bank), then `paramCount`
//                               OverflowParam entries (only the used ones).
//            bytes = 16 + normalBytes + reserveBytes + fallbackBytes (all
//            multiples of 8).
//   Lod      RecordLod{kind=2, level, block}: the LOD level in effect from
//            `block` on. The first record of a recording is a Lod record with
//            the level at the start; its block is the first recorded block.
//   Stall    RecordStall{kind=3, streamSlot, block}: one per stall (6.3).
//   BankMap  RecordBankMap{kind=4, bankId, contentHash}: the banks registered
//            at the start of the recording and every bank registered during it
//            (in the block it was registered; written before the arenas of the
//            next block).
//
// 1.1 (livelink_protocol.hpp kRecord*; constants, not RecordKind enumerators):
//   LiveData RecordLiveData{kind=5, fourcc, key, bankId, bytes} + data padded to 8:
//            the data of a LiveReplaceChunk (a table chunk, or StructEdit + args with
//            fourcc kStructEditTag) whose command is in the NEXT Arena record; `key`
//            is the command's `prepared` pointer value in the recording process.
//   Begin    RecordBegin{kind=6, origin, lostState, block} (16 bytes): right
//            after the first Lod and the BankMap records of the start. origin
//            FromCreation: no command had been applied before the recording started;
//            MidRun: the state before it is only partly in the recording (what follows).
//   Cell     RecordCell{kind=7, ...}: one override-layer cell at the start (`cells` of them).
//   Override RecordOverride{kind=8, ...}: one live override at the start.
// A 1.0 recording has no Begin record (the replay reports it).
//
// Records are platform specific (commands contain pointers, commands.hpp).
// A reader rejects unknown kinds and anything that does not fit.
#ifndef SND_LIVELINK_RECORD_FORMAT_HPP
#define SND_LIVELINK_RECORD_FORMAT_HPP

#include <cstddef>
#include <cstdint>

#include "snd/config.h"
#include "snd/format/livelink_protocol.hpp"
#include "snd/internal/commands.hpp"
#include "snd/livelink/rings.hpp"
#include "snd/types.h"

namespace snd::core {
struct LiveArenaView;
}  // namespace snd::core

namespace snd::livelink {

// Recording-only flag. The arena reproduces routes present before StartRecording.
inline constexpr std::uint32_t kInitialRoutingArena = 1u << 30;

struct ArenaSections {
  std::uint32_t normalBytes;
  std::uint32_t reserveBytes;
  std::uint32_t fallbackBytes;
  std::uint32_t arenaFlags;  // cmd::ArenaFlags
};
static_assert(sizeof(ArenaSections) == 16);

struct FallbackHeader {
  std::uint32_t playbackBits;    // Config.max_playbacks
  std::uint32_t emitterBits;     // max_emitters
  std::uint32_t transportBits;   // max_transports
  std::uint32_t userBufferBits;  // max_user_buffers
  std::uint32_t bankBits;        // max_banks
  std::uint32_t paramCount;      // used OverflowParam entries that follow
  std::uint32_t lostParams;
  std::uint32_t reserved;
};
static_assert(sizeof(FallbackHeader) == 32);

// Capacities of the fallback bitsets (from the runtime's snd_config).
struct FallbackCaps {
  std::uint32_t playbacks = 0, emitters = 0, transports = 0, userBuffers = 0, banks = 0;
};
FallbackCaps CapsOf(const snd_config& config) noexcept;

inline constexpr std::uint32_t BitWordsOf(std::uint32_t bits) noexcept { return (bits + 63u) / 64u; }

// Audio thread (RT-safe): size of the Arena record for `arena` and the writer into
// a MessageRing (one message = one record). false if it does not fit (the caller
// counts the drop; the recording is then incomplete).
std::uint32_t ArenaRecordBytes(const core::LiveArenaView& arena, const FallbackCaps& caps) noexcept;
bool WriteArenaRecord(MessageRing& ring, const core::LiveArenaView& arena, const FallbackCaps& caps,
                      const void* const* rejectedStructs = nullptr, std::uint32_t rejectedCount = 0) noexcept;

// --- reading (non-RT) ------------------------------------------------------------------
struct RecordItem {
  RecordKind kind = RecordKind::Arena;
  std::uint64_t offset = 0;  // of the record in the stream
  RecordArena arena{};       // kind Arena
  ArenaSections sections{};
  const std::uint8_t* normal = nullptr;
  const std::uint8_t* reserve = nullptr;
  const std::uint8_t* fallback = nullptr;  // FallbackHeader + bitsets + params
  RecordLod lod{};
  RecordStall stall{};
  RecordBankMap bankMap{};
  // 1.1 (kind == static_cast<RecordKind>(kRecord*))
  RecordLiveData liveData{};
  const std::uint8_t* data = nullptr;  // LiveData: `liveData.bytes` bytes
  RecordBegin begin{};
  RecordCell cell{};
  RecordOverride override_{};
};
inline bool IsKind(const RecordItem& it, std::uint32_t kind) noexcept { return static_cast<std::uint32_t>(it.kind) == kind; }

enum class RecordError : std::uint8_t {
  None = 0,
  Truncated = 1,     // a record does not fit in the remaining bytes
  UnknownKind = 2,
  BadSections = 3,   // section sizes do not add up / not multiples of 8
  BadCommands = 4,   // a region is not a well-formed command stream
  BadFallback = 5,   // fallback header / sizes inconsistent
};

// Walks a record stream. Every Next validates the record completely (sizes,
// command streams with cmd::ForEachCommand, fallback layout) before returning it.
class RecordReader {
 public:
  RecordReader(const std::uint8_t* data, std::uint64_t bytes) noexcept : data_(data), bytes_(bytes) {}
  // true: *out is the next record. false: end of stream (Error() == None) or a bad record.
  bool Next(RecordItem* out) noexcept;
  RecordError Error() const noexcept { return error_; }
  std::uint64_t Offset() const noexcept { return at_; }

 private:
  const std::uint8_t* data_;
  std::uint64_t bytes_;
  std::uint64_t at_ = 0;
  RecordError error_ = RecordError::None;
};

// Validates a fallback section (FallbackHeader + bitsets + params) of `bytes` bytes.
bool CheckFallback(const std::uint8_t* p, std::uint32_t bytes) noexcept;

const char* RecordErrorName(RecordError e) noexcept;

}  // namespace snd::livelink

#endif  // SND_LIVELINK_RECORD_FORMAT_HPP
