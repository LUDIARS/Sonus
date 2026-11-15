// Test-side bank writer, written from bank-format.md alone (independent of
// buildtool) so that the bank tests do not share a writer with the code that
// produces banks in production.
#ifndef SND_BANK_TEST_FIXTURE_WRITER_HPP
#define SND_BANK_TEST_FIXTURE_WRITER_HPP

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "snd/format/bank_format.hpp"

namespace fixture {

namespace bf = snd::bank_format;

struct Column {
  std::uint16_t id = 0;
  std::uint16_t elemBytes = 4;
  std::vector<std::uint8_t> data;
  std::int64_t offsetOverride = -1;  // corrupt on purpose
};

inline Column U32(std::uint16_t id, std::initializer_list<std::uint32_t> v) {
  Column c;
  c.id = id;
  c.elemBytes = 4;
  for (std::uint32_t x : v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
    c.data.insert(c.data.end(), p, p + 4);
  }
  return c;
}
inline Column F32(std::uint16_t id, std::initializer_list<float> v) {
  Column c;
  c.id = id;
  c.elemBytes = 4;
  for (float x : v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
    c.data.insert(c.data.end(), p, p + 4);
  }
  return c;
}
inline Column U64(std::uint16_t id, std::initializer_list<std::uint64_t> v) {
  Column c;
  c.id = id;
  c.elemBytes = 8;
  for (std::uint64_t x : v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
    c.data.insert(c.data.end(), p, p + 8);
  }
  return c;
}

struct Chunk {
  std::uint32_t fourcc = 0;
  std::uint16_t version = 1;
  std::uint16_t flags = 0;
  bool table = false;
  std::uint32_t rows = 0;
  std::vector<Column> columns;
  std::vector<std::uint8_t> blob;
  std::int64_t offsetOverride = -1;
  std::int64_t bytesOverride = -1;
};

struct Bank {
  std::uint32_t bankId = 0x1234u;
  std::uint32_t platform = bf::kPlatformAny;
  std::uint32_t flags = 0;
  std::uint32_t languageId = 0;
  std::uint16_t formatMinor = bf::kFormatMinor;
  std::uint64_t contentHash = 0xC0FFEEull;
  std::vector<bf::Dependency> deps;
  std::vector<Chunk> chunks;

