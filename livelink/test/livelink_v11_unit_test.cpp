// Protocol 1.1 building blocks without a running link: the contract additions (sizes,
// minor of each type, mask bits), the new validators, the dev reserve costs, the new
// record kinds in the reader, the replay rule of the new markers, and what the Replayer
// reports for hand-made 1.1 recordings.
#include <cstring>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/internal/notify.hpp"
#include "snd/livelink/command_check.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/livelink/replayer.hpp"
#include "snd/livelink/struct_edit.hpp"
#include "snd/livelink/wire.hpp"
#include "snd/test/test.hpp"

namespace ll = snd::livelink;
namespace bf = snd::bank_format;
namespace cmd = snd::cmd;

namespace {

template <class T>
void Put(std::vector<std::uint8_t>& v, const T& x) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
  v.insert(v.end(), p, p + sizeof(T));
}

std::uint64_t F(float f) {
  std::uint32_t b = 0;
  std::memcpy(&b, &f, 4);
  return b;
}

snd_config Cfg() {
  snd_config c;
  snd_config_init(&c);
  c.max_playbacks = 64;
  c.max_emitters = 16;
  c.max_transports = 4;
  c.max_user_buffers = 4;
  c.max_banks = 8;
  return c;
}

// An empty arena record at `block`.
void Arena(std::vector<std::uint8_t>& v, std::uint64_t block, const std::vector<std::uint8_t>& normal = {}) {
  const ll::RecordArena ra{static_cast<std::uint32_t>(ll::RecordKind::Arena), 0u, block,
                           static_cast<std::uint32_t>(sizeof(ll::ArenaSections) + normal.size()), 0u};
  Put(v, ra);
  Put(v, ll::ArenaSections{static_cast<std::uint32_t>(normal.size()), 0u, 0u, 0u});
  v.insert(v.end(), normal.begin(), normal.end());
}

std::vector<std::uint8_t> Start(std::uint64_t block, ll::RecordOrigin origin = ll::RecordOrigin::FromCreation) {
  std::vector<std::uint8_t> v;
  Put(v, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, block});
  Put(v, ll::RecordBegin{ll::kRecordBegin, static_cast<std::uint16_t>(origin), 0u, block});
  return v;
}

int Create(const std::vector<std::uint8_t>& rec, ll::ReplayInfo* info = nullptr) {
  const snd_config cfg = Cfg();
  ll::ReplayDesc d;
  d.config = &cfg;
  d.allocator = &snd::base::DefaultAllocator();
  d.record = rec.data();
  d.bytes = rec.size();
  ll::Replayer* r = nullptr;
  const int res = ll::Replayer::Create(d, &r);
  if (r != nullptr && info != nullptr) *info = r->Info();
  ll::Replayer::Destroy(r);
  return res;
}

}  // namespace

SND_TEST(LivelinkV11Unit, ContractAdditions) {
  SND_CHECK_EQ(ll::kProtocolMajor, 1u);
  SND_CHECK_EQ(ll::kProtocolMinor, 1u);
  using MT = ll::MessageType;
  for (MT t : {MT::RevertValue, MT::ReadCell, MT::QueryBanks, MT::ReloadBank, MT::CellValue, MT::BankList, MT::StreamList,
               MT::BlockDetail, MT::ReloadStatus}) {
    SND_CHECK(ll::IsKnown(static_cast<std::uint16_t>(t)));
    SND_CHECK_EQ(ll::MinorOf(t), 1u);
    SND_CHECK(std::strcmp(ll::MessageTypeName(static_cast<std::uint16_t>(t)), "Unknown") != 0);
  }
  SND_CHECK_EQ(ll::MinorOf(MT::SetValue), 0u);
  SND_CHECK_EQ(ll::MinorOf(MT::RecordData), 0u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::RevertValue), 48u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::ReadCell), 48u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::QueryBanks), 0u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::ReloadBank), 16u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::CellValue), 32u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::BankList), 8u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::StreamList), 8u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::BlockDetail), 48u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::ReloadStatus), 24u);
  SND_CHECK_EQ(sizeof(ll::BankEntry), 32u);
  SND_CHECK_EQ(sizeof(ll::StreamInfo), 16u);
  SND_CHECK_EQ(sizeof(ll::RecordBegin), 16u);
  SND_CHECK_EQ(sizeof(ll::RecordCell), 40u);
  SND_CHECK_EQ(sizeof(ll::RecordOverride), 24u);
  SND_CHECK_EQ(sizeof(ll::RecordLiveData), 24u);
  // 1.0 values are unchanged.
  SND_CHECK_EQ(static_cast<std::uint32_t>(ll::RecordKind::BankMap), 4u);
  SND_CHECK_EQ(ll::kRecordLiveData, 5u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::Hello), 56u);
  SND_CHECK_EQ(ll::ProfilerMaskOf(0), 0x7Fu);
  SND_CHECK_EQ(ll::ProfilerMaskOf(1), 0x1FFu);
}

