// Independent tests, protocol 1.1 (组 L, indep_s7): contract constants, wire checks,
// structural edit costs, record reader and hand-made recordings for the replay
// conditions. Properties A1-A9, I7-I9, J5 of indep_s7_properties.md.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/livelink/replayer.hpp"
#include "snd/livelink/struct_edit.hpp"
#include "snd/livelink/wire.hpp"
#include "snd/test/test.hpp"

namespace {

namespace ll = snd::livelink;
using MT = ll::MessageType;

constexpr MT kNew[] = {MT::RevertValue, MT::ReadCell,    MT::QueryBanks,  MT::ReloadBank,  MT::CellValue,
                       MT::BankList,    MT::StreamList,  MT::BlockDetail, MT::ReloadStatus};
constexpr MT kOld[] = {MT::Hello,         MT::HelloReply,    MT::SyncDecision,   MT::Ping,           MT::Pong,
                       MT::Ack,           MT::Error,         MT::SetValue,       MT::ReplaceChunk,   MT::Audition,
                       MT::OverrideParam, MT::OverrideState, MT::ClearOverrides, MT::SetProfilerMask, MT::RecordControl,
                       MT::StructEdit,    MT::VoiceList,     MT::BlockTiming,    MT::MemoryUsage,    MT::StreamStall,
                       MT::LodChange,     MT::EventLog,      MT::BusMeters,      MT::RecordData};

std::uint64_t Bits(float f) {
  std::uint32_t b = 0;
  std::memcpy(&b, &f, 4);
  return b;
}

ll::StructEdit Edit(ll::StructOp op, std::uint32_t target, std::uint32_t argCount) {
  ll::StructEdit e{};
  e.op = static_cast<std::uint32_t>(op);
  e.targetId = target;
  e.argCount = argCount;
  return e;
}
snd_result Args(ll::StructOp op, std::uint32_t target, std::vector<std::uint64_t> a) {
  const ll::StructEdit e = Edit(op, target, static_cast<std::uint32_t>(a.size()));
  return ll::CheckStructArgs(e, a.empty() ? nullptr : a.data());
}

}  // namespace

// A1
SND_TEST(IndepS7Contract, VersionAndMinorOfEveryType) {
  SND_CHECK_EQ(ll::kProtocolMajor, 1u);
  SND_CHECK_EQ(ll::kProtocolMinor, 1u);
  for (MT t : kNew) SND_CHECK_MSG(ll::MinorOf(t) == 1u, "type 0x%04x", static_cast<unsigned>(t));
  for (MT t : kOld) SND_CHECK_MSG(ll::MinorOf(t) == 0u, "type 0x%04x", static_cast<unsigned>(t));
}

// A2
SND_TEST(IndepS7Contract, ProfilerMaskOfMinor) {
  SND_CHECK_EQ(ll::ProfilerMaskOf(0), 0x7Fu);
  SND_CHECK_EQ(ll::ProfilerMaskOf(1), 0x1FFu);
  SND_CHECK_EQ(ll::ProfilerMaskOf(2), 0x1FFu);
  SND_CHECK_EQ(ll::ProfilerMaskOf(0xFFFF), 0x1FFu);
  SND_CHECK_EQ(static_cast<std::uint32_t>(ll::kProfStreamList), 1u << 7);
  SND_CHECK_EQ(static_cast<std::uint32_t>(ll::kProfBlockDetail), 1u << 8);
}

// A3
SND_TEST(IndepS7Contract, FixedPayloadBytesAndIsKnown) {
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::RevertValue), 48u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::ReadCell), 48u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::QueryBanks), 0u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::ReloadBank), 16u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::CellValue), 32u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::BankList), 8u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::StreamList), 8u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::BlockDetail), 48u);
  SND_CHECK_EQ(ll::FixedPayloadBytes(MT::ReloadStatus), 24u);
  for (MT t : kNew) SND_CHECK(ll::IsKnown(static_cast<std::uint16_t>(t)));
  for (MT t : kOld) SND_CHECK(ll::IsKnown(static_cast<std::uint16_t>(t)));
  for (std::uint16_t t : {std::uint16_t{0}, std::uint16_t{0x001D}, std::uint16_t{0x010D}, std::uint16_t{0x0008},
                          std::uint16_t{0x00FF}, std::uint16_t{0xFFFF}}) {
    SND_CHECK_MSG(!ll::IsKnown(t), "type 0x%04x", static_cast<unsigned>(t));
  }
  // A9
  for (MT t : kNew) SND_CHECK(ll::MessageTypeName(static_cast<std::uint16_t>(t)) != nullptr);
}

