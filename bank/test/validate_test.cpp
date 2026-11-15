// ParseBank: the valid path, typed accessors and one test per §8 check.
#include <cstring>
#include <string>
#include <vector>

#include "corruption_cases.hpp"
#include "fixture_writer.hpp"
#include "snd/bank/validate.hpp"
#include "snd/id.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

namespace bf = snd::bank_format;
namespace c = bf::col;
namespace ch = bf::chunk;
using fixture::Bank;
using fixture::Image;
using snd::HashId;
using namespace snd::bank;

namespace {

snd_result Parse(const std::vector<std::uint8_t>& bytes, ParseError* err = nullptr,
                 std::uint32_t platform = bf::kPlatformLinux) {
  Image img(bytes);
  BankView v;
  ParseOptions o;
  o.platform = platform;
  ParseError local;
  const snd_result r = ParseBank(img.data(), img.size(), o, &v, err != nullptr ? err : &local);
  if (r != SND_OK && err == nullptr) snd::test::Note("parse failed: %s", local.message);
  return r;
}

snd_result ParseFixture(const Bank& b, ParseError* err = nullptr) {
  return Parse(b.Build(), err);
}

// Mutates a header field and re-signs the file (so the field check itself is reached).
template <class F>
std::vector<std::uint8_t> WithHeader(F&& f) {
  std::vector<std::uint8_t> bytes = fixture::RichBank().Build();
  bf::FileHeader h;
  std::memcpy(&h, bytes.data(), sizeof(h));
  f(h);
  std::memcpy(bytes.data(), &h, sizeof(h));
  Bank::Rehash(bytes);
  return bytes;
}

void ExpectFail(const Bank& b, snd_result code, const char* needle) {
  ParseError e;
  const snd_result r = ParseFixture(b, &e);
  SND_CHECK_EQ(r, code);
  SND_CHECK_MSG(std::strstr(e.message, needle) != nullptr, "message '%s' lacks '%s'", e.message, needle);
}
void ExpectFail(const std::vector<std::uint8_t>& bytes, snd_result code, const char* needle) {
  ParseError e;
  const snd_result r = Parse(bytes, &e);
  SND_CHECK_EQ(r, code);
  SND_CHECK_MSG(std::strstr(e.message, needle) != nullptr, "message '%s' lacks '%s'", e.message, needle);
}

}  // namespace

