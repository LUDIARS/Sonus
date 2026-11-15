// Bank validation (bank-format.md §8). See validate.hpp for the list of checks.
#include "snd/bank/validate.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "detail/crc32c.hpp"
#include "view_access.hpp"

namespace snd::bank {
namespace {

using bf::kNone;

// Hard limits that keep validation linear-ish on hostile input.
constexpr std::uint32_t kMaxDependencies = 1024;
constexpr std::uint32_t kMaxChunks = 1024;

constexpr std::uint32_t kNoCol = 0xFFFFFFFFu;
constexpr std::uint32_t kNoRow = 0xFFFFFFFFu;

bool InRange(std::uint64_t off, std::uint64_t len, std::uint64_t size) noexcept {
  return off <= size && len <= size - off;
}

bool Overlap(std::uint64_t a, std::uint64_t an, std::uint64_t b, std::uint64_t bn) noexcept {
  if (an == 0 || bn == 0) return false;
  return a < b + bn && b < a + an;
}

void FourccText(std::uint32_t f, char out[5]) noexcept {
  for (int i = 0; i < 4; ++i) {
    const char c = static_cast<char>((f >> (8 * i)) & 0xFFu);
    out[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  out[4] = '\0';
}

class Checker {
 public:
  Checker(const std::uint8_t* d, std::uint64_t n, ParseError* err) noexcept
      : d_(d), n_(n), err_(err) {}

  __attribute__((format(printf, 6, 7))) snd_result Fail(snd_result code, std::uint32_t chunk,
                                                        std::uint32_t column, std::uint32_t row,
                                                        const char* fmt, ...) noexcept {
    if (err_ != nullptr) {
      err_->code = code;
      err_->chunk = chunk;
      err_->column = column;
      err_->row = row;
      char prefix[64];
      if (chunk != 0) {
        char fc[5];
        FourccText(chunk, fc);
        if (column != kNoCol && row != kNoRow) {
          std::snprintf(prefix, sizeof(prefix), "%s col %u row %u: ", fc, column, row);
        } else if (column != kNoCol) {
          std::snprintf(prefix, sizeof(prefix), "%s col %u: ", fc, column);
        } else if (row != kNoRow) {
          std::snprintf(prefix, sizeof(prefix), "%s row %u: ", fc, row);
        } else {
          std::snprintf(prefix, sizeof(prefix), "%s: ", fc);
        }
      } else {
        prefix[0] = '\0';
      }
      std::size_t used = std::strlen(prefix);
      std::memcpy(err_->message, prefix, used + 1);
      va_list ap;
      va_start(ap, fmt);
      std::vsnprintf(err_->message + used, sizeof(err_->message) - used, fmt, ap);
      va_end(ap);
    }
    return code;
  }

  snd_result Run(const ParseOptions& opt, BankView* out) noexcept;

 private:
  snd_result CheckHeader(const ParseOptions& opt) noexcept;
  snd_result CheckDependencies() noexcept;
  snd_result CheckChunks() noexcept;
  snd_result CheckTable(int idx) noexcept;
  snd_result CheckReferences() noexcept;
  snd_result CheckBlobs() noexcept;

  // Column reader: one known column of one table, typed by its ColumnInfo.
  struct Col {
    const void* p = nullptr;
    std::uint32_t bytes = 4;
    bool isFloat = false;
    std::uint64_t def = 0;
    std::uint32_t chunk = 0;
    std::uint16_t id = 0;
    std::uint64_t Get(std::uint32_t r) const noexcept {
      if (p == nullptr) return def;
      if (bytes == 4) {
        std::uint32_t v;
        std::memcpy(&v, static_cast<const std::uint8_t*>(p) + std::uint64_t{r} * 4u, 4);
        return v;
      }
      std::uint64_t v;
      std::memcpy(&v, static_cast<const std::uint8_t*>(p) + std::uint64_t{r} * 8u, 8);
      return v;
    }
  };
  Col C(std::uint32_t chunk, std::uint16_t id) const noexcept {
    Col c;
    c.chunk = chunk;
    c.id = id;
    const int idx = KnownChunkIndex(chunk);
    const bf::ColumnInfo* info = FindColumnInfo(chunk, id);
    if (idx < 0 || info == nullptr) return c;
    c.p = tables_[idx].columns[id];
    c.bytes = bf::ColumnTypeBytes(info->type);
    c.isFloat = info->type == bf::ColumnType::F32;
    c.def = static_cast<std::uint64_t>(info->defaultValue);
    return c;
  }
  std::uint32_t RowsOf(std::uint32_t chunk) const noexcept {
    const int idx = KnownChunkIndex(chunk);
    return idx >= 0 ? tables_[idx].rows : 0u;
  }
  std::uint64_t BytesOf(std::uint32_t chunk) const noexcept {
    const int idx = KnownChunkIndex(chunk);
    return idx >= 0 && chunks_[idx].data != nullptr ? chunks_[idx].bytes : 0u;
  }

  snd_result Row(std::uint32_t chunk, std::uint16_t col, std::uint32_t target, bool allowNone) noexcept;
  snd_result Range(std::uint32_t chunk, std::uint16_t first, std::uint16_t count,
                   std::uint32_t target) noexcept;
  snd_result Blob(std::uint32_t chunk, std::uint16_t off, std::uint16_t len, std::uint32_t target,
                  std::uint32_t align) noexcept;
  snd_result Str(std::uint32_t chunk, std::uint16_t off, std::uint32_t pool) noexcept;
  // Offset of the last NUL in a blob chunk + 1 (0 = none): offsets below it are terminated.
  std::uint64_t TerminatedLimit(std::uint32_t pool) const noexcept {
    const int idx = KnownChunkIndex(pool);
    if (idx < 0 || chunks_[idx].data == nullptr) return 0;
    const std::uint8_t* p = chunks_[idx].data;
    for (std::uint64_t i = chunks_[idx].bytes; i > 0; --i) {
      if (p[i - 1] == 0) return i;
    }
    return 0;
  }

  const std::uint8_t* d_;
  std::uint64_t n_;
  ParseError* err_;
  bf::FileHeader h_{};
  ChunkRef chunks_[kKnownChunkCount] = {};
  TableView tables_[kKnownChunkCount] = {};
};

bool OptionalChunk(std::uint32_t fourcc) noexcept {
  return fourcc == bf::chunk::STRS || fourcc == bf::chunk::NAME || fourcc == bf::chunk::GUID ||
         fourcc == bf::chunk::PTCH;
}

snd_result Checker::CheckHeader(const ParseOptions& opt) noexcept {
  if (n_ < sizeof(bf::FileHeader)) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "file is %llu bytes, smaller than the %u-byte header",
                static_cast<unsigned long long>(n_), static_cast<unsigned>(sizeof(bf::FileHeader)));
  }
  std::memcpy(&h_, d_, sizeof(h_));
  const std::uint32_t hc = detail::Crc32cFast(0, d_, offsetof(bf::FileHeader, headerChecksum));
  if (hc != h_.headerChecksum) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "header checksum mismatch (stored %08x, computed %08x)",
                h_.headerChecksum, hc);
  }
  if (h_.magic != bf::kMagic) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "bad magic %08x (expected 'SNDB')", h_.magic);
  }
  if (h_.formatMajor != bf::kFormatMajor) {
    return Fail(SND_ERR_VERSION_MISMATCH, 0, kNoCol, kNoRow, "format %u.%u, runtime reads %u.x",
                h_.formatMajor, h_.formatMinor, bf::kFormatMajor);
  }
  if (h_.endianness != bf::kLittleEndian) {
    return Fail(SND_ERR_VERSION_MISMATCH, 0, kNoCol, kNoRow, "endianness %u is not little endian (1)",
                h_.endianness);
  }
  if (h_.alignLog2 != bf::kAlignLog2) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "alignLog2 %u (expected %u)", h_.alignLog2,
                bf::kAlignLog2);
  }
  if (h_.headerBytes != sizeof(bf::FileHeader)) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "headerBytes %u (expected 128)", h_.headerBytes);
  }
  if (h_.platform != bf::kPlatformAny && h_.platform != opt.platform) {
    char a[5];
    char b[5];
    FourccText(h_.platform, a);
    FourccText(opt.platform, b);
    return Fail(SND_ERR_VERSION_MISMATCH, 0, kNoCol, kNoRow, "bank platform '%s' does not match runtime '%s'",
                a, b);
  }
  if ((h_.flags & bf::kFileLanguageBank) != 0u && h_.languageId == 0u) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "language bank without languageId");
  }
  if (h_.bankId == 0u) return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "bankId is 0");
  if (h_.fileBytes != n_) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "fileBytes %llu but %llu bytes were read",
                static_cast<unsigned long long>(h_.fileBytes), static_cast<unsigned long long>(n_));
  }
  const std::uint32_t c = detail::Crc32cFast(0, d_ + h_.headerBytes, n_ - h_.headerBytes);
  if (c != h_.checksum) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "checksum mismatch (stored %08x, computed %08x)",
                h_.checksum, c);
  }
  return SND_OK;
}