  Chunk& Table(std::uint32_t fourcc, std::uint32_t rows, std::vector<Column> cols) {
    Chunk c;
    c.fourcc = fourcc;
    c.table = true;
    c.flags = bf::kChunkTable;
    c.rows = rows;
    c.columns = std::move(cols);
    chunks.push_back(std::move(c));
    return chunks.back();
  }
  Chunk& Blob(std::uint32_t fourcc, std::vector<std::uint8_t> data, std::uint16_t chunkFlags = 0) {
    Chunk c;
    c.fourcc = fourcc;
    c.flags = chunkFlags;
    c.blob = std::move(data);
    chunks.push_back(std::move(c));
    return chunks.back();
  }
  Chunk* Find(std::uint32_t fourcc) {
    for (Chunk& c : chunks) {
      if (c.fourcc == fourcc) return &c;
    }
    return nullptr;
  }
  Column* FindColumn(std::uint32_t fourcc, std::uint16_t id) {
    Chunk* c = Find(fourcc);
    if (c == nullptr) return nullptr;
    for (Column& col : c->columns) {
      if (col.id == id) return &col;
    }
    return nullptr;
  }
  void RemoveColumn(std::uint32_t fourcc, std::uint16_t id) {
    Chunk* c = Find(fourcc);
    for (std::size_t i = 0; c != nullptr && i < c->columns.size(); ++i) {
      if (c->columns[i].id == id) {
        c->columns.erase(c->columns.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }
  void SetU32(std::uint32_t fourcc, std::uint16_t id, std::uint32_t row, std::uint32_t v) {
    Column* c = FindColumn(fourcc, id);
    std::memcpy(c->data.data() + row * 4u, &v, 4);
  }
  void SetU64(std::uint32_t fourcc, std::uint16_t id, std::uint32_t row, std::uint64_t v) {
    Column* c = FindColumn(fourcc, id);
    std::memcpy(c->data.data() + row * 8u, &v, 8);
  }

  static std::uint64_t Align(std::uint64_t v) { return (v + 63) & ~std::uint64_t{63}; }

  std::vector<std::uint8_t> Build() const {
    std::vector<std::uint8_t> out(128, 0);
    const std::uint64_t depOff = deps.empty() ? 0 : 128;
    for (const bf::Dependency& d : deps) {
      const auto* p = reinterpret_cast<const std::uint8_t*>(&d);
      out.insert(out.end(), p, p + sizeof(d));
    }
    const std::uint64_t tableOff = out.size();
    out.resize(out.size() + chunks.size() * sizeof(bf::ChunkEntry), 0);
    std::vector<bf::ChunkEntry> entries;
    for (const Chunk& c : chunks) {
      out.resize(Align(out.size()), 0);
      const std::uint64_t start = out.size();
      if (c.table) {
        std::vector<std::uint8_t> body(16 + c.columns.size() * 16, 0);
        bf::TableChunkHeader th{c.rows, static_cast<std::uint32_t>(c.columns.size()), 0, 0};
        std::memcpy(body.data(), &th, sizeof(th));
        for (std::size_t i = 0; i < c.columns.size(); ++i) {
          body.resize(Align(body.size()), 0);
          bf::ColumnEntry ce{c.columns[i].id, c.columns[i].elemBytes, 0,
                             c.columns[i].offsetOverride >= 0
                                 ? static_cast<std::uint64_t>(c.columns[i].offsetOverride)
                                 : static_cast<std::uint64_t>(body.size())};
          std::memcpy(body.data() + 16 + i * 16, &ce, sizeof(ce));
          body.insert(body.end(), c.columns[i].data.begin(), c.columns[i].data.end());
        }
        out.insert(out.end(), body.begin(), body.end());
      } else {
        out.insert(out.end(), c.blob.begin(), c.blob.end());
      }
      bf::ChunkEntry e{c.fourcc, c.version, c.flags, 0, 0,
                       c.offsetOverride >= 0 ? static_cast<std::uint64_t>(c.offsetOverride) : start,
                       c.bytesOverride >= 0 ? static_cast<std::uint64_t>(c.bytesOverride) : out.size() - start};
      entries.push_back(e);
    }
    out.resize(Align(out.size()), 0);
    if (!entries.empty()) std::memcpy(out.data() + tableOff, entries.data(), entries.size() * sizeof(bf::ChunkEntry));
    bf::FileHeader h{};
    h.magic = bf::kMagic;
    h.formatMajor = bf::kFormatMajor;
    h.formatMinor = formatMinor;
    h.platform = platform;
    h.endianness = bf::kLittleEndian;
    h.alignLog2 = bf::kAlignLog2;
    h.headerBytes = 128;
    h.flags = flags;
    h.bankId = bankId;
    h.fileBytes = out.size();
    h.contentHash = contentHash;
    h.dependencyCount = static_cast<std::uint32_t>(deps.size());
    h.chunkCount = static_cast<std::uint32_t>(chunks.size());
    h.dependencyOffset = depOff;
    h.chunkTableOffset = tableOff;
    h.languageId = languageId;
    std::memcpy(out.data(), &h, sizeof(h));
    Rehash(out);
    return out;
  }

  // Recomputes checksum and headerChecksum (after a deliberate mutation).
  static void Rehash(std::vector<std::uint8_t>& f) {
    if (f.size() < 128) return;
    bf::FileHeader h;
    std::memcpy(&h, f.data(), sizeof(h));
    h.checksum = bf::Crc32c(f.data() + 128, f.size() - 128);
    std::memcpy(f.data(), &h, sizeof(h));
    h.headerChecksum = bf::Crc32c(f.data(), 52);
    std::memcpy(f.data(), &h, sizeof(h));
  }
};

// Exact-size, 64-aligned copy (ASan sees reads past the end).
class Image {
 public:
  explicit Image(const std::vector<std::uint8_t>& bytes) : size_(bytes.size()) {
    void* p = nullptr;
    if (posix_memalign(&p, 64, size_ == 0 ? 1 : size_) != 0) std::abort();
    data_ = static_cast<std::uint8_t*>(p);
    if (size_ > 0) std::memcpy(data_, bytes.data(), size_);
  }
  ~Image() { std::free(data_); }
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  const std::uint8_t* data() const { return data_; }
  std::uint64_t size() const { return size_; }

 private:
  std::uint8_t* data_ = nullptr;
  std::uint64_t size_ = 0;
};

inline std::vector<std::uint8_t> Bytes(const std::string& s, bool nul = true) {
  std::vector<std::uint8_t> v(s.begin(), s.end());
  if (nul) v.push_back(0);
  return v;
}

// A bank that uses every chunk kind and every reference rule (valid).
Bank RichBank();

}  // namespace fixture

#endif  // SND_BANK_TEST_FIXTURE_WRITER_HPP
