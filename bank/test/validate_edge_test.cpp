// ParseBank targeted cases (independent tester): chunk layout corner cases,
// versions, optional chunks with unknown layout, and structures that pass §8
// but are cyclic (reported as a spec gap).
#include <cstring>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/bank/validate.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"
#include "spec_walk.hpp"

namespace bf = snd::bank_format;
namespace ch = bf::chunk;
namespace c = bf::col;
using fixture::Bank;
using fixture::Image;
using namespace snd::bank;

namespace {

snd_result Parse(const std::vector<std::uint8_t>& bytes, BankView* keep = nullptr, Image** img = nullptr,
                 ParseError* err = nullptr) {
  Image* local = new Image(bytes);
  BankView v;
  ParseError e;
  const snd_result r = ParseBank(local->data(), local->size(), ParseOptions{}, &v, err != nullptr ? err : &e);
  if (r == SND_OK) {
    SND_CHECK(specwalk::CheckInvariants(v).empty());
    (void)specwalk::DeepWalk(v);
  }
  if (keep != nullptr) *keep = v;
  if (img != nullptr) {
    *img = local;
  } else {
    delete local;
  }
  return r;
}

Bank Small() {
  Bank b;
  b.Table(ch::BUSS, 2, {fixture::U32(c::BUSS_id, {1, 2}), fixture::U32(c::BUSS_parent, {1, bf::kNone})});
  return b;
}

template <class T>
void Put(std::vector<std::uint8_t>& f, std::size_t off, T v) {
  std::memcpy(f.data() + off, &v, sizeof v);
}

bf::FileHeader Header(const std::vector<std::uint8_t>& f) {
  bf::FileHeader h;
  std::memcpy(&h, f.data(), sizeof h);
  return h;
}

}  // namespace

SND_TEST(BankValidateEdge, ChunkLayoutCorners) {
  // Zero-length unknown chunk exactly at the end of the file: legal (in range, aligned).
  {
    Bank b = Small();
    b.Blob(snd::Fourcc('Z', 'E', 'R', 'O'), {});
    const std::vector<std::uint8_t> f = b.Build();
    SND_CHECK_EQ(Parse(f), SND_OK);
  }
  // Chunk starting inside the header (offset 64, aligned): rejected.
  {
    Bank b = Small();
    b.Blob(snd::Fourcc('H', 'D', 'R', '_'), std::vector<std::uint8_t>(16, 0)).offsetOverride = 64;
    SND_CHECK_EQ(Parse(b.Build()), SND_ERR_CORRUPT);
  }
  // Chunk over the chunk table.
  {
    Bank b = Small();
    std::vector<std::uint8_t> f = b.Build();
    const bf::FileHeader h = Header(f);
    bf::ChunkEntry e;
    std::memcpy(&e, f.data() + h.chunkTableOffset, sizeof e);
    e.offset = 128;  // the chunk table lives at 128
    std::memcpy(f.data() + h.chunkTableOffset, &e, sizeof e);
    Bank::Rehash(f);
    SND_CHECK_EQ(Parse(f), SND_ERR_CORRUPT);
  }
  // Chunk one byte past the end of the file.
  {
    Bank b = Small();
    std::vector<std::uint8_t> f = b.Build();
    const bf::FileHeader h = Header(f);
    bf::ChunkEntry e;
    std::memcpy(&e, f.data() + h.chunkTableOffset, sizeof e);
    e.bytes = f.size() - e.offset + 1;
    std::memcpy(f.data() + h.chunkTableOffset, &e, sizeof e);
    Bank::Rehash(f);
    SND_CHECK_EQ(Parse(f), SND_ERR_CORRUPT);
    e.bytes = f.size() - e.offset;  // exactly to the end: fine
    std::memcpy(f.data() + h.chunkTableOffset, &e, sizeof e);
    Bank::Rehash(f);
    SND_CHECK_EQ(Parse(f), SND_OK);
  }
  // Same fourcc twice (unknown chunk too), and two chunks sharing bytes.
  {
    Bank b = Small();
    b.Blob(snd::Fourcc('X', 'X', 'X', 'X'), {1});
    b.Blob(snd::Fourcc('X', 'X', 'X', 'X'), {2});
    SND_CHECK_EQ(Parse(b.Build()), SND_ERR_CORRUPT);
  }
  {
    Bank b = Small();
    b.Blob(snd::Fourcc('A', 'A', 'A', 'A'), std::vector<std::uint8_t>(100, 1));
    b.Blob(snd::Fourcc('B', 'B', 'B', 'B'), std::vector<std::uint8_t>(10, 2));
    std::vector<std::uint8_t> f = b.Build();
    const bf::FileHeader h = Header(f);
    bf::ChunkEntry a, bb;
    std::memcpy(&a, f.data() + h.chunkTableOffset + 32, sizeof a);
    std::memcpy(&bb, f.data() + h.chunkTableOffset + 64, sizeof bb);
    bb.offset = a.offset + 64;  // inside AAAA
    std::memcpy(f.data() + h.chunkTableOffset + 64, &bb, sizeof bb);
    Bank::Rehash(f);
    SND_CHECK_EQ(Parse(f), SND_ERR_CORRUPT);
  }
  // Misaligned chunk offset.
  {
    Bank b = Small();
    std::vector<std::uint8_t> f = b.Build();
    const bf::FileHeader h = Header(f);
    bf::ChunkEntry e;
    std::memcpy(&e, f.data() + h.chunkTableOffset, sizeof e);
    e.offset += 4;
    std::memcpy(f.data() + h.chunkTableOffset, &e, sizeof e);
    Bank::Rehash(f);
    SND_CHECK_EQ(Parse(f), SND_ERR_CORRUPT);
  }
  // Known blob chunk claiming the table flag (and vice versa).
  {
    Bank b = Small();
    b.Blob(ch::WAVE, std::vector<std::uint8_t>(64, 0)).flags = bf::kChunkTable;
    SND_CHECK_EQ(Parse(b.Build()), SND_ERR_CORRUPT);
  }
}