snd_result Checker::CheckDependencies() noexcept {
  if (h_.dependencyCount == 0) return SND_OK;
  if (h_.dependencyCount > kMaxDependencies) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "%u dependencies (limit %u)", h_.dependencyCount,
                kMaxDependencies);
  }
  const std::uint64_t bytes = std::uint64_t{h_.dependencyCount} * sizeof(bf::Dependency);
  if (h_.dependencyOffset < h_.headerBytes || !InRange(h_.dependencyOffset, bytes, n_)) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "dependency table [%llu, +%llu) out of range",
                static_cast<unsigned long long>(h_.dependencyOffset), static_cast<unsigned long long>(bytes));
  }
  for (std::uint32_t i = 0; i < h_.dependencyCount; ++i) {
    bf::Dependency di;
    std::memcpy(&di, d_ + h_.dependencyOffset + std::uint64_t{i} * sizeof(di), sizeof(di));
    if (di.bankId == 0u || di.bankId == h_.bankId) {
      return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "dependency %u has invalid bank id %08x", i, di.bankId);
    }
    for (std::uint32_t j = 0; j < i; ++j) {
      bf::Dependency dj;
      std::memcpy(&dj, d_ + h_.dependencyOffset + std::uint64_t{j} * sizeof(dj), sizeof(dj));
      if (dj.bankId == di.bankId) {
        return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "dependency on bank %08x listed twice", di.bankId);
      }
    }
  }
  return SND_OK;
}

