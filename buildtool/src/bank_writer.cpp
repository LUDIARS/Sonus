#include "snd/buildtool/bank_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "detail/fnv.hpp"

namespace snd::buildtool {
namespace bf = snd::bank_format;
namespace {

std::uint64_t Align64(std::uint64_t v) { return (v + 63) & ~std::uint64_t{63}; }

std::vector<std::uint8_t> DefaultBits(const bf::ColumnInfo& c) {
  std::vector<std::uint8_t> b(bf::ColumnTypeBytes(c.type));
  switch (c.type) {
    case bf::ColumnType::U32: {
      const auto v = static_cast<std::uint32_t>(c.defaultValue);
      std::memcpy(b.data(), &v, 4);
      break;
    }
    case bf::ColumnType::F32: {
      const auto v = static_cast<float>(c.defaultValue);
      std::memcpy(b.data(), &v, 4);
      break;
    }
    case bf::ColumnType::U64: {
      const auto v = static_cast<std::uint64_t>(c.defaultValue);
      std::memcpy(b.data(), &v, 8);
      break;
    }
    case bf::ColumnType::I64: {
      const auto v = static_cast<std::int64_t>(c.defaultValue);
      std::memcpy(b.data(), &v, 8);
      break;
    }
  }
  return b;
}

[[noreturn]] void Misuse(const char* what) {
  std::fprintf(stderr, "sonus-build internal error: %s\n", what);
  std::abort();
}

}  // namespace

std::uint32_t ChunkOrder(std::uint32_t fourcc) {
  std::uint32_t i = 0;
  for (const bf::ChunkInfo& c : bf::kChunks) {
    if (c.fourcc == fourcc) return i;
    ++i;
  }
  return 0x10000u;
}

TableBuilder::TableBuilder(std::uint32_t fourcc) : fourcc_(fourcc) {
  for (const bf::ColumnInfo& c : bf::kColumns) {
    if (c.chunk != fourcc) continue;
    if (cols_.size() <= c.columnId) cols_.resize(c.columnId + 1u);
    Col& col = cols_[c.columnId];
    col.info = &c;
    col.elemBytes = bf::ColumnTypeBytes(c.type);
    col.defaultBits = DefaultBits(c);
  }
  if (cols_.empty()) Misuse("TableBuilder for a chunk without columns");
}

std::uint32_t TableBuilder::AddRow() {
  for (Col& c : cols_) c.data.insert(c.data.end(), c.defaultBits.begin(), c.defaultBits.end());
  return rows_++;
}

TableBuilder::Col& TableBuilder::At(std::uint16_t column, bf::ColumnType type) {
  if (column >= cols_.size() || cols_[column].info == nullptr || cols_[column].info->type != type) {
    Misuse("column id / type mismatch");
  }
  return cols_[column];
}

void TableBuilder::Write(std::uint16_t column, bf::ColumnType type, std::uint32_t row, const void* v) {
  Col& c = At(column, type);
  if (row >= rows_) Misuse("row out of range");
  std::memcpy(c.data.data() + std::size_t{row} * c.elemBytes, v, c.elemBytes);
}

void TableBuilder::U32(std::uint16_t column, std::uint32_t row, std::uint32_t v) {
  Write(column, bf::ColumnType::U32, row, &v);
}
void TableBuilder::F32(std::uint16_t column, std::uint32_t row, float v) {
  Write(column, bf::ColumnType::F32, row, &v);
}
void TableBuilder::U64(std::uint16_t column, std::uint32_t row, std::uint64_t v) {
  Write(column, bf::ColumnType::U64, row, &v);
}
void TableBuilder::I64(std::uint16_t column, std::uint32_t row, std::int64_t v) {
  Write(column, bf::ColumnType::I64, row, &v);
}
std::uint32_t TableBuilder::GetU32(std::uint16_t column, std::uint32_t row) const {
  const Col& c = cols_[column];
  std::uint32_t v;
  std::memcpy(&v, c.data.data() + std::size_t{row} * 4, 4);
  return v;
}

std::vector<std::uint8_t> TableBuilder::Serialize() const {
  std::vector<const Col*> out;
  for (const Col& c : cols_) {
    if (c.info == nullptr) continue;
    bool keep = c.info->required;
    for (std::uint32_t r = 0; !keep && r < rows_; ++r) {
      keep = std::memcmp(c.data.data() + std::size_t{r} * c.elemBytes, c.defaultBits.data(), c.elemBytes) != 0;
    }
    if (keep) out.push_back(&c);
  }
  std::vector<std::uint8_t> b(16 + out.size() * 16, 0);
  const bf::TableChunkHeader th{rows_, static_cast<std::uint32_t>(out.size()), 0, 0};
  std::memcpy(b.data(), &th, sizeof(th));
  for (std::size_t i = 0; i < out.size(); ++i) {
    b.resize(Align64(b.size()), 0);
    const bf::ColumnEntry ce{out[i]->info->columnId, static_cast<std::uint16_t>(out[i]->elemBytes), 0,
                             static_cast<std::uint64_t>(b.size())};
    std::memcpy(b.data() + 16 + i * 16, &ce, sizeof(ce));
    b.insert(b.end(), out[i]->data.begin(), out[i]->data.end());
  }
  return b;
}

void BankWriter::AddTable(const TableBuilder& t) {
  if (t.rows() == 0) return;
  std::uint16_t flags = bf::kChunkTable;
  for (const bf::ChunkInfo& c : bf::kChunks) {
    if (c.fourcc == t.fourcc()) flags = static_cast<std::uint16_t>(flags | c.requiredFlags);
  }
  chunks_[t.fourcc()] = Chunk{flags, t.Serialize()};
}

void BankWriter::AddBlob(std::uint32_t fourcc, std::vector<std::uint8_t> bytes, std::uint16_t extraFlags) {
  std::uint16_t flags = extraFlags;
  for (const bf::ChunkInfo& c : bf::kChunks) {
    if (c.fourcc == fourcc) flags = static_cast<std::uint16_t>(flags | c.requiredFlags);
  }
  chunks_[fourcc] = Chunk{flags, std::move(bytes)};
}

std::vector<std::uint8_t> BankWriter::Layout(const BankHeaderInfo& h, std::uint64_t contentHash) const {
  std::vector<std::pair<std::uint32_t, const Chunk*>> order;
  for (const auto& kv : chunks_) order.emplace_back(kv.first, &kv.second);
  std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
    const std::uint32_t oa = ChunkOrder(a.first);
    const std::uint32_t ob = ChunkOrder(b.first);
    return oa != ob ? oa < ob : a.first < b.first;
  });
  std::vector<std::uint8_t> f(128, 0);
  const std::uint64_t depOff = h.dependencies.empty() ? 0 : f.size();
  for (const bf::Dependency& d : h.dependencies) {
    const std::size_t at = f.size();
    f.resize(at + sizeof(d), 0);
    std::memcpy(f.data() + at, &d, sizeof(d));
  }
  const std::uint64_t tableOff = order.empty() ? 0 : f.size();
  f.resize(f.size() + order.size() * sizeof(bf::ChunkEntry), 0);
  std::vector<bf::ChunkEntry> entries;
  for (const auto& [fourcc, chunk] : order) {
    f.resize(Align64(f.size()), 0);
    const std::uint64_t at = f.size();
    f.insert(f.end(), chunk->bytes.begin(), chunk->bytes.end());
    entries.push_back(bf::ChunkEntry{fourcc, 1, chunk->flags, 0, 0, at, chunk->bytes.size()});
  }
  f.resize(Align64(f.size()), 0);
  if (!entries.empty()) std::memcpy(f.data() + tableOff, entries.data(), entries.size() * sizeof(bf::ChunkEntry));
  bf::FileHeader hd{};
  hd.magic = bf::kMagic;
  hd.formatMajor = bf::kFormatMajor;
  hd.formatMinor = bf::kFormatMinor;
  hd.platform = h.platform;
  hd.endianness = bf::kLittleEndian;
  hd.alignLog2 = bf::kAlignLog2;
  hd.headerBytes = sizeof(bf::FileHeader);
  hd.flags = h.flags;
  hd.bankId = h.bankId;
  hd.fileBytes = f.size();
  hd.projectBuildHash = h.projectBuildHash;
  hd.contentHash = contentHash;
  hd.dependencyCount = static_cast<std::uint32_t>(h.dependencies.size());
  hd.chunkCount = static_cast<std::uint32_t>(entries.size());
  hd.dependencyOffset = depOff;
  hd.chunkTableOffset = tableOff;
  hd.languageId = h.languageId;
  hd.checksum = bf::Crc32c(f.data() + 128, f.size() - 128);
  std::memcpy(f.data(), &hd, sizeof(hd));
  hd.headerChecksum = bf::Crc32c(f.data(), 52);
  std::memcpy(f.data(), &hd, sizeof(hd));
  return f;
}

std::uint64_t BankWriter::ContentHash(const BankHeaderInfo& h) const {
  const std::vector<std::uint8_t> f = Layout(h, 0);
  detail::Fnv64 fnv;
  fnv.U64(h.platform).U64(h.flags).U64(h.bankId).U64(h.languageId);
  fnv.Bytes(f.data() + 128, f.size() - 128);
  return fnv.value();
}

std::vector<std::uint8_t> BankWriter::Finish(const BankHeaderInfo& h) const { return Layout(h, ContentHash(h)); }

}  // namespace snd::buildtool
