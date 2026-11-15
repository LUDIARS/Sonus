// Independent tests of the shared validator snd/format/bank_check.hpp (D-53).
//  - Every truncation of a valid bank is rejected, reading only [data, data+len)
//    (exact-size allocations: ASan catches any over-read).
//  - Every single-byte flip is rejected (headerChecksum covers [0,128), checksum the rest).
//  - Re-signed structural lies (huge counts, offsets past the end, overlapping
//    chunks, huge row / column counts, a column past its chunk) are CORRUPT.
//  - The CRC function passed in is the one used for both checksums.
//  - bank's ParseBank and CheckBank give the same result for every case.
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/bank/validate.hpp"
#include "snd/base/crc32c.hpp"
#include "snd/format/bank_check.hpp"
#include "snd/test/test.hpp"

namespace bf = snd::bank_format;
namespace ck = snd::bank_format::check;

namespace {

// Exact-size, 64-byte aligned copy.
struct Exact {
  std::uint8_t* p = nullptr;
  explicit Exact(const std::uint8_t* src, std::size_t n) {
    void* m = nullptr;
    if (posix_memalign(&m, 64, n == 0 ? 1 : n) == 0) p = static_cast<std::uint8_t*>(m);
    if (p != nullptr && n != 0) std::memcpy(p, src, n);
  }
  ~Exact() { std::free(p); }
  Exact(const Exact&) = delete;
  Exact& operator=(const Exact&) = delete;
};

std::uint32_t Rd32(const std::vector<std::uint8_t>& b, std::size_t at) {
  std::uint32_t v;
  std::memcpy(&v, b.data() + at, 4);
  return v;
}
std::uint64_t Rd64(const std::vector<std::uint8_t>& b, std::size_t at) {
  std::uint64_t v;
  std::memcpy(&v, b.data() + at, 8);
  return v;
}
void Wr32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
void Wr64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) { std::memcpy(b.data() + at, &v, 8); }

// Recomputes both checksums (as a writer would) after a structural edit.
void Resign(std::vector<std::uint8_t>& b) {
  Wr32(b, 48, bf::Crc32c(b.data() + 128, b.size() - 128));
  Wr32(b, 52, bf::HeaderChecksum(b.data()));
}

snd_result Check(const std::vector<std::uint8_t>& b, ck::CheckError* err = nullptr,
                 ck::Crc32cFn crc = &snd::base::Crc32cUpdate) {
  Exact e(b.data(), b.size());
  ck::CheckedBank out;
  ck::CheckError local;
  return ck::CheckBank(e.p, b.size(), ck::CheckOptions{}, &out, err != nullptr ? err : &local, crc);
}

snd_result Parse(const std::vector<std::uint8_t>& b) {
  Exact e(b.data(), b.size());
  snd::bank::BankView view;
  snd::bank::ParseError err;
  snd::bank::ParseOptions o;
  return snd::bank::ParseBank(e.p, b.size(), o, &view, &err);
}

std::uint32_t BogusCrc(std::uint32_t, const std::uint8_t*, std::size_t) noexcept { return 0x12345678u; }

// Position of the ChunkEntry of `fourcc` (or SIZE_MAX).
std::size_t EntryOf(const std::vector<std::uint8_t>& b, std::uint32_t fourcc) {
  const std::uint32_t n = Rd32(b, 60);
  const std::uint64_t at = Rd64(b, 72);
  for (std::uint32_t i = 0; i < n; ++i) {
    if (Rd32(b, static_cast<std::size_t>(at + 32u * i)) == fourcc) return static_cast<std::size_t>(at + 32u * i);
  }
  return SIZE_MAX;
}

}  // namespace

