// Independent tests, protocol 1.1 (组 L, indep_s7): written after reading the
// implementation, aimed at its boundaries (indep_s7_properties.md step 3):
//   - the structural-edit log is full (maxStructLog): reserve of a live-added bus that a
//     later RemoveBus takes away; a live-added bus removed while it still has live effects;
//   - chunk slots shared by ReplaceChunk and StructEdit (chunkSlots = 1);
//   - argCount at the 64 limit with a full payload;
//   - a cell held by two banks (cells, kCellsDiffer, revert of both);
//   - the override layer's capacity (maxEdits): refused whole, nothing written;
//   - auditions that never start (unknown event): no place leaks;
//   - a ReloadBank whose file holds another bank (spec gap probe).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "indep_s7_common.hpp"

namespace {

using namespace s7;
using indep::BankOpts;
using indep::IndepBank;
using indep::Tool;
using lltest::Fixture;
using lltest::Hooks;
constexpr snd::Id kLive = lltest::kBankId;
const std::uint64_t kMasterArgs[2] = {HashId("Master"), 2};

bool WaitFramesIn(Fixture& f, std::uint64_t base, std::uint64_t n, std::uint32_t ms = 5000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (f.server->Stats().framesIn < base + n) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace

// §8.4 "ライブで足したバスを外すと、その分を返す": also when the struct log was full when the
// bus was added (maxStructLog). The implementation keeps the reserve of an unlogged edit
// taken for good, so removing that bus never gives its unit back.
SND_TEST(IndepS7Impl, Defect_RemoveOfLiveBusAddedWithFullStructLogKeepsReserve) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  rd.maxStructLog = 2;
  Fixture f(Hooks::Live, ReserveConfig(3, 3, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B2"), kMasterArgs, 2)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B3"), kMasterArgs, 2)), SND_OK);  // log full
  SND_CHECK_EQ(f.rt->Drops().structLog, 1u);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 3u);
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::RemoveBus, HashId("B3"), nullptr, 0)), SND_OK);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 2u);  // B3 was added live and is gone
  // so a new bus fits again
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B4"), kMasterArgs, 2)), SND_OK);
}

// A live-added bus with a live-added effect on it is removed; the disconnect revert then
// undoes what is left. Every unit comes back exactly once.
SND_TEST(IndepS7Impl, RemoveLiveBusWithLiveEffectThenRevert) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(4, 4, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  ll::ClientDesc d = lltest::MemoryClient(f.link);
  d.autoReconnect = false;
  Tool t(f, d);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);
  const std::uint64_t fx[2] = {HashId("snd.fx.gain"), 0xFFFFFFFFu};
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddEffect, HashId("B1"), fx, 2)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::RemoveBus, HashId("B1"), nullptr, 0)), SND_OK);
  ll::DevReserveUsage u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 0u);
  std::printf("[indep_s7] after removing a live bus that has a live effect: effects=%u bytes=%llu\n", u.effects,
              static_cast<unsigned long long>(u.bytes));
  const std::uint32_t before = ed.Count();
  t.c->Disconnect();
  SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  f.Steps(10);
  u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 0u);
  SND_CHECK_EQ(u.effects, 0u);
  SND_CHECK_EQ(u.bytes, 0u);
  for (std::uint32_t i = before; i < ed.Count(); ++i) {
    const FakeEditor::Entry e = ed.At(i);
    std::printf("[indep_s7]   revert -> %s op %u target %08x\n", e.undo != 0u ? "Undo" : "Apply", e.op, e.targetId);
  }
}

// chunkSlots = 1: a second ReplaceChunk / a StructEdit while the slot is queued is
// LIMIT_REACHED, and the StructEdit's reserve comes back.
SND_TEST(IndepS7Impl, SharedChunkSlotIsLimitReached) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  rd.chunkSlots = 1;
  Fixture f(Hooks::Live, ReserveConfig(4, 4, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  f.Steps(3);
  const std::uint64_t base = f.server->Stats().framesIn;
  const std::vector<std::uint8_t> ch = BussChunk(-9.0f);
  const std::uint32_t a = t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size());
  const std::uint32_t b = t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size());
  const std::uint32_t s = t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2);
  SND_REQUIRE(WaitFramesIn(f, base, 3));
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 0u);
  SND_CHECK_EQ(t.Ack(b), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(t.Ack(s), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(t.Ack(a), SND_OK);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 0u);
  SND_CHECK_EQ(ed.Count(), 0u);
  // the slot is free again
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size())), SND_OK);
}

// argCount 64 (the implementation's cap) and 65 with complete payloads.
SND_TEST(IndepS7Impl, StructArgCountAtTheCap) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(4, 4, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  for (std::uint32_t n : {34u, 35u, 64u, 65u, 1000u}) {
    std::vector<std::uint8_t> b(16 + std::size_t{n} * 8u);
    const ll::StructEdit se{static_cast<std::uint32_t>(ll::StructOp::AddEffect), HashId("Sfx"), n, 0};
    std::memcpy(b.data(), &se, 16);
    std::vector<std::uint64_t> a(n, F32Bits(0.25f));
    a[0] = HashId("snd.fx.gain");
    a[1] = 0xFFFFFFFFu;
    std::memcpy(b.data() + 16, a.data(), std::size_t{n} * 8u);
    // raw: the client itself refuses argCount > 64 (Client::StructEdit returns 0)
    const std::int32_t r = t.Ack(t.c->SendRaw(static_cast<std::uint16_t>(ll::MessageType::StructEdit),
                                              ll::kFrameWantsAck, b.data(), static_cast<std::uint32_t>(b.size())));
    SND_CHECK_MSG(r == (n == 34u ? SND_OK : SND_ERR_INVALID_ARGUMENT), "argCount %u -> %d", n, r);
  }
  {
    std::vector<std::uint64_t> a(65, F32Bits(0.25f));
    SND_CHECK_EQ(t.c->StructEdit(ll::StructOp::AddEffect, HashId("Sfx"), a.data(), 65), 0u);
  }
  SND_CHECK_EQ(f.rt->ReserveUsage().effects, 1u);
  SND_CHECK_EQ(f.rt->ReserveUsage().bytes, 64u + 8u * 34u);
}

