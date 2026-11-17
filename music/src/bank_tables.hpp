// Read-only access to the music-related bank tables (bank-format.md 6) through
// MusicServices::bankChunk. The music module depends on core only (12.1), so it
// reads the table chunks itself from the contract's layout
// (snd/format/bank_format.hpp): TableChunkHeader + ColumnEntry[]. A missing
// optional column (or a malformed one) reads as the column's default value.
// Everything here is RT-safe: no allocation, bounded loops.
#ifndef SND_MUSIC_SRC_BANK_TABLES_HPP
#define SND_MUSIC_SRC_BANK_TABLES_HPP

#include <cstdint>
#include <cstring>

#include "snd/format/bank_format.hpp"
#include "snd/internal/music_if.hpp"

namespace snd::music::detail {

namespace bf = snd::bank_format;

inline constexpr std::uint32_t kMaxColumns = 32;

// Default value of column `id` of table `fourcc` (bank_format::kColumns).
constexpr double ColumnDefault(std::uint32_t fourcc, std::uint16_t id) noexcept {
  for (const bf::ColumnInfo& c : bf::kColumns) {
    if (c.chunk == fourcc && c.columnId == id) return c.defaultValue;
  }
  return 0.0;
}

class Table {
 public:
  // Binds a table chunk. An absent or malformed chunk is an empty table.
  void Bind(std::uint32_t fourcc, const std::uint8_t* chunk, std::uint64_t bytes) noexcept;
  std::uint32_t Rows() const noexcept { return rows_; }
  bool Has(std::uint32_t row) const noexcept { return row < rows_; }

  std::uint32_t U32(std::uint16_t col, std::uint32_t row) const noexcept { return Read<std::uint32_t>(col, row); }
  float F32(std::uint16_t col, std::uint32_t row) const noexcept { return Read<float>(col, row); }
  std::uint64_t U64(std::uint16_t col, std::uint32_t row) const noexcept { return Read<std::uint64_t>(col, row); }
  std::int64_t I64(std::uint16_t col, std::uint32_t row) const noexcept { return Read<std::int64_t>(col, row); }

 private:
  template <class T>
  T Read(std::uint16_t col, std::uint32_t row) const noexcept {
    if (col < kMaxColumns && row < rows_ && cols_[col] != nullptr && elem_[col] == sizeof(T)) {
      T v;
      std::memcpy(&v, cols_[col] + std::size_t{row} * sizeof(T), sizeof(T));
      return v;
    }
    return static_cast<T>(ColumnDefault(fourcc_, col));
  }

  std::uint32_t fourcc_ = 0;
  std::uint32_t rows_ = 0;
  const std::uint8_t* cols_[kMaxColumns] = {};
  std::uint8_t elem_[kMaxColumns] = {};
};

// The tables of one loaded bank that the music system reads.
struct BankTables {
  std::uint16_t bank = 0xFFFFu;
  Table evnt, trak, clip, asst, curv, cpnt, parm;
  Table mseg, mpls, mple, mstm, msta, mtrn, mstg, mmrk, mtmp;
  // Binds every table of `bankSlot`. false when the bank has no EVNT (not loaded).
  bool Bind(const MusicServices& s, std::uint16_t bankSlot) noexcept;
};

}  // namespace snd::music::detail

#endif  // SND_MUSIC_SRC_BANK_TABLES_HPP
