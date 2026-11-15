// Independent test helpers (written from spec/interface/bank-format.md §6/§8,
// not from validate.cpp): the list of references the format defines, an
// invariant checker that re-verifies every reference of an accepted bank, and
// an exhaustive "use everything" walk over all public accessors.
//
// The point: ParseBank's contract is "after SND_OK every row reference, range
// and blob offset can be used as an index without further checks". ASan only
// sees reads outside the allocation; a bad row index that still lands inside
// the file is silent. So CheckInvariants re-checks the references against the
// spec and reports a violation explicitly, and DeepWalk dereferences them
// through the typed accessors (ASan catches anything leaving the image).
#ifndef SND_BANK_TEST_SPEC_WALK_HPP
#define SND_BANK_TEST_SPEC_WALK_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "snd/bank/bank_view.hpp"
#include "snd/types.h"

namespace specwalk {

__extension__ typedef unsigned __int128 U128;
namespace bf = snd::bank_format;
using snd::bank::BankView;
using snd::bank::ByteSpan;
using snd::bank::TableView;

struct RowRef {
  std::uint32_t chunk;
  std::uint16_t col;
  std::uint32_t target;
  bool allowNone;
};
struct RangeRef {
  std::uint32_t chunk;
  std::uint16_t first;
  std::uint16_t count;
  std::uint32_t target;
};
struct BlobRef {
  std::uint32_t chunk;
  std::uint16_t off;
  std::uint16_t len;
  std::uint32_t target;
  std::uint32_t align;
};

namespace ch = bf::chunk;
namespace c = bf::col;

// §6 "備考" columns (row references), first/count ranges and blob ranges.
inline constexpr RowRef kRowRefs[] = {
    {ch::EVNT, c::EVNT_bus, ch::BUSS, false},
    {ch::EVNT, c::EVNT_category, ch::CATG, true},
    {ch::EVNT, c::EVNT_limitGroup, ch::VLGR, true},
    {ch::EVNT, c::EVNT_curveVolume, ch::CURV, true},
    {ch::EVNT, c::EVNT_curveLowpass, ch::CURV, true},
    {ch::EVNT, c::EVNT_curveSpread, ch::CURV, true},
    {ch::EVNT, c::EVNT_curveReverbSend, ch::CURV, true},
    {ch::EVNT, c::EVNT_curveBlend3D, ch::CURV, true},
    {ch::EVNT, c::EVNT_musicState, ch::MSTA, true},
    {ch::TRAK, c::TRAK_event, ch::EVNT, false},
    {ch::TRAK, c::TRAK_bus, ch::BUSS, true},
    {ch::BUSS, c::BUSS_parent, ch::BUSS, true},
    {ch::SEND, c::SEND_targetBus, ch::BUSS, false},
    {ch::CATG, c::CATG_parent, ch::CATG, true},
    {ch::DUCK, c::DUCK_targetBus, ch::BUSS, false},
    {ch::ASST, c::ASST_stream, ch::STRM, true},
    {ch::SYNP, c::SYNP_asset, ch::ASST, true},
    {ch::MSEG, c::MSEG_asset, ch::ASST, false},
    {ch::MPLE, c::MPLE_segment, ch::MSEG, false},
    {ch::MSTM, c::MSTM_segment, ch::MSEG, false},
    {ch::MSTM, c::MSTM_volumeCurve, ch::CURV, true},
    {ch::MSTM, c::MSTM_asset, ch::ASST, true},
    {ch::MSTA, c::MSTA_playlist, ch::MPLS, false},
    {ch::MSTA, c::MSTA_musicEvent, ch::EVNT, false},
    {ch::MTRN, c::MTRN_musicEvent, ch::EVNT, false},
    {ch::MTRN, c::MTRN_bridgeSegment, ch::MSEG, true},
    {ch::MSTG, c::MSTG_event, ch::EVNT, false},
};
inline constexpr RangeRef kRangeRefs[] = {
    {ch::EVNT, c::EVNT_trackFirst, c::EVNT_trackCount, ch::TRAK},
    {ch::EVNT, c::EVNT_curveFirst, c::EVNT_curveCount, ch::CURV},
    {ch::TRAK, c::TRAK_clipFirst, c::TRAK_clipCount, ch::CLIP},
    {ch::TRAK, c::TRAK_actionFirst, c::TRAK_actionCount, ch::ACTN},
    {ch::BUSS, c::BUSS_sendFirst, c::BUSS_sendCount, ch::SEND},
    {ch::BUSS, c::BUSS_effectFirst, c::BUSS_effectCount, ch::EFFX},
    {ch::EFFX, c::EFFX_paramFirst, c::EFFX_paramCount, ch::EFPV},
    {ch::CURV, c::CURV_pointFirst, c::CURV_pointCount, ch::CPNT},
    {ch::SNAP, c::SNAP_valueFirst, c::SNAP_valueCount, ch::SNPV},
    {ch::ASST, c::ASST_seekFirst, c::ASST_seekCount, ch::SEEK},
    {ch::MSEG, c::MSEG_tempoFirst, c::MSEG_tempoCount, ch::MTMP},
    {ch::MSEG, c::MSEG_markerFirst, c::MSEG_markerCount, ch::MMRK},
    {ch::MPLS, c::MPLS_entryFirst, c::MPLS_entryCount, ch::MPLE},
};
inline constexpr BlobRef kBlobRefs[] = {
    {ch::EFFX, c::EFFX_blobOffset, c::EFFX_blobBytes, ch::EFBL, 1},
    {ch::ASST, c::ASST_dataOffset, c::ASST_dataBytes, ch::WAVE, 64},
    {ch::ASST, c::ASST_extraOffset, c::ASST_extraBytes, ch::WAVE, 1},
    {ch::ASST, c::ASST_preloadOffset, c::ASST_preloadBytes, ch::PREL, 1},
    {ch::SYNP, c::SYNP_programOffset, c::SYNP_programBytes, ch::PTCH, 1},
};

// Reads cell `row` of a known column as u64 (absent column -> default), or
// false when the column id is unknown.
inline bool Cell(const BankView& v, std::uint32_t chunk, std::uint16_t col, std::uint32_t row, std::uint64_t* out) {
  const bf::ColumnInfo* info = snd::bank::FindColumnInfo(chunk, col);
  const TableView* t = v.Table(chunk);
  if (info == nullptr || t == nullptr) return false;
  const void* p = t->columns[col];
  if (p == nullptr) {
    *out = static_cast<std::uint64_t>(info->defaultValue);
    return true;
  }
  if (bf::ColumnTypeBytes(info->type) == 4) {
    std::uint32_t x;
    std::memcpy(&x, static_cast<const std::uint8_t*>(p) + std::uint64_t{row} * 4u, 4);
    *out = x;
  } else {
    std::memcpy(out, static_cast<const std::uint8_t*>(p) + std::uint64_t{row} * 8u, 8);
  }
  return true;
}

// Returns "" when every reference of the accepted bank is valid per spec,
// otherwise a description of the first violation.
inline std::string CheckInvariants(const BankView& v) {
  char buf[256];
  auto rows = [&](std::uint32_t chunk) { return v.Rows(chunk); };
  // Every present column array lies inside the file.
  const std::uint8_t* f = v.file();
  const std::uint64_t n = v.fileBytes();
  for (const bf::ColumnInfo& ci : bf::kColumns) {
    const TableView* t = v.Table(ci.chunk);
    if (t == nullptr || t->columns[ci.columnId] == nullptr) continue;
    if (v.writable()) continue;  // overlay: separate allocation
    const auto* p = static_cast<const std::uint8_t*>(t->columns[ci.columnId]);
    const std::uint64_t bytes = std::uint64_t{t->rows} * bf::ColumnTypeBytes(ci.type);
    if (p < f || p > f + n || bytes > static_cast<std::uint64_t>(f + n - p)) {
      std::snprintf(buf, sizeof buf, "column %s array outside the file", ci.name);
      return buf;
    }
    if ((reinterpret_cast<std::uintptr_t>(p) & 63u) != 0) {
      std::snprintf(buf, sizeof buf, "column %s array not 64-aligned", ci.name);
      return buf;
    }
  }
  for (const RowRef& r : kRowRefs) {
    for (std::uint32_t i = 0; i < rows(r.chunk); ++i) {
      std::uint64_t x = 0;
      Cell(v, r.chunk, r.col, i, &x);
      if (x == bf::kNone && r.allowNone) continue;
      if (x >= rows(r.target)) {
        std::snprintf(buf, sizeof buf, "row ref chunk %08x col %u row %u = %llu >= %u", r.chunk, r.col, i,
                      static_cast<unsigned long long>(x), rows(r.target));
        return buf;
      }
    }
  }
  for (const RangeRef& r : kRangeRefs) {
    for (std::uint32_t i = 0; i < rows(r.chunk); ++i) {
      std::uint64_t first = 0, count = 0;
      Cell(v, r.chunk, r.first, i, &first);
      Cell(v, r.chunk, r.count, i, &count);
      if (count == 0) continue;
      if (first > rows(r.target) || count > rows(r.target) - first) {
        std::snprintf(buf, sizeof buf, "range chunk %08x col %u row %u = [%llu,+%llu) of %u", r.chunk, r.first, i,
                      static_cast<unsigned long long>(first), static_cast<unsigned long long>(count),
                      rows(r.target));
        return buf;
      }
    }
  }
  for (const BlobRef& r : kBlobRefs) {
    const ByteSpan blob = v.Chunk(r.target);
    for (std::uint32_t i = 0; i < rows(r.chunk); ++i) {
      std::uint64_t off = 0, len = 0;
      Cell(v, r.chunk, r.off, i, &off);
      Cell(v, r.chunk, r.len, i, &len);
      if (len == 0) continue;
      if (blob.data == nullptr || off > blob.bytes || len > blob.bytes - off || off % r.align != 0) {
        std::snprintf(buf, sizeof buf, "blob chunk %08x col %u row %u = [%llu,+%llu) of %llu", r.chunk, r.off, i,
                      static_cast<unsigned long long>(off), static_cast<unsigned long long>(len),
                      static_cast<unsigned long long>(blob.bytes));
        return buf;
      }
    }
  }
  // Kind dependent targets.
  for (std::uint32_t i = 0; i < rows(ch::CLIP); ++i) {
    std::uint64_t k = 0, t = 0;
    Cell(v, ch::CLIP, c::CLIP_kind, i, &k);
    Cell(v, ch::CLIP, c::CLIP_target, i, &t);
    const std::uint32_t tgt[3] = {ch::ASST, ch::EVNT, ch::SYNP};
    if (k > 2 || t >= rows(tgt[k])) return "CLIP target out of range";
  }
  for (std::uint32_t i = 0; i < rows(ch::SNPV); ++i) {
    std::uint64_t k = 0, t = 0, pi = 0;
    Cell(v, ch::SNPV, c::SNPV_targetKind, i, &k);
    Cell(v, ch::SNPV, c::SNPV_targetRow, i, &t);
    Cell(v, ch::SNPV, c::SNPV_paramIndex, i, &pi);
    const std::uint32_t tgt[3] = {ch::BUSS, ch::EFFX, ch::SEND};
    if (k > 2 || t >= rows(tgt[k])) return "SNPV target out of range";
    if (k == 1) {
      std::uint64_t pc = 0;
      Cell(v, ch::EFFX, c::EFFX_paramCount, static_cast<std::uint32_t>(t), &pc);
      if (pi >= pc) return "SNPV paramIndex out of range";
    }
  }
  for (std::uint32_t i = 0; i < rows(ch::DUCK); ++i) {
    std::uint64_t k = 0, s = 0;
    Cell(v, ch::DUCK, c::DUCK_kind, i, &k);
    Cell(v, ch::DUCK, c::DUCK_source, i, &s);
    if (k > 1 || s >= rows(k == 0 ? ch::BUSS : ch::CATG)) return "DUCK source out of range";
  }
  // Strings are terminated inside their pool.
  auto terminated = [](const ByteSpan& pool, std::uint64_t off) {
    return pool.data != nullptr && off < pool.bytes &&
           std::memchr(pool.data + off, 0, static_cast<std::size_t>(pool.bytes - off)) != nullptr;
  };
  for (std::uint32_t i = 0; i < rows(ch::STRM); ++i) {
    std::uint64_t off = 0;
    Cell(v, ch::STRM, c::STRM_pathOffset, i, &off);
    if (!terminated(v.Chunk(ch::SPTH), off)) return "STRM path not a terminated string";
  }
  for (std::uint32_t i = 0; i < rows(ch::NAME); ++i) {
    std::uint64_t off = 0;
    Cell(v, ch::NAME, c::NAME_stringOffset, i, &off);
    if (!terminated(v.Chunk(ch::STRS), off)) return "NAME string not a terminated string";
  }
  // Dependencies inside the file.
  if (v.dependencyCount() > 0) {
    const bf::FileHeader& h = v.header();
    if (h.dependencyOffset > n || std::uint64_t{h.dependencyCount} * 16u > n - h.dependencyOffset) {
      return "dependency table outside the file";
    }
  }
  // DSPN / PHYS sizes.
  if (v.HasChunk(ch::DSPN) && v.Chunk(ch::DSPN).bytes < sizeof(bf::DspPoolNeed)) return "DSPN too small";
  {
    const ByteSpan phys = v.Chunk(ch::PHYS);
    const std::uint32_t tables = v.PhysTableCount();
    for (std::uint32_t i = 0; i < tables; ++i) {
      snd::bank::PhysTableView t;
      if (!v.PhysTable(i, &t)) return "PhysTable(i < count) failed";
      const std::uint64_t m = t.materialCount;
      auto inside = [&](const void* p, U128 bytes) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        return b >= phys.data && b <= phys.data + phys.bytes &&
               bytes <= static_cast<U128>(phys.data + phys.bytes - b) &&
               (reinterpret_cast<std::uintptr_t>(b) & 3u) == 0;
      };
      const U128 M = m;
      if (!inside(t.single, M * 16) || !inside(t.pair, M * M * 4) || !inside(t.footstep, M * 4) ||
          !inside(t.curves, M * 32)) {
        return "PHYS arrays outside the chunk or misaligned";
      }
    }
  }
  return "";
}