// ---------------------------------------------------------------------------
// Valid banks and accessors
// ---------------------------------------------------------------------------
SND_TEST(BankValidate, RichBankParsesAndReads) {
  const std::vector<std::uint8_t> bytes = fixture::RichBank().Build();
  Image img(bytes);
  BankView v;
  ParseError e;
  ParseOptions o;
  o.platform = bf::kPlatformLinux;
  SND_REQUIRE_EQ(snd::bank::ParseBank(img.data(), img.size(), o, &v, &e), SND_OK);
  SND_CHECK(v.valid());
  SND_CHECK_EQ(v.bankId(), HashId("Common"));
  SND_CHECK_EQ(v.Rows(ch::EVNT), 3u);
  // Typed columns, present and defaulted.
  auto ids = v.Column<col::EVNT_id>();
  SND_CHECK_EQ(ids[0], HashId("SE/Explosion"));
  SND_CHECK_EQ(v.Column<col::EVNT_priority>()[0], 200u);
  auto retrig = v.Column<col::EVNT_retriggerMs>();  // absent optional -> default
  SND_CHECK(!retrig.present());
  SND_CHECK_EQ(retrig[2], 0.0f);
  SND_CHECK_EQ(v.Column<col::CLIP_weight>()[1], 1.0f);
  SND_CHECK_EQ(v.Column<col::CLIP_loopCount>()[0], 1u);
  SND_CHECK_EQ(v.Column<col::STRM_sectorAlign>()[0], 2048u);
  SND_CHECK_EQ(v.Column<col::DUCK_releaseMs>()[0], 200.0f);
  SND_CHECK_EQ(v.Column<col::SYNP_frequencyHz>()[0], 440.0f);
  SND_CHECK_EQ(v.Column<col::SYNP_duty>()[0], 0.5f);
  SND_CHECK_EQ(v.Column<col::EVNT_category>()[1], bf::kNone);
  SND_CHECK_EQ(v.Column<col::EVNT_category>().Get(99), bf::kNone);  // range checked
  SND_CHECK_EQ(v.Column<col::MTMP_tick>()[0], std::int64_t{0});
  SND_CHECK_EQ(v.FindRow<col::BUSS_id>(HashId("Master")), 2u);
  SND_CHECK_EQ(v.FindRow<col::BUSS_id>(HashId("Nope")), bf::kNone);
  SND_CHECK(v.MutableColumn<col::EVNT_priority>() == nullptr);  // release view: read-only
  // Blobs and typed helpers.
  const ByteSpan wave = v.WaveData(0);
  SND_CHECK_EQ(wave.bytes, 256u);
  SND_CHECK_EQ(wave.data[5], 5u);
  SND_CHECK_EQ(v.WaveData(2).data[0], 0u);  // offset 256 -> byte value 256 & 0xFF
  SND_CHECK(v.WaveData(1).empty());
  SND_CHECK_EQ(v.Preload(1).bytes, 64u);
  SND_CHECK_EQ(v.SeekRange(1).count, 2u);
  SND_CHECK_EQ(v.Column<col::SEEK_frame>()[1], 24000u);
  SND_CHECK(std::strcmp(v.StreamPath(0), "streams/amb.snds") == 0);
  SND_CHECK(v.StreamPath(1) == nullptr);
  SND_CHECK(std::strcmp(v.Name(HashId("SE/Explosion")), "SE/Explosion") == 0);  // alias row skipped
  SND_CHECK(std::strcmp(v.Name(HashId("Master")), "Master") == 0);
  SND_CHECK(v.Name(HashId("SE/OldExplosion")) == nullptr);
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;
  SND_CHECK(v.Guid(HashId("Master"), &lo, &hi));
  SND_CHECK_EQ(lo, 0x3333u);
  SND_CHECK_EQ(hi, 0x4444u);
  SND_CHECK(!v.Guid(HashId("SFX"), &lo, &hi));
  SND_CHECK_EQ(v.DspNeed().instanceBytes, 384u);
  SND_CHECK_EQ(v.PhysTableCount(), 1u);
  PhysTableView pt;
  SND_REQUIRE(v.PhysTable(0, &pt));
  SND_CHECK_EQ(pt.materialCount, 2u);
  SND_CHECK_EQ(pt.pair[1], HashId("SE/Nested"));
  SND_CHECK_EQ(pt.defaultEvent[0], HashId("SE/Explosion"));
  SND_CHECK(!v.PhysTable(1, &pt));
  SND_CHECK_EQ(v.ApplicationChunk(ch::PHYS).bytes, v.Chunk(ch::PHYS).bytes);
  SND_CHECK(v.ApplicationChunk(ch::WAVE).empty());  // not an application chunk
  const ByteSpan x = v.ApplicationChunk(snd::Fourcc('X', 'T', 'R', 'A'));
  SND_CHECK(x.bytes == 6 && std::memcmp(x.data, "hello", 6) == 0);
  SND_CHECK(!v.HasChunk(snd::Fourcc('X', 'T', 'R', 'A')));  // unknown: not a known chunk
}

SND_TEST(BankValidate, AccessorsAreRtSafe) {
  const std::vector<std::uint8_t> bytes = fixture::RichBank().Build();
  Image img(bytes);
  BankView v;
  ParseOptions o;
  SND_REQUIRE_EQ(snd::bank::ParseBank(img.data(), img.size(), o, &v, nullptr), SND_OK);
  std::uint64_t sum = 0;
  {
    SND_RT_SCOPE();
    auto prio = v.Column<col::EVNT_priority>();
    for (std::uint32_t r = 0; r < prio.rows(); ++r) sum += prio[r];
    sum += v.WaveData(0).bytes + v.Preload(1).bytes + v.SeekRange(1).count;
    sum += v.Column<col::BUSS_parent>()[0];
    sum += v.DspNeed().instanceCount;
  }
  SND_CHECK(sum > 0);
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
}