snd_result Checker::CheckChunks() noexcept {
  if (h_.chunkCount > kMaxChunks) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "%u chunks (limit %u)", h_.chunkCount, kMaxChunks);
  }
  if (h_.chunkCount == 0) return SND_OK;
  const std::uint64_t tableBytes = std::uint64_t{h_.chunkCount} * sizeof(bf::ChunkEntry);
  if (h_.chunkTableOffset < h_.headerBytes || !InRange(h_.chunkTableOffset, tableBytes, n_)) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "chunk table [%llu, +%llu) out of range",
                static_cast<unsigned long long>(h_.chunkTableOffset),
                static_cast<unsigned long long>(tableBytes));
  }
  const std::uint64_t depBytes = std::uint64_t{h_.dependencyCount} * sizeof(bf::Dependency);
  if (Overlap(h_.chunkTableOffset, tableBytes, h_.dependencyOffset, depBytes)) {
    return Fail(SND_ERR_CORRUPT, 0, kNoCol, kNoRow, "chunk table overlaps the dependency table");
  }
  auto entry = [&](std::uint32_t i) {
    bf::ChunkEntry e;
    std::memcpy(&e, d_ + h_.chunkTableOffset + std::uint64_t{i} * sizeof(e), sizeof(e));
    return e;
  };
  for (std::uint32_t i = 0; i < h_.chunkCount; ++i) {
    const bf::ChunkEntry e = entry(i);
    if (e.offset % bf::kAlignment != 0) {
      return Fail(SND_ERR_CORRUPT, e.fourcc, kNoCol, kNoRow, "chunk offset %llu is not a multiple of 64",
                  static_cast<unsigned long long>(e.offset));
    }
    if (e.offset < h_.headerBytes || !InRange(e.offset, e.bytes, n_)) {
      return Fail(SND_ERR_CORRUPT, e.fourcc, kNoCol, kNoRow, "chunk [%llu, +%llu) out of range",
                  static_cast<unsigned long long>(e.offset), static_cast<unsigned long long>(e.bytes));
    }
    if (Overlap(e.offset, e.bytes, h_.chunkTableOffset, tableBytes) ||
        Overlap(e.offset, e.bytes, h_.dependencyOffset, depBytes)) {
      return Fail(SND_ERR_CORRUPT, e.fourcc, kNoCol, kNoRow, "chunk overlaps the chunk or dependency table");
    }
    for (std::uint32_t j = 0; j < i; ++j) {
      const bf::ChunkEntry o = entry(j);
      if (o.fourcc == e.fourcc) {
        return Fail(SND_ERR_CORRUPT, e.fourcc, kNoCol, kNoRow, "chunk appears twice");
      }
      if (Overlap(e.offset, e.bytes, o.offset, o.bytes)) {
        char fc[5];
        FourccText(o.fourcc, fc);
        return Fail(SND_ERR_CORRUPT, e.fourcc, kNoCol, kNoRow, "chunk overlaps chunk %s", fc);
      }
    }
    const int idx = KnownChunkIndex(e.fourcc);
    if (idx < 0) continue;  // unknown chunk: ignored (§5, §7)
    const bf::ChunkInfo& info = bf::kChunks[idx];
    const bool isTable = (e.flags & bf::kChunkTable) != 0u;
    if (isTable != (info.kind == bf::ChunkKind::Table)) {
      return Fail(SND_ERR_CORRUPT, e.fourcc, kNoCol, kNoRow, "table flag %u does not match the chunk kind",
                  isTable ? 1u : 0u);
    }
    if (e.version != 1u) {
      if (OptionalChunk(e.fourcc)) continue;  // optional: ignore the unknown layout
      return Fail(SND_ERR_VERSION_MISMATCH, e.fourcc, kNoCol, kNoRow, "chunk layout version %u (runtime knows 1)",
                  e.version);
    }
    chunks_[idx].data = d_ + e.offset;
    chunks_[idx].bytes = e.bytes;
    chunks_[idx].version = e.version;
    chunks_[idx].flags = e.flags;
  }
  return SND_OK;
}

