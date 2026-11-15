// Typed, zero-copy read access to a validated bank (design.md 10.2,
// spec/interface/bank-format.md).
//
// A BankView is filled by snd::bank::ParseBank (validate.hpp) and is valid as
// long as the bank memory it points to. Every accessor reads the bank memory in
// place (offset based, no copies, no fix-ups). Because ParseBank checked every
// structure, row reference and blob range (bank-format.md §8), the accessors
// only need cheap bounds checks against the row counts they return.
//
// Column access is typed through tag types generated from the frozen column
// list (SND_BANK_COLUMNS):
//
//   const snd::bank::BankView& v = prepared->view;
//   auto prio = v.Column<snd::bank::col::EVNT_priority>();   // ColumnView<uint32_t>
//   for (uint32_t r = 0; r < prio.rows(); ++r) use(prio[r]);
//
// Missing optional columns read as their documented default.
//
// Thread safety: a BankView is immutable after ParseBank. Any thread may read
// it. In dev builds with the override layer (prepared.hpp), MutableColumn
// gives write access; writes follow the core's block-boundary rule (11.2).
#ifndef SND_BANK_BANK_VIEW_HPP
#define SND_BANK_BANK_VIEW_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "snd/format/bank_format.hpp"
#include "snd/id.hpp"
#include "snd/internal/common.hpp"

namespace snd::bank {

namespace bf = ::snd::bank_format;

// ---------------------------------------------------------------------------
// Compile-time tables derived from the frozen format definition
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kKnownChunkCount =
    static_cast<std::uint32_t>(sizeof(bf::kChunks) / sizeof(bf::kChunks[0]));
inline constexpr std::uint32_t kKnownColumnCount =
    static_cast<std::uint32_t>(sizeof(bf::kColumns) / sizeof(bf::kColumns[0]));

// Index of a known chunk in bf::kChunks, or -1.
constexpr int KnownChunkIndex(std::uint32_t fourcc) noexcept {
  for (std::uint32_t i = 0; i < kKnownChunkCount; ++i) {
    if (bf::kChunks[i].fourcc == fourcc) return static_cast<int>(i);
  }
  return -1;
}

// Number of columns the format defines for a table chunk.
constexpr std::uint32_t KnownColumnCount(std::uint32_t fourcc) noexcept {
  std::uint32_t n = 0;
  for (const bf::ColumnInfo& c : bf::kColumns) n += c.chunk == fourcc ? 1u : 0u;
  return n;
}

constexpr std::uint32_t MaxKnownColumnsPerTable() noexcept {
  std::uint32_t m = 0;
  for (const bf::ChunkInfo& ch : bf::kChunks) {
    const std::uint32_t n = KnownColumnCount(ch.fourcc);
    if (n > m) m = n;
  }
  return m;
}
inline constexpr std::uint32_t kMaxTableColumns = MaxKnownColumnsPerTable();

// Column description, or nullptr for an unknown (chunk, column id).
constexpr const bf::ColumnInfo* FindColumnInfo(std::uint32_t chunk, std::uint32_t columnId) noexcept {
  for (const bf::ColumnInfo& c : bf::kColumns) {
    if (c.chunk == chunk && c.columnId == columnId) return &c;
  }
  return nullptr;
}

template <bf::ColumnType T>
struct ColumnCType;
template <>
struct ColumnCType<bf::ColumnType::U32> {
  using Type = std::uint32_t;
};
template <>
struct ColumnCType<bf::ColumnType::F32> {
  using Type = float;
};
template <>
struct ColumnCType<bf::ColumnType::U64> {
  using Type = std::uint64_t;
};
template <>
struct ColumnCType<bf::ColumnType::I64> {
  using Type = std::int64_t;
};

// Column tag types: col::EVNT_priority, col::ASST_dataOffset, ...
namespace col {
using ::snd::bank_format::kNone;
#define SND_BANK_COLUMN_TAG(ch, nm, id_, ty, req, def)                                  \
  struct ch##_##nm {                                                                     \
    static constexpr std::uint32_t kChunk = ::snd::bank_format::chunk::ch;               \
    static constexpr std::uint16_t kId = static_cast<std::uint16_t>(id_);                \
    static constexpr ::snd::bank_format::ColumnType kType = ::snd::bank_format::ColumnType::ty; \
    using Type = ColumnCType<kType>::Type;                                               \
    static constexpr bool kRequired = (req) != 0;                                        \
    static constexpr Type kDefault = static_cast<Type>(def);                             \
  };
SND_BANK_COLUMNS(SND_BANK_COLUMN_TAG)
#undef SND_BANK_COLUMN_TAG
}  // namespace col

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------
struct ByteSpan {
  const std::uint8_t* data = nullptr;
  std::uint64_t bytes = 0;
  bool empty() const noexcept { return bytes == 0; }
};