SND_TEST(BankValidate, MinimalBanks) {
  Bank empty;
  SND_CHECK_EQ(ParseFixture(empty), SND_OK);  // no chunks at all
  Bank b;
  b.Table(ch::BUSS, 1, {fixture::U32(c::BUSS_id, {1}), fixture::U32(c::BUSS_parent, {bf::kNone})});
  SND_CHECK_EQ(ParseFixture(b), SND_OK);
  // Platform: ANY_, the runtime's, or mismatch.
  b.platform = bf::kPlatformLinux;
  SND_CHECK_EQ(ParseFixture(b), SND_OK);
  b.platform = bf::kPlatformWindows;
  ExpectFail(b, SND_ERR_VERSION_MISMATCH, "platform");
}

SND_TEST(BankValidate, NewerMinorUnknownChunksAndColumnsAreIgnored) {
  Bank b = fixture::RichBank();
  b.formatMinor = bf::kFormatMinor + 3;
  b.Find(ch::EVNT)->columns.push_back(fixture::U32(200, {7, 7, 7}));         // unknown column
  b.Find(ch::EVNT)->columns.push_back(fixture::Column{201, 3, std::vector<std::uint8_t>(9, 1), -1});
  b.Table(snd::Fourcc('N', 'E', 'W', 'T'), 2, {fixture::U32(0, {1, 2})});    // unknown table
  b.Blob(snd::Fourcc('N', 'E', 'W', 'B'), std::vector<std::uint8_t>(10, 9)); // unknown blob
  SND_CHECK_EQ(ParseFixture(b), SND_OK);
  // Optional chunk with an unknown layout version is ignored (strings stripped in effect).
  Bank s = fixture::RichBank();
  s.Find(ch::STRS)->version = 2;
  const std::vector<std::uint8_t> bytes = s.Build();
  Image img(bytes);
  BankView v;
  SND_REQUIRE_EQ(snd::bank::ParseBank(img.data(), img.size(), ParseOptions{}, &v, nullptr), SND_OK);
  SND_CHECK(!v.HasChunk(ch::STRS));
  SND_CHECK_EQ(v.Rows(ch::NAME), 0u);  // NAME dropped with its string pool
  SND_CHECK(v.Name(HashId("Master")) == nullptr);
}

SND_TEST(BankValidate, RejectsUnalignedMemory) {
  const std::vector<std::uint8_t> bytes = fixture::RichBank().Build();
  std::vector<std::uint8_t> shifted(bytes.size() + 64 + 8);
  std::uint8_t* p = shifted.data();
  while ((reinterpret_cast<std::uintptr_t>(p) % 64) != 8) ++p;
  std::memcpy(p, bytes.data(), bytes.size());
  BankView v;
  ParseError e;
  SND_CHECK_EQ(snd::bank::ParseBank(p, bytes.size(), ParseOptions{}, &v, &e), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd::bank::ParseBank(nullptr, 0, ParseOptions{}, &v, &e), SND_ERR_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// §8.1 header
// ---------------------------------------------------------------------------
SND_TEST(BankValidate, Header) {
  std::vector<std::uint8_t> small(100, 0);
  ExpectFail(small, SND_ERR_CORRUPT, "smaller than");
  std::vector<std::uint8_t> bytes = fixture::RichBank().Build();
  bytes[20] ^= 1;  // bankId changed without re-signing the header
  ExpectFail(bytes, SND_ERR_CORRUPT, "header checksum");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.magic = 0x12345678; }), SND_ERR_CORRUPT, "magic");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.formatMajor = 3; }), SND_ERR_VERSION_MISMATCH, "format 3.");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.endianness = 2; }), SND_ERR_VERSION_MISMATCH, "endian");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.alignLog2 = 4; }), SND_ERR_CORRUPT, "alignLog2");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.headerBytes = 132; }), SND_ERR_CORRUPT, "headerBytes");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.platform = bf::kPlatformMac; }), SND_ERR_VERSION_MISMATCH,
             "platform");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.flags |= bf::kFileLanguageBank; }), SND_ERR_CORRUPT,
             "languageId");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.bankId = 0; }), SND_ERR_CORRUPT, "bankId");
}