snd_result Checker::CheckTable(int idx) noexcept {
  const std::uint32_t fourcc = bf::kChunks[idx].fourcc;
  const std::uint8_t* c = chunks_[idx].data;
  const std::uint64_t n = chunks_[idx].bytes;
  if (n < sizeof(bf::TableChunkHeader)) {
    return Fail(SND_ERR_CORRUPT, fourcc, kNoCol, kNoRow, "table chunk of %llu bytes has no header",
                static_cast<unsigned long long>(n));
  }
  bf::TableChunkHeader th;
  std::memcpy(&th, c, sizeof(th));
  const std::uint64_t dirEnd = sizeof(th) + std::uint64_t{th.columnCount} * sizeof(bf::ColumnEntry);
  if (dirEnd > n) {
    return Fail(SND_ERR_CORRUPT, fourcc, kNoCol, kNoRow, "column directory (%u columns) exceeds the chunk",
                th.columnCount);
  }
  static_assert(sizeof(std::uint64_t) * 1024 * 8 == 65536);
  std::uint64_t seen[1024] = {};
  TableView& t = tables_[idx];
  t.rows = th.rowCount;
  t.capacity = th.rowCount;
  std::uint64_t knownOff[kMaxTableColumns] = {};
  std::uint64_t knownLen[kMaxTableColumns] = {};
  for (std::uint32_t i = 0; i < th.columnCount; ++i) {
    bf::ColumnEntry ce;
    std::memcpy(&ce, c + sizeof(th) + std::uint64_t{i} * sizeof(ce), sizeof(ce));
    std::uint64_t& word = seen[ce.columnId >> 6];
    const std::uint64_t bit = std::uint64_t{1} << (ce.columnId & 63u);
    if ((word & bit) != 0) {
      return Fail(SND_ERR_CORRUPT, fourcc, ce.columnId, kNoRow, "column id appears twice");
    }
    word |= bit;
    const std::uint64_t len = std::uint64_t{th.rowCount} * ce.elemBytes;
    if (ce.offset % bf::kAlignment != 0 || ce.offset < dirEnd || !InRange(ce.offset, len, n)) {
      return Fail(SND_ERR_CORRUPT, fourcc, ce.columnId, kNoRow,
                  "column array [%llu, +%llu) misaligned or out of the chunk (%llu bytes)",
                  static_cast<unsigned long long>(ce.offset), static_cast<unsigned long long>(len),
                  static_cast<unsigned long long>(n));
    }
    const bf::ColumnInfo* info = FindColumnInfo(fourcc, ce.columnId);
    if (info == nullptr) continue;  // unknown column (newer minor): ignored
    if (ce.elemBytes != bf::ColumnTypeBytes(info->type)) {
      return Fail(SND_ERR_CORRUPT, fourcc, ce.columnId, kNoRow, "elemBytes %u, type needs %u", ce.elemBytes,
                  bf::ColumnTypeBytes(info->type));
    }
    for (std::uint32_t k = 0; k < kMaxTableColumns; ++k) {
      if (t.columns[k] != nullptr && Overlap(ce.offset, len, knownOff[k], knownLen[k])) {
        return Fail(SND_ERR_CORRUPT, fourcc, ce.columnId, kNoRow, "column array overlaps column %u", k);
      }
    }
    knownOff[ce.columnId] = ce.offset;
    knownLen[ce.columnId] = len;
    t.columns[ce.columnId] = const_cast<std::uint8_t*>(c + ce.offset);
  }
  for (const bf::ColumnInfo& info : bf::kColumns) {
    if (info.chunk == fourcc && info.required && t.columns[info.columnId] == nullptr) {
      return Fail(SND_ERR_CORRUPT, fourcc, info.columnId, kNoRow, "required column %s is missing", info.name);
    }
  }
  return SND_OK;
}

snd_result Checker::Row(std::uint32_t chunk, std::uint16_t col, std::uint32_t target, bool allowNone) noexcept {
  const Col c = C(chunk, col);
  const std::uint32_t rows = RowsOf(chunk);
  const std::uint32_t limit = RowsOf(target);
  for (std::uint32_t r = 0; r < rows; ++r) {
    const std::uint64_t v = c.Get(r);
    if (v == kNone && allowNone) continue;
    if (v >= limit) {
      char fc[5];
      FourccText(target, fc);
      return Fail(SND_ERR_CORRUPT, chunk, col, r, "row reference %llu out of range (%s has %u rows)",
                  static_cast<unsigned long long>(v), fc, limit);
    }
  }
  return SND_OK;
}

snd_result Checker::Range(std::uint32_t chunk, std::uint16_t first, std::uint16_t count,
                          std::uint32_t target) noexcept {
  const Col f = C(chunk, first);
  const Col n = C(chunk, count);
  const std::uint32_t rows = RowsOf(chunk);
  const std::uint64_t limit = RowsOf(target);
  for (std::uint32_t r = 0; r < rows; ++r) {
    const std::uint64_t cnt = n.Get(r);
    if (cnt == 0) continue;
    const std::uint64_t fst = f.Get(r);
    if (fst > limit || cnt > limit - fst) {
      char fc[5];
      FourccText(target, fc);
      return Fail(SND_ERR_CORRUPT, chunk, first, r, "range [%llu, +%llu) out of %s (%llu rows)",
                  static_cast<unsigned long long>(fst), static_cast<unsigned long long>(cnt), fc,
                  static_cast<unsigned long long>(limit));
    }
  }
  return SND_OK;
}