// One typed column. operator[] requires row < rows(); Get() is range checked.
template <class T>
class ColumnView {
 public:
  constexpr ColumnView() noexcept = default;
  constexpr ColumnView(const T* data, std::uint32_t rows, T def) noexcept
      : data_(data), rows_(rows), default_(def) {}
  std::uint32_t rows() const noexcept { return rows_; }
  // nullptr when the column is absent in the bank (all rows are default()).
  const T* data() const noexcept { return data_; }
  bool present() const noexcept { return data_ != nullptr; }
  T default_value() const noexcept { return default_; }
  T operator[](std::uint32_t row) const noexcept { return data_ != nullptr ? data_[row] : default_; }
  T Get(std::uint32_t row) const noexcept { return row < rows_ ? (*this)[row] : default_; }

 private:
  const T* data_ = nullptr;
  std::uint32_t rows_ = 0;
  T default_{};
};

// A table chunk as parsed: row count and one pointer per known column
// (nullptr = column absent, read the default). In the dev override layer
// (prepared.hpp) every known column is materialised and `capacity` may exceed
// `rows` by the devReserve rows.
struct TableView {
  std::uint32_t rows = 0;
  std::uint32_t capacity = 0;
  void* columns[kMaxTableColumns] = {};
};

struct ChunkRef {
  const std::uint8_t* data = nullptr;  // nullptr = chunk absent (or ignored: unknown version)
  std::uint64_t bytes = 0;
  std::uint16_t version = 0;
  std::uint16_t flags = 0;
};

// PHYS (bank-format.md §6.2). Pointers into the chunk, all range checked.
struct PhysTableView {
  std::uint32_t id = 0;
  std::uint32_t materialCount = 0;               // N
  const std::uint32_t* single = nullptr;         // [N][4]
  const std::uint32_t* pair = nullptr;           // [N*N], index min*N+max, 0 = no override
  const std::uint32_t* footstep = nullptr;       // [N]
  std::uint32_t defaultEvent[4] = {};
  const bf::PhysCurves* curves = nullptr;        // [N]
};

class BankView {
 public:
  BankView() noexcept = default;

  bool valid() const noexcept { return file_ != nullptr; }
  const bf::FileHeader& header() const noexcept { return header_; }
  const std::uint8_t* file() const noexcept { return file_; }
  std::uint64_t fileBytes() const noexcept { return header_.fileBytes; }
  Id bankId() const noexcept { return header_.bankId; }
  std::uint32_t languageId() const noexcept {
    return (header_.flags & bf::kFileLanguageBank) != 0u ? header_.languageId : 0u;
  }
  std::uint64_t contentHash() const noexcept { return header_.contentHash; }
  bool devBuild() const noexcept { return (header_.flags & bf::kFileDevBuild) != 0u; }
  // True when tables point into the writable override layer (prepared.hpp).
  bool writable() const noexcept { return writable_; }

  // Dependencies (bank-format.md §3), validated.
  std::uint32_t dependencyCount() const noexcept { return header_.dependencyCount; }
  bf::Dependency dependency(std::uint32_t i) const noexcept {
    bf::Dependency d{};
    if (i < header_.dependencyCount) {
      std::memcpy(&d, file_ + header_.dependencyOffset + std::uint64_t{i} * sizeof(d), sizeof(d));
    }
    return d;
  }

  // --- chunks ----------------------------------------------------------------
  bool HasChunk(std::uint32_t fourcc) const noexcept {
    const int i = KnownChunkIndex(fourcc);
    return i >= 0 && chunks_[i].data != nullptr;
  }
  // Known chunk bytes (blob or table), empty if absent.
  ByteSpan Chunk(std::uint32_t fourcc) const noexcept {
    const int i = KnownChunkIndex(fourcc);
    if (i < 0 || chunks_[i].data == nullptr) return {};
    return ByteSpan{chunks_[i].data, chunks_[i].bytes};
  }
  // Any chunk by fourcc, including unknown ones (scans the validated chunk table).
  ByteSpan FindChunk(std::uint32_t fourcc, std::uint16_t* flags = nullptr) const noexcept;
  // Application chunk (ChunkEntry flag kChunkApplication), e.g. 'PHYS'; for snd_get_bank_chunk.
  ByteSpan ApplicationChunk(std::uint32_t fourcc) const noexcept {
    std::uint16_t f = 0;
    ByteSpan s = FindChunk(fourcc, &f);
    return (f & bf::kChunkApplication) != 0u ? s : ByteSpan{};
  }