// §8.2 size and checksum
SND_TEST(BankValidate, SizeAndChecksum) {
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.fileBytes += 64; }), SND_ERR_CORRUPT, "fileBytes");
  std::vector<std::uint8_t> bytes = fixture::RichBank().Build();
  bytes.push_back(0);  // trailing byte: fileBytes no longer matches
  ExpectFail(bytes, SND_ERR_CORRUPT, "fileBytes");
  bytes = fixture::RichBank().Build();
  bytes[bytes.size() - 70] ^= 0x40;  // body bit flip, header intact
  ExpectFail(bytes, SND_ERR_CORRUPT, "checksum mismatch");
}

// §8.3 dependency table and chunk table
SND_TEST(BankValidate, DependenciesAndChunkTable) {
  Bank b = fixture::RichBank();
  b.deps.push_back(bf::Dependency{b.bankId, 0, 1});
  ExpectFail(b, SND_ERR_CORRUPT, "invalid bank id");
  b.deps = {bf::Dependency{7, 0, 1}, bf::Dependency{7, 0, 2}};
  ExpectFail(b, SND_ERR_CORRUPT, "listed twice");
  b.deps = {bf::Dependency{7, 0, 1}};
  SND_CHECK_EQ(ParseFixture(b), SND_OK);
  ExpectFail(WithHeader([](bf::FileHeader& h) {
               h.dependencyCount = 1;
               h.dependencyOffset = h.fileBytes - 8;
             }),
             SND_ERR_CORRUPT, "dependency table");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.dependencyCount = 5000; }), SND_ERR_CORRUPT, "limit");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.chunkTableOffset = h.fileBytes; }), SND_ERR_CORRUPT,
             "chunk table");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.chunkTableOffset = 64; }), SND_ERR_CORRUPT, "chunk table");
  ExpectFail(WithHeader([](bf::FileHeader& h) { h.chunkCount = 4000; }), SND_ERR_CORRUPT, "limit");
}

SND_TEST(BankValidate, ChunkEntries) {
  Bank b = fixture::RichBank();
  b.Find(ch::WAVE)->offsetOverride = 64 * 1000000;
  ExpectFail(b, SND_ERR_CORRUPT, "out of range");
  b = fixture::RichBank();
  b.Find(ch::WAVE)->bytesOverride = 1ll << 40;
  ExpectFail(b, SND_ERR_CORRUPT, "out of range");
  b = fixture::RichBank();
  b.Find(ch::WAVE)->offsetOverride = 130;  // not a multiple of 64
  ExpectFail(b, SND_ERR_CORRUPT, "multiple of 64");
  b = fixture::RichBank();
  b.Find(ch::WAVE)->offsetOverride = 0;  // over the header
  ExpectFail(b, SND_ERR_CORRUPT, "out of range");
  b = fixture::RichBank();
  b.Find(ch::PREL)->offsetOverride = 128;  // over the chunk table
  ExpectFail(b, SND_ERR_CORRUPT, "chunk or dependency table");
  // Overlap: point PREL at WAVE's start.
  {
    Bank o = fixture::RichBank();
    const std::vector<std::uint8_t> built = o.Build();
    bf::FileHeader h;
    std::memcpy(&h, built.data(), sizeof(h));
    std::uint64_t waveOff = 0;
    for (std::uint32_t i = 0; i < h.chunkCount; ++i) {
      bf::ChunkEntry e;
      std::memcpy(&e, built.data() + h.chunkTableOffset + i * sizeof(e), sizeof(e));
      if (e.fourcc == ch::WAVE) waveOff = e.offset;
    }
    o.Find(ch::PREL)->offsetOverride = static_cast<std::int64_t>(waveOff);
    ExpectFail(o, SND_ERR_CORRUPT, "overlaps");
  }
  b = fixture::RichBank();
  b.Blob(ch::WAVE, std::vector<std::uint8_t>(4, 0));
  ExpectFail(b, SND_ERR_CORRUPT, "twice");
  b = fixture::RichBank();
  b.Find(ch::WAVE)->flags = bf::kChunkTable;
  ExpectFail(b, SND_ERR_CORRUPT, "table flag");
  b = fixture::RichBank();
  b.Find(ch::EVNT)->flags = 0;
  ExpectFail(b, SND_ERR_CORRUPT, "table flag");
  b = fixture::RichBank();
  b.Find(ch::EVNT)->version = 2;  // required chunk with an unknown layout
  ExpectFail(b, SND_ERR_VERSION_MISMATCH, "layout version");
}