SND_TEST(LivelinkV11Unit, ProfilerMaskByMinor) {
  const ll::SetProfilerMask m{ll::kProfStreamList | ll::kProfBlockDetail, 30};
  SND_CHECK_EQ(ll::CheckSetProfilerMask(m), SND_ERR_INVALID_ARGUMENT);  // a 1.0 game's rule
  SND_CHECK_EQ(ll::CheckSetProfilerMask(m, 1), SND_OK);
  SND_CHECK_EQ(ll::CheckSetProfilerMask(ll::SetProfilerMask{1u << 9, 30}, 1), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckSetProfilerMask(ll::SetProfilerMask{0x7F, 5000}, 1), SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(LivelinkV11Unit, StructArgumentRules) {
  using SO = ll::StructOp;
  auto check = [](SO op, std::uint32_t target, std::vector<std::uint64_t> args) {
    const ll::StructEdit m{static_cast<std::uint32_t>(op), target, static_cast<std::uint32_t>(args.size()), 0u};
    return ll::CheckStructArgs(m, args.empty() ? nullptr : args.data());
  };
  SND_CHECK_EQ(check(SO::AddBus, 7, {9, 2}), SND_OK);
  SND_CHECK_EQ(check(SO::AddBus, 7, {9, 2, F(-3.0f)}), SND_OK);
  SND_CHECK_EQ(check(SO::AddBus, 7, {9}), SND_ERR_INVALID_ARGUMENT);             // channels missing
  SND_CHECK_EQ(check(SO::AddBus, 7, {9, 0}), SND_ERR_INVALID_ARGUMENT);          // 0 channels
  SND_CHECK_EQ(check(SO::AddBus, 7, {9, 9}), SND_ERR_INVALID_ARGUMENT);          // > 8
  SND_CHECK_EQ(check(SO::AddBus, 7, {7, 2}), SND_ERR_INVALID_ARGUMENT);          // its own parent
  SND_CHECK_EQ(check(SO::AddBus, 7, {0, 2}), SND_ERR_INVALID_ARGUMENT);          // no parent
  SND_CHECK_EQ(check(SO::AddBus, 0, {9, 2}), SND_ERR_INVALID_ARGUMENT);          // no id
  SND_CHECK_EQ(check(SO::AddBus, 7, {9, 2, 0x7FC00000u}), SND_ERR_INVALID_ARGUMENT);  // NaN
  SND_CHECK_EQ(check(SO::AddBus, 7, {9, 2, 1ull << 40}), SND_ERR_INVALID_ARGUMENT);   // not 32 bits
  SND_CHECK_EQ(check(SO::RemoveBus, 7, {}), SND_OK);
  SND_CHECK_EQ(check(SO::RemoveBus, 7, {1}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(check(SO::AddEffect, 7, {5, ~0u}), SND_OK);
  SND_CHECK_EQ(check(SO::AddEffect, 7, {5, 0, F(1.0f), F(0.5f)}), SND_OK);
  SND_CHECK_EQ(check(SO::AddEffect, 7, {0, 0}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(check(SO::AddEffect, 7, {5, 1ull << 33}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(check(SO::AddEffect, 7, {5, 0, 0x7F800000u}), SND_ERR_INVALID_ARGUMENT);  // +Inf
  std::vector<std::uint64_t> many(ll::kMaxStructEffectParams + 3u, F(0.0f));
  many[0] = 5;
  SND_CHECK_EQ(check(SO::AddEffect, 7, many), SND_ERR_INVALID_ARGUMENT);
  many.pop_back();
  SND_CHECK_EQ(check(SO::AddEffect, 7, many), SND_OK);
  SND_CHECK_EQ(check(SO::RemoveEffect, 7, {3}), SND_OK);
  SND_CHECK_EQ(check(SO::RemoveEffect, 7, {1u << 16}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(check(SO::RemoveEffect, 7, {}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(check(SO::AddSend, 7, {8, F(-6.0f)}), SND_OK);
  SND_CHECK_EQ(check(SO::AddSend, 7, {8, F(-6.0f), 1}), SND_OK);
  SND_CHECK_EQ(check(SO::AddSend, 7, {8, F(-6.0f), 2}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(check(SO::AddSend, 7, {7, F(-6.0f)}), SND_ERR_INVALID_ARGUMENT);  // to itself
  SND_CHECK_EQ(check(static_cast<SO>(5), 7, {}), SND_ERR_INVALID_ARGUMENT);
  // Costs.
  const ll::StructCost bus = ll::StructCostOf(SO::AddBus, 3);
  SND_CHECK_EQ(bus.buses, 1u);
  SND_CHECK_EQ(bus.effects, 0u);
  SND_CHECK_EQ(bus.bytes, ll::kStructEditBaseBytes + 24u);
  SND_CHECK_EQ(ll::StructCostOf(SO::AddEffect, 2).effects, 1u);
  SND_CHECK_EQ(ll::StructCostOf(SO::AddSend, 2).bytes, ll::kStructEditBaseBytes + 16u);
  SND_CHECK_EQ(ll::StructCostOf(SO::RemoveBus, 0).bytes, 0u);
  SND_CHECK_EQ(ll::StructCostOf(SO::RemoveEffect, 1).effects, 0u);
}

SND_TEST(LivelinkV11Unit, ReloadAndCellAddressRules) {
  const char path[] = "banks/Live.sndb";
  const auto* pp = reinterpret_cast<const std::uint8_t*>(path);
  SND_CHECK_EQ(ll::CheckReloadBank(ll::ReloadBank{5, 0, 0, 15}, pp), SND_OK);
  SND_CHECK_EQ(ll::CheckReloadBank(ll::ReloadBank{5, 0, ll::kReloadLoadIfAbsent, 15}, pp), SND_OK);
  SND_CHECK_EQ(ll::CheckReloadBank(ll::ReloadBank{0, 0, 0, 15}, pp), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckReloadBank(ll::ReloadBank{5, 0, 4, 15}, pp), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckReloadBank(ll::ReloadBank{5, 0, 0, 0}, pp), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckReloadBank(ll::ReloadBank{5, 0, 0, 16}, pp), SND_ERR_INVALID_ARGUMENT);  // includes the NUL
  SND_CHECK_EQ(ll::CheckReloadBank(ll::ReloadBank{5, 0, 0, ll::kMaxReloadPathBytes + 1u}, pp), SND_ERR_INVALID_ARGUMENT);
  ll::SetValue a{};
  a.chunk = bf::chunk::BUSS;
  a.column = bf::col::BUSS_volumeDb;
  a.valueType = 77;  // ignored by RevertValue / ReadCell
  a.value = ~0ull;
  SND_CHECK_EQ(ll::CheckCellAddress(a), SND_OK);
  a.chunk = bf::chunk::WAVE;
  SND_CHECK_EQ(ll::CheckCellAddress(a), SND_ERR_NOT_FOUND);
  a.chunk = bf::chunk::BUSS;
  a.column = 0x7777;
  SND_CHECK_EQ(ll::CheckCellAddress(a), SND_ERR_NOT_FOUND);
}

SND_TEST(LivelinkV11Unit, ReplayRuleOfTheNewMarkers) {
  const ll::CommandLimits k{};
  auto ok = [&](const cmd::LiveSetValue& v) {
    std::uint8_t buf[cmd::kWireSize<cmd::LiveSetValue>] = {};
    const cmd::CommandHeader h{static_cast<std::uint16_t>(cmd::CommandType::LiveSetValue),
                               static_cast<std::uint16_t>(sizeof(buf)), 0u};
    std::memcpy(buf, &h, sizeof(h));
    std::memcpy(buf + sizeof(h), &v, sizeof(v));
    return ll::CheckCommand(h, buf + sizeof(h), k);
  };
  cmd::LiveSetValue v{};
  v.field = ll::kFieldAuditionStop;
  SND_CHECK(ok(v));
  v.objectId = 3;
  SND_CHECK(!ok(v));
  v.field = ll::EncodeField(bf::chunk::BUSS, bf::col::BUSS_volumeDb);
  v.valueType = ll::kValueTypeRevertCell;
  SND_CHECK(ok(v));
  v.valueType = ll::kValueTypeReadCell;
  SND_CHECK(ok(v));
  v.valueType = 4;
  SND_CHECK(!ok(v));
  SND_CHECK(ll::IsMarkerField(ll::kFieldAuditionStop));
  std::uint32_t c = 0, col = 0;
  SND_CHECK(!ll::DecodeField(ll::kFieldAuditionStop, &c, &col));
}

SND_TEST(LivelinkV11Unit, ReaderKnowsTheNewRecords) {
  std::vector<std::uint8_t> v = Start(100, ll::RecordOrigin::MidRun);
  Put(v, ll::RecordCell{ll::kRecordCell, 9u, bf::chunk::BUSS, 3u, 1u, 0u, 42u, 7u});
  Put(v, ll::RecordOverride{ll::kRecordOverride, 0u, 11u, 0x3F800000u, 0u});
  const std::uint8_t data[13] = {1, 2, 3};
  Put(v, ll::RecordLiveData{ll::kRecordLiveData, bf::chunk::BUSS, 0x1234u, 9u, 13u});
  v.insert(v.end(), data, data + 13);
  v.insert(v.end(), 3u, 0u);  // padding to 8
  Arena(v, 100);
  ll::RecordReader rr(v.data(), v.size());
  ll::RecordItem it;
  std::uint32_t kinds[9] = {};
  while (rr.Next(&it)) {
    const std::uint32_t k = static_cast<std::uint32_t>(it.kind);
    if (k < 9u) ++kinds[k];
    if (ll::IsKind(it, ll::kRecordLiveData)) {
      SND_CHECK_EQ(it.liveData.bytes, 13u);
      SND_CHECK(it.data != nullptr && it.data[2] == 3u);
    }
    if (ll::IsKind(it, ll::kRecordCell)) SND_CHECK_EQ(it.cell.value, 42u);
    if (ll::IsKind(it, ll::kRecordBegin)) SND_CHECK_EQ(it.begin.block, 100u);
  }
  SND_CHECK(rr.Error() == ll::RecordError::None);
  for (std::uint32_t k : {1u, 2u, 5u, 6u, 7u, 8u}) SND_CHECK_MSG(kinds[k] == 1u, "kind %u", k);
  // Truncations and a bad origin.
  for (std::size_t cut = 1; cut < v.size(); ++cut) {
    ll::RecordReader t(v.data(), cut);
    while (t.Next(&it)) {
    }
  }
  std::vector<std::uint8_t> bad = Start(5);
  bad[16 + 4] = 9;  // origin
  ll::RecordReader b(bad.data(), bad.size());
  while (b.Next(&it)) {
  }
  SND_CHECK(b.Error() == ll::RecordError::BadSections);
  std::vector<std::uint8_t> big = Start(5);
  Put(big, ll::RecordLiveData{ll::kRecordLiveData, bf::chunk::BUSS, 1u, 9u, 1000u});  // data missing
  ll::RecordReader g(big.data(), big.size());
  while (g.Next(&it)) {
  }
  SND_CHECK(g.Error() == ll::RecordError::Truncated);
}

SND_TEST(LivelinkV11Unit, ReplayerReportsConditionsAndRejectsMisplacedState) {
  ll::ReplayInfo i{};
  // A 1.0 recording (no Begin): reported.
  {
    std::vector<std::uint8_t> v;
    Put(v, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, 10u});
    Arena(v, 10);
    SND_REQUIRE_EQ(Create(v, &i), SND_OK);
    SND_CHECK((i.conditions & ll::kReplayNoBegin) != 0u);
    SND_CHECK_EQ(i.origin, 0u);
    SND_CHECK_EQ(i.conditions & ll::kReplayBreaksBitExact, 0u);
  }
  // LOD level / change and stalls in the recording break 2.5.
  {
    std::vector<std::uint8_t> v = Start(10);
    Arena(v, 10);
    Put(v, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 1u, 11u});
    Put(v, ll::RecordStall{static_cast<std::uint32_t>(ll::RecordKind::Stall), 0u, 12u});
    Arena(v, 12);
    SND_REQUIRE_EQ(Create(v, &i), SND_OK);
    SND_CHECK((i.conditions & ll::kReplayLodRecorded) != 0u);
    SND_CHECK((i.conditions & ll::kReplayStallRecorded) != 0u);
    SND_CHECK_EQ(i.origin, static_cast<std::uint32_t>(ll::RecordOrigin::FromCreation));
  }
  // A bank registered during the recording (hot reload / load) is reported.
  {
    std::vector<std::uint8_t> v = Start(10);
    Arena(v, 10);
    Put(v, ll::RecordBankMap{static_cast<std::uint32_t>(ll::RecordKind::BankMap), 5u, 6u});
    Arena(v, 11);
    SND_REQUIRE_EQ(Create(v, &i), SND_OK);
    SND_CHECK_EQ(i.bankChanges, 1u);
    SND_CHECK((i.conditions & ll::kReplayBankChanges) != 0u);
  }
  // Starting state records out of place, two Begins, bad live data: corrupt.
  {
    std::vector<std::uint8_t> v;
    Put(v, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, 10u});
    Put(v, ll::RecordCell{ll::kRecordCell, 9u, bf::chunk::BUSS, 3u, 1u, 0u, 42u, 7u});  // before Begin
    SND_CHECK_EQ(Create(v), SND_ERR_CORRUPT);
    v = Start(10);
    Put(v, ll::RecordBegin{ll::kRecordBegin, 1u, 0u, 10u});
    SND_CHECK_EQ(Create(v), SND_ERR_CORRUPT);
    v = Start(10);
    Arena(v, 10);
    Put(v, ll::RecordOverride{ll::kRecordOverride, 0u, 11u, 0u, 0u});  // after an arena
    SND_CHECK_EQ(Create(v), SND_ERR_CORRUPT);
    v = Start(10);
    Put(v, ll::RecordOverride{ll::kRecordOverride, 9u, 11u, 0u, 0u});  // unknown override kind
    SND_CHECK_EQ(Create(v), SND_ERR_CORRUPT);
    v = Start(10);
    Put(v, ll::RecordLiveData{ll::kRecordLiveData, bf::chunk::BUSS, 1u, 9u, 8u});  // not a table chunk
    Put(v, std::uint64_t{0});
    Arena(v, 10);
    SND_CHECK_EQ(Create(v), SND_ERR_CORRUPT);
    v = Start(10);
    const ll::StructEdit se{0u, 7u, 1u, 0u};  // AddBus with one argument: invalid
    Put(v, ll::RecordLiveData{ll::kRecordLiveData, ll::kStructEditTag, 1u, 0u, 24u});
    Put(v, se);
    Put(v, std::uint64_t{9});
    Arena(v, 10);
    SND_CHECK_EQ(Create(v), SND_ERR_CORRUPT);
  }
  // A LiveReplaceChunk without its data (1.0 recording) is skipped; with data it is kept.
  {
    cmd::LiveReplaceChunk rc{};
    rc.bankId = 9;
    rc.fourcc = bf::chunk::BUSS;
    rc.prepared = reinterpret_cast<const void*>(std::uintptr_t{0x1000});
    std::vector<std::uint8_t> normal(cmd::kWireSize<cmd::LiveReplaceChunk>, 0u);
    const cmd::CommandHeader h{static_cast<std::uint16_t>(cmd::CommandType::LiveReplaceChunk),
                               static_cast<std::uint16_t>(normal.size()), 0u};
    std::memcpy(normal.data(), &h, sizeof(h));
    std::memcpy(normal.data() + sizeof(h), &rc, sizeof(rc));
    std::vector<std::uint8_t> v = Start(10);
    Arena(v, 10, normal);
    SND_REQUIRE_EQ(Create(v, &i), SND_OK);
    SND_CHECK_EQ(i.skippedCommands, 1u);
    SND_CHECK_EQ(i.replacedChunks, 0u);
    // With a table chunk recorded under the same key just before the arena.
    std::vector<std::uint8_t> table;
    {
      bf::TableChunkHeader th{};
      th.rowCount = 1;
      th.columnCount = 1;
      Put(table, th);
      bf::ColumnEntry ce{};
      ce.columnId = bf::col::BUSS_volumeDb;
      ce.elemBytes = 4;
      ce.offset = 64;
      Put(table, ce);
      table.resize(64, 0u);
      Put(table, static_cast<std::uint32_t>(F(-3.0f)));
    }
    v = Start(10);
    Put(v, ll::RecordLiveData{ll::kRecordLiveData, bf::chunk::BUSS, 0x1000u, 9u, static_cast<std::uint32_t>(table.size())});
    v.insert(v.end(), table.begin(), table.end());
    v.resize((v.size() + 7u) & ~std::size_t{7}, 0u);
    Arena(v, 10, normal);
    SND_REQUIRE_EQ(Create(v, &i), SND_OK);
    SND_CHECK_EQ(i.liveData, 1u);
    SND_CHECK_EQ(i.replacedChunks, 1u);
    SND_CHECK_EQ(i.skippedCommands, 0u);
    // Data of another key: not paired (skipped), the data is unused.
    v = Start(10);
    Put(v, ll::RecordLiveData{ll::kRecordLiveData, bf::chunk::BUSS, 0x2000u, 9u, static_cast<std::uint32_t>(table.size())});
    v.insert(v.end(), table.begin(), table.end());
    v.resize((v.size() + 7u) & ~std::size_t{7}, 0u);
    Arena(v, 10, normal);
    SND_REQUIRE_EQ(Create(v, &i), SND_OK);
    SND_CHECK_EQ(i.replacedChunks, 0u);
    SND_CHECK_EQ(i.skippedCommands, 1u);
  }
}

SND_TEST(LivelinkV11Unit, OutputDigestIsTheBitPattern) {
  const float a[4] = {0.0f, 1.0f, -0.5f, 0.25f};
  float b[4] = {0.0f, 1.0f, -0.5f, 0.25f};
  ll::OutputDigest x, y;
  x.Add(a, 4);
  y.Add(b, 4);
  SND_CHECK_EQ(x.value, y.value);
  SND_CHECK_EQ(x.samples, 4u);
  b[0] = -0.0f;  // equal as numbers, different bits
  ll::OutputDigest z;
  z.Add(b, 4);
  SND_CHECK(z.value != x.value);
}

// The replaying System's own LOD changes and stalls are counted and break 2.5 (the hooks
// are called directly: whether a real System changes its LOD depends on the machine).
SND_TEST(LivelinkV11Unit, ReplayLodEventIsReported) {
  std::vector<std::uint8_t> v = Start(10);
  Arena(v, 10);
  const snd_config cfg = Cfg();
  ll::ReplayDesc d;
  d.config = &cfg;
  d.allocator = &snd::base::DefaultAllocator();
  d.record = v.data();
  d.bytes = v.size();
  ll::Replayer* r = nullptr;
  SND_REQUIRE_EQ(ll::Replayer::Create(d, &r), SND_OK);
  SND_CHECK(r->BitExactExpected());
  snd::notify::EventRecord e{};
  e.type = static_cast<std::uint16_t>(snd::notify::EventType::LodChange);
  e.a = 1;
  r->OnEvent(e);
  SND_CHECK_EQ(r->Info().replayLodChanges, 1u);
  SND_CHECK((r->Info().conditions & ll::kReplayLodInReplay) != 0u);
  SND_CHECK(!r->BitExactExpected());
  r->OnStall(0);
  SND_CHECK((r->Info().conditions & ll::kReplayStallInReplay) != 0u);
  ll::Replayer::Destroy(r);
}