// A4
SND_TEST(IndepS7Wire, ProfilerMaskRulesPerMinor) {
  for (std::uint32_t bit = 0; bit < 32; ++bit) {
    ll::SetProfilerMask m{1u << bit, 0};
    const bool v10 = bit < 7, v11 = bit < 9;
    SND_CHECK_MSG((ll::CheckSetProfilerMask(m) == SND_OK) == v10, "1.0 bit %u", bit);
    SND_CHECK_MSG((ll::CheckSetProfilerMask(m, 0) == SND_OK) == v10, "minor 0 bit %u", bit);
    SND_CHECK_MSG((ll::CheckSetProfilerMask(m, 1) == SND_OK) == v11, "minor 1 bit %u", bit);
  }
  ll::SetProfilerMask all{0x1FFu, 30};
  SND_CHECK_EQ(ll::CheckSetProfilerMask(all, 1), SND_OK);
  ll::SetProfilerMask none{0u, 0};
  SND_CHECK_EQ(ll::CheckSetProfilerMask(none, 1), SND_OK);
}

// A5: AddBus
SND_TEST(IndepS7Wire, StructArgsAddBus) {
  using O = ll::StructOp;
  const std::uint64_t parent = 0x1234u;
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 2}), SND_OK);
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 1}), SND_OK);
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 8}), SND_OK);
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 2, Bits(-6.0f)}), SND_OK);
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 0}), SND_ERR_INVALID_ARGUMENT);  // channels 1..8
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 9}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddBus, 0, {parent, 2}), SND_ERR_INVALID_ARGUMENT);  // id 0
  SND_CHECK_EQ(Args(O::AddBus, 7, {0, 2}), SND_ERR_INVALID_ARGUMENT);       // parent id 0
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent}), SND_ERR_INVALID_ARGUMENT);     // too few
  SND_CHECK_EQ(Args(O::AddBus, 7, {}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 2, Bits(0.0f), 0}), SND_ERR_INVALID_ARGUMENT);  // too many
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 2, Bits(std::numeric_limits<float>::quiet_NaN())}),
               SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 2, Bits(std::numeric_limits<float>::infinity())}),
               SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 2, Bits(-std::numeric_limits<float>::infinity())}),
               SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 2, Bits(1.0f) | (1ull << 32)}), SND_ERR_INVALID_ARGUMENT);  // high bits
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent | (1ull << 32), 2}), SND_ERR_INVALID_ARGUMENT);  // id > 32 bits
  SND_CHECK_EQ(Args(O::AddBus, 7, {parent, 2ull | (1ull << 40)}), SND_ERR_INVALID_ARGUMENT);
}

