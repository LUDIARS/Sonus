// Structure-aware fuzz of ParseBank (independent tester).
//
// Unlike fuzz_test.cpp (random byte flips, mostly stopped by the checksums),
// every mutation here hits one structural field the format defines (header
// counts / offsets, chunk directory, table header, column directory, row
// references, ranges, blob offsets, string offsets, PHYS / DSPN layouts) with a
// boundary value, and the file is re-signed (checksum + headerChecksum) so the
// structural checks are what decides. Every accepted bank is
//   1. re-checked against the spec's reference rules (specwalk::CheckInvariants:
//      a bad row index that still lands inside the file is invisible to ASan),
//   2. walked through every public accessor (specwalk::DeepWalk, ASan/UBSan).
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/bank/validate.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"
#include "spec_walk.hpp"
#include "test_support.hpp"

namespace bf = snd::bank_format;
namespace ch = bf::chunk;
namespace c = bf::col;
using fixture::Bank;
using fixture::Image;
using namespace snd::bank;

namespace {

struct Field {
  std::uint64_t pos;
  std::uint8_t size;  // 1, 2, 4 or 8
  std::uint32_t chunk;
  int kind;  // 0 header, 1 chunk entry, 2 table header, 3 column entry, 4 cell, 5 blob field
};

struct Layout {
  bf::FileHeader h{};
  std::vector<bf::ChunkEntry> entries;
  std::vector<Field> fields;
  std::vector<std::uint64_t> interesting;  // context values (sizes, row counts, offsets)
  std::vector<std::uint64_t> rowCounts;    // row counts of all tables
};

void AddField(Layout& L, std::uint64_t pos, std::uint8_t size, std::uint32_t chunk, int kind) {
  L.fields.push_back(Field{pos, size, chunk, kind});
}

// Discovers the structure of a valid bank (from bank-format.md, not from the validator).
Layout Discover(const std::vector<std::uint8_t>& f) {
  Layout L;
  std::memcpy(&L.h, f.data(), sizeof(L.h));
  const bf::FileHeader& h = L.h;
  // Header fields (all of them, including the ones outside headerChecksum).
  const std::uint8_t sizes[][2] = {{4, 4},  {6, 2},  {8, 4},  {12, 1}, {13, 1}, {14, 2}, {16, 4},
                                   {20, 4}, {24, 8}, {56, 4}, {60, 4}, {64, 8}, {72, 8}, {80, 4},
                                   {84, 4}, {88, 8}, {120, 8}};
  for (const auto& s : sizes) AddField(L, s[0], s[1], 0, 0);
  L.interesting = {f.size(), h.chunkTableOffset, h.dependencyOffset, h.chunkCount, h.dependencyCount};
  for (std::uint32_t d = 0; d < h.dependencyCount; ++d) {
    AddField(L, h.dependencyOffset + d * 16ull, 4, 0, 0);
    AddField(L, h.dependencyOffset + d * 16ull + 8, 8, 0, 0);
  }
  for (std::uint32_t i = 0; i < h.chunkCount; ++i) {
    const std::uint64_t at = h.chunkTableOffset + i * 32ull;
    bf::ChunkEntry e;
    std::memcpy(&e, f.data() + at, sizeof(e));
    L.entries.push_back(e);
    AddField(L, at + 0, 4, e.fourcc, 1);
    AddField(L, at + 4, 2, e.fourcc, 1);
    AddField(L, at + 6, 2, e.fourcc, 1);
    AddField(L, at + 16, 8, e.fourcc, 1);
    AddField(L, at + 24, 8, e.fourcc, 1);
    L.interesting.push_back(e.offset);
    L.interesting.push_back(e.bytes);
    L.interesting.push_back(e.offset + e.bytes);
    if ((e.flags & bf::kChunkTable) != 0) {
      bf::TableChunkHeader th;
      std::memcpy(&th, f.data() + e.offset, sizeof(th));
      AddField(L, e.offset + 0, 4, e.fourcc, 2);
      AddField(L, e.offset + 4, 4, e.fourcc, 2);
      L.interesting.push_back(th.rowCount);
      L.rowCounts.push_back(th.rowCount);
      for (std::uint32_t k = 0; k < th.columnCount; ++k) {
        const std::uint64_t ce = e.offset + 16 + k * 16ull;
        bf::ColumnEntry col;
        std::memcpy(&col, f.data() + ce, sizeof(col));
        AddField(L, ce + 0, 2, e.fourcc, 3);
        AddField(L, ce + 2, 2, e.fourcc, 3);
        AddField(L, ce + 8, 8, e.fourcc, 3);
        L.interesting.push_back(col.offset);
        // Every cell of every column (row references, ranges, offsets, ...).
        for (std::uint32_t r = 0; r < th.rowCount; ++r) {
          AddField(L, e.offset + col.offset + std::uint64_t{r} * col.elemBytes,
                   static_cast<std::uint8_t>(col.elemBytes == 8 ? 8 : 4), e.fourcc, 4);
        }
      }
    } else if (e.fourcc == ch::PHYS) {
      bf::PhysChunkHeader ph;
      std::memcpy(&ph, f.data() + e.offset, sizeof(ph));
      AddField(L, e.offset + 0, 4, e.fourcc, 5);
      AddField(L, e.offset + 8, 8, e.fourcc, 5);
      for (std::uint32_t t = 0; t < ph.tableCount; ++t) {
        const std::uint64_t base = e.offset + ph.tablesOffset + t * 64ull;
        AddField(L, base + 4, 4, e.fourcc, 5);
        for (std::uint64_t o : {8, 16, 24, 48}) AddField(L, base + o, 8, e.fourcc, 5);
      }
    } else if (e.fourcc == ch::DSPN) {
      AddField(L, e.offset, 8, e.fourcc, 5);
      AddField(L, e.offset + 8, 4, e.fourcc, 5);
    } else if (e.fourcc == ch::SPTH || e.fourcc == ch::STRS) {
      // NUL terminators: the last byte and a middle byte.
      if (e.bytes > 0) {
        AddField(L, e.offset + e.bytes - 1, 1, e.fourcc, 5);
        AddField(L, e.offset + e.bytes / 2, 1, e.fourcc, 5);
      }
    }
  }
  return L;
}

std::uint64_t Boundary(testsupport::Rng& rng, const Layout& L, std::uint64_t old, std::uint8_t size) {
  static const std::uint64_t fixed[] = {0,
                                        1,
                                        2,
                                        3,
                                        15,
                                        16,
                                        63,
                                        64,
                                        65,
                                        127,
                                        128,
                                        129,
                                        0x7FFFu,
                                        0xFFFFu,
                                        0x10000u,
                                        0x7FFFFFFFu,
                                        0x80000000u,
                                        0xFFFFFFFEu,
                                        0xFFFFFFFFu,
                                        0x100000000ull,
                                        0x7FFFFFFFFFFFFFFFull,
                                        0x8000000000000000ull,
                                        0xFFFFFFFFFFFFFFC0ull,
                                        0xFFFFFFFFFFFFFFFEull,
                                        0xFFFFFFFFFFFFFFFFull};
  std::uint64_t v;
  switch (rng.Below(5)) {
    case 0: v = fixed[rng.Below(sizeof(fixed) / sizeof(fixed[0]))]; break;
    case 1: v = old + 1; break;
    case 2: v = old - 1; break;
    case 3: {
      const std::uint64_t base = L.interesting[rng.Below(static_cast<std::uint32_t>(L.interesting.size()))];
      const std::int64_t delta = static_cast<std::int64_t>(rng.Below(5)) - 2;  // -2..+2
      v = base + static_cast<std::uint64_t>(delta * (rng.Below(2) == 0 ? 1 : 64));
      break;
    }
    default:
      // "overflows to something small": 2^k - small
      v = (std::uint64_t{1} << (size * 8 - 1 - rng.Below(3))) - rng.Below(4);
      break;
  }
  if (size < 8) v &= (std::uint64_t{1} << (size * 8)) - 1;
  return v;
}

void Put(std::vector<std::uint8_t>& w, std::uint64_t pos, std::uint8_t size, std::uint64_t v) {
  if (pos + size > w.size()) return;
  std::memcpy(w.data() + pos, &v, size);  // little endian host
}

std::uint64_t Get(const std::vector<std::uint8_t>& w, std::uint64_t pos, std::uint8_t size) {
  std::uint64_t v = 0;
  if (pos + size <= w.size()) std::memcpy(&v, w.data() + pos, size);
  return v;
}

struct Outcome {
  std::uint32_t accepted = 0;
  std::uint32_t rejected = 0;
  std::uint32_t violations = 0;
  std::string firstViolation;
};

// Parses `work`; on success re-checks the invariants and walks everything.
void Judge(const std::vector<std::uint8_t>& work, Outcome* out, const char* context) {
  Image img(work);
  BankView v;
  ParseError err;
  const snd_result r = ParseBank(img.data(), img.size(), ParseOptions{}, &v, &err);
  if (r != SND_OK) {
    ++out->rejected;
    if (err.message[0] == '\0') {
      ++out->violations;
      if (out->firstViolation.empty()) out->firstViolation = std::string(context) + ": rejected without a message";
    }
    if (v.valid()) {
      ++out->violations;
      if (out->firstViolation.empty()) out->firstViolation = std::string(context) + ": rejected but view valid";
    }
    return;
  }
  ++out->accepted;
  const std::string bad = specwalk::CheckInvariants(v);
  if (!bad.empty()) {
    ++out->violations;
    if (out->firstViolation.empty()) out->firstViolation = std::string(context) + ": " + bad;
    return;  // walking would read out of bounds by construction
  }
  (void)specwalk::DeepWalk(v);
}

std::vector<std::vector<std::uint8_t>> Bases() {
  std::vector<std::vector<std::uint8_t>> out;
  out.push_back(fixture::RichBank().Build());
  // A bank with dependencies and a language: covers the dependency table.
  Bank b = fixture::RichBank();
  b.deps.push_back(bf::Dependency{snd::HashId("Other"), 0, 0x1234});
  b.deps.push_back(bf::Dependency{snd::HashId("Third"), 0, 0x5678});
  b.flags |= bf::kFileLanguageBank;
  b.languageId = snd::HashId("ja");
  out.push_back(b.Build());
  return out;
}

}  // namespace

