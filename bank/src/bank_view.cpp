#include "snd/bank/bank_view.hpp"

#include <cstring>

namespace snd::bank {
namespace {
template <class T>
const T* Cast(const std::uint8_t* p) noexcept {
  return static_cast<const T*>(static_cast<const void*>(p));
}
}  // namespace

ByteSpan BankView::FindChunk(std::uint32_t fourcc, std::uint16_t* flags) const noexcept {
  if (flags != nullptr) *flags = 0;
  if (file_ == nullptr) return {};
  for (std::uint32_t i = 0; i < header_.chunkCount; ++i) {
    bf::ChunkEntry e;
    std::memcpy(&e, file_ + header_.chunkTableOffset + std::uint64_t{i} * sizeof(e), sizeof(e));
    if (e.fourcc == fourcc) {
      if (flags != nullptr) *flags = e.flags;
      return ByteSpan{file_ + e.offset, e.bytes};
    }
  }
  return {};
}

ByteSpan BankView::WaveData(std::uint32_t assetRow) const noexcept {
  if (assetRow >= Rows(bf::chunk::ASST)) return {};
  const ByteSpan wave = Chunk(bf::chunk::WAVE);
  const std::uint64_t bytes = Column<col::ASST_dataBytes>()[assetRow];
  if (bytes == 0 || wave.data == nullptr) return {};
  return ByteSpan{wave.data + Column<col::ASST_dataOffset>()[assetRow], bytes};
}

ByteSpan BankView::AssetExtra(std::uint32_t assetRow) const noexcept {
  if (assetRow >= Rows(bf::chunk::ASST)) return {};
  const ByteSpan wave = Chunk(bf::chunk::WAVE);
  const std::uint64_t bytes = Column<col::ASST_extraBytes>()[assetRow];
  if (bytes == 0 || wave.data == nullptr) return {};
  return ByteSpan{wave.data + Column<col::ASST_extraOffset>()[assetRow], bytes};
}

ByteSpan BankView::Preload(std::uint32_t assetRow) const noexcept {
  if (assetRow >= Rows(bf::chunk::ASST)) return {};
  const ByteSpan prel = Chunk(bf::chunk::PREL);
  const std::uint64_t bytes = Column<col::ASST_preloadBytes>()[assetRow];
  if (bytes == 0 || prel.data == nullptr) return {};
  return ByteSpan{prel.data + Column<col::ASST_preloadOffset>()[assetRow], bytes};
}

RangeU32 BankView::SeekRange(std::uint32_t assetRow) const noexcept {
  if (assetRow >= Rows(bf::chunk::ASST)) return RangeU32{bf::kNone, 0};
  return RangeU32{Column<col::ASST_seekFirst>()[assetRow], Column<col::ASST_seekCount>()[assetRow]};
}

namespace {
const char* PoolString(const ByteSpan& pool, std::uint64_t offset) noexcept {
  if (pool.data == nullptr || offset >= pool.bytes) return nullptr;
  const void* nul = std::memchr(pool.data + offset, 0, static_cast<std::size_t>(pool.bytes - offset));
  return nul != nullptr ? reinterpret_cast<const char*>(pool.data + offset) : nullptr;
}
}  // namespace

const char* BankView::StreamPath(std::uint32_t streamRow) const noexcept {
  if (streamRow >= Rows(bf::chunk::STRM)) return nullptr;
  return PoolString(Chunk(bf::chunk::SPTH), Column<col::STRM_pathOffset>()[streamRow]);
}

const char* BankView::String(std::uint32_t offset) const noexcept {
  return PoolString(Chunk(bf::chunk::STRS), offset);
}

const char* BankView::Name(Id id) const noexcept {
  const auto ids = Column<col::NAME_id>();
  const auto kinds = Column<col::NAME_kind>();
  const auto offs = Column<col::NAME_stringOffset>();
  for (std::uint32_t r = 0; r < ids.rows(); ++r) {
    if (ids[r] == id && kinds[r] != static_cast<std::uint32_t>(bf::NameKind::Alias)) return String(offs[r]);
  }
  return nullptr;
}

bool BankView::Guid(Id id, std::uint64_t* lo, std::uint64_t* hi) const noexcept {
  const std::uint32_t r = FindRow<col::GUID_id>(id);
  if (r == bf::kNone) return false;
  if (lo != nullptr) *lo = Column<col::GUID_guidLo>()[r];
  if (hi != nullptr) *hi = Column<col::GUID_guidHi>()[r];
  return true;
}

bf::DspPoolNeed BankView::DspNeed() const noexcept {
  bf::DspPoolNeed n{};
  const ByteSpan s = Chunk(bf::chunk::DSPN);
  if (s.bytes >= sizeof(n)) std::memcpy(&n, s.data, sizeof(n));
  return n;
}

std::uint32_t BankView::PhysTableCount() const noexcept {
  const ByteSpan s = Chunk(bf::chunk::PHYS);
  if (s.bytes < sizeof(bf::PhysChunkHeader)) return 0;
  bf::PhysChunkHeader h;
  std::memcpy(&h, s.data, sizeof(h));
  return h.tableCount;
}

bool BankView::PhysTable(std::uint32_t index, PhysTableView* out) const noexcept {
  if (out == nullptr || index >= PhysTableCount()) return false;
  const ByteSpan s = Chunk(bf::chunk::PHYS);
  bf::PhysChunkHeader h;
  std::memcpy(&h, s.data, sizeof(h));
  bf::PhysTable t;
  std::memcpy(&t, s.data + h.tablesOffset + std::uint64_t{index} * sizeof(t), sizeof(t));
  out->id = t.id;
  out->materialCount = t.materialCount;
  out->single = Cast<std::uint32_t>(s.data + t.singleOffset);
  out->pair = Cast<std::uint32_t>(s.data + t.pairOffset);
  out->footstep = Cast<std::uint32_t>(s.data + t.footstepOffset);
  std::memcpy(out->defaultEvent, t.defaultEvent, sizeof(out->defaultEvent));
  out->curves = Cast<bf::PhysCurves>(s.data + t.curveOffset);
  return true;
}

}  // namespace snd::bank