// A5: the other ops and unknown ops
SND_TEST(IndepS7Wire, StructArgsOtherOps) {
  using O = ll::StructOp;
  SND_CHECK_EQ(Args(O::RemoveBus, 7, {}), SND_OK);
  SND_CHECK_EQ(Args(O::RemoveBus, 7, {1}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::RemoveBus, 0, {}), SND_ERR_INVALID_ARGUMENT);
  // AddEffect: {type, position, params...} up to 32 params
  SND_CHECK_EQ(Args(O::AddEffect, 7, {0x55u, 0xFFFFFFFFu}), SND_OK);
  SND_CHECK_EQ(Args(O::AddEffect, 7, {0x55u, 0u, Bits(1.0f)}), SND_OK);
  std::vector<std::uint64_t> a(2 + ll::kMaxStructEffectParams, Bits(0.5f));
  a[0] = 0x55u;
  a[1] = 3u;
  SND_CHECK_EQ(Args(O::AddEffect, 7, a), SND_OK);
  a.push_back(Bits(0.5f));
  SND_CHECK_EQ(Args(O::AddEffect, 7, a), SND_ERR_INVALID_ARGUMENT);  // 33 params
  SND_CHECK_EQ(Args(O::AddEffect, 7, {0x55u}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddEffect, 7, {0u, 0u}), SND_ERR_INVALID_ARGUMENT);  // type id 0
  SND_CHECK_EQ(Args(O::AddEffect, 7, {0x55u, 0u, Bits(std::numeric_limits<float>::quiet_NaN())}),
               SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddEffect, 7, {0x55u, 1ull << 32}), SND_ERR_INVALID_ARGUMENT);  // position > 32 bits
  // RemoveEffect: {index < 65536}
  SND_CHECK_EQ(Args(O::RemoveEffect, 7, {0}), SND_OK);
  SND_CHECK_EQ(Args(O::RemoveEffect, 7, {65535}), SND_OK);
  SND_CHECK_EQ(Args(O::RemoveEffect, 7, {65536}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::RemoveEffect, 7, {}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::RemoveEffect, 7, {0, 0}), SND_ERR_INVALID_ARGUMENT);
  // AddSend: {dest, level dB [, 1 = pre-fader]}
  SND_CHECK_EQ(Args(O::AddSend, 7, {9, Bits(-3.0f)}), SND_OK);
  SND_CHECK_EQ(Args(O::AddSend, 7, {9, Bits(-3.0f), 1}), SND_OK);
  SND_CHECK_EQ(Args(O::AddSend, 7, {0, Bits(-3.0f)}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddSend, 7, {9}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddSend, 7, {9, Bits(std::numeric_limits<float>::infinity())}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddSend, 7, {9, Bits(-3.0f), 1, 0}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Args(O::AddSend, 7, {9, Bits(-3.0f), 2}), SND_ERR_INVALID_ARGUMENT);  // flag is 0/1 only
  // unknown op
  ll::StructEdit e = Edit(static_cast<ll::StructOp>(5), 7, 0);
  SND_CHECK_EQ(ll::CheckStructArgs(e, nullptr), SND_ERR_INVALID_ARGUMENT);
  e = Edit(static_cast<ll::StructOp>(0xFFFFFFFFu), 7, 0);
  SND_CHECK_EQ(ll::CheckStructArgs(e, nullptr), SND_ERR_INVALID_ARGUMENT);
}

// A6
SND_TEST(IndepS7Wire, StructCostOfEveryOp) {
  for (std::uint32_t n = 0; n <= 34; ++n) {
    const std::uint64_t bytes = 64u + 8u * n;
    ll::StructCost c = ll::StructCostOf(ll::StructOp::AddBus, n);
    SND_CHECK(c.buses == 1u && c.effects == 0u && c.bytes == bytes);
    c = ll::StructCostOf(ll::StructOp::AddEffect, n);
    SND_CHECK(c.buses == 0u && c.effects == 1u && c.bytes == bytes);
    c = ll::StructCostOf(ll::StructOp::AddSend, n);
    SND_CHECK(c.buses == 0u && c.effects == 0u && c.bytes == bytes);
    c = ll::StructCostOf(ll::StructOp::RemoveBus, n);
    SND_CHECK(c.buses == 0u && c.effects == 0u && c.bytes == 0u);
    c = ll::StructCostOf(ll::StructOp::RemoveEffect, n);
    SND_CHECK(c.buses == 0u && c.effects == 0u && c.bytes == 0u);
  }
  // A huge argCount does not overflow the 64-bit byte cost.
  const ll::StructCost big = ll::StructCostOf(ll::StructOp::AddBus, 0xFFFFFFFFu);
  SND_CHECK_EQ(big.bytes, 64ull + 8ull * 0xFFFFFFFFull);
}

// A7
SND_TEST(IndepS7Wire, ReloadBankPathRules) {
  std::vector<std::uint8_t> path(ll::kMaxReloadPathBytes + 1u, 'a');
  ll::ReloadBank m{};
  m.bankId = 0x10;
  m.pathBytes = 0;
  SND_CHECK_EQ(ll::CheckReloadBank(m, path.data()), SND_ERR_INVALID_ARGUMENT);
  m.pathBytes = 1;
  SND_CHECK_EQ(ll::CheckReloadBank(m, path.data()), SND_OK);
  m.pathBytes = ll::kMaxReloadPathBytes;
  SND_CHECK_EQ(ll::CheckReloadBank(m, path.data()), SND_OK);
  m.pathBytes = ll::kMaxReloadPathBytes + 1u;
  SND_CHECK_EQ(ll::CheckReloadBank(m, path.data()), SND_ERR_INVALID_ARGUMENT);
  m.pathBytes = 8;
  path[3] = 0;  // NUL inside
  SND_CHECK_EQ(ll::CheckReloadBank(m, path.data()), SND_ERR_INVALID_ARGUMENT);
  path[3] = 'a';
  path[7] = 0;  // NUL at the end: "no NUL"
  SND_CHECK_EQ(ll::CheckReloadBank(m, path.data()), SND_ERR_INVALID_ARGUMENT);
  path[7] = 'a';
  m.flags = ll::kReloadLoadIfAbsent;
  SND_CHECK_EQ(ll::CheckReloadBank(m, path.data()), SND_OK);
}