SND_TEST(BankValidateEdge, ColumnsAndTables) {
  // Duplicate column id.
  {
    Bank b = Small();
    b.Find(ch::BUSS)->columns.push_back(fixture::U32(c::BUSS_id, {3, 4}));
    SND_CHECK_EQ(Parse(b.Build()), SND_ERR_CORRUPT);
  }
  // Missing required column.
  {
    Bank b = Small();
    b.RemoveColumn(ch::BUSS, c::BUSS_parent);
    SND_CHECK_EQ(Parse(b.Build()), SND_ERR_CORRUPT);
  }
  // Unknown columns with odd element sizes are ignored; optional columns read defaults.
  {
    Bank b = Small();
    fixture::Column odd;
    odd.id = 200;
    odd.elemBytes = 3;
    odd.data = std::vector<std::uint8_t>(6, 0xEE);
    b.Find(ch::BUSS)->columns.push_back(odd);
    fixture::Column zero;
    zero.id = 201;
    zero.elemBytes = 0;
    b.Find(ch::BUSS)->columns.push_back(zero);
    BankView v;
    Image* img = nullptr;
    SND_CHECK_EQ(Parse(b.Build(), &v, &img), SND_OK);
    SND_CHECK_EQ(v.Column<col::BUSS_sendFirst>()[1], bf::kNone);
    SND_CHECK_EQ(v.Column<col::BUSS_volumeDb>()[0], 0.0f);
    SND_CHECK(!v.Column<col::BUSS_flags>().present());
    delete img;
  }
  // Known column with the wrong element size.
  {
    Bank b = Small();
    b.FindColumn(ch::BUSS, c::BUSS_parent)->elemBytes = 8;
    SND_CHECK_EQ(Parse(b.Build()), SND_ERR_CORRUPT);
  }
  // Column array inside the column directory (offset 0 of the chunk).
  {
    Bank b = Small();
    b.FindColumn(ch::BUSS, c::BUSS_id)->offsetOverride = 0;
    SND_CHECK_EQ(Parse(b.Build()), SND_ERR_CORRUPT);
  }
  // Two known columns on the same bytes.
  {
    Bank b = Small();
    b.FindColumn(ch::BUSS, c::BUSS_parent)->offsetOverride = 64;
    b.FindColumn(ch::BUSS, c::BUSS_id)->offsetOverride = 64;
    SND_CHECK_EQ(Parse(b.Build()), SND_ERR_CORRUPT);
  }
  // Table chunk shorter than its header.
  {
    Bank b = Small();
    b.Find(ch::BUSS)->bytesOverride = 8;
    SND_CHECK_EQ(Parse(b.Build()), SND_ERR_CORRUPT);
  }
}

