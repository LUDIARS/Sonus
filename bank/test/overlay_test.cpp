// Dev override layer (design.md 11.2, prepared.hpp), independent tester.
#include <cstring>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/bank/prepared.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"
#include "spec_walk.hpp"
#include "test_support.hpp"

namespace bf = snd::bank_format;
namespace ch = bf::chunk;
namespace c = bf::col;
using fixture::Image;
using namespace snd::bank;

namespace {

PrepareOptions DevOptions(std::uint32_t buses, std::uint32_t effects, std::uint32_t events, std::uint64_t bytes) {
  PrepareOptions o;
  o.devOverlay = true;
  o.devReserve.buses = buses;
  o.devReserve.effects = effects;
  o.devReserve.events = events;
  o.devReserve.bytes = bytes;
  return o;
}

std::uint32_t Extra(std::uint32_t fourcc, const PrepareOptions& o) {
  if (fourcc == ch::BUSS) return o.devReserve.buses;
  if (fourcc == ch::EFFX) return o.devReserve.effects;
  if (fourcc == ch::EVNT) return o.devReserve.events;
  return 0;
}

}  // namespace

SND_TEST(BankOverlay, CopiesMatchDefaultsFillCapacityAndAlignment) {
  const std::vector<std::uint8_t> bytes = fixture::RichBank().Build();
  Image img(bytes);
  BankView plain;
  SND_REQUIRE_EQ(ParseBank(img.data(), img.size(), ParseOptions{}, &plain, nullptr), SND_OK);
  testsupport::CountingAllocator alloc;
  const PrepareOptions opt = DevOptions(3, 5, 7, 1000);
  PreparedBank* pb = nullptr;
  SND_REQUIRE_EQ(PrepareBank(img.data(), img.size(), opt, alloc, &pb, nullptr), SND_OK);
  const BankView& v = pb->view;
  SND_CHECK(v.writable());
  SND_CHECK(specwalk::CheckInvariants(v).empty());
  (void)specwalk::DeepWalk(v);
  for (const bf::ChunkInfo& ci : bf::kChunks) {
    if (ci.kind != bf::ChunkKind::Table) continue;
    const TableView* t = v.Table(ci.fourcc);
    const TableView* p = plain.Table(ci.fourcc);
    const bool present = plain.HasChunk(ci.fourcc);
    const std::uint32_t extra = Extra(ci.fourcc, opt);
    if (!present && extra == 0) {
      SND_CHECK_EQ(t->capacity, 0u);
      continue;
    }
    SND_CHECK_EQ(t->rows, p->rows);
    SND_CHECK_MSG(t->capacity == p->rows + extra, "%08x capacity %u rows %u extra %u", ci.fourcc, t->capacity, p->rows,
                  extra);
    for (const bf::ColumnInfo& col : bf::kColumns) {
      if (col.chunk != ci.fourcc) continue;
      const std::uint32_t eb = bf::ColumnTypeBytes(col.type);
      const auto* dst = static_cast<const std::uint8_t*>(t->columns[col.columnId]);
      SND_CHECK_MSG(dst != nullptr, "%s not materialised", col.name);
      if (dst == nullptr) continue;
      SND_CHECK_MSG((reinterpret_cast<std::uintptr_t>(dst) & 63u) == 0, "%s not 64-aligned", col.name);
      SND_CHECK_MSG(dst < img.data() || dst >= img.data() + img.size(), "%s points into the image", col.name);
      const auto* src = static_cast<const std::uint8_t*>(p->columns[col.columnId]);
      for (std::uint32_t r = 0; r < t->capacity; ++r) {
        std::uint8_t expect[8];
        if (src != nullptr && r < p->rows) {
          std::memcpy(expect, src + std::uint64_t{r} * eb, eb);
        } else if (col.type == bf::ColumnType::F32) {
          const float f = static_cast<float>(col.defaultValue);
          std::memcpy(expect, &f, 4);
        } else if (eb == 4) {
          const std::uint32_t u = static_cast<std::uint32_t>(col.defaultValue);
          std::memcpy(expect, &u, 4);
        } else {
          const std::uint64_t u = static_cast<std::uint64_t>(col.defaultValue);
          std::memcpy(expect, &u, 8);
        }
        if (std::memcmp(expect, dst + std::uint64_t{r} * eb, eb) != 0) {
          SND_CHECK_MSG(false, "%s row %u differs from the bank / default", col.name, r);
          break;
        }
      }
      // FindWritableColumn agrees with the view.
      WritableColumn wc;
      SND_CHECK_EQ(FindWritableColumn(*pb, ci.fourcc, col.columnId, &wc), SND_OK);
      SND_CHECK(wc.data == t->columns[col.columnId]);
      SND_CHECK_EQ(wc.capacity, t->capacity);
      SND_CHECK_EQ(wc.rows, t->rows);
      SND_CHECK_EQ(wc.elemBytes, eb);
    }
  }
  // Writes over the whole capacity of every writable column stay inside the
  // overlay (ASan) and never touch the image.
  const std::vector<std::uint8_t> before(img.data(), img.data() + img.size());
  for (const bf::ColumnInfo& col : bf::kColumns) {
    WritableColumn wc;
    if (FindWritableColumn(*pb, col.chunk, col.columnId, &wc) != SND_OK) continue;
    std::memset(wc.data, 0x5A, std::uint64_t{wc.capacity} * wc.elemBytes);
  }
  SND_CHECK(std::memcmp(before.data(), img.data(), img.size()) == 0);
  std::uint32_t* prio = v.MutableColumn<col::EVNT_priority>();
  SND_REQUIRE(prio != nullptr);
  prio[v.Table(ch::EVNT)->capacity - 1] = 1;
  // Spare region: zeroed, 64-aligned, devReserve.bytes rounded up.
  SND_CHECK(pb->overlay.spare != nullptr);
  SND_CHECK_EQ(pb->overlay.spareBytes, 1024u);
  SND_CHECK((reinterpret_cast<std::uintptr_t>(pb->overlay.spare) & 63u) == 0);
  bool zero = true;
  for (std::uint64_t i = 0; i < pb->overlay.spareBytes; ++i) zero = zero && pb->overlay.spare[i] == 0;
  SND_CHECK(zero);
  // Unknown column / table without overlay rows.
  WritableColumn wc;
  SND_CHECK_EQ(FindWritableColumn(*pb, ch::EVNT, 99, &wc), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(FindWritableColumn(*pb, snd::Fourcc('Z', 'Z', 'Z', 'Z'), 0, &wc), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(FindWritableColumn(*pb, ch::EVNT, 0, nullptr), SND_ERR_INVALID_ARGUMENT);
  DestroyPrepared(pb, alloc);
  SND_CHECK_EQ(alloc.Live(), 0u);
}

// Release banks (no kFileDevBuild) and devOverlay=false: no copy, read-only.
SND_TEST(BankOverlay, NotBuiltForReleaseBanksOrWithoutTheOption) {
  fixture::Bank b = fixture::RichBank();
  b.flags &= ~static_cast<std::uint32_t>(bf::kFileDevBuild);
  const std::vector<std::uint8_t> rel = b.Build();
  const std::vector<std::uint8_t> dev = fixture::RichBank().Build();
  testsupport::CountingAllocator alloc;
  for (int k = 0; k < 2; ++k) {
    Image img(k == 0 ? rel : dev);
    PrepareOptions o = DevOptions(4, 4, 4, 4096);
    if (k == 1) o.devOverlay = false;
    PreparedBank* pb = nullptr;
    SND_REQUIRE_EQ(PrepareBank(img.data(), img.size(), o, alloc, &pb, nullptr), SND_OK);
    SND_CHECK(!pb->view.writable());
    SND_CHECK(pb->overlay.base == nullptr);
    SND_CHECK(pb->view.MutableColumn<col::EVNT_priority>() == nullptr);
    WritableColumn wc;
    SND_CHECK_EQ(FindWritableColumn(*pb, ch::EVNT, c::EVNT_priority, &wc), SND_ERR_UNSUPPORTED);
    SND_CHECK_EQ(pb->view.Table(ch::EVNT)->capacity, pb->view.Table(ch::EVNT)->rows);
    DestroyPrepared(pb, alloc);
  }
  SND_CHECK_EQ(alloc.Live(), 0u);
}

// A dev bank without BUSS/EFFX/EVNT still gets reserve rows for them (11.2
// structural edits), every column materialised with defaults.
SND_TEST(BankOverlay, ReserveRowsForAbsentTables) {
  fixture::Bank b;
  b.flags = bf::kFileDevBuild;
  b.Table(ch::PARM, 1, {fixture::U32(c::PARM_id, {1}), fixture::U32(c::PARM_kind, {0})});
  const std::vector<std::uint8_t> bytes = b.Build();
  Image img(bytes);
  testsupport::CountingAllocator alloc;
  PreparedBank* pb = nullptr;
  SND_REQUIRE_EQ(PrepareBank(img.data(), img.size(), DevOptions(2, 3, 4, 0), alloc, &pb, nullptr), SND_OK);
  SND_CHECK_EQ(pb->view.Table(ch::BUSS)->rows, 0u);
  SND_CHECK_EQ(pb->view.Table(ch::BUSS)->capacity, 2u);
  SND_CHECK_EQ(pb->view.Table(ch::EVNT)->capacity, 4u);
  std::uint32_t* parent = pb->view.MutableColumn<col::BUSS_parent>();
  SND_REQUIRE(parent != nullptr);
  SND_CHECK_EQ(parent[1], bf::kNone);  // default of BUSS.parent
  SND_CHECK(pb->overlay.spare == nullptr);
  SND_CHECK_EQ(pb->view.Column<col::BUSS_id>().rows(), 0u);
  DestroyPrepared(pb, alloc);
  SND_CHECK_EQ(alloc.Live(), 0u);
}

// Reserve that overflows a u32 capacity, and allocation failure: clean failure.
SND_TEST(BankOverlay, ReserveOverflowAndAllocationFailure) {
  const std::vector<std::uint8_t> bytes = fixture::RichBank().Build();
  Image img(bytes);
  testsupport::CountingAllocator alloc;
  PreparedBank* pb = nullptr;
  ParseError err;
  SND_CHECK_EQ(PrepareBank(img.data(), img.size(), DevOptions(0xFFFFFFFFu, 0, 0, 0), alloc, &pb, &err),
               SND_ERR_OUT_OF_BUDGET);
  SND_CHECK(pb == nullptr);
  SND_CHECK_EQ(alloc.Live(), 0u);
  for (long n = 0; n < 4; ++n) {
    alloc.FailAt(n);
    const snd_result r = PrepareBank(img.data(), img.size(), DevOptions(1, 1, 1, 64), alloc, &pb, &err);
    alloc.FailAt(-1);
    if (r == SND_OK) {
      DestroyPrepared(pb, alloc);
    } else {
      SND_CHECK(pb == nullptr);
      SND_CHECK(err.message[0] != '\0');
    }
    SND_CHECK_EQ(alloc.Live(), 0u);
  }
}