// A8: the value part of a cell address is ignored.
SND_TEST(IndepS7Wire, CellAddressIgnoresValue) {
  ll::SetValue v{};
  v.runtimeId = 0x99;
  v.chunk = snd::bank_format::chunk::BUSS;
  v.column = snd::bank_format::col::BUSS_volumeDb;
  const snd_result base = ll::CheckCellAddress(v);
  SND_CHECK_EQ(base, SND_OK);
  for (std::uint32_t vt : {0u, 1u, 2u, 3u, 4u, 0xFFFFFFFFu}) {
    v.valueType = vt;
    v.value = 0xFFFFFFFFFFFFFFFFull;
    SND_CHECK_EQ(ll::CheckCellAddress(v), base);
  }
  ll::SetValue bad = v;
  bad.chunk = snd::Fourcc('Z', 'Z', 'Z', 'Z');
  SND_CHECK_EQ(ll::CheckCellAddress(bad), SND_ERR_NOT_FOUND);
  bad = v;
  bad.column = 200;
  SND_CHECK_EQ(ll::CheckCellAddress(bad), SND_ERR_NOT_FOUND);
  bad = v;
  bad.chunk = snd::bank_format::chunk::WAVE;  // a blob, not a table
  SND_CHECK_EQ(ll::CheckCellAddress(bad), SND_ERR_NOT_FOUND);
}

// --- recordings made by hand -----------------------------------------------------------------
namespace {

template <class T>
void Put(std::vector<std::uint8_t>& v, const T& rec) {
  const std::size_t at = v.size();
  v.resize(at + ((sizeof(T) + 7u) & ~std::size_t{7}));
  std::memcpy(v.data() + at, &rec, sizeof(T));
}
void PutEmptyArena(std::vector<std::uint8_t>& v, std::uint64_t block) {
  ll::RecordArena a{static_cast<std::uint32_t>(ll::RecordKind::Arena), 0u, block, 16u, 0u};
  Put(v, a);
  ll::ArenaSections s{0, 0, 0, 0};
  Put(v, s);
}
void PutLod(std::vector<std::uint8_t>& v, std::uint32_t level, std::uint64_t block) {
  Put(v, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), level, block});
}
void PutBegin(std::vector<std::uint8_t>& v, ll::RecordOrigin o, std::uint64_t block, std::uint16_t lost = 0) {
  Put(v, ll::RecordBegin{ll::kRecordBegin, static_cast<std::uint16_t>(o), lost, block});
}

struct Rp {
  ll::Replayer* r = nullptr;
  int result = 0;
  snd_config cfg{};
  explicit Rp(const std::vector<std::uint8_t>& bytes) {
    snd_config_init(&cfg);
    ll::ReplayDesc d;
    d.config = &cfg;
    d.allocator = &snd::base::DefaultAllocator();
    d.record = bytes.data();
    d.bytes = bytes.size();
    result = ll::Replayer::Create(d, &r);
  }
  ~Rp() { ll::Replayer::Destroy(r); }
};

}  // namespace

// I9 / I10: a 1.0 recording (no Begin) of three empty arenas.
SND_TEST(IndepS7Replay, OldRecordingWithoutBegin) {
  std::vector<std::uint8_t> v;
  PutLod(v, 0, 100);
  PutEmptyArena(v, 100);
  PutEmptyArena(v, 103);
  Rp rp(v);
  SND_REQUIRE_EQ(rp.result, SND_OK);
  const ll::ReplayInfo i = rp.r->Info();
  SND_CHECK_EQ(i.origin, 0u);
  SND_CHECK((i.conditions & ll::kReplayNoBegin) != 0u);
  SND_CHECK_EQ(i.leadBlocks, 0u);
  SND_CHECK_EQ(rp.r->Blocks(), 4u);  // 100..103
  SND_CHECK_EQ(i.firstBlock, 100u);
  SND_CHECK_EQ(i.lastBlock, 103u);
}

