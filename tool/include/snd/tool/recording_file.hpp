// Recording files of the tool (design.md 11.3, livelink-protocol.md §7).
//
// A file is a 64-byte header followed by the record stream exactly as the game
// sent it (RecordData). Little endian, like the protocol.
//   0  char[4] magic "SNRC"      4 u32 version (2)      8 u32 headerBytes (64)
//   12 u32 flags (bit0: the client's buffer overflowed, the stream is incomplete)
//   16 u64 projectBuildHash (the tool's)   24 u64 runtimeBuildHash (HelloReply)
//   32 u32 sampleRate   36 u32 blockSize   40 u32 platform   44 u32 contractVersion
//   48 u64 recordBytes  56 u64 FNV-1a 64 of header bytes [0, 56) followed by the records
// Records are platform specific (protocol §7); the header says which game wrote them.
// Decoding (and so loading) refuses a file whose records cannot all be read
// (RecordReader: unknown kind, bad sizes...), even when its checksum is right.
// Version 1 (checksum over the records only) is no longer read.
#ifndef SND_TOOL_RECORDING_FILE_HPP
#define SND_TOOL_RECORDING_FILE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace snd::tool {

inline constexpr std::uint32_t kRecordingFileVersion = 2;
inline constexpr std::uint32_t kRecordingHeaderBytes = 64;

struct RecordingInfo {
  std::uint32_t flags = 0;  // kRecordingOverflow
  std::uint64_t projectBuildHash = 0;
  std::uint64_t runtimeBuildHash = 0;
  std::uint32_t sampleRate = 0;
  std::uint32_t blockSize = 0;
  std::uint32_t platform = 0;
  std::uint32_t contractVersion = 0;
  bool operator==(const RecordingInfo&) const = default;
};
inline constexpr std::uint32_t kRecordingOverflow = 1u << 0;

// What a record stream contains (livelink RecordReader).
struct RecordingSummary {
  std::uint32_t arenas = 0;
  std::uint32_t lods = 0;
  std::uint32_t stalls = 0;
  std::uint32_t bankMaps = 0;
  std::uint64_t firstBlock = 0;
  std::uint64_t lastBlock = 0;
  std::uint64_t bytes = 0;
  bool valid = true;          // every record read
  std::string error;          // RecordErrorName when !valid
  std::uint64_t errorOffset = 0;
};
RecordingSummary Summarize(const std::uint8_t* data, std::uint64_t bytes);

struct Recording {
  RecordingInfo info;
  std::vector<std::uint8_t> records;
};

std::uint64_t Fnv1a64(const std::uint8_t* data, std::uint64_t bytes);
// Encodes / decodes the file bytes. Decode checks magic, version, sizes and the checksum.
std::vector<std::uint8_t> EncodeRecording(const Recording& r);
bool DecodeRecording(const std::uint8_t* data, std::uint64_t bytes, Recording* out, std::string* error);
bool SaveRecording(const std::string& path, const Recording& r, std::string* error);
bool LoadRecording(const std::string& path, Recording* out, std::string* error);

}  // namespace snd::tool

#endif  // SND_TOOL_RECORDING_FILE_HPP
