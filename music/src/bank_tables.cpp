#include "bank_tables.hpp"

namespace snd::music::detail {

namespace {
constexpr std::uint32_t TypeBytes(std::uint32_t fourcc, std::uint16_t id) noexcept {
  for (const bf::ColumnInfo& c : bf::kColumns) {
    if (c.chunk == fourcc && c.columnId == id) return bf::ColumnTypeBytes(c.type);
  }
  return 0;
}
}  // namespace

void Table::Bind(std::uint32_t fourcc, const std::uint8_t* chunk, std::uint64_t bytes) noexcept {
  fourcc_ = fourcc;
  rows_ = 0;
  for (std::uint32_t i = 0; i < kMaxColumns; ++i) {
    cols_[i] = nullptr;
    elem_[i] = 0;
  }
  if (chunk == nullptr || bytes < sizeof(bf::TableChunkHeader)) return;
  bf::TableChunkHeader h;
  std::memcpy(&h, chunk, sizeof h);
  if (h.columnCount > 64u || sizeof(bf::TableChunkHeader) + std::uint64_t{h.columnCount} * sizeof(bf::ColumnEntry) > bytes) {
    return;
  }
  for (std::uint32_t i = 0; i < h.columnCount; ++i) {
    bf::ColumnEntry e;
    std::memcpy(&e, chunk + sizeof(bf::TableChunkHeader) + std::size_t{i} * sizeof(bf::ColumnEntry), sizeof e);
    if (e.columnId >= kMaxColumns) continue;  // newer minor version: unknown column
    const std::uint32_t want = TypeBytes(fourcc, e.columnId);
    if (want == 0u || e.elemBytes != want) continue;
    const std::uint64_t need = std::uint64_t{h.rowCount} * e.elemBytes;
    if (e.offset > bytes || need > bytes - e.offset) continue;
    cols_[e.columnId] = chunk + e.offset;
    elem_[e.columnId] = static_cast<std::uint8_t>(e.elemBytes);
  }
  rows_ = h.rowCount;
}

bool BankTables::Bind(const MusicServices& s, std::uint16_t bankSlot) noexcept {
  bank = bankSlot;
  auto bind = [&](Table& t, std::uint32_t fourcc) {
    std::uint64_t bytes = 0;
    const std::uint8_t* p = s.bankChunk != nullptr ? s.bankChunk(s.bankCtx, bankSlot, fourcc, &bytes) : nullptr;
    t.Bind(fourcc, p, p != nullptr ? bytes : 0u);
  };
  bind(evnt, bf::chunk::EVNT);
  if (evnt.Rows() == 0u) return false;
  bind(trak, bf::chunk::TRAK);
  bind(clip, bf::chunk::CLIP);
  bind(asst, bf::chunk::ASST);
  bind(curv, bf::chunk::CURV);
  bind(cpnt, bf::chunk::CPNT);
  bind(parm, bf::chunk::PARM);
  bind(mseg, bf::chunk::MSEG);
  bind(mpls, bf::chunk::MPLS);
  bind(mple, bf::chunk::MPLE);
  bind(mstm, bf::chunk::MSTM);
  bind(msta, bf::chunk::MSTA);
  bind(mtrn, bf::chunk::MTRN);
  bind(mstg, bf::chunk::MSTG);
  bind(mmrk, bf::chunk::MMRK);
  bind(mtmp, bf::chunk::MTMP);
  return true;
}

}  // namespace snd::music::detail