// I7: a recording that starts at LOD level 2 / with a LOD change / with a stall.
SND_TEST(IndepS7Replay, LodAndStallRecordsBreakBitExactness) {
  {
    std::vector<std::uint8_t> v;
    PutLod(v, 0, 10);
    PutBegin(v, ll::RecordOrigin::FromCreation, 10);
    PutEmptyArena(v, 10);
    Rp rp(v);
    SND_REQUIRE_EQ(rp.result, SND_OK);
    SND_CHECK_EQ(rp.r->Info().origin, static_cast<std::uint32_t>(ll::RecordOrigin::FromCreation));
    SND_CHECK_EQ(rp.r->Info().conditions & ll::kReplayBreaksBitExact, 0u);
    SND_CHECK(rp.r->BitExactExpected());
  }
  {
    std::vector<std::uint8_t> v;
    PutLod(v, 2, 10);  // starts at level 2
    PutBegin(v, ll::RecordOrigin::FromCreation, 10);
    PutEmptyArena(v, 10);
    Rp rp(v);
    SND_REQUIRE_EQ(rp.result, SND_OK);
    SND_CHECK((rp.r->Info().conditions & ll::kReplayLodRecorded) != 0u);
    SND_CHECK(!rp.r->BitExactExpected());
  }
  {
    std::vector<std::uint8_t> v;
    PutLod(v, 0, 10);
    PutBegin(v, ll::RecordOrigin::FromCreation, 10);
    PutEmptyArena(v, 10);
    PutLod(v, 1, 11);  // a change during the recording
    PutEmptyArena(v, 12);
    Rp rp(v);
    SND_REQUIRE_EQ(rp.result, SND_OK);
    SND_CHECK((rp.r->Info().conditions & ll::kReplayLodRecorded) != 0u);
    SND_CHECK(!rp.r->BitExactExpected());
  }
  {
    std::vector<std::uint8_t> v;
    PutLod(v, 0, 10);
    PutBegin(v, ll::RecordOrigin::FromCreation, 10);
    PutEmptyArena(v, 10);
    Put(v, ll::RecordStall{static_cast<std::uint32_t>(ll::RecordKind::Stall), 0u, 11u});
    PutEmptyArena(v, 12);
    Rp rp(v);
    SND_REQUIRE_EQ(rp.result, SND_OK);
    SND_CHECK_EQ(rp.r->Info().stallRecords, 1u);
    SND_CHECK((rp.r->Info().conditions & ll::kReplayStallRecorded) != 0u);
    SND_CHECK(!rp.r->BitExactExpected());
  }
}

// I8: a BankMap after the first arena = a bank registered during the recording.
SND_TEST(IndepS7Replay, BankMapDuringRecordingIsABankChange) {
  std::vector<std::uint8_t> v;
  PutLod(v, 0, 10);
  Put(v, ll::RecordBankMap{static_cast<std::uint32_t>(ll::RecordKind::BankMap), 0x77u, 0x1111u});
  PutBegin(v, ll::RecordOrigin::MidRun, 10);
  PutEmptyArena(v, 10);
  Put(v, ll::RecordBankMap{static_cast<std::uint32_t>(ll::RecordKind::BankMap), 0x77u, 0x2222u});
  PutEmptyArena(v, 11);
  Rp rp(v);
  SND_REQUIRE_EQ(rp.result, SND_OK);
  const ll::ReplayInfo i = rp.r->Info();
  SND_CHECK_EQ(i.bankChanges, 1u);
  SND_CHECK((i.conditions & ll::kReplayBankChanges) != 0u);
  SND_CHECK((i.conditions & ll::kReplayMidRun) != 0u);
  SND_CHECK_EQ(i.origin, static_cast<std::uint32_t>(ll::RecordOrigin::MidRun));
  SND_CHECK(!rp.r->BitExactExpected());
}

