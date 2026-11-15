// Fuzz (design.md 14): valid banks with random mutations must never crash,
// read out of bounds (ASan/UBSan builds) or pass with dangling references.
// Fixed seeds; every accepted bank is walked through all accessors.
#include <cstdlib>
#include <cstring>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/bank/validate.hpp"
#include "snd/test/test.hpp"
#include "test_support.hpp"

namespace bf = snd::bank_format;
using namespace snd::bank;

namespace {

// Reads every column cell, every typed helper and every referenced blob byte.
std::uint64_t Walk(const BankView& v) {
  std::uint64_t sum = 0;
  for (const bf::ColumnInfo& c : bf::kColumns) {
    const TableView* t = v.Table(c.chunk);
    if (t == nullptr || t->columns[c.columnId] == nullptr) continue;
    const auto* p = static_cast<const std::uint8_t*>(t->columns[c.columnId]);
    const std::uint32_t eb = bf::ColumnTypeBytes(c.type);
    for (std::uint32_t r = 0; r < t->rows; ++r) sum += p[std::uint64_t{r} * eb + eb - 1];
  }
  for (std::uint32_t a = 0; a < v.Rows(bf::chunk::ASST); ++a) {
    for (const ByteSpan s : {v.WaveData(a), v.AssetExtra(a), v.Preload(a)}) {
      if (s.bytes > 0) sum += s.data[0] + s.data[s.bytes - 1];
    }
    const snd::RangeU32 seek = v.SeekRange(a);
    for (std::uint32_t i = 0; i < seek.count; ++i) sum += v.Column<col::SEEK_frame>()[seek.first + i];
  }
  for (std::uint32_t s = 0; s < v.Rows(bf::chunk::STRM); ++s) {
    const char* p = v.StreamPath(s);
    if (p != nullptr) sum += std::strlen(p);
  }
  for (std::uint32_t r = 0; r < v.Rows(bf::chunk::NAME); ++r) {
    const char* n = v.String(v.Column<col::NAME_stringOffset>()[r]);
    if (n != nullptr) sum += std::strlen(n);
    const char* m = v.Name(v.Column<col::NAME_id>()[r]);
    if (m != nullptr) sum += static_cast<unsigned char>(m[0]);
  }
  for (std::uint32_t r = 0; r < v.Rows(bf::chunk::EFFX); ++r) {
    const std::uint64_t off = v.Column<col::EFFX_blobOffset>()[r];
    const std::uint64_t n = v.Column<col::EFFX_blobBytes>()[r];
    if (n > 0) sum += v.Chunk(bf::chunk::EFBL).data[off + n - 1];
  }
  // Every row reference indexes its target (validated): follow a few chains.
  for (std::uint32_t e = 0; e < v.Rows(bf::chunk::EVNT); ++e) {
    sum += v.Column<col::BUSS_id>()[v.Column<col::EVNT_bus>()[e]];
    const std::uint32_t tc = v.Column<col::EVNT_trackCount>()[e];
    for (std::uint32_t t = 0; t < tc; ++t) {
      const std::uint32_t tr = v.Column<col::EVNT_trackFirst>()[e] + t;
      const std::uint32_t cc = v.Column<col::TRAK_clipCount>()[tr];
      for (std::uint32_t k = 0; k < cc; ++k) sum += v.Column<col::CLIP_target>()[v.Column<col::TRAK_clipFirst>()[tr] + k];
    }
  }
  sum += v.DspNeed().instanceBytes;
  for (std::uint32_t i = 0; i < v.PhysTableCount(); ++i) {
    PhysTableView pt;
    if (!v.PhysTable(i, &pt)) continue;
    const std::uint64_t n = pt.materialCount;
    if (n > 0) {
      sum += pt.single[n * 4 - 1] + pt.pair[n * n - 1] + pt.footstep[n - 1];
      sum += static_cast<std::uint64_t>(pt.curves[n - 1].massToPitchCents[1]);
    }
  }
  for (std::uint32_t d = 0; d < v.dependencyCount(); ++d) sum += v.dependency(d).bankId;
  const ByteSpan app = v.ApplicationChunk(snd::Fourcc('X', 'T', 'R', 'A'));
  if (app.bytes > 0) sum += app.data[app.bytes - 1];
  return sum;
}

struct Stats {
  std::uint32_t accepted = 0;
  std::uint32_t rejected = 0;
};

void FuzzRound(const std::vector<std::uint8_t>& base, std::uint64_t seed, std::uint32_t iterations,
               bool rehash, bool structural, Stats* stats) {
  testsupport::Rng rng(seed);
  bf::FileHeader h;
  std::memcpy(&h, base.data(), sizeof(h));
  std::vector<std::uint8_t> work;
  BankView* v = new BankView();
  for (std::uint32_t it = 0; it < iterations; ++it) {
    work = base;
    const std::uint32_t edits = 1 + rng.Below(6);
    for (std::uint32_t e = 0; e < edits; ++e) {
      std::uint64_t pos;
      if (structural && rng.Below(2) == 0) {
        // Aim at header fields, the chunk table and table directories (first bytes of chunks).
        const std::uint32_t zone = rng.Below(3);
        if (zone == 0) {
          pos = 56 + rng.Below(72);
        } else if (zone == 1) {
          pos = h.chunkTableOffset + rng.Below(h.chunkCount * 32);
        } else {
          bf::ChunkEntry ce;
          std::memcpy(&ce, base.data() + h.chunkTableOffset + rng.Below(h.chunkCount) * 32ull, sizeof(ce));
          pos = ce.offset + rng.Below(static_cast<std::uint32_t>(ce.bytes < 256 ? ce.bytes + 1 : 256));
        }
      } else {
        pos = rng.Below(static_cast<std::uint32_t>(work.size()));
      }
      if (pos >= work.size()) continue;
      switch (rng.Below(4)) {
        case 0: work[pos] ^= static_cast<std::uint8_t>(1u << rng.Below(8)); break;
        case 1: work[pos] = static_cast<std::uint8_t>(rng.Next()); break;
        case 2: work[pos] = 0xFF; break;
        default: work[pos] = 0; break;
      }
    }
    if (rng.Below(16) == 0) work.resize(rng.Below(static_cast<std::uint32_t>(work.size()) + 1));  // truncate
    if (rehash) {
      if (work.size() >= 128 && rng.Below(8) != 0) {
        bf::FileHeader wh;
        std::memcpy(&wh, work.data(), sizeof(wh));
        wh.fileBytes = work.size();  // keep the size check satisfied most of the time
        std::memcpy(work.data(), &wh, sizeof(wh));
      }
      fixture::Bank::Rehash(work);
    }
    fixture::Image img(work);
    ParseError err;
    ParseOptions opt;
    const snd_result r = ParseBank(img.data(), img.size(), opt, v, &err);
    if (r == SND_OK) {
      ++stats->accepted;
      (void)Walk(*v);
    } else {
      ++stats->rejected;
      SND_CHECK(err.message[0] != '\0');
      SND_CHECK(!v->valid());
    }
  }
  delete v;
}

}  // namespace