snd_result Checker::Blob(std::uint32_t chunk, std::uint16_t off, std::uint16_t len, std::uint32_t target,
                         std::uint32_t align) noexcept {
  const Col o = C(chunk, off);
  const Col l = C(chunk, len);
  const std::uint32_t rows = RowsOf(chunk);
  const std::uint64_t size = BytesOf(target);
  for (std::uint32_t r = 0; r < rows; ++r) {
    const std::uint64_t bytes = l.Get(r);
    if (bytes == 0) continue;
    const std::uint64_t at = o.Get(r);
    if (!InRange(at, bytes, size) || at % align != 0) {
      char fc[5];
      FourccText(target, fc);
      return Fail(SND_ERR_CORRUPT, chunk, off, r, "blob range [%llu, +%llu) outside %s (%llu bytes) or misaligned",
                  static_cast<unsigned long long>(at), static_cast<unsigned long long>(bytes), fc,
                  static_cast<unsigned long long>(size));
    }
  }
  return SND_OK;
}

snd_result Checker::Str(std::uint32_t chunk, std::uint16_t off, std::uint32_t pool) noexcept {
  const Col o = C(chunk, off);
  const std::uint32_t rows = RowsOf(chunk);
  const std::uint64_t limit = TerminatedLimit(pool);
  for (std::uint32_t r = 0; r < rows; ++r) {
    const std::uint64_t at = o.Get(r);
    if (at >= limit) {
      return Fail(SND_ERR_CORRUPT, chunk, off, r, "string offset %llu is not a terminated string in the pool",
                  static_cast<unsigned long long>(at));
    }
  }
  return SND_OK;
}

#define SND_TRY(expr)                    \
  do {                                   \
    const snd_result snd_try_r_ = (expr); \
    if (snd_try_r_ != SND_OK) return snd_try_r_; \
  } while (0)