// lostState saturates in the record; the replay reports it.
SND_TEST(IndepS7Replay, LostStateIsReported) {
  std::vector<std::uint8_t> v;
  PutLod(v, 0, 10);
  PutBegin(v, ll::RecordOrigin::MidRun, 10, 65535);
  PutEmptyArena(v, 10);
  Rp rp(v);
  SND_REQUIRE_EQ(rp.result, SND_OK);
  SND_CHECK_EQ(rp.r->Info().lostState, 65535u);
  SND_CHECK((rp.r->Info().conditions & ll::kReplayStateLost) != 0u);
  // D-175: lost starting state does not make two replays differ; it means the replay does not
  // reproduce the recorded run.
  SND_CHECK(rp.r->BitExactExpected());
  SND_CHECK(!rp.r->ReproducesRecordedRun());
}

// J5: malformed 1.1 records are rejected by the reader and by Replayer::Create.
SND_TEST(IndepS7Replay, MalformedNewRecordsAreRejected) {
  auto rejected = [](const std::vector<std::uint8_t>& v) {
    ll::RecordReader rr(v.data(), v.size());
    ll::RecordItem it;
    while (rr.Next(&it)) {
    }
    const bool readerError = rr.Error() != ll::RecordError::None;
    Rp rp(v);
    return readerError && rp.result == SND_ERR_CORRUPT;
  };
  std::vector<std::uint8_t> head;
  PutLod(head, 0, 10);
  {  // Begin cut in the middle
    std::vector<std::uint8_t> v = head;
    PutBegin(v, ll::RecordOrigin::FromCreation, 10);
    v.resize(v.size() - 4);
    SND_CHECK(rejected(v));
  }
  {  // LiveData announcing more bytes than there are
    std::vector<std::uint8_t> v = head;
    PutBegin(v, ll::RecordOrigin::MidRun, 10);
    Put(v, ll::RecordLiveData{ll::kRecordLiveData, snd::bank_format::chunk::BUSS, 1u, 1u, 0xFFFFFFF0u});
    v.resize(v.size() + 64);
    SND_CHECK(rejected(v));
  }
  {  // LiveData whose bytes are not followed by an Arena
    std::vector<std::uint8_t> v = head;
    PutBegin(v, ll::RecordOrigin::MidRun, 10);
    Put(v, ll::RecordLiveData{ll::kRecordLiveData, snd::bank_format::chunk::BUSS, 1u, 1u, 8u});
    v.resize(v.size() + 8);
    ll::RecordReader rr(v.data(), v.size());
    ll::RecordItem it;
    while (rr.Next(&it)) {
    }
    SND_CHECK(rr.Error() == ll::RecordError::None);  // well-formed records; the replay simply has no use for it
  }
  {  // Cell cut short
    std::vector<std::uint8_t> v = head;
    PutBegin(v, ll::RecordOrigin::MidRun, 10);
    ll::RecordCell c{};
    c.kind = ll::kRecordCell;
    Put(v, c);
    v.resize(v.size() - 8);
    SND_CHECK(rejected(v));
  }
  {  // Override cut short
    std::vector<std::uint8_t> v = head;
    PutBegin(v, ll::RecordOrigin::MidRun, 10);
    ll::RecordOverride o{};
    o.kind = ll::kRecordOverride;
    Put(v, o);
    v.resize(v.size() - 8);
    SND_CHECK(rejected(v));
  }
  {  // unknown kinds right after the 1.1 ones
    for (std::uint32_t k : {9u, 0u, 0xFFFF0000u}) {
      std::vector<std::uint8_t> v = head;
      Put(v, ll::RecordLod{k, 0u, 0u});
      SND_CHECK_MSG(rejected(v), "kind %u", k);
    }
  }
}

// Spec gap probe: an origin value out of range (not 1 / 2). Recorded, not decided.
// D-175: a Begin origin out of range is CORRUPT.
SND_TEST(IndepS7Replay, BeginWithUnknownOriginIsCorrupt) {
  for (std::uint16_t origin : {std::uint16_t{0}, std::uint16_t{3}, std::uint16_t{7}, std::uint16_t{0xFFFF}}) {
    std::vector<std::uint8_t> v;
    PutLod(v, 0, 10);
    Put(v, ll::RecordBegin{ll::kRecordBegin, origin, 0u, 10u});
    PutEmptyArena(v, 10);
    Rp rp(v);
    SND_CHECK_MSG(rp.result == SND_ERR_CORRUPT, "origin %u -> %d", origin, rp.result);
  }
}
