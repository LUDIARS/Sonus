// Bank binary format (design.md 10.2). FROZEN CONTRACT.
//
// Byte-level specification: spec/interface/bank-format.md. This header is the
// normative structure definition; the md file explains it. Both must agree
// (contract/test/format_test.cpp checks offsets and sizes).
//
// Principles:
//  - Little endian, no pointers: every reference is an offset from the start
//    of the file (u64) or a row index into a table chunk (u32). A bank loaded
//    to a 64-byte aligned address is usable without fix-ups.
//  - Every chunk starts at a multiple of 64 bytes; every column array inside a
//    table chunk starts at a multiple of 64 bytes.
//  - Table chunks are SoA: a column directory followed by one array per column.
//    Columns are identified by a small id; unknown columns are ignored,
//    missing optional columns take their documented default.
#ifndef SND_FORMAT_BANK_FORMAT_HPP
#define SND_FORMAT_BANK_FORMAT_HPP

#include <cstddef>
#include <cstdint>

#include "snd/ext/types.hpp"

namespace snd::bank_format {

// ---------------------------------------------------------------------------
// Versions and identification
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kMagic = Fourcc('S', 'N', 'D', 'B');
inline constexpr std::uint16_t kFormatMajor = 2;  // incompatible change (2.0: header checksum over [0,128))
inline constexpr std::uint16_t kFormatMinor = 0;  // additive change (new chunk / column / enum value)
inline constexpr std::uint8_t kLittleEndian = 1;
inline constexpr std::uint8_t kAlignLog2 = 6;  // 64 bytes
inline constexpr std::uint32_t kAlignment = 1u << kAlignLog2;
inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;  // "no row" / "no string"

// Platform tags (10.3 per-platform conversion). Any = platform independent data.
inline constexpr std::uint32_t kPlatformAny = Fourcc('A', 'N', 'Y', '_');
inline constexpr std::uint32_t kPlatformWindows = Fourcc('W', 'I', 'N', '_');
inline constexpr std::uint32_t kPlatformLinux = Fourcc('L', 'N', 'X', '_');
inline constexpr std::uint32_t kPlatformMac = Fourcc('M', 'A', 'C', '_');
inline constexpr std::uint32_t kPlatformIos = Fourcc('I', 'O', 'S', '_');
inline constexpr std::uint32_t kPlatformAndroid = Fourcc('A', 'N', 'D', '_');

enum FileFlags : std::uint32_t {
  kFileDevBuild = 1u << 0,      // contains GUID chunk, tables meant to be copied for overrides
  kFileHasStrings = 1u << 1,    // STRS/NAME present (strippable in release)
  kFileLanguageBank = 1u << 2,  // voice-language bank (10.4); languageId valid
};

// ---------------------------------------------------------------------------
// File header (128 bytes at offset 0)
// ---------------------------------------------------------------------------
struct FileHeader {
  std::uint32_t magic;            //   0 'SNDB'
  std::uint16_t formatMajor;      //   4
  std::uint16_t formatMinor;      //   6
  std::uint32_t platform;         //   8 kPlatform*
  std::uint8_t endianness;        //  12 kLittleEndian
  std::uint8_t alignLog2;         //  13 6
  std::uint16_t headerBytes;      //  14 sizeof(FileHeader) = 128
  std::uint32_t flags;            //  16 FileFlags
  std::uint32_t bankId;           //  20 HashId(bank name)
  std::uint64_t fileBytes;        //  24 total file size
  std::uint64_t projectBuildHash; //  32 live link handshake (11.2)
  std::uint64_t contentHash;      //  40 differential build cache key (10.3)
  std::uint32_t checksum;         //  48 CRC-32C of bytes [headerBytes, fileBytes)
  std::uint32_t headerChecksum;   //  52 HeaderChecksum(): CRC-32C of [0, 128) with this field read as 0
  std::uint32_t dependencyCount;  //  56
  std::uint32_t chunkCount;       //  60
  std::uint64_t dependencyOffset; //  64 -> Dependency[dependencyCount]
  std::uint64_t chunkTableOffset; //  72 -> ChunkEntry[chunkCount]
  std::uint32_t languageId;       //  80 HashId(language) or 0
  std::uint32_t reserved0;        //  84 must be 0
  std::uint8_t reserved[40];      //  88..127 must be 0
};
static_assert(sizeof(FileHeader) == 128);
static_assert(offsetof(FileHeader, checksum) == 48);
static_assert(offsetof(FileHeader, dependencyOffset) == 64);
static_assert(offsetof(FileHeader, languageId) == 80);

struct Dependency {
  std::uint32_t bankId;
  std::uint32_t reserved;
  std::uint64_t buildHash;  // contentHash of the dependency this bank was built against
};
static_assert(sizeof(Dependency) == 16);

enum ChunkFlags : std::uint16_t {
  kChunkTable = 1u << 0,       // TableChunkHeader layout; otherwise an opaque blob
  kChunkStrippable = 1u << 1,  // may be removed for release (strings, names)
  kChunkDevOnly = 1u << 2,     // present only in dev builds (GUID map)
  kChunkApplication = 1u << 3, // readable through snd_get_bank_chunk (e.g. PHYS)
};

struct ChunkEntry {
  std::uint32_t fourcc;   //  0
  std::uint16_t version;  //  4 per-chunk layout version (1 for all chunks below)
  std::uint16_t flags;    //  6 ChunkFlags
  std::uint32_t reserved0;//  8
  std::uint32_t reserved1;// 12
  std::uint64_t offset;   // 16 from file start, multiple of 64
  std::uint64_t bytes;    // 24
};
static_assert(sizeof(ChunkEntry) == 32);

// ---------------------------------------------------------------------------
// Table chunks (SoA)
// ---------------------------------------------------------------------------
struct TableChunkHeader {
  std::uint32_t rowCount;
  std::uint32_t columnCount;
  std::uint32_t reserved0;
  std::uint32_t reserved1;
  // followed by ColumnEntry[columnCount]
};
static_assert(sizeof(TableChunkHeader) == 16);

struct ColumnEntry {
  std::uint16_t columnId;  // per table, see SND_BANK_COLUMNS
  std::uint16_t elemBytes; // must equal the element size of the column type
  std::uint32_t reserved;
  std::uint64_t offset;    // from chunk start, multiple of 64; rowCount * elemBytes bytes
};
static_assert(sizeof(ColumnEntry) == 16);

enum class ColumnType : std::uint8_t { U32 = 0, F32 = 1, U64 = 2, I64 = 3 };
constexpr std::uint32_t ColumnTypeBytes(ColumnType t) noexcept {
  return (t == ColumnType::U32 || t == ColumnType::F32) ? 4u : 8u;
}

// Chunk ids --------------------------------------------------------------------
#define SND_BANK_CHUNKS(K)                                                          \
  /* fourcc, kind (Table/Blob), flags, meaning */                                   \
  K(EVNT, Table, 0, "events (10.1, 4.2)")                                           \
  K(TRAK, Table, 0, "tracks of events")                                             \
  K(CLIP, Table, 0, "clips on tracks")                                              \
  K(ACTN, Table, 0, "action track items (4.2)")                                     \
  K(BUSS, Table, 0, "buses in graph order (4.6)")                                   \
  K(SEND, Table, 0, "bus sends")                                                    \
  K(EFFX, Table, 0, "effect instances")                                             \
  K(EFPV, Table, 0, "effect initial parameter values")                              \
  K(EFBL, Blob, 0, "effect blobs (impulse responses ...)")                          \
  K(PARM, Table, 0, "parameter definitions (4.3)")                                  \
  K(CURV, Table, 0, "curves")                                                       \
  K(CPNT, Table, 0, "curve points")                                                 \
  K(CATG, Table, 0, "categories (4.6)")                                             \
  K(SNAP, Table, 0, "snapshots (4.6)")                                              \
  K(SNPV, Table, 0, "snapshot values")                                              \
  K(VLGR, Table, 0, "voice limit groups (4.4)")                                     \
  K(DUCK, Table, 0, "ducking rules (4.6)")                                          \
  K(ASST, Table, 0, "waveform assets (6.2)")                                        \
  K(WAVE, Blob, 0, "resident waveform data, each asset 64-byte aligned")            \
  K(PREL, Blob, 0, "stream / music entry preload chunks (6.1, 8.3)")                \
  K(SEEK, Table, 0, "seek tables (6.3)")                                            \
  K(STRM, Table, 0, "external stream files")                                        \
  K(SPTH, Blob, 0, "stream file paths, NUL-terminated UTF-8 (not strippable)")      \
  K(SYNP, Table, 0, "synthesised source parameters (5)")                            \
  K(PTCH, Blob, 0, "patch programs (5.1, stage 6)")                                 \
  K(DSPN, Blob, 0, "DSP instance pool requirement (3.3): DspPoolNeed")              \
  K(MSEG, Table, 0, "music segments (8.3)")                                         \
  K(MPLS, Table, 0, "music playlists")                                              \
  K(MPLE, Table, 0, "music playlist entries")                                       \
  K(MSTM, Table, 0, "music stems")                                                  \
  K(MSTA, Table, 0, "music states")                                                 \
  K(MTRN, Table, 0, "music transition rules in match order (8.5)")                  \
  K(MSTG, Table, 0, "music stingers")                                               \
  K(MMRK, Table, 0, "music markers")                                                \
  K(MTMP, Table, 0, "tempo map points")                                             \
  K(PHYS, Blob, kChunkApplication, "physics material tables (9): PhysChunkHeader")  \
  K(STRS, Blob, kChunkStrippable, "string pool, NUL-terminated UTF-8")              \
  K(NAME, Table, kChunkStrippable, "id -> name (and rename aliases, 10.1)")         \
  K(GUID, Table, kChunkDevOnly, "id -> GUID (dev builds, 11.2)")                    \
  K(ALIS, Table, 0, "rename aliases: old id -> current id (10.1, not strippable)")

namespace chunk {
#define SND_BANK_CHUNK_ID(id, kind, flags, doc) \
  inline constexpr std::uint32_t id = Fourcc(#id[0], #id[1], #id[2], #id[3]);
SND_BANK_CHUNKS(SND_BANK_CHUNK_ID)
#undef SND_BANK_CHUNK_ID
}  // namespace chunk

enum class ChunkKind : std::uint8_t { Table = 0, Blob = 1 };
struct ChunkInfo {
  std::uint32_t fourcc;
  ChunkKind kind;
  std::uint16_t requiredFlags;
  const char* doc;
};
inline constexpr ChunkInfo kChunks[] = {
#define SND_BANK_CHUNK_INFO(id, kind, flags, doc) \
  ChunkInfo{chunk::id, ChunkKind::kind, static_cast<std::uint16_t>(flags), doc},
    SND_BANK_CHUNKS(SND_BANK_CHUNK_INFO)
#undef SND_BANK_CHUNK_INFO
};

// ---------------------------------------------------------------------------
// Columns: COL(chunk, name, id, type, required, defaultBits)
// Row references are U32 row indices (kNone = none) into the named table of
// the SAME bank unless stated otherwise. "id" columns hold runtime ids (10.1).
// Times are milliseconds (f32), levels dB (f32), pitch cents (f32).
// ---------------------------------------------------------------------------
#define SND_BANK_COLUMNS(COL)                                                       \
  /* EVNT ------------------------------------------------------------------ */  \
  COL(EVNT, id, 0, U32, 1, 0)                                                     \
  COL(EVNT, trackFirst, 1, U32, 1, 0)                                             \
  COL(EVNT, trackCount, 2, U32, 1, 0)                                             \
  COL(EVNT, sequenceKind, 3, U32, 1, 0) /* SequenceKind */                        \
  COL(EVNT, priority, 4, U32, 1, 128)   /* 0..255, higher wins */                 \
  COL(EVNT, bus, 5, U32, 1, 0)          /* BUSS row */                            \
  COL(EVNT, category, 6, U32, 0, kNone) /* CATG row */                            \
  COL(EVNT, limitGroup, 7, U32, 0, kNone) /* VLGR row */                          \
  COL(EVNT, maxInstances, 8, U32, 0, 0) /* 0 = unlimited */                       \
  COL(EVNT, retriggerMs, 9, F32, 0, 0)                                            \
  COL(EVNT, flags, 10, U32, 1, 0)       /* EventFlags */                          \
  COL(EVNT, curveVolume, 11, U32, 0, kNone) /* CURV rows: distance curves (4.5) */ \
  COL(EVNT, curveLowpass, 12, U32, 0, kNone)                                      \
  COL(EVNT, curveSpread, 13, U32, 0, kNone)                                       \
  COL(EVNT, curveReverbSend, 14, U32, 0, kNone)                                   \
  COL(EVNT, curveBlend3D, 15, U32, 0, kNone)                                      \
  COL(EVNT, maxDistance, 16, F32, 0, 0)                                           \
  COL(EVNT, coneInnerDeg, 17, F32, 0, 0)                                          \
  COL(EVNT, coneOuterDeg, 18, F32, 0, 0)                                          \
  COL(EVNT, coneOuterDb, 19, F32, 0, 0)                                           \
  COL(EVNT, fadeOutMs, 20, F32, 0, 0)                                             \
  COL(EVNT, musicState, 21, U32, 0, kNone) /* MSTA row: initial state of a music event */ \
  COL(EVNT, curveFirst, 22, U32, 0, kNone) /* parameter curves on this event */   \
  COL(EVNT, curveCount, 23, U32, 0, 0)                                            \
  COL(EVNT, comboWindowMs, 24, F32, 0, 0)                                         \
  COL(EVNT, virtualReturn, 25, U32, 0, 0)  /* 0 continue, 1 restart, 2 never (4.4) */ \
  /* TRAK ------------------------------------------------------------------ */  \
  COL(TRAK, event, 0, U32, 1, 0)                                                  \
  COL(TRAK, clipFirst, 1, U32, 1, 0)                                              \
  COL(TRAK, clipCount, 2, U32, 1, 0)                                              \
  COL(TRAK, flags, 3, U32, 0, 0)                                                  \
  COL(TRAK, actionFirst, 4, U32, 0, kNone)                                        \
  COL(TRAK, actionCount, 5, U32, 0, 0)                                            \
  COL(TRAK, layerParam, 6, U32, 0, 0) /* param id for layer crossfade, 0 none */  \
  COL(TRAK, bus, 7, U32, 0, kNone)                                                \
  /* CLIP ------------------------------------------------------------------ */  \
  COL(CLIP, kind, 0, U32, 1, 0)  /* ClipKind */                                   \
  COL(CLIP, target, 1, U32, 1, 0)  /* ASST / EVNT (nested: row > owner) / SYNP */ \
  COL(CLIP, startMs, 2, F32, 0, 0)                                                \
  COL(CLIP, weight, 3, F32, 0, 1)                                                 \
  COL(CLIP, volumeDb, 4, F32, 0, 0)                                               \
  COL(CLIP, pitchCents, 5, F32, 0, 0)                                             \
  COL(CLIP, randVolumeDb, 6, F32, 0, 0)                                           \
  COL(CLIP, randPitchCents, 7, F32, 0, 0)                                         \
  COL(CLIP, randPan, 8, F32, 0, 0)                                                \
  COL(CLIP, randStartMs, 9, F32, 0, 0)                                            \
  COL(CLIP, randFilterHz, 10, F32, 0, 0)                                          \
  COL(CLIP, switchValue, 11, U32, 0, 0)                                           \
  COL(CLIP, layerMin, 12, F32, 0, 0)                                              \
  COL(CLIP, layerMax, 13, F32, 0, 0)                                              \
  COL(CLIP, fadeInMs, 14, F32, 0, 0)                                              \
  COL(CLIP, fadeOutMs, 15, F32, 0, 0)                                             \
  COL(CLIP, loopCount, 16, U32, 0, 1) /* 0 = infinite */                          \
  /* ACTN ------------------------------------------------------------------ */  \
  COL(ACTN, timeMs, 0, F32, 1, 0)                                                 \
  COL(ACTN, action, 1, U32, 1, 0)  /* ActionKind */                               \
  COL(ACTN, target, 2, U32, 1, 0)  /* runtime id */                               \
  COL(ACTN, value, 3, F32, 0, 0)                                                  \
  /* BUSS ------------------------------------------------------------------ */  \
  COL(BUSS, id, 0, U32, 1, 0)                                                     \
  COL(BUSS, parent, 1, U32, 1, kNone) /* row; kNone only for the master bus */    \
  COL(BUSS, sendFirst, 2, U32, 0, kNone)                                          \
  COL(BUSS, sendCount, 3, U32, 0, 0)                                              \
  COL(BUSS, effectFirst, 4, U32, 0, kNone)                                        \
  COL(BUSS, effectCount, 5, U32, 0, 0)                                            \
  COL(BUSS, volumeDb, 6, F32, 0, 0)                                               \
  COL(BUSS, channels, 7, U32, 0, 0) /* 0 = parent's */                            \
  COL(BUSS, flags, 8, U32, 0, 0)    /* BusFlags */                                \
  COL(BUSS, lodPriority, 9, U32, 0, 0)                                            \
  /* SEND ------------------------------------------------------------------ */  \
  COL(SEND, targetBus, 0, U32, 1, 0)                                              \
  COL(SEND, levelDb, 1, F32, 0, 0)                                                \
  COL(SEND, preFader, 2, U32, 0, 0)                                               \
  /* EFFX ------------------------------------------------------------------ */  \
  COL(EFFX, effectType, 0, U32, 1, 0) /* HashId of the kind (standard or plugin) */ \
  COL(EFFX, paramFirst, 1, U32, 0, kNone) /* EFPV row */                          \
  COL(EFFX, paramCount, 2, U32, 0, 0)                                             \
  COL(EFFX, blobOffset, 3, U64, 0, 0) /* within EFBL */                           \
  COL(EFFX, blobBytes, 4, U64, 0, 0)                                              \
  COL(EFFX, stateBytes, 5, U32, 0, 0) /* computed by buildtool for DSPN */        \
  COL(EFFX, flags, 6, U32, 0, 0)                                                  \
  COL(EFPV, value, 0, F32, 1, 0)                                                  \
  /* PARM ------------------------------------------------------------------ */  \
  COL(PARM, id, 0, U32, 1, 0)                                                     \
  COL(PARM, kind, 1, U32, 1, 0)  /* 0 continuous, 1 switch */                     \
  COL(PARM, scopeMask, 2, U32, 0, 7) /* bit0 global, bit1 emitter, bit2 playback */ \
  COL(PARM, minValue, 3, F32, 0, 0)                                               \
  COL(PARM, maxValue, 4, F32, 0, 1)                                               \
  COL(PARM, defaultValue, 5, F32, 0, 0)                                           \
  COL(PARM, rampRate, 6, F32, 0, 0) /* units per second, 0 = immediate */         \
  /* CURV / CPNT ---------------------------------------------------------- */  \
  COL(CURV, pointFirst, 0, U32, 1, 0)                                             \
  COL(CURV, pointCount, 1, U32, 1, 0)                                             \
  COL(CURV, input, 2, U32, 1, 0)   /* param id, or kInputDistance */              \
  COL(CURV, target, 3, U32, 1, 0)  /* CurveTarget */                              \
  COL(CURV, compose, 4, U32, 0, 0) /* CurveCompose (4.3) */                       \
  COL(CPNT, x, 0, F32, 1, 0)                                                      \
  COL(CPNT, y, 1, F32, 1, 0)                                                      \
  COL(CPNT, shape, 2, F32, 0, 0)   /* 0 linear, >0 exponent */                    \
  /* CATG ------------------------------------------------------------------ */  \
  COL(CATG, id, 0, U32, 1, 0)                                                     \
  COL(CATG, parent, 1, U32, 0, kNone)   /* row > own row (2.0) */                  \
  COL(CATG, flags, 2, U32, 0, 0)   /* CategoryFlags */                            \
  COL(CATG, volumeDb, 3, F32, 0, 0)                                               \
  /* SNAP / SNPV ---------------------------------------------------------- */  \
  COL(SNAP, id, 0, U32, 1, 0)                                                     \
  COL(SNAP, valueFirst, 1, U32, 1, 0)                                             \
  COL(SNAP, valueCount, 2, U32, 1, 0)                                             \
  COL(SNPV, targetKind, 0, U32, 1, 0) /* 0 bus volume, 1 effect param, 2 send level */ \
  COL(SNPV, targetRow, 1, U32, 1, 0)                                              \
  COL(SNPV, paramIndex, 2, U32, 0, 0)                                             \
  COL(SNPV, value, 3, F32, 1, 0)                                                  \
  /* VLGR ------------------------------------------------------------------ */  \
  COL(VLGR, id, 0, U32, 1, 0)                                                     \
  COL(VLGR, maxVoices, 1, U32, 1, 0)                                              \
  COL(VLGR, stealMode, 2, U32, 0, 0) /* 0 oldest, 1 quietest, 2 lowest priority, 3 never */ \
  /* DUCK ------------------------------------------------------------------ */  \
  COL(DUCK, kind, 0, U32, 1, 0)      /* 0 sidechain (source = bus row), 1 rule (source = category row) */ \
  COL(DUCK, source, 1, U32, 1, 0)                                                 \
  COL(DUCK, targetBus, 2, U32, 1, 0)                                              \
  COL(DUCK, amountDb, 3, F32, 1, 0)                                               \
  COL(DUCK, attackMs, 4, F32, 0, 10)                                              \
  COL(DUCK, releaseMs, 5, F32, 0, 200)                                            \
  COL(DUCK, thresholdDb, 6, F32, 0, 0)                                            \
  /* ASST ------------------------------------------------------------------ */  \
  COL(ASST, id, 0, U32, 1, 0)                                                     \
  COL(ASST, loadForm, 1, U32, 1, 0)  /* LoadForm (6.2) */                         \
  COL(ASST, codec, 2, U32, 1, 0)     /* SND_CODEC_* fourcc */                     \
  COL(ASST, channels, 3, U32, 1, 1)                                               \
  COL(ASST, sampleRate, 4, U32, 1, 48000)                                         \
  COL(ASST, totalFrames, 5, U64, 1, 0)                                            \
  COL(ASST, preSkip, 6, U32, 0, 0)                                                \
  COL(ASST, preRoll, 7, U32, 0, 0)                                                \
  COL(ASST, framesPerPacket, 8, U32, 0, 0)                                        \
  COL(ASST, blockAlign, 9, U32, 0, 0)                                             \
  COL(ASST, dataOffset, 10, U64, 0, 0)  /* within WAVE (resident forms) */        \
  COL(ASST, dataBytes, 11, U64, 0, 0)                                             \
  COL(ASST, loopStart, 12, U64, 0, 0)                                             \
  COL(ASST, loopEnd, 13, U64, 0, 0)     /* exclusive end; 0 = no loop (6.3) */    \
  COL(ASST, preloadOffset, 14, U64, 0, 0) /* within PREL */                       \
  COL(ASST, preloadBytes, 15, U32, 0, 0)                                          \
  COL(ASST, preloadFrames, 16, U32, 0, 0)                                         \
  COL(ASST, seekFirst, 17, U32, 0, kNone)                                         \
  COL(ASST, seekCount, 18, U32, 0, 0)                                             \
  COL(ASST, stream, 19, U32, 0, kNone)  /* STRM row (Stream form) */              \
  COL(ASST, extraOffset, 20, U64, 0, 0) /* codec setup data within WAVE */        \
  COL(ASST, extraBytes, 21, U32, 0, 0)                                            \
  COL(ASST, flags, 22, U32, 0, 0)       /* AssetFlags */                          \
  COL(SEEK, frame, 0, U64, 1, 0)                                                  \
  COL(SEEK, byteOffset, 1, U64, 1, 0)   /* from the data start; row 0 = {0,0} */  \
  COL(STRM, pathOffset, 0, U32, 1, 0)   /* within SPTH */                         \
  COL(STRM, fileBytes, 1, U64, 1, 0)                                              \
  COL(STRM, dataOffset, 2, U64, 0, 0)   /* first packet, sector aligned */        \
  COL(STRM, sectorAlign, 3, U32, 0, 2048)                                         \
  /* SYNP ------------------------------------------------------------------ */  \
  COL(SYNP, kind, 0, U32, 1, 0)      /* SynthKind */                              \
  COL(SYNP, waveform, 1, U32, 0, 0)  /* Waveform */                               \
  COL(SYNP, frequencyHz, 2, F32, 0, 440)                                          \
  COL(SYNP, duty, 3, F32, 0, 0.5)                                                 \
  COL(SYNP, noiseColor, 4, U32, 0, 0) /* 0 white, 1 pink, 2 brown */              \
  COL(SYNP, asset, 5, U32, 0, kNone) /* wavetable / granular source ASST row */   \
  COL(SYNP, programOffset, 6, U32, 0, 0) /* within PTCH */                        \
  COL(SYNP, programBytes, 7, U32, 0, 0)                                           \
  COL(SYNP, channels, 8, U32, 0, 1)                                               \
  /* Music ------------------------------------------------------------------ */ \
  COL(MSEG, id, 0, U32, 1, 0)                                                     \
  COL(MSEG, asset, 1, U32, 1, 0)       /* ASST row (interleaved stems recommended, 8.4) */ \
  COL(MSEG, tempoFirst, 2, U32, 1, 0)  /* MTMP rows */                            \
  COL(MSEG, tempoCount, 3, U32, 1, 1)                                             \
  COL(MSEG, bars, 4, U32, 1, 1)                                                   \
  COL(MSEG, entryFrame, 5, U64, 0, 0)                                             \
  COL(MSEG, exitFrame, 6, U64, 0, 0)                                              \
  COL(MSEG, markerFirst, 7, U32, 0, kNone)                                        \
  COL(MSEG, markerCount, 8, U32, 0, 0)                                            \
  COL(MSEG, flags, 9, U32, 0, 0)       /* bit0: entry preload chunks present (8.3) */ \
  COL(MPLS, id, 0, U32, 1, 0)                                                     \
  COL(MPLS, entryFirst, 1, U32, 1, 0)                                             \
  COL(MPLS, entryCount, 2, U32, 1, 0)                                             \
  COL(MPLS, mode, 3, U32, 0, 0)        /* 0 sequential, 1 random, 2 shuffle */    \
  COL(MPLS, loopCount, 4, U32, 0, 0)   /* 0 = infinite */                         \
  COL(MPLE, segment, 0, U32, 1, 0)                                                \
  COL(MPLE, weight, 1, F32, 0, 1)                                                 \
  COL(MPLE, repeat, 2, U32, 0, 1)                                                 \
  COL(MSTM, segment, 0, U32, 1, 0)                                                \
  COL(MSTM, channelFirst, 1, U32, 1, 0)                                           \
  COL(MSTM, channelCount, 2, U32, 1, 2)                                           \
  COL(MSTM, volumeCurve, 3, U32, 0, kNone) /* CURV row (param -> volume) */       \
  COL(MSTM, asset, 4, U32, 0, kNone)   /* separate-file stem, synchronised (8.4) */ \
  COL(MSTA, id, 0, U32, 1, 0)                                                     \
  COL(MSTA, playlist, 1, U32, 1, 0)                                               \
  COL(MSTA, musicEvent, 2, U32, 1, 0)  /* EVNT row of the owning music event */   \
  COL(MSTA, flags, 3, U32, 0, 0)                                                  \
  COL(MTRN, musicEvent, 0, U32, 1, 0)  /* EVNT row */                             \
  COL(MTRN, fromState, 1, U32, 1, 0)   /* state id, 0 = wildcard */               \
  COL(MTRN, toState, 2, U32, 1, 0)     /* state id, 0 = wildcard */               \
  COL(MTRN, timing, 3, U32, 1, 2)      /* snd_when_kind */                        \
  COL(MTRN, timingCount, 4, U32, 0, 0)                                            \
  COL(MTRN, fadeOutBeats, 5, F32, 0, 0)                                           \
  COL(MTRN, fadeInBeats, 6, F32, 0, 0)                                            \
  COL(MTRN, bridgeSegment, 7, U32, 0, kNone) /* MSEG row */                       \
  COL(MTRN, startPosition, 8, U32, 0, 0) /* 0 head, 1 same bar, 2 marker, 3 last position */ \
  COL(MTRN, startMarker, 9, U32, 0, 0)                                            \
  COL(MTRN, interrupt, 10, U32, 0, 0)  /* 0 allowed, 1 after bridge completes */  \
  COL(MTRN, lateRule, 11, U32, 0, 0)   /* ReservationLateRule */                  \
  COL(MTRN, flags, 12, U32, 0, 0)      /* bit0: prepare (pre-prime) target */     \
  COL(MSTG, id, 0, U32, 1, 0)                                                     \
  COL(MSTG, event, 1, U32, 1, 0)       /* EVNT row */                             \
  COL(MSTG, timing, 2, U32, 1, 1)                                                 \
  COL(MSTG, timingCount, 3, U32, 0, 0)                                            \
  COL(MSTG, overlap, 4, U32, 0, 0)     /* 0 replace, 1 stack */                   \
  COL(MMRK, id, 0, U32, 1, 0)                                                     \
  COL(MMRK, frame, 1, U64, 1, 0)                                                  \
  COL(MMRK, kind, 2, U32, 0, 0)        /* 0 user, 1 entry, 2 exit, 3 pickup */    \
  COL(MTMP, tick, 0, I64, 1, 0)        /* position of the change in ticks */      \
  COL(MTMP, bpm, 1, F32, 1, 120)                                                  \
  COL(MTMP, beatsPerBar, 2, U32, 0, 4)                                            \
  COL(MTMP, beatUnit, 3, U32, 0, 4)                                               \
  /* NAME / GUID ----------------------------------------------------------- */ \
  COL(NAME, id, 0, U32, 1, 0)                                                     \
  COL(NAME, stringOffset, 1, U32, 1, 0) /* within STRS */                         \
  COL(NAME, kind, 2, U32, 0, 0)        /* NameKind; aliases use kind Alias */     \
  COL(GUID, id, 0, U32, 1, 0)                                                     \
  COL(GUID, guidLo, 1, U64, 1, 0)                                                 \
  COL(GUID, guidHi, 2, U64, 1, 0)                                                 \
  COL(GUID, kind, 3, U32, 0, 0)                                                   \
  /* ALIS (2.0): rename history, kept in release banks. Rows sorted by oldId   */ \
  /* (strictly increasing), oldId != 0, id != 0, oldId != id; id is always the */ \
  /* current id (chains folded by the writer); unknown kind values: ignore.    */ \
  COL(ALIS, oldId, 0, U32, 1, 0)        /* snd_hash_id(old name) */              \
  COL(ALIS, id, 1, U32, 1, 0)           /* current runtime id */                 \
  COL(ALIS, kind, 2, U32, 0, 0)         /* NameKind of the object */

struct ColumnInfo {
  std::uint32_t chunk;
  const char* name;
  std::uint16_t columnId;
  ColumnType type;
  bool required;
  double defaultValue;  // for optional columns (U32/U64 defaults are exact integers)
};

inline constexpr ColumnInfo kColumns[] = {
#define SND_BANK_COLUMN_INFO(ch, nm, id, ty, req, def)                              \
  ColumnInfo{chunk::ch, #ch "." #nm, static_cast<std::uint16_t>(id), ColumnType::ty, \
             (req) != 0, static_cast<double>(def)},
    SND_BANK_COLUMNS(SND_BANK_COLUMN_INFO)
#undef SND_BANK_COLUMN_INFO
};

namespace col {
#define SND_BANK_COLUMN_ID(ch, nm, id, ty, req, def) \
  inline constexpr std::uint16_t ch##_##nm = static_cast<std::uint16_t>(id);
SND_BANK_COLUMNS(SND_BANK_COLUMN_ID)
#undef SND_BANK_COLUMN_ID
}  // namespace col

// Column ids of a table must be dense 0..n-1 in declaration order (checked).
constexpr bool ValidateColumnIds() noexcept {
  const std::size_t n = sizeof(kColumns) / sizeof(kColumns[0]);
  for (std::size_t i = 0; i < n; ++i) {
    std::uint16_t expected = 0;
    for (std::size_t j = 0; j < i; ++j) {
      if (kColumns[j].chunk == kColumns[i].chunk) ++expected;
    }
    if (kColumns[i].columnId != expected) return false;
  }
  return true;
}
static_assert(ValidateColumnIds(), "bank column ids must be dense per chunk");

// ---------------------------------------------------------------------------
// Enumerations stored in columns
// ---------------------------------------------------------------------------
enum class SequenceKind : std::uint32_t {  // 4.2
  Polyphonic = 0,
  Random = 1,
  RandomNoRepeat = 2,
  Sequential = 3,
  Shuffle = 4,
  Switch = 5,
  ComboSequential = 6,
  LayerCrossfade = 7,
};
enum EventFlags : std::uint32_t {
  kEventMusic = 1u << 0,
  kEvent3D = 1u << 1,
  kEventIgnorePause = 1u << 2,  // via category normally; per-event override
  kEventNeedsPriming = 1u << 3, // contains streams without preload
  kEventStinger = 1u << 4,
};
enum class ClipKind : std::uint32_t { Asset = 0, NestedEvent = 1, Synth = 2 };
enum class ActionKind : std::uint32_t { PlayEvent = 0, StopEvent = 1, SetParam = 2, ApplySnapshot = 3 };
enum BusFlags : std::uint32_t {
  kBusMaster = 1u << 0,
  kBusOutput = 1u << 1,         // auxiliary output bus (controller speaker, haptics, 4.6)
  kBusLodBypassable = 1u << 2,  // effects may be bypassed by load LOD (2.4)
  kBusMeter = 1u << 3,          // publish meters / loudness
};
enum CategoryFlags : std::uint32_t {
  kCategoryIgnorePause = 1u << 0,
  kCategoryMuteOnFocusLoss = 1u << 1,
};
enum class CurveTarget : std::uint32_t {
  Volume = 0, Pitch = 1, Lowpass = 2, Highpass = 3, Pan = 4, Send = 5, EffectParam = 6, Spread = 7,
  ReverbSend = 8, Blend3D = 9,
};
enum class CurveCompose : std::uint32_t { AddDb = 0, AddCents = 1, Min = 2, Max = 3, Multiply = 4 };
inline constexpr std::uint32_t kInputDistance = 0xFFFFFFFEu;  // CURV.input special value

enum class LoadForm : std::uint32_t {  // 6.2
  PcmResident = 0,
  CompressedResident = 1,
  DecompressCache = 2,
  Stream = 3,
};
enum AssetFlags : std::uint32_t {
  kAssetStallSoundPriority = 1u << 0,  // 6.3: resume where it stopped (default for voice)
  kAssetVirtualRestart = 1u << 1,
  kAssetVirtualKill = 1u << 2,
  kAssetPriorityMusic = 1u << 3,       // IO priority class (6.1)
  kAssetPriorityVoice = 1u << 4,
};
enum class SynthKind : std::uint32_t { Oscillator = 0, Noise = 1, Granular = 2, Patch = 3, Wavetable = 4 };
enum class Waveform : std::uint32_t { Sine = 0, Saw = 1, Square = 2, Triangle = 3, Pulse = 4 };
enum class NameKind : std::uint32_t { Event = 0, Bus = 1, Param = 2, Category = 3, Snapshot = 4, State = 5, Alias = 6, Other = 7 };

// ---------------------------------------------------------------------------
// Blob layouts
// ---------------------------------------------------------------------------
// Stream file (10.2): separate file referenced by STRM / SPTH. One header
// sector, then the encoded data from dataOffset (= STRM.dataOffset, a multiple
// of sectorAlign); the file is zero padded to a multiple of sectorAlign
// (STRM.fileBytes is the padded size). SEEK.byteOffset counts from dataOffset.
inline constexpr std::uint32_t kStreamMagic = Fourcc('S', 'N', 'D', 'S');
inline constexpr std::uint32_t kStreamVersion = 1;
struct StreamFileHeader {
  std::uint32_t magic;        //  0 kStreamMagic
  std::uint32_t version;      //  4 kStreamVersion
  std::uint32_t assetId;      //  8 ASST.id
  std::uint32_t codec;        // 12 fourcc
  std::uint32_t channels;     // 16
  std::uint32_t sampleRate;   // 20
  std::uint64_t totalFrames;  // 24
  std::uint64_t dataOffset;   // 32 first packet, multiple of sectorAlign
  std::uint64_t dataBytes;    // 40 encoded bytes (before padding)
  std::uint32_t sectorAlign;  // 48
  std::uint32_t reserved0;    // 52 zero
  std::uint64_t reserved1;    // 56 zero
};
static_assert(sizeof(StreamFileHeader) == 64);

struct DspPoolNeed {  // DSPN
  std::uint64_t instanceBytes;  // sum of EFFX.stateBytes, each rounded up to 64
  std::uint32_t instanceCount;
  std::uint32_t reserved;
};
static_assert(sizeof(DspPoolNeed) == 16);

struct PhysChunkHeader {  // PHYS (application chunk, read by the physics module)
  std::uint32_t tableCount;
  std::uint32_t reserved;
  std::uint64_t tablesOffset;  // from chunk start -> PhysTable[tableCount]
};
static_assert(sizeof(PhysChunkHeader) == 16);

struct PhysTable {
  std::uint32_t id;              // table id (snd_phys_desc.table)
  std::uint32_t materialCount;   // N sound materials
  std::uint64_t singleOffset;    // u32 event id [N][4 contact kinds] (from chunk start)
  std::uint64_t pairOffset;      // u32 event id [N*N], 0 = no override; index min*N+max (9.3)
  std::uint64_t footstepOffset;  // u32 event id [N]
  std::uint32_t defaultEvent[4]; // per contact kind
  std::uint64_t curveOffset;     // PhysCurves[N] (strength mapping, 9.3)
  std::uint64_t reserved;
};
static_assert(sizeof(PhysTable) == 64);

struct PhysCurves {
  float impulseToVelocity[4];  // thresholds for weak/medium/strong layers + max impulse
  float massToPitchCents[2];   // at 1 kg / at 100 kg
  float speedToVolumeDb[2];    // slide/roll volume at 0.1 m/s / at 10 m/s
};
static_assert(sizeof(PhysCurves) == 32);

// ---------------------------------------------------------------------------
// Checksum: CRC-32C (Castagnoli), reflected, poly 0x82F63B78, init/xorout
// 0xFFFFFFFF. Reference bitwise implementation; faster implementations must
// produce identical results ("123456789" -> 0xE3069283).
// ---------------------------------------------------------------------------
constexpr std::uint32_t Crc32cUpdate(std::uint32_t crc, const std::uint8_t* data,
                                     std::size_t n) noexcept {
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
  }
  return ~crc;
}
constexpr std::uint32_t Crc32c(const std::uint8_t* data, std::size_t n) noexcept {
  return Crc32cUpdate(0u, data, n);
}
// FileHeader.headerChecksum (2.0): CRC-32C of the 128 header bytes with the
// headerChecksum field itself (bytes [52, 56)) read as zero.
constexpr std::uint32_t HeaderChecksum(const std::uint8_t* header128) noexcept {
  const std::uint8_t zero[4] = {0, 0, 0, 0};
  std::uint32_t c = Crc32cUpdate(0u, header128, 52);
  c = Crc32cUpdate(c, zero, 4);
  return Crc32cUpdate(c, header128 + 56, 72);
}

}  // namespace snd::bank_format

#endif  // SND_FORMAT_BANK_FORMAT_HPP