// A cell held by two banks: cells = 2, a replacement of one bank makes them differ, the
// item revert restores both.
SND_TEST(IndepS7Impl, CellHeldByTwoBanks) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  BankOpts two;
  two.name = "Two";
  f.Load(IndepBank(two), "b.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  ll::CellValue cv{};
  SND_REQUIRE(WaitCell(t, t.c->ReadCell(SfxVolumeCell()), &cv));
  SND_CHECK_EQ(cv.cells, 2u);
  SND_CHECK_EQ(cv.flags, 0u);
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
  SND_REQUIRE(WaitCell(t, t.c->ReadCell(SfxVolumeCell()), &cv));
  SND_CHECK_EQ(cv.cells, 2u);
  SND_CHECK_EQ(cv.current, F32Bits(-6.0f));
  SND_CHECK_EQ(cv.flags & ll::kCellsDiffer, 0u);
  const std::vector<std::uint8_t> ch = BussChunk(-9.0f);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(HashId("Two"), bf::chunk::BUSS, ch.data(), ch.size())), SND_OK);
  SND_REQUIRE(WaitCell(t, t.c->ReadCell(SfxVolumeCell()), &cv));
  SND_CHECK((cv.flags & ll::kCellsDiffer) != 0u);
  SND_CHECK((cv.flags & ll::kCellOverridden) != 0u);
  SND_CHECK(cv.current == F32Bits(-6.0f) || cv.current == F32Bits(-9.0f));
  SND_CHECK_EQ(cv.original, F32Bits(0.0f));
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(SfxVolumeCell())), SND_OK);
  SND_REQUIRE(WaitCell(t, t.c->ReadCell(SfxVolumeCell()), &cv));
  SND_CHECK_EQ(cv.current, F32Bits(0.0f));
  SND_CHECK_EQ(cv.flags, 0u);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 0u);
}

// maxEdits: an edit that needs more remembered cells than are left is refused whole.
SND_TEST(IndepS7Impl, OverrideLayerCapacityRefusesWhole) {
  ll::RuntimeDesc rd;
  rd.maxEdits = 1;
  Fixture f(Hooks::Live, lltest::Config(), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  BankOpts two;
  two.name = "Two";
  f.Load(IndepBank(two), "b.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  // two banks hold Sfx: two cells to remember, one place
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_ERR_LIMIT_REACHED);
  ll::CellValue cv{};
  SND_REQUIRE(WaitCell(t, t.c->ReadCell(SfxVolumeCell()), &cv));
  SND_CHECK_EQ(cv.current, F32Bits(0.0f));
  SND_CHECK_EQ(cv.flags, 0u);
  // a replacement of one bank changing one cell fits
  const std::vector<std::uint8_t> ch = BussChunk(-9.0f);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size())), SND_OK);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 1u);
  // the same replacement again needs nothing new
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size())), SND_OK);
  // the other bank: refused whole
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(HashId("Two"), bf::chunk::BUSS, ch.data(), ch.size())), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 1u);
  // a revert frees the place
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(SfxVolumeCell())), SND_OK);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 0u);
}

// Auditions that never start do not keep their place.
SND_TEST(IndepS7Impl, AuditionsThatNeverStartGiveTheirPlaceBack) {
  ll::RuntimeDesc rd;
  rd.maxAuditions = 2;
  Fixture f(Hooks::Live, lltest::Config(), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  for (int i = 0; i < 6; ++i) {
    const std::int32_t r = t.Ack(t.c->Audition(HashId("NoSuchEvent"), nullptr, false, true));
    f.Steps(3);
    SND_CHECK_MSG(r == SND_OK || r == SND_ERR_NOT_FOUND || r == SND_ERR_LIMIT_REACHED, "audition %d -> %d", i, r);
  }
  SND_CHECK(f.StepUntil([&] { return f.rt->ActiveAuditions() == 0u; }, 2000));
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
}

// Spec gap probe (§8.5): the file at `path` holds another bank than `bankId`.
SND_TEST(IndepS7Impl, ReloadWithAFileOfAnotherBankIsRecordedBehaviour) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  BankOpts two;
  two.name = "Two";
  f.fs.AddFile("two.sndb", IndepBank(two));
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  const std::int32_t r = t.Ack(t.c->ReloadBank(kLive, 0, "two.sndb"));
  const std::uint32_t q = t.c->QueryBanks(true);
  SND_CHECK_EQ(t.Ack(q), SND_OK);
  ll::BankEntry b[8];
  const std::uint32_t n = t.c->CopyBanks(b, 8);
  std::printf("[indep_s7] ReloadBank(Live, \"two.sndb\") -> %d; banks now:", r);
  for (std::uint32_t i = 0; i < n; ++i) std::printf(" %08x", b[i].bankId);
  std::printf("\n");
  SND_CHECK(r != 1 && r != 2);
}