snd_result Checker::CheckReferences() noexcept {
  namespace ch = bf::chunk;
  namespace c = bf::col;
  // EVNT
  SND_TRY(Range(ch::EVNT, c::EVNT_trackFirst, c::EVNT_trackCount, ch::TRAK));
  SND_TRY(Range(ch::EVNT, c::EVNT_curveFirst, c::EVNT_curveCount, ch::CURV));
  SND_TRY(Row(ch::EVNT, c::EVNT_bus, ch::BUSS, false));
  SND_TRY(Row(ch::EVNT, c::EVNT_category, ch::CATG, true));
  SND_TRY(Row(ch::EVNT, c::EVNT_limitGroup, ch::VLGR, true));
  SND_TRY(Row(ch::EVNT, c::EVNT_curveVolume, ch::CURV, true));
  SND_TRY(Row(ch::EVNT, c::EVNT_curveLowpass, ch::CURV, true));
  SND_TRY(Row(ch::EVNT, c::EVNT_curveSpread, ch::CURV, true));
  SND_TRY(Row(ch::EVNT, c::EVNT_curveReverbSend, ch::CURV, true));
  SND_TRY(Row(ch::EVNT, c::EVNT_curveBlend3D, ch::CURV, true));
  SND_TRY(Row(ch::EVNT, c::EVNT_musicState, ch::MSTA, true));
  // TRAK
  SND_TRY(Row(ch::TRAK, c::TRAK_event, ch::EVNT, false));
  SND_TRY(Range(ch::TRAK, c::TRAK_clipFirst, c::TRAK_clipCount, ch::CLIP));
  SND_TRY(Range(ch::TRAK, c::TRAK_actionFirst, c::TRAK_actionCount, ch::ACTN));
  SND_TRY(Row(ch::TRAK, c::TRAK_bus, ch::BUSS, true));
  // CLIP: target depends on kind.
  {
    const Col kind = C(ch::CLIP, c::CLIP_kind);
    const Col target = C(ch::CLIP, c::CLIP_target);
    const std::uint32_t targets[3] = {ch::ASST, ch::EVNT, ch::SYNP};
    for (std::uint32_t r = 0; r < RowsOf(ch::CLIP); ++r) {
      const std::uint64_t k = kind.Get(r);
      if (k > 2) return Fail(SND_ERR_CORRUPT, ch::CLIP, c::CLIP_kind, r, "unknown clip kind %llu",
                             static_cast<unsigned long long>(k));
      const std::uint64_t t = target.Get(r);
      if (t >= RowsOf(targets[k])) {
        return Fail(SND_ERR_CORRUPT, ch::CLIP, c::CLIP_target, r, "clip target row %llu out of range",
                    static_cast<unsigned long long>(t));
      }
    }
  }
  // BUSS: graph order (inputs before outputs), sends, effects.
  SND_TRY(Row(ch::BUSS, c::BUSS_parent, ch::BUSS, true));
  SND_TRY(Range(ch::BUSS, c::BUSS_sendFirst, c::BUSS_sendCount, ch::SEND));
  SND_TRY(Range(ch::BUSS, c::BUSS_effectFirst, c::BUSS_effectCount, ch::EFFX));
  SND_TRY(Row(ch::SEND, c::SEND_targetBus, ch::BUSS, false));
  {
    const Col parent = C(ch::BUSS, c::BUSS_parent);
    const Col sf = C(ch::BUSS, c::BUSS_sendFirst);
    const Col sc = C(ch::BUSS, c::BUSS_sendCount);
    const Col st = C(ch::SEND, c::SEND_targetBus);
    for (std::uint32_t r = 0; r < RowsOf(ch::BUSS); ++r) {
      const std::uint64_t p = parent.Get(r);
      if (p != kNone && p <= r) {
        return Fail(SND_ERR_CORRUPT, ch::BUSS, c::BUSS_parent, r,
                    "parent row %llu is not after the bus (graph order)", static_cast<unsigned long long>(p));
      }
      const std::uint64_t cnt = sc.Get(r);
      for (std::uint64_t s = 0; s < cnt; ++s) {
        const std::uint64_t t = st.Get(static_cast<std::uint32_t>(sf.Get(r) + s));
        if (t <= r) {
          return Fail(SND_ERR_CORRUPT, ch::BUSS, c::BUSS_sendFirst, r,
                      "send target row %llu is not after the bus (graph order)", static_cast<unsigned long long>(t));
        }
      }
    }
  }
  // EFFX / CURV / CATG / SNAP
  SND_TRY(Range(ch::EFFX, c::EFFX_paramFirst, c::EFFX_paramCount, ch::EFPV));
  SND_TRY(Blob(ch::EFFX, c::EFFX_blobOffset, c::EFFX_blobBytes, ch::EFBL, 1));
  SND_TRY(Range(ch::CURV, c::CURV_pointFirst, c::CURV_pointCount, ch::CPNT));
  SND_TRY(Row(ch::CATG, c::CATG_parent, ch::CATG, true));
  SND_TRY(Range(ch::SNAP, c::SNAP_valueFirst, c::SNAP_valueCount, ch::SNPV));
  {
    const Col kind = C(ch::SNPV, c::SNPV_targetKind);
    const Col row = C(ch::SNPV, c::SNPV_targetRow);
    const Col pi = C(ch::SNPV, c::SNPV_paramIndex);
    const Col pc = C(ch::EFFX, c::EFFX_paramCount);
    const std::uint32_t targets[3] = {ch::BUSS, ch::EFFX, ch::SEND};
    for (std::uint32_t r = 0; r < RowsOf(ch::SNPV); ++r) {
      const std::uint64_t k = kind.Get(r);
      if (k > 2) return Fail(SND_ERR_CORRUPT, ch::SNPV, c::SNPV_targetKind, r, "unknown target kind %llu",
                             static_cast<unsigned long long>(k));
      const std::uint64_t t = row.Get(r);
      if (t >= RowsOf(targets[k])) {
        return Fail(SND_ERR_CORRUPT, ch::SNPV, c::SNPV_targetRow, r, "target row %llu out of range",
                    static_cast<unsigned long long>(t));
      }
      if (k == 1 && pi.Get(r) >= pc.Get(static_cast<std::uint32_t>(t))) {
        return Fail(SND_ERR_CORRUPT, ch::SNPV, c::SNPV_paramIndex, r, "effect parameter index out of range");
      }
    }
  }
  // DUCK
  SND_TRY(Row(ch::DUCK, c::DUCK_targetBus, ch::BUSS, false));
  {
    const Col kind = C(ch::DUCK, c::DUCK_kind);
    const Col src = C(ch::DUCK, c::DUCK_source);
    for (std::uint32_t r = 0; r < RowsOf(ch::DUCK); ++r) {
      const std::uint64_t k = kind.Get(r);
      if (k > 1) return Fail(SND_ERR_CORRUPT, ch::DUCK, c::DUCK_kind, r, "unknown ducking kind %llu",
                             static_cast<unsigned long long>(k));
      if (src.Get(r) >= RowsOf(k == 0 ? ch::BUSS : ch::CATG)) {
        return Fail(SND_ERR_CORRUPT, ch::DUCK, c::DUCK_source, r, "source row out of range");
      }
    }
  }
  // ASST
  SND_TRY(Blob(ch::ASST, c::ASST_dataOffset, c::ASST_dataBytes, ch::WAVE, bf::kAlignment));
  SND_TRY(Blob(ch::ASST, c::ASST_extraOffset, c::ASST_extraBytes, ch::WAVE, 1));
  SND_TRY(Blob(ch::ASST, c::ASST_preloadOffset, c::ASST_preloadBytes, ch::PREL, 1));
  SND_TRY(Range(ch::ASST, c::ASST_seekFirst, c::ASST_seekCount, ch::SEEK));
  SND_TRY(Row(ch::ASST, c::ASST_stream, ch::STRM, true));
  {
    const Col form = C(ch::ASST, c::ASST_loadForm);
    const Col stream = C(ch::ASST, c::ASST_stream);
    const Col chans = C(ch::ASST, c::ASST_channels);
    const Col rate = C(ch::ASST, c::ASST_sampleRate);
    for (std::uint32_t r = 0; r < RowsOf(ch::ASST); ++r) {
      if (form.Get(r) == static_cast<std::uint32_t>(bf::LoadForm::Stream) && stream.Get(r) == kNone) {
        return Fail(SND_ERR_CORRUPT, ch::ASST, c::ASST_stream, r, "stream asset without STRM row");
      }
      const std::uint64_t n = chans.Get(r);
      if (n == 0 || n > SND_MAX_CHANNELS) {
        return Fail(SND_ERR_CORRUPT, ch::ASST, c::ASST_channels, r, "channel count %llu not in 1..%u",
                    static_cast<unsigned long long>(n), SND_MAX_CHANNELS);
      }
      if (rate.Get(r) == 0) return Fail(SND_ERR_CORRUPT, ch::ASST, c::ASST_sampleRate, r, "sample rate 0");
    }
  }
  // STRM
  SND_TRY(Str(ch::STRM, c::STRM_pathOffset, ch::SPTH));
  {
    const Col align = C(ch::STRM, c::STRM_sectorAlign);
    const Col off = C(ch::STRM, c::STRM_dataOffset);
    const Col size = C(ch::STRM, c::STRM_fileBytes);
    for (std::uint32_t r = 0; r < RowsOf(ch::STRM); ++r) {
      const std::uint64_t a = align.Get(r);
      if (a == 0 || (a & (a - 1)) != 0 || off.Get(r) % a != 0 || off.Get(r) > size.Get(r)) {
        return Fail(SND_ERR_CORRUPT, ch::STRM, c::STRM_sectorAlign, r,
                    "sector alignment %llu invalid or data offset not aligned / beyond the file",
                    static_cast<unsigned long long>(a));
      }
    }
  }
  // SYNP
  SND_TRY(Row(ch::SYNP, c::SYNP_asset, ch::ASST, true));
  SND_TRY(Blob(ch::SYNP, c::SYNP_programOffset, c::SYNP_programBytes, ch::PTCH, 1));
  // Music
  SND_TRY(Row(ch::MSEG, c::MSEG_asset, ch::ASST, false));
  SND_TRY(Range(ch::MSEG, c::MSEG_tempoFirst, c::MSEG_tempoCount, ch::MTMP));
  SND_TRY(Range(ch::MSEG, c::MSEG_markerFirst, c::MSEG_markerCount, ch::MMRK));
  SND_TRY(Range(ch::MPLS, c::MPLS_entryFirst, c::MPLS_entryCount, ch::MPLE));
  SND_TRY(Row(ch::MPLE, c::MPLE_segment, ch::MSEG, false));
  SND_TRY(Row(ch::MSTM, c::MSTM_segment, ch::MSEG, false));
  SND_TRY(Row(ch::MSTM, c::MSTM_volumeCurve, ch::CURV, true));
  SND_TRY(Row(ch::MSTM, c::MSTM_asset, ch::ASST, true));
  {
    const Col seg = C(ch::MSTM, c::MSTM_segment);
    const Col asset = C(ch::MSTM, c::MSTM_asset);
    const Col first = C(ch::MSTM, c::MSTM_channelFirst);
    const Col count = C(ch::MSTM, c::MSTM_channelCount);
    const Col segAsset = C(ch::MSEG, c::MSEG_asset);
    const Col chans = C(ch::ASST, c::ASST_channels);
    for (std::uint32_t r = 0; r < RowsOf(ch::MSTM); ++r) {
      std::uint64_t a = asset.Get(r);
      if (a == kNone) a = segAsset.Get(static_cast<std::uint32_t>(seg.Get(r)));
      const std::uint64_t n = chans.Get(static_cast<std::uint32_t>(a));
      const std::uint64_t f = first.Get(r);
      const std::uint64_t k = count.Get(r);
      if (f > n || k > n - f) {
        return Fail(SND_ERR_CORRUPT, ch::MSTM, c::MSTM_channelFirst, r, "stem channels [%llu, +%llu) exceed %llu",
                    static_cast<unsigned long long>(f), static_cast<unsigned long long>(k),
                    static_cast<unsigned long long>(n));
      }
    }
  }
  SND_TRY(Row(ch::MSTA, c::MSTA_playlist, ch::MPLS, false));
  SND_TRY(Row(ch::MSTA, c::MSTA_musicEvent, ch::EVNT, false));
  SND_TRY(Row(ch::MTRN, c::MTRN_musicEvent, ch::EVNT, false));
  SND_TRY(Row(ch::MTRN, c::MTRN_bridgeSegment, ch::MSEG, true));
  SND_TRY(Row(ch::MSTG, c::MSTG_event, ch::EVNT, false));
  // NAME (only when the string pool is usable; both are strippable together)
  if (RowsOf(ch::NAME) > 0) SND_TRY(Str(ch::NAME, c::NAME_stringOffset, ch::STRS));
  return SND_OK;
}