// §8.4 table chunks
SND_TEST(BankValidate, TableChunks) {
  Bank b = fixture::RichBank();
  b.Find(ch::EVNT)->bytesOverride = 8;
  ExpectFail(b, SND_ERR_CORRUPT, "no header");
  b = fixture::RichBank();
  b.Find(ch::SEND)->bytesOverride = 20;  // directory (16 + 2*16) beyond the chunk
  ExpectFail(b, SND_ERR_CORRUPT, "directory");
  b = fixture::RichBank();
  b.Find(ch::EVNT)->columns.push_back(fixture::U32(c::EVNT_bus, {0, 0, 0}));
  ExpectFail(b, SND_ERR_CORRUPT, "twice");
  b = fixture::RichBank();
  b.Find(ch::EVNT)->columns.push_back(fixture::U64(c::EVNT_retriggerMs, {0, 0, 0}));
  ExpectFail(b, SND_ERR_CORRUPT, "elemBytes");
  b = fixture::RichBank();
  b.FindColumn(ch::EVNT, c::EVNT_bus)->offsetOverride = 100;
  ExpectFail(b, SND_ERR_CORRUPT, "misaligned");
  b = fixture::RichBank();
  b.FindColumn(ch::EVNT, c::EVNT_bus)->offsetOverride = 64 * 4096;
  ExpectFail(b, SND_ERR_CORRUPT, "out of the chunk");
  b = fixture::RichBank();
  b.FindColumn(ch::EVNT, c::EVNT_bus)->offsetOverride = 0;  // inside the directory
  ExpectFail(b, SND_ERR_CORRUPT, "out of the chunk");
  b = fixture::RichBank();
  b.FindColumn(ch::EVNT, c::EVNT_bus)->offsetOverride = 256;  // column 0's array (13 columns: directory ends at 224)
  ExpectFail(b, SND_ERR_CORRUPT, "overlaps column");
  b = fixture::RichBank();
  b.RemoveColumn(ch::EVNT, c::EVNT_bus);
  ExpectFail(b, SND_ERR_CORRUPT, "required column EVNT.bus");
  b = fixture::RichBank();
  b.Find(ch::EVNT)->rows = 1000;  // columns shorter than rowCount
  ExpectFail(b, SND_ERR_CORRUPT, "out of the chunk");
}

// §8.5 references
SND_TEST(BankValidate, RowReferences) {
  Bank b = fixture::RichBank();
  b.SetU32(ch::EVNT, c::EVNT_bus, 0, 3);
  ExpectFail(b, SND_ERR_CORRUPT, "EVNT col 5 row 0");
  b = fixture::RichBank();
  b.SetU32(ch::EVNT, c::EVNT_bus, 1, bf::kNone);  // required reference may not be none
  ExpectFail(b, SND_ERR_CORRUPT, "out of range");
  b = fixture::RichBank();
  b.SetU32(ch::EVNT, c::EVNT_category, 0, 2);
  ExpectFail(b, SND_ERR_CORRUPT, "CATG has 2 rows");
  b = fixture::RichBank();
  b.SetU32(ch::EVNT, c::EVNT_trackCount, 0, 4);
  ExpectFail(b, SND_ERR_CORRUPT, "range [0, +4)");
  b = fixture::RichBank();
  b.SetU32(ch::EVNT, c::EVNT_trackFirst, 1, bf::kNone);  // count 1 with first none
  ExpectFail(b, SND_ERR_CORRUPT, "range");
  b = fixture::RichBank();
  b.SetU32(ch::CLIP, c::CLIP_kind, 3, 7);
  ExpectFail(b, SND_ERR_CORRUPT, "unknown clip kind");
  b = fixture::RichBank();
  b.SetU32(ch::CLIP, c::CLIP_target, 2, 3);  // nested event row 3 of 3
  ExpectFail(b, SND_ERR_CORRUPT, "clip target");
  b = fixture::RichBank();
  b.SetU32(ch::BUSS, c::BUSS_parent, 1, 0);  // parent before the bus
  ExpectFail(b, SND_ERR_CORRUPT, "graph order");
  b = fixture::RichBank();
  b.SetU32(ch::SEND, c::SEND_targetBus, 0, 0);  // send to itself
  ExpectFail(b, SND_ERR_CORRUPT, "graph order");
  b = fixture::RichBank();
  b.SetU32(ch::SNPV, c::SNPV_targetKind, 0, 9);
  ExpectFail(b, SND_ERR_CORRUPT, "unknown target kind");
  b = fixture::RichBank();
  b.SetU32(ch::SNPV, c::SNPV_paramIndex, 1, 2);
  ExpectFail(b, SND_ERR_CORRUPT, "parameter index");
  b = fixture::RichBank();
  b.SetU32(ch::DUCK, c::DUCK_kind, 0, 1);
  b.SetU32(ch::DUCK, c::DUCK_source, 0, 2);  // CATG has 2 rows
  ExpectFail(b, SND_ERR_CORRUPT, "source row");
  b = fixture::RichBank();
  b.SetU32(ch::ASST, c::ASST_stream, 1, bf::kNone);
  ExpectFail(b, SND_ERR_CORRUPT, "without STRM");
  b = fixture::RichBank();
  b.SetU32(ch::ASST, c::ASST_channels, 0, 0);
  ExpectFail(b, SND_ERR_CORRUPT, "channel count");
  b = fixture::RichBank();
  b.SetU32(ch::MSTM, c::MSTM_channelCount, 0, 3);
  ExpectFail(b, SND_ERR_CORRUPT, "stem channels");
  b = fixture::RichBank();
  b.SetU32(ch::MSTA, c::MSTA_musicEvent, 0, 3);
  ExpectFail(b, SND_ERR_CORRUPT, "MSTA");
}