SND_TEST(BankValidateEdge, VersionsAndPlatforms) {
  auto withHeader = [](auto&& f) {
    std::vector<std::uint8_t> b = Small().Build();
    bf::FileHeader h = Header(b);
    f(h);
    std::memcpy(b.data(), &h, sizeof h);
    Bank::Rehash(b);
    return b;
  };
  SND_CHECK_EQ(Parse(withHeader([](bf::FileHeader& h) { h.formatMinor = 0xFFFF; })), SND_OK);
  SND_CHECK_EQ(Parse(withHeader([](bf::FileHeader& h) { h.formatMajor = 3; })), SND_ERR_VERSION_MISMATCH);  // format 2.0: was 2
  SND_CHECK_EQ(Parse(withHeader([](bf::FileHeader& h) { h.formatMajor = 0; })), SND_ERR_VERSION_MISMATCH);
  SND_CHECK_EQ(Parse(withHeader([](bf::FileHeader& h) { h.platform = bf::kPlatformWindows; })),
               RuntimePlatform() == bf::kPlatformWindows ? SND_OK : SND_ERR_VERSION_MISMATCH);
  SND_CHECK_EQ(Parse(withHeader([](bf::FileHeader& h) { h.platform = RuntimePlatform(); })), SND_OK);
  SND_CHECK_EQ(Parse(withHeader([](bf::FileHeader& h) { h.platform = snd::Fourcc('?', '?', '?', '?'); })),
               SND_ERR_VERSION_MISMATCH);
  // Endianness: §8.6 maps only version/platform to VERSION_MISMATCH; report.
  const snd_result en = Parse(withHeader([](bf::FileHeader& h) { h.endianness = 2; }));
  snd::test::Note("endianness 2 -> %d (VERSION_MISMATCH=%d, CORRUPT=%d)", en, SND_ERR_VERSION_MISMATCH, SND_ERR_CORRUPT);
  SND_CHECK(en != SND_OK);
  SND_CHECK_EQ(Parse(withHeader([](bf::FileHeader& h) { h.alignLog2 = 5; })), SND_ERR_CORRUPT);
  SND_CHECK_EQ(Parse(withHeader([](bf::FileHeader& h) { h.headerBytes = 256; })), SND_ERR_CORRUPT);
  SND_CHECK_EQ(Parse(withHeader([](bf::FileHeader& h) { h.magic = 0; })), SND_ERR_CORRUPT);
  // Known optional chunk with an unknown layout version: ignored (GUID, NAME, STRS).
  {
    Bank b = fixture::RichBank();
    b.Find(ch::GUID)->version = 2;
    b.Find(ch::STRS)->version = 7;
    BankView v;
    Image* img = nullptr;
    SND_CHECK_EQ(Parse(b.Build(), &v, &img), SND_OK);
    SND_CHECK(!v.HasChunk(ch::GUID));
    SND_CHECK(!v.HasChunk(ch::NAME));  // NAME without its pool is dropped
    SND_CHECK(v.Name(snd::HashId("SE/Explosion")) == nullptr);
    delete img;
  }
  // Required chunk with an unknown layout version: rejected.
  {
    Bank b = fixture::RichBank();
    b.Find(ch::WAVE)->version = 2;
    SND_CHECK_EQ(Parse(b.Build()), SND_ERR_VERSION_MISMATCH);
  }
}

// §8 checks ranges, not acyclicity: a signed bank whose event nests itself
// (CLIP kind 1 -> its own EVNT row) or whose categories form a parent cycle
// is accepted. Consumers that follow these links recursively (nested event
// playback, category volume inheritance) must bound the walk themselves.
// Reported as a spec gap (buildtool rejects such projects, a hostile or
// corrupted-and-re-signed file is not).
SND_TEST(BankValidateEdge, CyclesPassValidation) {
  Bank b;
  b.Table(ch::BUSS, 1, {fixture::U32(c::BUSS_id, {1}), fixture::U32(c::BUSS_parent, {bf::kNone})});
  b.Table(ch::EVNT, 1,
          {fixture::U32(c::EVNT_id, {7}), fixture::U32(c::EVNT_trackFirst, {0}), fixture::U32(c::EVNT_trackCount, {1}),
           fixture::U32(c::EVNT_sequenceKind, {0}), fixture::U32(c::EVNT_priority, {128}), fixture::U32(c::EVNT_bus, {0}),
           fixture::U32(c::EVNT_flags, {0}), fixture::U32(c::EVNT_category, {0})});
  b.Table(ch::TRAK, 1, {fixture::U32(c::TRAK_event, {0}), fixture::U32(c::TRAK_clipFirst, {0}), fixture::U32(c::TRAK_clipCount, {1})});
  b.Table(ch::CLIP, 1, {fixture::U32(c::CLIP_kind, {1}), fixture::U32(c::CLIP_target, {0})});
  b.Table(ch::CATG, 2, {fixture::U32(c::CATG_id, {1, 2}), fixture::U32(c::CATG_parent, {1, 0})});
  const snd_result r = Parse(b.Build());
  snd::test::Note("self-nesting event + category parent cycle: %s", r == SND_OK ? "accepted by ParseBank" : "rejected");
}