  // --- tables ----------------------------------------------------------------
  std::uint32_t Rows(std::uint32_t fourcc) const noexcept {
    const int i = KnownChunkIndex(fourcc);
    return i >= 0 ? tables_[i].rows : 0u;
  }
  const TableView* Table(std::uint32_t fourcc) const noexcept {
    const int i = KnownChunkIndex(fourcc);
    return i >= 0 ? &tables_[i] : nullptr;
  }

  template <class C>
  ColumnView<typename C::Type> Column() const noexcept {
    constexpr int idx = KnownChunkIndex(C::kChunk);
    static_assert(idx >= 0, "unknown chunk");
    static_assert(C::kId < kMaxTableColumns, "column id out of range");
    const TableView& t = tables_[idx];
    return ColumnView<typename C::Type>(static_cast<const typename C::Type*>(t.columns[C::kId]),
                                        t.rows, C::kDefault);
  }
  // Writable column of the dev override layer (11.2); nullptr in release
  // views (bank memory is read-only there). Valid for [0, Table()->capacity).
  template <class C>
  typename C::Type* MutableColumn() const noexcept {
    constexpr int idx = KnownChunkIndex(C::kChunk);
    static_assert(idx >= 0, "unknown chunk");
    if (!writable_) return nullptr;
    return static_cast<typename C::Type*>(tables_[idx].columns[C::kId]);
  }
  // First row whose column equals `value` (linear scan), kNone if none.
  template <class C>
  std::uint32_t FindRow(typename C::Type value) const noexcept {
    const ColumnView<typename C::Type> c = Column<C>();
    for (std::uint32_t r = 0; r < c.rows(); ++r) {
      if (c[r] == value) return r;
    }
    return bf::kNone;
  }

  // --- typed helpers -----------------------------------------------------------
  // Resident waveform data / codec setup data / preload chunk of an ASST row.
  ByteSpan WaveData(std::uint32_t assetRow) const noexcept;
  ByteSpan AssetExtra(std::uint32_t assetRow) const noexcept;
  ByteSpan Preload(std::uint32_t assetRow) const noexcept;
  // SEEK rows of an asset (count 0 when none).
  RangeU32 SeekRange(std::uint32_t assetRow) const noexcept;
  // Stream file path of a STRM row (NUL-terminated UTF-8 inside SPTH), nullptr if invalid.
  const char* StreamPath(std::uint32_t streamRow) const noexcept;
  // String pool (STRS) entry, nullptr if strings were stripped or offset invalid.
  const char* String(std::uint32_t offset) const noexcept;
  // NAME table lookup: first non-alias name of `id` (nullptr if unknown / stripped).
  const char* Name(Id id) const noexcept;
  // ALIS (2.0): current id of a renamed object's old id (10.1), 0 if none.
  // Kept in release banks (not strippable).
  Id ResolveAlias(Id oldId) const noexcept {
    const ColumnView<std::uint32_t> olds = Column<col::ALIS_oldId>();
    const ColumnView<std::uint32_t> ids = Column<col::ALIS_id>();
    for (std::uint32_t r = 0; r < olds.rows(); ++r) {
      if (olds[r] == oldId) return ids[r];
    }
    return 0;
  }
  // GUID table lookup (dev builds). false if absent.
  bool Guid(Id id, std::uint64_t* lo, std::uint64_t* hi) const noexcept;
  // DSPN (zero need if absent).
  bf::DspPoolNeed DspNeed() const noexcept;
  // PHYS tables.
  std::uint32_t PhysTableCount() const noexcept;
  bool PhysTable(std::uint32_t index, PhysTableView* out) const noexcept;

 private:
  friend struct ViewAccess;  // validate.cpp / prepared.cpp fill the view

  const std::uint8_t* file_ = nullptr;
  bf::FileHeader header_{};
  ChunkRef chunks_[kKnownChunkCount] = {};
  TableView tables_[kKnownChunkCount] = {};
  bool writable_ = false;
};

}  // namespace snd::bank

#endif  // SND_BANK_BANK_VIEW_HPP