SND_TEST(BankValidate, BlobAndStringReferences) {
  Bank b = fixture::RichBank();
  b.SetU64(ch::ASST, c::ASST_dataBytes, 0, 321);
  ExpectFail(b, SND_ERR_CORRUPT, "outside WAVE");
  b = fixture::RichBank();
  b.SetU64(ch::ASST, c::ASST_dataOffset, 2, 200);  // not 64-aligned
  ExpectFail(b, SND_ERR_CORRUPT, "misaligned");
  b = fixture::RichBank();
  b.SetU64(ch::ASST, c::ASST_dataOffset, 0, ~0ull - 10);  // offset + length overflows
  ExpectFail(b, SND_ERR_CORRUPT, "outside WAVE");
  b = fixture::RichBank();
  b.SetU32(ch::ASST, c::ASST_preloadBytes, 1, 65);
  ExpectFail(b, SND_ERR_CORRUPT, "outside PREL");
  b = fixture::RichBank();
  b.SetU64(ch::EFFX, c::EFFX_blobBytes, 1, 17);
  ExpectFail(b, SND_ERR_CORRUPT, "outside EFBL");
  b = fixture::RichBank();
  b.SetU32(ch::STRM, c::STRM_pathOffset, 0, 17);  // == SPTH size
  ExpectFail(b, SND_ERR_CORRUPT, "terminated");
  b = fixture::RichBank();
  b.Find(ch::SPTH)->blob = fixture::Bytes("no-terminator", false);
  ExpectFail(b, SND_ERR_CORRUPT, "terminated");
  b = fixture::RichBank();
  b.SetU32(ch::NAME, c::NAME_stringOffset, 1, 1000);
  ExpectFail(b, SND_ERR_CORRUPT, "terminated");
  b = fixture::RichBank();
  b.Find(ch::STRM)->columns.push_back(fixture::U32(c::STRM_sectorAlign, {3000}));
  ExpectFail(b, SND_ERR_CORRUPT, "sector alignment");
  b = fixture::RichBank();
  b.Find(ch::DSPN)->blob.resize(8);
  ExpectFail(b, SND_ERR_CORRUPT, "DSPN");
  b = fixture::RichBank();
  b.Find(ch::PHYS)->blob.resize(40);
  ExpectFail(b, SND_ERR_CORRUPT, "PHYS");
}

// Shared with buildtool's self-check (buildtool/test/self_check_test.cpp).
SND_TEST(BankValidate, SharedCorruptionCases) {
  for (const corruption::Case& k : corruption::Cases()) {
    ParseError e;
    const snd_result r = Parse(corruption::Build(k), &e);
    SND_CHECK_MSG(r == k.expected, "%s: got %d (%s), expected %d", k.name, r, e.message, k.expected);
  }
}
