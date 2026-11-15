// Low-level bank writer from the frozen format definition
// (snd/format/bank_format.hpp, spec/interface/bank-format.md).
//
// Deterministic: chunks are written in bank_format::kChunks order (unknown
// chunks after, by fourcc), columns by id; required columns always, optional
// columns only when a row differs from the documented default; padding is zero.
#ifndef SND_BUILDTOOL_BANK_WRITER_HPP
#define SND_BUILDTOOL_BANK_WRITER_HPP

#include <cstdint>
#include <map>
#include <vector>

#include "snd/format/bank_format.hpp"

namespace snd::buildtool {

class TableBuilder {
 public:
  explicit TableBuilder(std::uint32_t fourcc);
  std::uint32_t fourcc() const { return fourcc_; }
  std::uint32_t rows() const { return rows_; }
  // Appends a row with every column at its default; returns the row index.
  std::uint32_t AddRow();
  // Typed setters; the column type must match (aborts otherwise: programming error).
  void U32(std::uint16_t column, std::uint32_t row, std::uint32_t v);
  void F32(std::uint16_t column, std::uint32_t row, float v);
  void U64(std::uint16_t column, std::uint32_t row, std::uint64_t v);
  void I64(std::uint16_t column, std::uint32_t row, std::int64_t v);
  std::uint32_t GetU32(std::uint16_t column, std::uint32_t row) const;
  // TableChunkHeader + ColumnEntry[] + 64-aligned column arrays.
  std::vector<std::uint8_t> Serialize() const;

 private:
  struct Col {
    const bank_format::ColumnInfo* info = nullptr;
    std::uint32_t elemBytes = 4;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> defaultBits;
  };
  Col& At(std::uint16_t column, bank_format::ColumnType type);
  void Write(std::uint16_t column, bank_format::ColumnType type, std::uint32_t row, const void* v);

  std::uint32_t fourcc_;
  std::uint32_t rows_ = 0;
  std::vector<Col> cols_;  // indexed by column id
};

struct BankHeaderInfo {
  std::uint32_t platform = bank_format::kPlatformAny;
  std::uint32_t flags = 0;
  std::uint32_t bankId = 0;
  std::uint32_t languageId = 0;
  std::uint64_t projectBuildHash = 0;
  std::vector<bank_format::Dependency> dependencies;
};

class BankWriter {
 public:
  void AddTable(const TableBuilder& t);  // empty tables (0 rows) are omitted
  void AddBlob(std::uint32_t fourcc, std::vector<std::uint8_t> bytes, std::uint16_t extraFlags = 0);
  // Body hash used as contentHash (FNV-1a 64 over chunk directory + chunks +
  // dependencies + identity fields); independent of projectBuildHash.
  std::uint64_t ContentHash(const BankHeaderInfo& h) const;
  // Complete file: header, dependencies, chunk table, chunks, checksums.
  std::vector<std::uint8_t> Finish(const BankHeaderInfo& h) const;

 private:
  struct Chunk {
    std::uint16_t flags = 0;
    std::vector<std::uint8_t> bytes;
  };
  std::vector<std::uint8_t> Layout(const BankHeaderInfo& h, std::uint64_t contentHash) const;
  std::map<std::uint32_t, Chunk> chunks_;
};

// Sort key putting known chunks in kChunks order, unknown after.
std::uint32_t ChunkOrder(std::uint32_t fourcc);

}  // namespace snd::buildtool

#endif  // SND_BUILDTOOL_BANK_WRITER_HPP