// Reads every cell of every known column through the typed API (Column<C>).
inline std::uint64_t ReadAllColumns(const BankView& v) {
  std::uint64_t sum = 0;
#define SPECWALK_COL(ch_, nm, id_, ty, req, def)                                \
  {                                                                             \
    const auto col = v.Column<snd::bank::col::ch_##_##nm>();                    \
    for (std::uint32_t r = 0; r < col.rows(); ++r) {                            \
      const auto x = col[r];                                                    \
      std::uint64_t bits = 0;                                                   \
      std::memcpy(&bits, &x, sizeof(x));                                        \
      sum += bits;                                                              \
    }                                                                           \
    (void)col.Get(col.rows());                                                  \
    (void)col.Get(0xFFFFFFFFu);                                                 \
    (void)v.FindRow<snd::bank::col::ch_##_##nm>(col.default_value());           \
  }
  SND_BANK_COLUMNS(SPECWALK_COL)
#undef SPECWALK_COL
  return sum;
}

inline std::uint64_t Touch(const ByteSpan& s) {
  if (s.bytes == 0 || s.data == nullptr) return 0;
  return static_cast<std::uint64_t>(s.data[0]) + s.data[s.bytes - 1] + s.data[s.bytes / 2];
}

// Every accessor of BankView, every reference followed into its target
// (reading every column of the target row), every blob byte range touched.
// Only call on a bank that passed CheckInvariants (otherwise this can read
// out of bounds by design).
inline std::uint64_t DeepWalk(const BankView& v) {
  std::uint64_t sum = ReadAllColumns(v);
  const bf::FileHeader& h = v.header();
  sum += h.bankId + v.languageId() + v.contentHash() + (v.devBuild() ? 1u : 0u) + v.fileBytes();
  for (std::uint32_t d = 0; d <= v.dependencyCount(); ++d) sum += v.dependency(d).bankId + v.dependency(d).buildHash;
  for (const bf::ChunkInfo& ci : bf::kChunks) {
    sum += v.HasChunk(ci.fourcc) ? 1u : 0u;
    sum += Touch(v.Chunk(ci.fourcc));
    std::uint16_t fl = 0;
    sum += Touch(v.FindChunk(ci.fourcc, &fl)) + fl;
    sum += Touch(v.ApplicationChunk(ci.fourcc));
    sum += v.Rows(ci.fourcc);
    (void)v.Table(ci.fourcc);
  }
  for (std::uint32_t fc : {snd::Fourcc('X', 'T', 'R', 'A'), snd::Fourcc('Z', 'Z', 'Z', 'Z'), 0u, 0xFFFFFFFFu}) {
    sum += Touch(v.FindChunk(fc)) + Touch(v.ApplicationChunk(fc)) + Touch(v.Chunk(fc)) + v.Rows(fc);
  }
  // Every row reference into its target: read all columns of the target row.
  auto readRow = [&](std::uint32_t table, std::uint32_t row) {
    const TableView* t = v.Table(table);
    if (t == nullptr) return;
    for (std::uint32_t k = 0; k < snd::bank::kMaxTableColumns; ++k) {
      if (t->columns[k] == nullptr) continue;
      const bf::ColumnInfo* ci = snd::bank::FindColumnInfo(table, k);
      if (ci == nullptr) continue;
      std::uint64_t x = 0;
      const auto* p = static_cast<const std::uint8_t*>(t->columns[k]);
      const std::uint32_t eb = bf::ColumnTypeBytes(ci->type);
      std::memcpy(&x, p + std::uint64_t{row} * eb, eb);
      sum += x;
    }
  };
  for (const RowRef& r : kRowRefs) {
    for (std::uint32_t i = 0; i < v.Rows(r.chunk); ++i) {
      std::uint64_t x = 0;
      Cell(v, r.chunk, r.col, i, &x);
      if (x != bf::kNone) readRow(r.target, static_cast<std::uint32_t>(x));
    }
  }
  for (const RangeRef& r : kRangeRefs) {
    for (std::uint32_t i = 0; i < v.Rows(r.chunk); ++i) {
      std::uint64_t first = 0, count = 0;
      Cell(v, r.chunk, r.first, i, &first);
      Cell(v, r.chunk, r.count, i, &count);
      for (std::uint64_t k = 0; k < count && k < 4096; ++k) readRow(r.target, static_cast<std::uint32_t>(first + k));
      if (count > 0) readRow(r.target, static_cast<std::uint32_t>(first + count - 1));
    }
  }
  for (const BlobRef& r : kBlobRefs) {
    const ByteSpan blob = v.Chunk(r.target);
    for (std::uint32_t i = 0; i < v.Rows(r.chunk); ++i) {
      std::uint64_t off = 0, len = 0;
      Cell(v, r.chunk, r.off, i, &off);
      Cell(v, r.chunk, r.len, i, &len);
      if (len > 0) sum += blob.data[off] + blob.data[off + len - 1];
    }
  }
  for (std::uint32_t i = 0; i < v.Rows(ch::CLIP); ++i) {
    const std::uint32_t k = v.Column<snd::bank::col::CLIP_kind>()[i];
    const std::uint32_t tgt[3] = {ch::ASST, ch::EVNT, ch::SYNP};
    readRow(tgt[k], v.Column<snd::bank::col::CLIP_target>()[i]);
  }
  for (std::uint32_t i = 0; i < v.Rows(ch::SNPV); ++i) {
    const std::uint32_t k = v.Column<snd::bank::col::SNPV_targetKind>()[i];
    const std::uint32_t tgt[3] = {ch::BUSS, ch::EFFX, ch::SEND};
    readRow(tgt[k], v.Column<snd::bank::col::SNPV_targetRow>()[i]);
  }
  for (std::uint32_t i = 0; i < v.Rows(ch::DUCK); ++i) {
    const std::uint32_t k = v.Column<snd::bank::col::DUCK_kind>()[i];
    readRow(k == 0 ? ch::BUSS : ch::CATG, v.Column<snd::bank::col::DUCK_source>()[i]);
  }
  // Typed helpers, including out-of-range rows.
  const std::uint32_t assets = v.Rows(ch::ASST);
  for (std::uint32_t a = 0; a <= assets + 1; ++a) {
    sum += Touch(v.WaveData(a)) + Touch(v.AssetExtra(a)) + Touch(v.Preload(a));
    const snd::RangeU32 s = v.SeekRange(a);
    if (a < assets) {
      for (std::uint32_t k = 0; k < s.count && k < 4096; ++k) {
        sum += v.Column<snd::bank::col::SEEK_frame>()[s.first + k] + v.Column<snd::bank::col::SEEK_byteOffset>()[s.first + k];
      }
    }
  }
  sum += Touch(v.WaveData(0xFFFFFFFFu)) + Touch(v.Preload(0xFFFFFFFFu));
  for (std::uint32_t s = 0; s <= v.Rows(ch::STRM) + 1; ++s) {
    const char* p = v.StreamPath(s);
    if (p != nullptr) sum += std::strlen(p);
  }
  for (std::uint64_t off : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xFFFFFFFFu}}) {
    const char* p = v.String(static_cast<std::uint32_t>(off));
    if (p != nullptr) sum += std::strlen(p);
  }
  for (std::uint32_t r = 0; r < v.Rows(ch::NAME); ++r) {
    const char* s = v.String(v.Column<snd::bank::col::NAME_stringOffset>()[r]);
    if (s != nullptr) sum += std::strlen(s);
    const char* nm = v.Name(v.Column<snd::bank::col::NAME_id>()[r]);
    if (nm != nullptr) sum += std::strlen(nm);
  }
  for (std::uint32_t r = 0; r < v.Rows(ch::GUID); ++r) {
    std::uint64_t lo = 0, hi = 0;
    if (v.Guid(v.Column<snd::bank::col::GUID_id>()[r], &lo, &hi)) sum += lo ^ hi;
  }
  (void)v.Guid(0, nullptr, nullptr);
  (void)v.Name(0);
  sum += v.DspNeed().instanceBytes + v.DspNeed().instanceCount;
  const std::uint32_t pt = v.PhysTableCount();
  for (std::uint32_t i = 0; i <= pt && i < 100000; ++i) {
    snd::bank::PhysTableView t;
    if (!v.PhysTable(i, &t)) continue;
    const std::uint64_t m = t.materialCount;
    for (std::uint64_t k = 0; k < m * 4 && k < 1u << 16; ++k) sum += t.single[k];
    if (m > 0) sum += t.single[m * 4 - 1] + t.footstep[m - 1] + t.pair[m * m - 1];
    for (std::uint64_t a = 0; a < m && a < 256; ++a) {
      for (std::uint64_t b = a; b < m && b < 256; ++b) sum += t.pair[a * m + b];
      sum += t.footstep[a];
      float x = t.curves[a].impulseToVelocity[0] + t.curves[a].speedToVolumeDb[1];
      std::uint32_t bits;
      std::memcpy(&bits, &x, 4);
      sum += bits;
    }
    if (m > 0) sum += static_cast<std::uint64_t>(t.curves[m - 1].massToPitchCents[1] != 0.0f);
  }
  (void)v.PhysTable(pt, nullptr);
  return sum;
}

}  // namespace specwalk

#endif  // SND_BANK_TEST_SPEC_WALK_HPP