snd_result Checker::CheckBlobs() noexcept {
  namespace ch = bf::chunk;
  const int dspn = KnownChunkIndex(ch::DSPN);
  if (chunks_[dspn].data != nullptr && chunks_[dspn].bytes < sizeof(bf::DspPoolNeed)) {
    return Fail(SND_ERR_CORRUPT, ch::DSPN, kNoCol, kNoRow, "DSPN is %llu bytes (needs 16)",
                static_cast<unsigned long long>(chunks_[dspn].bytes));
  }
  const int phys = KnownChunkIndex(ch::PHYS);
  if (chunks_[phys].data == nullptr) return SND_OK;
  const std::uint8_t* p = chunks_[phys].data;
  const std::uint64_t n = chunks_[phys].bytes;
  if (n < sizeof(bf::PhysChunkHeader)) {
    return Fail(SND_ERR_CORRUPT, ch::PHYS, kNoCol, kNoRow, "PHYS has no header");
  }
  bf::PhysChunkHeader ph;
  std::memcpy(&ph, p, sizeof(ph));
  if (ph.tablesOffset % 8 != 0 ||
      !InRange(ph.tablesOffset, std::uint64_t{ph.tableCount} * sizeof(bf::PhysTable), n)) {
    return Fail(SND_ERR_CORRUPT, ch::PHYS, kNoCol, kNoRow, "PHYS table array out of range");
  }
  for (std::uint32_t i = 0; i < ph.tableCount; ++i) {
    bf::PhysTable t;
    std::memcpy(&t, p + ph.tablesOffset + std::uint64_t{i} * sizeof(t), sizeof(t));
    const std::uint64_t m = t.materialCount;
    if (m != 0 && m > (UINT64_MAX / 4u) / m) {
      return Fail(SND_ERR_CORRUPT, ch::PHYS, kNoCol, i, "material count %llu too large",
                  static_cast<unsigned long long>(m));
    }
    const bool ok = t.singleOffset % 4 == 0 && t.pairOffset % 4 == 0 && t.footstepOffset % 4 == 0 &&
                    t.curveOffset % 4 == 0 && InRange(t.singleOffset, m * 16u, n) &&
                    InRange(t.pairOffset, m * m * 4u, n) && InRange(t.footstepOffset, m * 4u, n) &&
                    m <= UINT64_MAX / 32u && InRange(t.curveOffset, m * 32u, n);
    if (!ok) return Fail(SND_ERR_CORRUPT, ch::PHYS, kNoCol, i, "PHYS table arrays out of range or misaligned");
  }
  return SND_OK;
}

