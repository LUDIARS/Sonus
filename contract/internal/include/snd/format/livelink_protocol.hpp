// Live link message format (design.md 11.2). FROZEN CONTRACT.
//
// Byte-level specification: spec/interface/livelink-protocol.md.
// Transport: one TCP stream, the game listens (Config.liveLink.port), the tool
// connects. Every message is a 16-byte FrameHeader followed by payloadBytes.
// All integers little endian, floats IEEE-754 binary32, no padding beyond
// what is spelled out (structs below have no implicit padding: asserted).
#ifndef SND_FORMAT_LIVELINK_PROTOCOL_HPP
#define SND_FORMAT_LIVELINK_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>

#include "snd/ext/types.hpp"
#include "snd/types.h"

namespace snd::livelink {

inline constexpr std::uint32_t kMagic = Fourcc('S', 'N', 'L', 'L');
inline constexpr std::uint16_t kProtocolMajor = 1;  // mismatch -> connection rejected
inline constexpr std::uint16_t kProtocolMinor = 1;  // additive changes only (1.1: see the 1.1 section below)
inline constexpr std::uint32_t kMaxPayloadBytes = 16u << 20;
inline constexpr std::uint16_t kDefaultPort = 27460;

struct FrameHeader {
  std::uint32_t magic;         // kMagic
  std::uint16_t type;          // MessageType
  std::uint16_t flags;         // FrameFlags
  std::uint32_t payloadBytes;  // <= kMaxPayloadBytes
  std::uint32_t seq;           // sender sequence; replies echo it in Ack
};
static_assert(sizeof(FrameHeader) == 16);

enum FrameFlags : std::uint16_t {
  kFrameWantsAck = 1u << 0,  // receiver answers with Ack(seq)
};

enum class MessageType : std::uint16_t {
  // both directions
  Hello = 0x0001,
  HelloReply = 0x0002,
  SyncDecision = 0x0003,
  Ping = 0x0004,
  Pong = 0x0005,
  Ack = 0x0006,
  Error = 0x0007,
  // tool -> game
  SetValue = 0x0010,
  ReplaceChunk = 0x0011,
  Audition = 0x0012,
  OverrideParam = 0x0013,
  OverrideState = 0x0014,
  ClearOverrides = 0x0015,
  SetProfilerMask = 0x0016,
  RecordControl = 0x0017,
  StructEdit = 0x0018,
  // tool -> game, protocol 1.1 (a 1.0 game skips them: send only when HelloReply.protocolMinor >= 1)
  RevertValue = 0x0019,
  ReadCell = 0x001A,
  QueryBanks = 0x001B,
  ReloadBank = 0x001C,
  // game -> tool
  VoiceList = 0x0100,
  BlockTiming = 0x0101,
  MemoryUsage = 0x0102,
  StreamStall = 0x0103,
  LodChange = 0x0104,
  EventLog = 0x0105,
  BusMeters = 0x0106,
  RecordData = 0x0107,
  // game -> tool, protocol 1.1 (replies to the 1.1 requests, or profiler streams a 1.1 tool asks for)
  CellValue = 0x0108,
  BankList = 0x0109,
  StreamList = 0x010A,
  BlockDetail = 0x010B,
  ReloadStatus = 0x010C,
};

// --- Handshake (11.2 接続時の照合) --------------------------------------------
enum class Role : std::uint32_t { Tool = 1, Game = 2 };

struct Hello {  // tool -> game, first message
  std::uint16_t protocolMajor;
  std::uint16_t protocolMinor;
  std::uint32_t role;              // Role::Tool
  std::uint64_t projectBuildHash;  // hash of the tool's current project build
  std::uint32_t contractVersion;   // SND_CONTRACT_VERSION of the tool
  std::uint32_t capabilities;      // reserved, 0
  char name[32];                   // UTF-8, NUL padded
};
static_assert(sizeof(Hello) == 56);

enum class HelloStatus : std::uint32_t {
  Accepted = 0,          // build hashes match
  BuildMismatch = 1,     // tool must answer SyncDecision (send diffs or disconnect)
  RejectedProtocol = 2,  // major mismatch: game closes after this reply
};

struct HelloReply {  // game -> tool
  std::uint16_t protocolMajor;
  std::uint16_t protocolMinor;
  std::uint32_t status;            // HelloStatus
  std::uint64_t runtimeBuildHash;  // projectBuildHash of the loaded banks (combined)
  std::uint32_t contractVersion;
  std::uint32_t sampleRate;
  std::uint32_t blockSize;
  std::uint32_t platform;          // bank_format kPlatform*
};
static_assert(sizeof(HelloReply) == 32);

enum class SyncMode : std::uint32_t { SendDiffs = 0, Disconnect = 1 };
struct SyncDecision {  // tool -> game after BuildMismatch
  std::uint32_t mode;  // SyncMode
  std::uint32_t reserved;
};
static_assert(sizeof(SyncDecision) == 8);

struct Ping {
  std::uint64_t senderTimeNs;
};
struct Pong {
  std::uint64_t senderTimeNs;     // echoed
  std::uint64_t gameClockSample;  // common clock at reply
};
static_assert(sizeof(Ping) == 8 && sizeof(Pong) == 16);

struct Ack {
  std::uint32_t seq;
  std::int32_t result;  // snd_result
};
static_assert(sizeof(Ack) == 8);

struct Error {
  std::int32_t code;  // snd_result
  std::uint32_t seq;  // offending message, 0 if none
  char message[56];   // UTF-8, NUL padded
};
static_assert(sizeof(Error) == 64);

// --- Tool -> game ------------------------------------------------------------
enum class ValueType : std::uint32_t { F32 = 0, U32 = 1, I32 = 2, Id = 3 };

// Value change: GUID -> runtime id + field + value (11.2). Fields are the
// bank column of the object's table: field = (chunk fourcc, column id).
struct SetValue {
  std::uint64_t guidLo;
  std::uint64_t guidHi;
  std::uint32_t runtimeId;
  std::uint32_t chunk;      // bank_format chunk fourcc of the edited table (EVNT, BUSS, ...)
  std::uint32_t column;     // bank_format column id in that chunk
  std::uint32_t valueType;  // ValueType
  std::uint32_t index;      // row offset for list-valued objects (e.g. 3rd effect param), else 0
  std::uint32_t reserved;
  std::uint64_t value;      // raw bits (low 32 bits for 32-bit types)
};
static_assert(sizeof(SetValue) == 48);

// Rebuilt chunk replacement; `bytes` of chunk data follow (64-byte aligned
// content, same layout as in a bank). Large replacements go through the
// normal load path and epoch release (11.2 大きな差し替え).
struct ReplaceChunk {
  std::uint32_t bankId;
  std::uint32_t fourcc;
  std::uint64_t bytes;
};
static_assert(sizeof(ReplaceChunk) == 16);

enum AuditionFlags : std::uint32_t {
  kAuditionStopAll = 1u << 0,
  kAuditionAtPosition = 1u << 1,
};
struct Audition {
  std::uint32_t eventId;
  std::uint32_t flags;  // AuditionFlags
  float position[3];
  std::uint32_t reserved;
};
static_assert(sizeof(Audition) == 24);

struct OverrideParam {
  std::uint32_t paramId;
  std::uint32_t enable;  // 0 removes the override
  float value;
  std::uint32_t reserved;
};
static_assert(sizeof(OverrideParam) == 16);

struct OverrideState {
  std::uint32_t musicEventId;
  std::uint32_t stateId;
  std::uint32_t enable;
  std::uint32_t reserved;
};
static_assert(sizeof(OverrideState) == 16);

enum ProfilerMask : std::uint32_t {
  kProfVoices = 1u << 0,
  kProfTiming = 1u << 1,
  kProfMemory = 1u << 2,
  kProfStreams = 1u << 3,
  kProfLod = 1u << 4,
  kProfEventLog = 1u << 5,
  kProfMeters = 1u << 6,
  // 1.1 (a 1.0 game refuses these bits: a tool sets them only when HelloReply.protocolMinor >= 1)
  kProfStreamList = 1u << 7,   // StreamList at the meter rate
  kProfBlockDetail = 1u << 8,  // BlockDetail (worst block of each timing interval)
};
struct SetProfilerMask {
  std::uint32_t mask;     // ProfilerMask
  std::uint32_t meterHz;  // 0 = Config.liveLink.meterHz
};
static_assert(sizeof(SetProfilerMask) == 8);

struct RecordControl {
  std::uint32_t start;  // 1 start recording (11.3), 0 stop
  std::uint32_t reserved;
};
static_assert(sizeof(RecordControl) == 8);

// Structural edit within Config.devReserve (11.2). `argCount` u64 args follow.
enum class StructOp : std::uint32_t { AddBus = 0, RemoveBus = 1, AddEffect = 2, RemoveEffect = 3, AddSend = 4 };
struct StructEdit {
  std::uint32_t op;        // StructOp
  std::uint32_t targetId;  // bus id
  std::uint32_t argCount;
  std::uint32_t reserved;
};
static_assert(sizeof(StructEdit) == 16);

// --- Game -> tool ----------------------------------------------------------------
// List messages: ListHeader then `count` elements of `elemBytes` each. Readers
// use min(elemBytes, sizeof(known element)) so elements may grow (minor).
struct ListHeader {
  std::uint32_t count;
  std::uint32_t elemBytes;
};
static_assert(sizeof(ListHeader) == 8);

struct VoiceInfo {  // VoiceList element
  std::uint32_t eventId;
  std::uint32_t playbackSlot;
  std::uint32_t playbackState;  // snd_playback_state
  float audibility;
  std::uint32_t isVirtual;
  std::uint32_t busId;
  float gain;
  std::uint32_t sourceKind;  // SourceKind
};
static_assert(sizeof(VoiceInfo) == 32);

struct BlockTiming {
  std::uint64_t blockIndex;
  float logicMicroseconds;
  float renderMicroseconds;
  float budgetMicroseconds;
  std::uint32_t lodLevel;
  std::uint32_t lateChunks;
  std::uint32_t underruns;
};
static_assert(sizeof(BlockTiming) == 32);

struct MemoryUsage {
  std::uint64_t used[SND_MEM_TAG_COUNT];
  std::uint64_t budget[SND_MEM_TAG_COUNT];
};
static_assert(sizeof(MemoryUsage) == 192);

struct StreamStall {
  std::uint32_t eventId;
  std::uint32_t streamSlot;
  std::uint64_t clockSample;
};
static_assert(sizeof(StreamStall) == 16);

struct LodChange {
  std::uint32_t newLevel;
  std::uint32_t oldLevel;
  std::uint64_t blockIndex;
};
static_assert(sizeof(LodChange) == 16);

struct LogEntry {  // EventLog element
  std::uint64_t clockSample;
  std::uint32_t kind;  // notify::EventType or command type | 0x8000
  std::uint32_t id;    // event / marker / param id
  std::uint32_t handleSlot;
  std::uint32_t detail;
};
static_assert(sizeof(LogEntry) == 24);

struct MeterEntry {  // BusMeters element
  std::uint32_t busId;
  std::uint32_t channels;
  float peak[SND_MAX_CHANNELS];
  float rms[SND_MAX_CHANNELS];
};
static_assert(sizeof(MeterEntry) == 72);

struct RecordData {  // `bytes` of replay data follow
  std::uint64_t offset;
  std::uint64_t bytes;
};
static_assert(sizeof(RecordData) == 16);

// --- Replay records (11.3): content of RecordData and of replay files ---------------
// Records are 8-byte aligned and follow each other; Arena records are followed
// by `bytes` of arena content (normal region, reserve region, fallback; see
// snd/internal/commands.hpp). Replay files are platform specific (commands
// contain pointers); BankMap records let the player re-resolve prepared banks.
enum class RecordKind : std::uint32_t { Arena = 1, Lod = 2, Stall = 3, BankMap = 4 };
struct RecordArena {
  std::uint32_t kind;  // RecordKind::Arena
  std::uint32_t producer;
  std::uint64_t appliedBlock;
  std::uint32_t bytes;
  std::uint32_t reserved;
};
struct RecordLod {
  std::uint32_t kind;  // RecordKind::Lod
  std::uint32_t level;
  std::uint64_t block;
};
struct RecordStall {
  std::uint32_t kind;  // RecordKind::Stall
  std::uint32_t streamSlot;
  std::uint64_t block;
};
struct RecordBankMap {
  std::uint32_t kind;  // RecordKind::BankMap
  std::uint32_t bankId;
  std::uint64_t contentHash;
};
static_assert(sizeof(RecordArena) == 24 && sizeof(RecordLod) == 16);
static_assert(sizeof(RecordStall) == 16 && sizeof(RecordBankMap) == 16);

// --- Protocol 1.1 (additive; spec/interface/livelink-protocol.md §8) -------------------
// Tool -> game.
// RevertValue / ReadCell carry a SetValue (same addressing: runtimeId, chunk, column,
// index; `valueType` and `value` are ignored). RevertValue puts the cell(s) back to the
// bank value the override layer remembered (Ack: applied result). ReadCell answers with
// CellValue (and Ack when asked).
// QueryBanks (no payload) answers with BankList; allowed during the handshake once
// HelloReply was sent (BuildMismatch: before SyncDecision).
// ReloadBank: `pathBytes` of UTF-8 path follow (relative to the game's file system root,
// D-103; no NUL). Progress: ReloadStatus (same seq); the Ack carries the final result.
enum ReloadFlags : std::uint32_t {
  kReloadLoadIfAbsent = 1u << 0,  // the bank is not loaded: just load it (else NOT_FOUND)
};
struct ReloadBank {
  std::uint32_t bankId;      // bank to replace (bankId + languageId, as loaded)
  std::uint32_t languageId;  // 0 = not a language bank
  std::uint32_t flags;       // ReloadFlags
  std::uint32_t pathBytes;   // <= kMaxReloadPathBytes, trailing data
};
static_assert(sizeof(ReloadBank) == 16);
inline constexpr std::uint32_t kMaxReloadPathBytes = 1024;

// Game -> tool.
enum CellFlags : std::uint32_t {
  kCellOverridden = 1u << 0,  // the override layer holds an edit of the (first) cell
  kCellsDiffer = 1u << 1,     // the banks holding the object do not all hold the same value
};
struct CellValue {  // answer to ReadCell (frame seq = the ReadCell's seq)
  std::uint32_t requestSeq;
  std::int32_t result;    // snd_result (NOT_FOUND: no such cell)
  std::uint32_t cells;    // banks holding the cell
  std::uint32_t flags;    // CellFlags
  std::uint64_t current;  // raw bits of the first cell now
  std::uint64_t original; // bank value (the remembered original when overridden, else current)
};
static_assert(sizeof(CellValue) == 32);

struct BankEntry {  // BankList element (answer to QueryBanks; frame seq = its seq)
  std::uint32_t bankId;
  std::uint32_t languageId;
  std::uint64_t projectBuildHash;
  std::uint64_t contentHash;
  std::uint32_t writable;  // 1 = dev override layer (SetValue / ReplaceChunk possible)
  std::uint32_t slot;      // bank handle slot (distinguishes two registrations)
};
static_assert(sizeof(BankEntry) == 32);

enum class StreamState : std::uint32_t { Idle = 0, Active = 1 };
struct StreamInfo {  // StreamList element (one per stream slot, Config.max_streams)
  std::uint32_t slot;
  std::uint32_t eventId;  // event of the playback whose voice reads it (0 = none)
  std::uint32_t state;    // StreamState
  std::uint32_t stalls;   // stalls of this slot since the live link runtime was created
};
static_assert(sizeof(StreamInfo) == 16);

struct BlockDetail {  // CPU detail of the worst block of a timing interval
  std::uint64_t blockIndex;
  float totalMicroseconds;
  float logicMicroseconds;
  float renderMicroseconds;
  float budgetMicroseconds;  // same definition as BlockTiming (§6)
  std::uint32_t renderedVoices;
  std::uint32_t chunks;
  std::uint32_t lateChunks;  // of that block
  std::uint32_t heldVoices;
  std::uint32_t parallel;    // 1 = the RT worker pool rendered it
  std::uint32_t lodLevel;
};
static_assert(sizeof(BlockDetail) == 48);

enum class ReloadPhase : std::uint32_t { Unloading = 1, Loading = 2, Done = 3, Failed = 4 };
struct ReloadStatus {  // frame seq = the ReloadBank's seq
  std::uint32_t requestSeq;
  std::uint32_t phase;   // ReloadPhase
  std::int32_t result;   // snd_result (Failed: why)
  std::uint32_t bankId;
  std::uint64_t bank;    // snd_bank_handle of the new registration (Loading / Done), else 0
};
static_assert(sizeof(ReloadStatus) == 24);

// Replay records of 1.1 (kinds after RecordKind::BankMap). They are constants, not
// RecordKind enumerators, so that switches over RecordKind elsewhere stay exhaustive;
// RecordItem::kind holds them as static_cast<RecordKind>(value). A 1.0 reader stops at
// them (unknown kind).
inline constexpr std::uint32_t kRecordLiveData = 5;  // data of a LiveReplaceChunk in the next Arena
inline constexpr std::uint32_t kRecordBegin = 6;     // how the recording started
inline constexpr std::uint32_t kRecordCell = 7;      // override-layer cell at the start
inline constexpr std::uint32_t kRecordOverride = 8;  // live override at the start
struct RecordLiveData {  // followed by `bytes` of data, padded to 8
  std::uint32_t kind;    // kRecordLiveData
  std::uint32_t fourcc;  // chunk fourcc, or kStructEditTag
  std::uint64_t key;     // LiveReplaceChunk::prepared of the recording process (pairing key)
  std::uint32_t bankId;
  std::uint32_t bytes;
};
static_assert(sizeof(RecordLiveData) == 24);
enum class RecordOrigin : std::uint32_t { FromCreation = 1, MidRun = 2 };
struct RecordBegin {  // 16 bytes like the 1.0 small records; RecordCell / RecordOverride follow it
  std::uint32_t kind;       // kRecordBegin
  std::uint16_t origin;     // RecordOrigin
  std::uint16_t lostState;  // starting-state items that were not written (saturates at 65535)
  std::uint64_t block;      // first recorded block
};
static_assert(sizeof(RecordBegin) == 16);
struct RecordCell {
  std::uint32_t kind;  // kRecordCell
  std::uint32_t bankId;
  std::uint32_t fourcc;
  std::uint32_t column;
  std::uint32_t row;
  std::uint32_t reserved;
  std::uint64_t value;     // value at the start
  std::uint64_t original;  // bank value (what a revert restores)
};
static_assert(sizeof(RecordCell) == 40);
struct RecordOverride {
  std::uint32_t kind;          // kRecordOverride
  std::uint32_t overrideKind;  // cmd::LiveOverrideKind
  std::uint32_t id;
  std::uint32_t value;         // float bits for Param, id otherwise
  std::uint64_t target;        // HandleRef value (MusicState), 0 otherwise
};
static_assert(sizeof(RecordOverride) == 24);
// ReplaceChunk fourcc tag of a structural edit travelling as LiveReplaceChunk (live link
// internal; never a bank chunk).
inline constexpr std::uint32_t kStructEditTag = Fourcc('L', 'S', 'E', 'D');

// Fixed payload size per type (0 = variable: list or trailing data).
constexpr std::uint32_t FixedPayloadBytes(MessageType t) noexcept {
  switch (t) {
    case MessageType::Hello: return sizeof(Hello);
    case MessageType::HelloReply: return sizeof(HelloReply);
    case MessageType::SyncDecision: return sizeof(SyncDecision);
    case MessageType::Ping: return sizeof(Ping);
    case MessageType::Pong: return sizeof(Pong);
    case MessageType::Ack: return sizeof(Ack);
    case MessageType::Error: return sizeof(Error);
    case MessageType::SetValue: return sizeof(SetValue);
    case MessageType::ReplaceChunk: return sizeof(ReplaceChunk);  // + data
    case MessageType::Audition: return sizeof(Audition);
    case MessageType::OverrideParam: return sizeof(OverrideParam);
    case MessageType::OverrideState: return sizeof(OverrideState);
    case MessageType::ClearOverrides: return 0;
    case MessageType::SetProfilerMask: return sizeof(SetProfilerMask);
    case MessageType::RecordControl: return sizeof(RecordControl);
    case MessageType::StructEdit: return sizeof(StructEdit);  // + args
    case MessageType::VoiceList: return sizeof(ListHeader);   // + elements
    case MessageType::BlockTiming: return sizeof(BlockTiming);
    case MessageType::MemoryUsage: return sizeof(MemoryUsage);
    case MessageType::StreamStall: return sizeof(StreamStall);
    case MessageType::LodChange: return sizeof(LodChange);
    case MessageType::EventLog: return sizeof(ListHeader);
    case MessageType::BusMeters: return sizeof(ListHeader);
    case MessageType::RecordData: return sizeof(RecordData);
    case MessageType::RevertValue: return sizeof(SetValue);
    case MessageType::ReadCell: return sizeof(SetValue);
    case MessageType::QueryBanks: return 0;
    case MessageType::ReloadBank: return sizeof(ReloadBank);  // + path
    case MessageType::CellValue: return sizeof(CellValue);
    case MessageType::BankList: return sizeof(ListHeader);
    case MessageType::StreamList: return sizeof(ListHeader);
    case MessageType::BlockDetail: return sizeof(BlockDetail);
    case MessageType::ReloadStatus: return sizeof(ReloadStatus);
  }
  return 0;
}

// Minimum payload a receiver must see for a known type (the fixed part; a
// newer minor may send more). Unknown types are skipped by payloadBytes.
constexpr bool IsKnown(std::uint16_t type) noexcept {
  switch (static_cast<MessageType>(type)) {
    case MessageType::Hello: case MessageType::HelloReply: case MessageType::SyncDecision:
    case MessageType::Ping: case MessageType::Pong: case MessageType::Ack: case MessageType::Error:
    case MessageType::SetValue: case MessageType::ReplaceChunk: case MessageType::Audition:
    case MessageType::OverrideParam: case MessageType::OverrideState: case MessageType::ClearOverrides:
    case MessageType::SetProfilerMask: case MessageType::RecordControl: case MessageType::StructEdit:
    case MessageType::VoiceList: case MessageType::BlockTiming: case MessageType::MemoryUsage:
    case MessageType::StreamStall: case MessageType::LodChange: case MessageType::EventLog:
    case MessageType::BusMeters: case MessageType::RecordData:
    case MessageType::RevertValue: case MessageType::ReadCell: case MessageType::QueryBanks:
    case MessageType::ReloadBank: case MessageType::CellValue: case MessageType::BankList:
    case MessageType::StreamList: case MessageType::BlockDetail: case MessageType::ReloadStatus:
      return true;
  }
  return false;
}

// 1.1: the protocol minor that introduced a message type (0 for the 1.0 types). A sender
// sends a type only when the peer's minor (Hello / HelloReply) is at least this.
constexpr std::uint16_t MinorOf(MessageType t) noexcept {
  switch (t) {
    case MessageType::RevertValue: case MessageType::ReadCell: case MessageType::QueryBanks:
    case MessageType::ReloadBank: case MessageType::CellValue: case MessageType::BankList:
    case MessageType::StreamList: case MessageType::BlockDetail: case MessageType::ReloadStatus:
      return 1;
    default:
      return 0;
  }
}
// Profiler mask bits a game of protocol minor `minor` accepts.
constexpr std::uint32_t ProfilerMaskOf(std::uint16_t minor) noexcept {
  const std::uint32_t v10 = kProfVoices | kProfTiming | kProfMemory | kProfStreams | kProfLod | kProfEventLog | kProfMeters;
  return minor >= 1u ? (v10 | kProfStreamList | kProfBlockDetail) : v10;
}

}  // namespace snd::livelink

#endif  // SND_FORMAT_LIVELINK_PROTOCOL_HPP