// Deterministic sweep: every structural field x every fixed boundary value, one at a time.
SND_TEST(BankStructFuzz, EveryFieldEveryBoundaryValue) {
  static const std::uint64_t values[] = {0,
                                         1,
                                         2,
                                         63,
                                         64,
                                         65,
                                         127,
                                         128,
                                         0x7FFFFFFFu,
                                         0x80000000u,
                                         0xFFFFFFFEu,
                                         0xFFFFFFFFu,
                                         0x100000000ull,
                                         0x7FFFFFFFFFFFFFFFull,
                                         0xFFFFFFFFFFFFFFC0ull,
                                         0xFFFFFFFFFFFFFFFFull};
  Outcome o;
  for (const std::vector<std::uint8_t>& base : Bases()) {
    const Layout L = Discover(base);
    for (const Field& fd : L.fields) {
      const std::uint64_t old = Get(base, fd.pos, fd.size);
      std::set<std::uint64_t> vals(std::begin(values), std::end(values));
      vals.insert(old + 1);
      vals.insert(old - 1);
      for (std::uint64_t ctx : fd.kind == 4 ? L.rowCounts : L.interesting) {
        vals.insert(ctx);
        if (fd.kind == 4) {
          vals.insert(ctx + 1);
          vals.insert(ctx - 1);
        }
      }
      for (std::uint64_t val : vals) {
        if (fd.size < 8 && val > ((std::uint64_t{1} << (fd.size * 8)) - 1)) continue;
        std::vector<std::uint8_t> w = base;
        Put(w, fd.pos, fd.size, val);
        Bank::Rehash(w);
        char ctx[96];
        std::snprintf(ctx, sizeof ctx, "field @%llu (%u B, kind %d) = %llx", static_cast<unsigned long long>(fd.pos),
                      fd.size, fd.kind, static_cast<unsigned long long>(val));
        Judge(w, &o, ctx);
      }
    }
  }
  snd::test::Note("sweep: %u accepted, %u rejected, %u violations", o.accepted, o.rejected, o.violations);
  SND_CHECK_MSG(o.violations == 0, "%u violations, first: %s", o.violations, o.firstViolation.c_str());
  SND_CHECK(o.accepted > 0 && o.rejected > 0);
}