snd_result Checker::Run(const ParseOptions& opt, BankView* out) noexcept {
  SND_TRY(CheckHeader(opt));
  SND_TRY(CheckDependencies());
  SND_TRY(CheckChunks());
  for (std::uint32_t i = 0; i < kKnownChunkCount; ++i) {
    if (chunks_[i].data != nullptr && bf::kChunks[i].kind == bf::ChunkKind::Table) {
      SND_TRY(CheckTable(static_cast<int>(i)));
    }
  }
  // NAME without a usable string pool is dropped (both are strippable, §6).
  const int strs = KnownChunkIndex(bf::chunk::STRS);
  const int name = KnownChunkIndex(bf::chunk::NAME);
  if (chunks_[strs].data == nullptr && chunks_[name].data != nullptr) {
    chunks_[name] = ChunkRef{};
    tables_[name] = TableView{};
  }
  SND_TRY(CheckReferences());
  SND_TRY(CheckBlobs());
  ViewAccess::File(*out) = d_;
  ViewAccess::Header(*out) = h_;
  std::memcpy(ViewAccess::Chunks(*out), chunks_, sizeof(chunks_));
  std::memcpy(static_cast<void*>(ViewAccess::Tables(*out)), tables_, sizeof(tables_));
  ViewAccess::Writable(*out) = false;
  return SND_OK;
}

}  // namespace

std::uint32_t RuntimePlatform() noexcept {
#if defined(_WIN32)
  return bf::kPlatformWindows;
#elif defined(__ANDROID__)
  return bf::kPlatformAndroid;
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
  return bf::kPlatformIos;
#else
  return bf::kPlatformMac;
#endif
#elif defined(__linux__)
  return bf::kPlatformLinux;
#else
  return bf::kPlatformAny;
#endif
}

snd_result ParseBank(const void* data, std::uint64_t bytes, const ParseOptions& options, BankView* out,
                     ParseError* error) noexcept {
  if (error != nullptr) *error = ParseError{};
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  *out = BankView{};
  if (data == nullptr || (reinterpret_cast<std::uintptr_t>(data) % bf::kAlignment) != 0) {
    if (error != nullptr) {
      error->code = SND_ERR_INVALID_ARGUMENT;
      std::snprintf(error->message, sizeof(error->message), "bank memory must be 64-byte aligned");
    }
    return SND_ERR_INVALID_ARGUMENT;
  }
  Checker c(static_cast<const std::uint8_t*>(data), bytes, error);
  const snd_result r = c.Run(options, out);
  if (r != SND_OK) *out = BankView{};
  return r;
}

}  // namespace snd::bank