SND_TEST(BankFuzz, RandomMutationsNeverCrash) {
  const std::vector<std::uint8_t> base = fixture::RichBank().Build();
  {
    fixture::Image img(base);
    BankView v;
    SND_REQUIRE_EQ(ParseBank(img.data(), img.size(), ParseOptions{}, &v, nullptr), SND_OK);
    SND_CHECK(Walk(v) > 0);
  }
  Stats raw, signed_, targeted;
  FuzzRound(base, 0x5EED0001ull, 5000, false, false, &raw);           // checksums break: header/CRC paths
  FuzzRound(base, 0x5EED0002ull, 25000, true, false, &signed_);       // re-signed: structure checks
  FuzzRound(base, 0x5EED0003ull, 25000, true, true, &targeted);       // re-signed, aimed at directories
  snd::test::Note("raw %u/%u, re-signed %u/%u, targeted %u/%u accepted", raw.accepted,
                  raw.accepted + raw.rejected, signed_.accepted, signed_.accepted + signed_.rejected,
                  targeted.accepted, targeted.accepted + targeted.rejected);
  // Both outcomes must be exercised (the mutations hit data and structure).
  SND_CHECK(signed_.accepted > 0 && signed_.rejected > 0);
  SND_CHECK(targeted.accepted > 0 && targeted.rejected > 0);
}