// Random combinations of 1..4 structural mutations (overflowing first+count
// pairs arise from two cells changed together), re-signed, fixed seeds.
SND_TEST(BankStructFuzz, RandomStructuralCombinations) {
  Outcome o;
  std::uint32_t seedIndex = 0;
  for (const std::vector<std::uint8_t>& base : Bases()) {
    const Layout L = Discover(base);
    testsupport::Rng rng(0xB0A7F022ull + seedIndex++);
    for (std::uint32_t it = 0; it < 60000; ++it) {
      std::vector<std::uint8_t> w = base;
      const std::uint32_t edits = 1 + rng.Below(4);
      for (std::uint32_t e = 0; e < edits; ++e) {
        const Field& fd = L.fields[rng.Below(static_cast<std::uint32_t>(L.fields.size()))];
        Put(w, fd.pos, fd.size, Boundary(rng, L, Get(w, fd.pos, fd.size), fd.size));
        if (rng.Below(4) == 0) {
          // Pair edit: the next field too (first/count, offset/bytes neighbours).
          const Field& nx = L.fields[(&fd - L.fields.data() + 1) % L.fields.size()];
          Put(w, nx.pos, nx.size, Boundary(rng, L, Get(w, nx.pos, nx.size), nx.size));
        }
      }
      // Sometimes grow / shrink the file and keep fileBytes consistent.
      if (rng.Below(32) == 0) {
        const std::uint32_t k = rng.Below(3);
        if (k == 0 && w.size() > 129) w.resize(w.size() - 1 - rng.Below(64));
        if (k == 1) w.resize(w.size() + 1 + rng.Below(128), 0);
        if (k == 2) w.resize(w.size() + 64, 0);
        std::uint64_t fb = w.size();
        std::memcpy(w.data() + 24, &fb, 8);
      }
      Bank::Rehash(w);
      char ctx[64];
      std::snprintf(ctx, sizeof ctx, "seed %u iteration %u", seedIndex, it);
      Judge(w, &o, ctx);
    }
  }
  snd::test::Note("random: %u accepted, %u rejected, %u violations", o.accepted, o.rejected, o.violations);
  SND_CHECK_MSG(o.violations == 0, "%u violations, first: %s", o.violations, o.firstViolation.c_str());
  SND_CHECK(o.accepted > 0 && o.rejected > 0);
}