SND_TEST(IndepBankCheck11, ValidBankPassesWithEitherCrcAndNotWithABogusOne) {
  const std::vector<std::uint8_t> b = fixture::RichBank().Build();
  SND_REQUIRE_EQ(Check(b), SND_OK);
  SND_CHECK_EQ(Check(b, nullptr, &ck::ReferenceCrc32c), SND_OK);
  SND_CHECK_EQ(Parse(b), SND_OK);
  ck::CheckError err;
  SND_CHECK_EQ(Check(b, &err, &BogusCrc), SND_ERR_CORRUPT);  // the given CRC is really used
  SND_CHECK(std::strstr(err.message, "checksum") != nullptr);
  // Unaligned memory and a null output are argument errors, not CORRUPT.
  std::vector<std::uint8_t> mem(b.size() + 64);
  std::uint8_t* p = mem.data();
  while (reinterpret_cast<std::uintptr_t>(p) % 64 != 1) ++p;
  std::memcpy(p, b.data(), b.size());
  ck::CheckedBank out;
  SND_CHECK_EQ(ck::CheckBank(p, b.size(), ck::CheckOptions{}, &out, &err), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ck::CheckBank(b.data(), b.size(), ck::CheckOptions{}, nullptr, &err), SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(IndepBankCheck11, EveryTruncationIsRejected) {
  const std::vector<std::uint8_t> b = fixture::RichBank().Build();
  std::size_t accepted = 0, disagree = 0;
  for (std::size_t len = 0; len < b.size(); ++len) {
    const std::vector<std::uint8_t> t(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(len));
    const snd_result r = Check(t);
    if (r == SND_OK) ++accepted;
    if (r != Parse(t)) ++disagree;
  }
  SND_CHECK_EQ(accepted, 0u);
  SND_CHECK_EQ(disagree, 0u);
  // ... and so is the bank with one trailing byte (fileBytes must equal the length).
  std::vector<std::uint8_t> longer = b;
  longer.push_back(0);
  SND_CHECK_EQ(Check(longer), SND_ERR_CORRUPT);
}

SND_TEST(IndepBankCheck11, EverySingleByteFlipIsRejected) {
  const std::vector<std::uint8_t> b = fixture::RichBank().Build();
  std::size_t accepted = 0, disagree = 0, firstAccepted = SIZE_MAX;
  for (std::size_t i = 0; i < b.size(); ++i) {
    std::vector<std::uint8_t> m = b;
    m[i] ^= static_cast<std::uint8_t>(1u << (i % 8));
    const snd_result r = Check(m);
    if (r == SND_OK && accepted++ == 0) firstAccepted = i;
    if (r != Parse(m)) ++disagree;
  }
  SND_CHECK_MSG(accepted == 0, "%zu flips accepted (first at byte %zu)", accepted, firstAccepted);
  SND_CHECK_EQ(disagree, 0u);
}

SND_TEST(IndepBankCheck11, ResignedStructuralLiesAreCorrupt) {
  const std::vector<std::uint8_t> base = fixture::RichBank().Build();
  const std::size_t evnt = EntryOf(base, bf::chunk::EVNT);
  const std::size_t buss = EntryOf(base, bf::chunk::BUSS);
  SND_REQUIRE(evnt != SIZE_MAX && buss != SIZE_MAX);
  const std::size_t evntBody = static_cast<std::size_t>(Rd64(base, evnt + 16));
  struct Case {
    const char* name;
    std::function<void(std::vector<std::uint8_t>&)> edit;
  };
  const Case cases[] = {
      {"chunkCount 0xFFFFFFFF", [](std::vector<std::uint8_t>& b) { Wr32(b, 60, 0xFFFFFFFFu); }},
      {"chunkCount + 1", [](std::vector<std::uint8_t>& b) { Wr32(b, 60, Rd32(b, 60) + 1u); }},
      {"dependencyCount huge", [](std::vector<std::uint8_t>& b) { Wr32(b, 56, 0x40000000u); }},
      {"dependencyOffset past the end", [](std::vector<std::uint8_t>& b) {
         Wr32(b, 56, 1u);
         Wr64(b, 64, b.size());
       }},
      {"chunkTableOffset huge", [](std::vector<std::uint8_t>& b) { Wr64(b, 72, 0xFFFFFFFFFFFFFFC0ull); }},
      {"chunk offset past the end", [evnt](std::vector<std::uint8_t>& b) { Wr64(b, evnt + 16, (b.size() + 63) / 64 * 64); }},
      {"chunk bytes wrap", [evnt](std::vector<std::uint8_t>& b) { Wr64(b, evnt + 24, 0xFFFFFFFFFFFFFFFFull); }},
      {"chunk offset not 64-aligned", [evnt](std::vector<std::uint8_t>& b) { Wr64(b, evnt + 16, Rd64(b, evnt + 16) + 8u); }},
      {"two chunks overlap", [evnt, buss](std::vector<std::uint8_t>& b) { Wr64(b, buss + 16, Rd64(b, evnt + 16)); }},
      {"rowCount huge", [evntBody](std::vector<std::uint8_t>& b) { Wr32(b, evntBody + 0, 0x7FFFFFFFu); }},
      {"columnCount huge", [evntBody](std::vector<std::uint8_t>& b) { Wr32(b, evntBody + 4, 0x10000u); }},
      {"column offset past the chunk", [evntBody](std::vector<std::uint8_t>& b) { Wr64(b, evntBody + 16 + 8, 1ull << 40); }},
      {"column elemBytes wrong", [evntBody](std::vector<std::uint8_t>& b) { b[evntBody + 16 + 2] = 3; }},
      {"bankId 0", [](std::vector<std::uint8_t>& b) { Wr32(b, 20, 0u); }},
      {"reserved header byte", [](std::vector<std::uint8_t>& b) { b[127] = 1; }},
      {"language id without the flag", [](std::vector<std::uint8_t>& b) { Wr32(b, 80, 7u); }},
  };
  for (const Case& c : cases) {
    std::vector<std::uint8_t> m = base;
    c.edit(m);
    Resign(m);
    ck::CheckError err;
    const snd_result r = Check(m, &err);
    SND_CHECK_MSG(r == SND_ERR_CORRUPT, "%s: result %d (%s)", c.name, r, err.message);
    SND_CHECK_MSG(Parse(m) == r, "%s: ParseBank %d, CheckBank %d", c.name, Parse(m), r);
  }
  // Version / platform / endianness: VERSION_MISMATCH (not CORRUPT), after re-signing.
  {
    std::vector<std::uint8_t> m = base;
    m[4] = 3;  // formatMajor 3
    Resign(m);
    SND_CHECK_EQ(Check(m), SND_ERR_VERSION_MISMATCH);
  }
  {
    std::vector<std::uint8_t> m = base;
    m[12] = 2;  // endianness
    Resign(m);
    SND_CHECK_EQ(Check(m), SND_ERR_VERSION_MISMATCH);
  }
  {
    std::vector<std::uint8_t> m = base;
    Wr32(m, 8, bf::kPlatformWindows);
    Resign(m);
    ck::CheckOptions linux;
    linux.platform = bf::kPlatformLinux;
    Exact e(m.data(), m.size());
    ck::CheckedBank out;
    ck::CheckError err;
    SND_CHECK_EQ(ck::CheckBank(e.p, m.size(), linux, &out, &err), SND_ERR_VERSION_MISMATCH);
    ck::CheckOptions win;
    win.platform = bf::kPlatformWindows;
    SND_CHECK_EQ(ck::CheckBank(e.p, m.size(), win, &out, &err), SND_OK);
  }
  {
    std::vector<std::uint8_t> m = base;
    m[6] = 9;  // a newer minor version is readable (§7)
    Resign(m);
    SND_CHECK_EQ(Check(m), SND_OK);
  }
}