// Every length 0..200 of a valid bank's prefix (with fileBytes fixed up and
// re-signed when the header is complete): never crashes, never accepted
// unless the cut happens to keep a self-consistent file.
SND_TEST(BankStructFuzz, AllShortPrefixes) {
  const std::vector<std::uint8_t> full = fixture::RichBank().Build();
  Outcome o;
  for (std::size_t len = 0; len <= 200 && len <= full.size(); ++len) {
    std::vector<std::uint8_t> w(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
    Judge(w, &o, "prefix raw");
    if (len >= 128) {
      std::uint64_t fb = len;
      std::memcpy(w.data() + 24, &fb, 8);
      Bank::Rehash(w);
      Judge(w, &o, "prefix re-signed");
    }
  }
  // Also all-zero / all-0xFF buffers of every length below the header size.
  for (std::size_t len = 0; len < 128; ++len) {
    Judge(std::vector<std::uint8_t>(len, 0), &o, "zeros");
    Judge(std::vector<std::uint8_t>(len, 0xFF), &o, "ones");
  }
  SND_CHECK_EQ(o.accepted, 0u);
  SND_CHECK_MSG(o.violations == 0, "%s", o.firstViolation.c_str());
}

// Header only (no dependencies, no chunks): a legal, empty bank.
SND_TEST(BankStructFuzz, HeaderOnlyBank) {
  Bank b;
  const std::vector<std::uint8_t> bytes = b.Build();
  SND_CHECK_EQ(bytes.size(), 128u);
  Outcome o;
  Judge(bytes, &o, "header only");
  SND_CHECK_EQ(o.accepted, 1u);
  SND_CHECK_EQ(o.violations, 0u);
}

// A tiny table that claims a huge row count: must be rejected (column arrays
// cannot hold it) and never read past the end.
SND_TEST(BankStructFuzz, HugeRowCountInSmallFile) {
  for (std::uint32_t rows : {0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x40000000u, 1000000u}) {
    Bank b;
    b.Table(ch::BUSS, 1, {fixture::U32(c::BUSS_id, {1}), fixture::U32(c::BUSS_parent, {bf::kNone})});
    std::vector<std::uint8_t> w = b.Build();
    const Layout L = Discover(w);
    Put(w, L.entries[0].offset, 4, rows);
    Bank::Rehash(w);
    Outcome o;
    Judge(w, &o, "huge rows");
    SND_CHECK_EQ(o.accepted, 0u);
    SND_CHECK_EQ(o.violations, 0u);
  }
}

// Zero-row tables of every kind with every column present are valid (and readable).
SND_TEST(BankStructFuzz, ZeroRowTables) {
  Bank b;
  for (const bf::ChunkInfo& ci : bf::kChunks) {
    if (ci.kind != bf::ChunkKind::Table) continue;
    std::vector<fixture::Column> cols;
    for (const bf::ColumnInfo& col : bf::kColumns) {
      if (col.chunk != ci.fourcc) continue;
      fixture::Column cc;
      cc.id = col.columnId;
      cc.elemBytes = static_cast<std::uint16_t>(bf::ColumnTypeBytes(col.type));
      cols.push_back(cc);
    }
    fixture::Chunk& t = b.Table(ci.fourcc, 0, cols);
    t.flags |= ci.requiredFlags;
  }
  b.Blob(ch::STRS, fixture::Bytes(""), bf::kChunkStrippable);
  Outcome o;
  Judge(b.Build(), &o, "zero rows");
  SND_CHECK_EQ(o.accepted, 1u);
  SND_CHECK_MSG(o.violations == 0, "%s", o.firstViolation.c_str());
}
