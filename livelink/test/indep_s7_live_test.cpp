// Independent tests, protocol 1.1 (组 L, indep_s7), end to end with a real core over
// the memory link: minor negotiation (both directions, hand-made 1.0 peers), the
// handshake edge rules and the deferred SyncDecision, item revert and read-back,
// ReplaceChunk acceptance, audition stop-all, structural edits within the dev reserve
// (fake editor), hot reload, update numbers, budget definition, sessions and
// back-pressure for the new messages, RT safety of the audio-thread paths.
// Properties B, C, D, E, F, G, H, K, L of indep_s7_properties.md.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "indep_s7_common.hpp"
#include "snd/test/rt_guard.hpp"

namespace {

using namespace s7;
using indep::BankOpts;
using indep::IndepBank;
using indep::Tool;
using lltest::Fixture;
using lltest::Hooks;
constexpr snd::Id kLive = lltest::kBankId;

std::uint64_t Db(float db) { return F32Bits(db); }

// Waits until the server received `n` more frames than `base` (no rendering).
bool WaitFramesIn(Fixture& f, std::uint64_t base, std::uint64_t n, std::uint32_t ms = 5000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (f.server->Stats().framesIn < base + n) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

ll::ClientDesc NoReconnect(Fixture& f, std::uint64_t hash = lltest::kProjectHash) {
  ll::ClientDesc d = lltest::MemoryClient(f.link, hash);
  d.autoReconnect = false;
  return d;
}

const std::uint64_t kMasterArgs[2] = {HashId("Master"), 2};

}  // namespace

// ============================== B: minor negotiation and handshake ==========================

// B1, B2, B3: a 1.0 tool (raw frames) on a 1.1 game.
SND_TEST(IndepS7Handshake, OldToolOnNewGame) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.timingHz = 100;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  lltest::RawPeer p;
  SND_REQUIRE(f.StepUntil([&] { return p.Connect(f.link.connector(), 10); }));
  SND_REQUIRE(p.Hello());  // minor 0
  const lltest::RawPeer::Frame* r = p.WaitFor(ll::MessageType::HelloReply);
  SND_REQUIRE(r != nullptr);
  const auto hr = ll::ReadFixed<ll::HelloReply>(r->payload.data(), static_cast<std::uint32_t>(r->payload.size()));
  SND_CHECK_EQ(hr.status, static_cast<std::uint32_t>(ll::HelloStatus::Accepted));
  SND_CHECK_EQ(hr.protocolMinor, 1u);
  // every 1.0 profiler stream; nothing of 1.1 may arrive
  ll::SetProfilerMask m{ll::ProfilerMaskOf(0), 60};
  SND_REQUIRE(p.Send(ll::MessageType::SetProfilerMask, &m, sizeof(m), ll::kFrameWantsAck, 2));
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  for (int i = 0; i < 200; ++i) {
    f.Step();
    p.Pump(0);
  }
  p.Pump(20);
  std::uint32_t newTypes = 0, acks = 0, timings = 0;
  for (const auto& fr : p.frames) {
    if (fr.header.type >= 0x0108u && fr.header.type <= 0x010Cu) ++newTypes;
    if (fr.header.type == static_cast<std::uint16_t>(ll::MessageType::Ack)) ++acks;
    if (fr.header.type == static_cast<std::uint16_t>(ll::MessageType::BlockTiming)) ++timings;
  }
  SND_CHECK_EQ(newTypes, 0u);
  SND_CHECK_EQ(acks, 1u);
  SND_CHECK(timings > 0u);
  // B3: a 1.0 tool that sets bit 7 anyway is answered by the game's (1.1) rule.
  p.frames.clear();
  ll::SetProfilerMask m11{ll::ProfilerMaskOf(1), 60};
  SND_REQUIRE(p.Send(ll::MessageType::SetProfilerMask, &m11, sizeof(m11), ll::kFrameWantsAck, 3));
  const lltest::RawPeer::Frame* a = nullptr;
  SND_CHECK(f.StepUntil([&] {
    p.Pump(1);
    for (const auto& fr : p.frames)
      if (fr.header.type == static_cast<std::uint16_t>(ll::MessageType::Ack) && fr.header.seq == 3u) return true;
    return false;
  }));
  a = p.WaitFor(ll::MessageType::Ack, 10);
  SND_REQUIRE(a != nullptr);
  SND_CHECK_EQ(ll::ReadFixed<ll::Ack>(a->payload.data(), 8).result, SND_OK);
  SND_CHECK_EQ(f.server->Stats().protocolErrors, 0u);
}

// B4: a 1.1 client on a 1.0 game (the game end is driven by hand).
SND_TEST(IndepS7Handshake, NewClientOnOldGame) {
  ll::MemoryLink link;
  SND_REQUIRE(link.Init(snd::base::DefaultAllocator(), 1u << 20));
  ll::ClientDesc d;
  d.connector = &link.connector();
  d.projectBuildHash = 77;
  d.autoReconnect = false;
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(d, &c), SND_OK);
  SND_REQUIRE_EQ(c->Start(), SND_OK);
  RawGame g;
  SND_REQUIRE(g.Accept(link.acceptor()));
  const RawGame::Frame* h = g.WaitFor(ll::MessageType::Hello);
  SND_REQUIRE(h != nullptr);
  const auto hello = ll::ReadFixed<ll::Hello>(h->payload.data(), static_cast<std::uint32_t>(h->payload.size()));
  SND_CHECK_EQ(hello.protocolMajor, 1u);
  SND_CHECK_EQ(hello.protocolMinor, 1u);  // the client names its own minor
  ll::HelloReply hr{};
  hr.protocolMajor = 1;
  hr.protocolMinor = 0;
  hr.status = static_cast<std::uint32_t>(ll::HelloStatus::Accepted);
  hr.runtimeBuildHash = 77;
  hr.sampleRate = 48000;
  hr.blockSize = 256;
  SND_REQUIRE(g.Send(ll::MessageType::HelloReply, &hr, sizeof(hr), 0, h->header.seq));
  SND_REQUIRE(c->WaitReady(3000));
  SND_CHECK_EQ(c->PeerMinor(), 0u);
  SND_CHECK(c->Supports(ll::MessageType::SetValue));
  for (ll::MessageType t : {ll::MessageType::RevertValue, ll::MessageType::ReadCell, ll::MessageType::QueryBanks,
                            ll::MessageType::ReloadBank}) {
    SND_CHECK_MSG(!c->Supports(t), "type 0x%04x", static_cast<unsigned>(t));
  }
  const ll::SetValue cell = SfxVolumeCell();
  SND_CHECK_EQ(c->RevertValue(cell, true), 0u);
  SND_CHECK_EQ(c->ReadCell(cell, true), 0u);
  SND_CHECK_EQ(c->QueryBanks(true), 0u);
  SND_CHECK_EQ(c->ReloadBank(1, 0, "x.sndb", false, true), 0u);
  g.PumpFor(50);
  g.frames.clear();
  const std::uint32_t ms = c->SetProfilerMask(ll::ProfilerMaskOf(1), 30, true);
  SND_CHECK(ms != 0u);
  const RawGame::Frame* pm = g.WaitFor(ll::MessageType::SetProfilerMask);
  SND_REQUIRE(pm != nullptr);
  const auto mask = ll::ReadFixed<ll::SetProfilerMask>(pm->payload.data(), 8);
  SND_CHECK_EQ(mask.mask, ll::ProfilerMaskOf(0));
  g.PumpFor(50);
  std::uint32_t newTypes = 0;
  for (const auto& fr : g.frames)
    if (fr.header.type >= 0x0019u && fr.header.type <= 0x001Cu) ++newTypes;
  SND_CHECK_EQ(newTypes, 0u);
  // 1.0 messages still work.
  const std::uint32_t sv = c->SetValue(lltest::BusVolume("Sfx", -6.0f), true);
  const RawGame::Frame* svf = g.WaitFor(ll::MessageType::SetValue);
  SND_REQUIRE(svf != nullptr);
  ll::Ack ack{sv, SND_OK};
  SND_REQUIRE(g.Send(ll::MessageType::Ack, &ack, sizeof(ack), 0, sv));
  std::int32_t res = 99;
  bool got = false;
  for (int i = 0; i < 300 && !got; ++i) {
    got = c->TakeAck(sv, &res);
    g.PumpFor(5);
  }
  SND_CHECK(got);
  SND_CHECK_EQ(res, SND_OK);
  c->Stop();
  ll::Client::Destroy(c);
  g.Close();
}

// B5
SND_TEST(IndepS7Handshake, MajorMismatchIsRejected) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  for (std::uint16_t major : {std::uint16_t{0}, std::uint16_t{2}, std::uint16_t{0xFFFF}}) {
    lltest::RawPeer p;
    SND_REQUIRE(f.StepUntil([&] { return p.Connect(f.link.connector(), 10); }));
    const std::uint32_t before = f.server->Stats().handshakesRejected;
    SND_REQUIRE(p.Hello(lltest::kProjectHash, major));
    const lltest::RawPeer::Frame* r = p.WaitFor(ll::MessageType::HelloReply);
    SND_REQUIRE(r != nullptr);
    const auto hr = ll::ReadFixed<ll::HelloReply>(r->payload.data(), static_cast<std::uint32_t>(r->payload.size()));
    SND_CHECK_EQ(hr.status, static_cast<std::uint32_t>(ll::HelloStatus::RejectedProtocol));
    SND_CHECK(f.StepUntil([&] { return !p.Pump(1) || p.closed; }));
    SND_CHECK(f.StepUntil([&] { return f.server->Stats().handshakesRejected == before + 1u; }));
    p.Close();
  }
}

// B6: no bank loaded.
SND_TEST(IndepS7Handshake, NoBankEdgeRule) {
  Fixture f;
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  {
    Tool t(f, NoReconnect(f, 0));
    SND_REQUIRE(t.Ready());
    SND_CHECK_EQ(t.c->LastHelloReply().status, static_cast<std::uint32_t>(ll::HelloStatus::Accepted));
    SND_CHECK_EQ(t.c->LastHelloReply().runtimeBuildHash, 0u);
  }
  SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  {
    Tool t(f, NoReconnect(f, 5));
    SND_REQUIRE(t.Ready());  // SendDiffs
    SND_CHECK_EQ(t.c->LastHelloReply().status, static_cast<std::uint32_t>(ll::HelloStatus::BuildMismatch));
    SND_CHECK_EQ(t.c->LastHelloReply().runtimeBuildHash, 0u);
  }
}

// B7: two banks of different builds.
SND_TEST(IndepS7Handshake, BanksOfDifferentBuildsAlwaysMismatch) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  BankOpts two;
  two.name = "Two";
  two.hash = 0xABCDu;
  f.Load(IndepBank(two), "b.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  for (std::uint64_t h : {lltest::kProjectHash, std::uint64_t{0xABCDu}, std::uint64_t{0}}) {
    Tool t(f, NoReconnect(f, h));
    SND_REQUIRE(t.Ready());
    SND_CHECK_EQ(t.c->LastHelloReply().status, static_cast<std::uint32_t>(ll::HelloStatus::BuildMismatch));
    SND_CHECK_EQ(t.c->LastHelloReply().runtimeBuildHash, lltest::kProjectHash);  // the first registered bank
    t.c->Stop();
    SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  }
}

// B8: deferred decision, QueryBanks before it, both choices.
SND_TEST(IndepS7Handshake, DeferredSyncDecisionWithBankList) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  BankOpts rel;
  rel.name = "Rel";
  rel.dev = false;  // release build bank: not writable, still listed and counted
  f.Load(IndepBank(rel), "r.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  {
    ll::ClientDesc d = NoReconnect(f, 0x999);
    d.deferSyncDecision = true;
    Tool t(f, d);
    SND_REQUIRE(t.c != nullptr);
    SND_CHECK(!t.c->DecideSync(ll::SyncMode::SendDiffs) || true);  // may or may not wait yet: no assertion
    SND_REQUIRE(f.StepUntil([&] { return t.c->AwaitingSync(); }));
    SND_CHECK(t.c->State() == ll::ClientState::Handshake);
    SND_CHECK(!f.server->Connected());
    const std::uint32_t q = t.c->QueryBanks(true);
    SND_REQUIRE(q != 0u);
    SND_CHECK_EQ(t.Ack(q), SND_OK);
    ll::BankEntry banks[8];
    std::uint32_t replySeq = 0;
    const std::uint32_t n = t.c->CopyBanks(banks, 8, &replySeq);
    SND_CHECK_EQ(replySeq, q);
    SND_REQUIRE_EQ(n, 2u);
    std::uint32_t seen = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      if (banks[i].bankId == HashId("Live")) {
        ++seen;
        SND_CHECK_EQ(banks[i].writable, 1u);
        SND_CHECK_EQ(banks[i].projectBuildHash, lltest::kProjectHash);
        SND_CHECK(banks[i].contentHash != 0u);
      }
      if (banks[i].bankId == HashId("Rel")) {
        ++seen;
        SND_CHECK_EQ(banks[i].writable, 0u);
      }
    }
    SND_CHECK_EQ(seen, 2u);
    SND_CHECK(t.c->DecideSync(ll::SyncMode::SendDiffs));
    SND_CHECK(!t.c->DecideSync(ll::SyncMode::SendDiffs));  // nothing waits any more
    SND_REQUIRE(t.Ready());
    SND_CHECK_EQ(t.Ack(t.c->OverrideParam(HashId("Knob"), 0.5f, true, true)), SND_OK);
    // D-171: the release bank "Rel" holds a copy of "Sfx" that cannot be written: the whole
    // SetValue is UNSUPPORTED and no cell is written
    SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_ERR_UNSUPPORTED);
    ll::CellValue cv{};
    SND_REQUIRE(WaitCell(t, t.c->ReadCell(SfxVolumeCell()), &cv));
    SND_CHECK_EQ(cv.current, F32Bits(0.0f));
    SND_CHECK_EQ(cv.flags & ll::kCellOverridden, 0u);
    t.c->Stop();
    SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  }
  {
    ll::ClientDesc d = NoReconnect(f, 0x999);
    d.deferSyncDecision = true;
    Tool t(f, d);
    SND_REQUIRE(f.StepUntil([&] { return t.c->AwaitingSync(); }));
    const std::uint32_t rejected = f.server->Stats().handshakesRejected;
    SND_CHECK(t.c->DecideSync(ll::SyncMode::Disconnect));
    SND_CHECK(f.StepUntil([&] { return t.c->State() == ll::ClientState::Rejected; }));
    SND_CHECK(f.StepUntil([&] { return f.server->Stats().handshakesRejected == rejected + 1u; }));
    SND_CHECK(!t.c->DecideSync(ll::SyncMode::SendDiffs));
  }
}

// B9: no decision within handshakeTimeoutMs -> the game disconnects.
SND_TEST(IndepS7Handshake, UndecidedMismatchTimesOut) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.handshakeTimeoutMs = 300;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  ll::ClientDesc d = NoReconnect(f, 0x999);
  d.deferSyncDecision = true;
  Tool t(f, d);
  SND_REQUIRE(f.StepUntil([&] { return t.c->AwaitingSync(); }));
  SND_CHECK(f.StepUntil([&] { return f.server->Stats().timeouts >= 1u; }, 5000));
  SND_CHECK(!f.server->Connected());
}

// ============================== C: item revert and read-back ==========================

SND_TEST(IndepS7Cell, ReadRevertReadAndEdges) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  const ll::SetValue cell = SfxVolumeCell();
  ll::CellValue cv{};
  // untouched cell
  std::uint32_t q = t.c->ReadCell(cell);
  SND_REQUIRE(WaitCell(t, q, &cv));
  SND_CHECK_EQ(cv.requestSeq, q);
  SND_CHECK_EQ(cv.result, SND_OK);
  SND_CHECK_EQ(cv.cells, 1u);
  SND_CHECK_EQ(cv.flags, 0u);
  SND_CHECK_EQ(cv.current, Db(0.0f));
  SND_CHECK_EQ(cv.original, cv.current);
  // C3: revert of a cell that is not overridden
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(cell)), SND_OK);
  // C1
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
  q = t.c->ReadCell(cell);
  SND_REQUIRE(WaitCell(t, q, &cv));
  SND_CHECK_EQ(cv.result, SND_OK);
  SND_CHECK((cv.flags & ll::kCellOverridden) != 0u);
  SND_CHECK_EQ(cv.flags & ll::kCellsDiffer, 0u);
  SND_CHECK_EQ(cv.current, Db(-6.0f));
  SND_CHECK_EQ(cv.original, Db(0.0f));
  // second write: original stays the bank value (first write wins)
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -12.0f))), SND_OK);
  q = t.c->ReadCell(cell);
  SND_REQUIRE(WaitCell(t, q, &cv));
  SND_CHECK_EQ(cv.current, Db(-12.0f));
  SND_CHECK_EQ(cv.original, Db(0.0f));
  // C2
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(cell)), SND_OK);
  q = t.c->ReadCell(cell);
  SND_REQUIRE(WaitCell(t, q, &cv));
  SND_CHECK_EQ(cv.current, Db(0.0f));
  SND_CHECK_EQ(cv.original, Db(0.0f));
  SND_CHECK_EQ(cv.flags & ll::kCellOverridden, 0u);
  // C4: revert of the revert, then edit + revert again
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(cell)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -3.0f))), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(cell)), SND_OK);
  q = t.c->ReadCell(cell);
  SND_REQUIRE(WaitCell(t, q, &cv));
  SND_CHECK_EQ(cv.current, Db(0.0f));
  SND_CHECK_EQ(cv.flags & ll::kCellOverridden, 0u);
  // C5: bad address at acceptance, missing row at application
  ll::SetValue bad = cell;
  bad.chunk = snd::Fourcc('Z', 'Z', 'Z', 'Z');
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(bad)), SND_ERR_NOT_FOUND);
  bad = cell;
  bad.column = 250;
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(bad)), SND_ERR_NOT_FOUND);
  ll::SetValue missing = cell;
  missing.runtimeId = HashId("NoSuchBus");
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(missing)), SND_ERR_NOT_FOUND);
  // C6
  bad = cell;
  bad.chunk = snd::Fourcc('Z', 'Z', 'Z', 'Z');
  q = t.c->ReadCell(bad);
  SND_REQUIRE(WaitCell(t, q, &cv));
  SND_CHECK_EQ(cv.result, SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(cv.requestSeq, q);
  q = t.c->ReadCell(missing);
  SND_REQUIRE(WaitCell(t, q, &cv));
  SND_CHECK_EQ(cv.result, SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(cv.cells, 0u);
  // index beyond a scalar column
  ll::SetValue idx = cell;
  idx.index = 1;
  q = t.c->ReadCell(idx);
  SND_REQUIRE(WaitCell(t, q, &cv));
  SND_CHECK_EQ(cv.result, SND_ERR_NOT_FOUND);
}

// C7: ReadCell with Ack: one CellValue and one Ack, same result.
SND_TEST(IndepS7Cell, ReadCellWithAckAnswersOnceEach) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  ll::SetValue cells[2] = {SfxVolumeCell(), SfxVolumeCell()};
  cells[1].runtimeId = HashId("NoSuchBus");
  for (const ll::SetValue& c : cells) {
    const std::uint32_t q = t.c->ReadCell(c, true);
    ll::CellValue cv{};
    SND_REQUIRE(WaitCell(t, q, &cv));
    const std::int32_t a = t.Ack(q);
    SND_CHECK_EQ(a, cv.result);
    f.Steps(20);
    std::int32_t again = 0;
    ll::CellValue cv2{};
    SND_CHECK(!t.c->TakeAck(q, &again));
    SND_CHECK(!t.c->TakeCell(q, &cv2));
  }
}

// C8: reads never write.
SND_TEST(IndepS7Cell, ManyReadsChangeNothing) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
  std::uint32_t ok = 0;
  for (int batch = 0; batch < 5; ++batch) {  // batches of 200: the client keeps the last 256 CellValues
    std::vector<std::uint32_t> seqs;
    for (int i = 0; i < 200; ++i) seqs.push_back(t.c->ReadCell(SfxVolumeCell()));
    for (std::uint32_t q : seqs) {
      ll::CellValue cv{};
      if (WaitCell(t, q, &cv) && cv.current == Db(-6.0f) && cv.original == Db(0.0f) &&
          (cv.flags & ll::kCellOverridden) != 0u)
        ++ok;
    }
  }
  SND_CHECK_EQ(ok, 1000u);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 1u);
}

// ============================== D: ReplaceChunk ==========================

SND_TEST(IndepS7Replace, AcceptanceTable) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  BankOpts rel;
  rel.name = "Rel";
  rel.dev = false;
  f.Load(IndepBank(rel), "r.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  auto readSfx = [&](ll::CellValue* cv) { return WaitCell(t, t.c->ReadCell(SfxVolumeCell()), cv); };
  ll::CellValue cv{};
  // D1
  std::vector<std::uint8_t> ch = BussChunk(-9.0f);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size())), SND_OK);
  SND_REQUIRE(readSfx(&cv));
  SND_CHECK_EQ(cv.current, Db(-9.0f));
  SND_CHECK_EQ(cv.original, Db(0.0f));
  SND_CHECK((cv.flags & ll::kCellOverridden) != 0u);
  // D6
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(SfxVolumeCell())), SND_OK);
  SND_REQUIRE(readSfx(&cv));
  SND_CHECK_EQ(cv.current, Db(0.0f));
  // D2: row count differs
  ch = BussChunk(-9.0f, true);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size())), SND_ERR_UNSUPPORTED);
  SND_REQUIRE(readSfx(&cv));
  SND_CHECK_EQ(cv.current, Db(0.0f));
  // a reference column that changes (parent): unsupported, nothing written
  ch = BussChunk(-9.0f, false, 0u);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size())), SND_ERR_UNSUPPORTED);
  SND_REQUIRE(readSfx(&cv));
  SND_CHECK_EQ(cv.current, Db(0.0f));
  // D3
  ch = BussChunk(-9.0f);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(HashId("NoBank"), bf::chunk::BUSS, ch.data(), ch.size())), SND_ERR_NOT_FOUND);
  // release bank
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(HashId("Rel"), bf::chunk::BUSS, ch.data(), ch.size())), SND_ERR_UNSUPPORTED);
  // blob
  std::vector<std::uint8_t> blob(128, 0);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::WAVE, blob.data(), blob.size())), SND_ERR_UNSUPPORTED);
  // D5: the same column id twice
  ch = BussChunk(-9.0f);
  {
    std::vector<std::uint8_t> dup = ch;
    bf::TableChunkHeader th{};
    std::memcpy(&th, dup.data(), sizeof(th));
    SND_REQUIRE(th.columnCount >= 2u);
    bf::ColumnEntry c0{}, c1{};
    std::memcpy(&c0, dup.data() + 16, 16);
    std::memcpy(&c1, dup.data() + 32, 16);
    c1.columnId = c0.columnId;
    std::memcpy(dup.data() + 32, &c1, 16);
    SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, dup.data(), dup.size())), SND_ERR_INVALID_ARGUMENT);
    // a column past the end of the data
    std::vector<std::uint8_t> out = ch;
    c0.offset = out.size();
    std::memcpy(out.data() + 16, &c0, 16);
    SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, out.data(), out.size())), SND_ERR_INVALID_ARGUMENT);
    // an element size that does not match the type
    std::vector<std::uint8_t> el = ch;
    std::memcpy(&c0, el.data() + 16, 16);
    c0.elemBytes = 8;
    std::memcpy(el.data() + 16, &c0, 16);
    SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, el.data(), el.size())), SND_ERR_INVALID_ARGUMENT);
    // offset not a multiple of 64
    std::vector<std::uint8_t> al = ch;
    std::memcpy(&c0, al.data() + 16, 16);
    c0.offset += 4;
    std::memcpy(al.data() + 16, &c0, 16);
    SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, al.data(), al.size())), SND_ERR_INVALID_ARGUMENT);
  }
  SND_REQUIRE(readSfx(&cv));
  SND_CHECK_EQ(cv.current, Db(0.0f));  // nothing was written by any refused replacement
  SND_CHECK_EQ(cv.flags & ll::kCellOverridden, 0u);
}

// D4: larger than chunkBytes.
SND_TEST(IndepS7Replace, LargerThanChunkBytesIsOutOfBudget) {
  ll::RuntimeDesc rd;
  rd.chunkBytes = 128;
  Fixture f(Hooks::Live, lltest::Config(), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  const std::vector<std::uint8_t> ch = BussChunk(-9.0f);
  SND_REQUIRE(ch.size() > 128u);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size())), SND_ERR_OUT_OF_BUDGET);
}

// ============================== E: auditions ==========================

SND_TEST(IndepS7Audition, StopAllStopsOnlyAuditions) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  // E2: nothing to stop
  SND_CHECK_EQ(t.Ack(t.c->StopAuditions(true)), SND_OK);
  const snd_playback_handle game = f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  f.Steps(4);
  SND_CHECK_EQ(VoiceCount(f), 3u);
  SND_CHECK_EQ(f.rt->ActiveAuditions(), 2u);
  const std::uint32_t stopped = f.server->Stats().auditionsStopped;
  SND_CHECK_EQ(t.Ack(t.c->StopAuditions(true)), SND_OK);
  SND_CHECK(f.StepUntil([&] { return VoiceCount(f) == 1u; }));
  f.Steps(10);
  SND_CHECK_EQ(VoiceCount(f), 1u);
  SND_CHECK_EQ(f.sys->GetPlaybackState(game), SND_PLAYBACK_STATE_PLAYING);
  SND_CHECK(f.StepUntil([&] { return f.rt->ActiveAuditions() == 0u; }));
  SND_CHECK_EQ(f.server->Stats().auditionsStopped, stopped + 2u);
  // E3: an audition sent after the stop is not stopped by it
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  f.Steps(30);
  SND_CHECK_EQ(VoiceCount(f), 2u);
  SND_CHECK_EQ(f.rt->ActiveAuditions(), 1u);
  // stop-all and an audition in the same burst: order of recording decides
  const std::uint32_t s1 = t.c->StopAuditions(true);
  const std::uint32_t a2 = t.c->Audition(HashId("Tone"), nullptr, false, true);
  SND_CHECK_EQ(t.Ack(s1), SND_OK);
  SND_CHECK_EQ(t.Ack(a2), SND_OK);
  f.Steps(40);
  SND_CHECK_EQ(VoiceCount(f), 2u);  // game + the audition sent after the stop
  SND_CHECK_EQ(f.rt->ActiveAuditions(), 1u);
  SND_CHECK_EQ(f.sys->GetPlaybackState(game), SND_PLAYBACK_STATE_PLAYING);
}

// E4: maxAuditions; places come back when auditions end.
SND_TEST(IndepS7Audition, LimitAndPlacesComeBack) {
  ll::RuntimeDesc rd;
  rd.maxAuditions = 2;
  Fixture f(Hooks::Live, lltest::Config(), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(f.rt->ActiveAuditions(), 2u);
  SND_CHECK_EQ(t.Ack(t.c->StopAuditions(true)), SND_OK);
  SND_CHECK(f.StepUntil([&] { return f.rt->ActiveAuditions() == 0u; }));
  // a one-shot ("Blip", 2400 frames) gives its place back when it ends by itself
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Blip"), nullptr, false, true)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Blip"), nullptr, false, true)), SND_OK);
  SND_CHECK(f.StepUntil([&] { return f.rt->ActiveAuditions() == 0u; }));
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  SND_CHECK_EQ(f.rt->Drops().auditions, 0u);
  // an audition of an unknown event takes no place
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("NoEvent"), nullptr, false, true)), SND_OK);  // D-171: recording result
  SND_CHECK(f.StepUntil([&] { return f.rt->ActiveAuditions() == 1u; }));
}

// E5: a disconnect does not stop auditions.
SND_TEST(IndepS7Audition, DisconnectKeepsAuditions) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;  // even with the revert
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  Tool t(f, NoReconnect(f));
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  f.Steps(4);
  t.c->Disconnect();
  SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  f.Steps(50);
  SND_CHECK_EQ(VoiceCount(f), 1u);
  SND_CHECK_EQ(f.rt->ActiveAuditions(), 1u);
}

// ============================== F: structural edits ==========================

// F1: no editor.
SND_TEST(IndepS7Struct, NoEditorIsUnsupportedAndReserveComesBack) {
  Fixture f(Hooks::Live, ReserveConfig(2, 2, 1u << 20));
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK(!f.rt->HasStructEditor());
  for (int i = 0; i < 5; ++i) {
    SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_ERR_UNSUPPORTED);
  }
  const ll::DevReserveUsage u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 0u);
  SND_CHECK_EQ(u.bytes, 0u);
  SND_CHECK_EQ(u.busesLimit, 2u);
  SND_CHECK_EQ(f.server->Stats().structRefused, 5u);
}

// F2, F5, F6, F9: bus units.
SND_TEST(IndepS7Struct, BusReserveExactlyFullOneOverAndGivenBack) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(2, 2, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  const std::uint64_t cost = 64u + 8u * 2u;
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B2"), kMasterArgs, 2)), SND_OK);
  ll::DevReserveUsage u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 2u);  // exactly full
  SND_CHECK_EQ(u.bytes, 2u * cost);
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B3"), kMasterArgs, 2)), SND_ERR_OUT_OF_BUDGET);
  t.c->ClearErrors();
  SND_CHECK(t.c->StructEdit(ll::StructOp::AddBus, HashId("B3"), kMasterArgs, 2, false) != 0u);
  SND_CHECK(f.StepUntil([&] { return HasErrorText(t.c, "dev reserve exhausted: rebuild and hot reload"); }));
  // F6: bad arguments take nothing
  const std::uint64_t badArgs[2] = {HashId("Master"), 0};
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B4"), badArgs, 2)), SND_ERR_INVALID_ARGUMENT);
  u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 2u);
  SND_CHECK_EQ(u.bytes, 2u * cost);
  SND_CHECK_EQ(ed.Count(), 2u);  // the refused ones never reached the editor
  // F5: removing a live-added bus gives its unit back
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::RemoveBus, HashId("B1"), nullptr, 0)), SND_OK);
  u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 1u);
  SND_CHECK_EQ(u.bytes, cost);  // D-172: the whole cost of that Add comes back (units and bytes)
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B3"), kMasterArgs, 2)), SND_OK);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 2u);
  // removing a bus of the bank (not live-added) gives nothing back
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::RemoveBus, HashId("Sfx"), nullptr, 0)), SND_OK);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 2u);
  // removing the same live-added bus twice gives back once
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::RemoveBus, HashId("B2"), nullptr, 0)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::RemoveBus, HashId("B2"), nullptr, 0)), SND_OK);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 1u);
}

// F3: bytes, exactly full and one over.
SND_TEST(IndepS7Struct, ByteReserveExactlyFullAndOneOver) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  const std::uint64_t addBus = 64u + 8u * 2u, addEffect3 = 64u + 8u * 3u, addSend = 64u + 8u * 2u;
  Fixture f(Hooks::Live, ReserveConfig(100, 100, addBus + addEffect3 + addSend), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);
  const std::uint64_t fx[3] = {HashId("snd.fx.gain"), 0xFFFFFFFFu, F32Bits(0.5f)};
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddEffect, HashId("B1"), fx, 3)), SND_OK);
  const std::uint64_t send[2] = {HashId("Master"), F32Bits(-3.0f)};
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddSend, HashId("B1"), send, 2)), SND_OK);
  ll::DevReserveUsage u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.bytes, u.bytesLimit);  // exactly full
  SND_CHECK_EQ(u.buses, 1u);
  SND_CHECK_EQ(u.effects, 1u);
  // one byte over (the smallest Add costs 64 + 16 bytes)
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddSend, HashId("B1"), send, 2)), SND_ERR_OUT_OF_BUDGET);
  SND_CHECK_EQ(f.rt->ReserveUsage().bytes, u.bytesLimit);
}

// F4: the editor refuses -> its result; reserve comes back.
SND_TEST(IndepS7Struct, EditorRefusalGivesReserveBack) {
  FakeEditor ed;
  ed.applyResult = SND_ERR_NOT_FOUND;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(1, 1, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  for (int i = 0; i < 3; ++i) {
    SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_ERR_NOT_FOUND);
    const ll::DevReserveUsage u = f.rt->ReserveUsage();
    SND_CHECK_EQ(u.buses, 0u);
    SND_CHECK_EQ(u.bytes, 0u);
  }
  ed.applyResult = SND_OK;
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 1u);
}

// F7, F9: the disconnect revert undoes newest first and gives everything back.
SND_TEST(IndepS7Struct, DisconnectRevertUndoesNewestFirst) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(4, 4, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  Tool t(f, NoReconnect(f));
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);
  const std::uint64_t fx[2] = {HashId("snd.fx.gain"), 0xFFFFFFFFu};
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddEffect, HashId("B1"), fx, 2)), SND_OK);
  const std::uint64_t send[2] = {HashId("Master"), F32Bits(-3.0f)};
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddSend, HashId("B1"), send, 2)), SND_OK);
  SND_CHECK_EQ(ed.Count(), 3u);
  t.c->Disconnect();
  SND_REQUIRE(f.StepUntil([&] { return ed.Count() >= 6u; }));
  f.Steps(5);
  SND_REQUIRE_EQ(ed.Count(), 6u);
  const FakeEditor::Entry u0 = ed.At(3), u1 = ed.At(4), u2 = ed.At(5);
  SND_CHECK(u0.undo == 1u && u0.op == static_cast<std::uint32_t>(ll::StructOp::AddSend));
  SND_CHECK(u1.undo == 1u && u1.op == static_cast<std::uint32_t>(ll::StructOp::AddEffect));
  SND_CHECK(u2.undo == 1u && u2.op == static_cast<std::uint32_t>(ll::StructOp::AddBus));
  const ll::DevReserveUsage u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 0u);
  SND_CHECK_EQ(u.effects, 0u);
  SND_CHECK_EQ(u.bytes, 0u);
}

// F8: the reserve outlives the Server; a new Server keeps counting.
SND_TEST(IndepS7Struct, ReserveOutlivesServer) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(1, 1, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  {
    Tool t(f, NoReconnect(f));
    SND_REQUIRE(t.Ready());
    SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);
  }
  f.StopServer();
  f.link.Destroy();
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 1u);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B2"), kMasterArgs, 2)), SND_ERR_OUT_OF_BUDGET);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 1u);
}

// ============================== G: hot reload ==========================

std::vector<ll::ReloadStatus> StatusOf(ll::Client* c, std::uint32_t seq) {
  ll::ReloadStatus s[64];
  const std::uint32_t n = c->CopyReloadStatus(s, 64);
  std::vector<ll::ReloadStatus> out;
  for (std::uint32_t i = 0; i < n; ++i)
    if (s[i].requestSeq == seq) out.push_back(s[i]);
  return out;
}
bool HasBank(Tool& t, snd::Id id) {
  const std::uint32_t q = t.c->QueryBanks(true);
  if (t.Ack(q) != SND_OK) return false;
  ll::BankEntry b[16];
  std::uint32_t rs = 0;
  const std::uint32_t n = t.c->CopyBanks(b, 16, &rs);
  bool has = false;
  for (std::uint32_t i = 0; i < n; ++i) has = has || b[i].bankId == id;
  return has;
}

// G1, G6, G8
SND_TEST(IndepS7Reload, SuccessOrderAndOverridesForgotten) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
  const std::uint32_t q = t.c->ReloadBank(kLive, 0, "a.sndb");
  SND_REQUIRE(q != 0u);
  std::int32_t res = 99;
  std::vector<ll::ReloadStatus> atAck;
  SND_REQUIRE(f.StepUntil([&] {
    if (!t.c->TakeAck(q, &res)) return false;
    atAck = StatusOf(t.c, q);  // what had arrived when the Ack was there
    return true;
  }));
  SND_CHECK_EQ(res, SND_OK);
  SND_REQUIRE_EQ(atAck.size(), 3u);
  SND_CHECK_EQ(atAck[0].phase, static_cast<std::uint32_t>(ll::ReloadPhase::Unloading));
  SND_CHECK_EQ(atAck[1].phase, static_cast<std::uint32_t>(ll::ReloadPhase::Loading));
  SND_CHECK_EQ(atAck[2].phase, static_cast<std::uint32_t>(ll::ReloadPhase::Done));
  SND_CHECK_EQ(atAck[2].result, SND_OK);
  SND_CHECK(atAck[2].bank != 0u);
  for (const auto& s : atAck) SND_CHECK_EQ(s.bankId, kLive);
  f.Steps(20);
  std::int32_t again = 0;
  SND_CHECK(!t.c->TakeAck(q, &again));  // G8
  SND_CHECK_EQ(StatusOf(t.c, q).size(), 3u);
  // G6: the new registration has the bank value, nothing remembered
  ll::CellValue cv{};
  SND_REQUIRE(WaitCell(t, t.c->ReadCell(SfxVolumeCell()), &cv));
  SND_CHECK_EQ(cv.result, SND_OK);
  SND_CHECK_EQ(cv.current, Db(0.0f));
  SND_CHECK_EQ(cv.flags & ll::kCellOverridden, 0u);
  SND_CHECK_EQ(f.server->Stats().reloadsDone, 1u);
  // edits work again on the new registration
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(SfxVolumeCell())), SND_OK);
}

// G2, G3
SND_TEST(IndepS7Reload, NotLoadedLoadIfAbsentAndBadPath) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  BankOpts two;
  two.name = "Two";
  f.fs.AddFile("two.sndb", IndepBank(two));
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->ReloadBank(HashId("Two"), 0, "two.sndb")), SND_ERR_NOT_FOUND);
  SND_CHECK(!HasBank(t, HashId("Two")));
  const std::uint32_t q = t.c->ReloadBank(HashId("Two"), 0, "two.sndb", true);
  SND_CHECK_EQ(t.Ack(q), SND_OK);
  SND_CHECK(HasBank(t, HashId("Two")));
  // G3: unreadable path: Failed, IO, the bank stays out
  const std::uint32_t r = t.c->ReloadBank(kLive, 0, "missing.sndb");
  SND_CHECK_EQ(t.Ack(r), SND_ERR_IO);
  const std::vector<ll::ReloadStatus> st = StatusOf(t.c, r);
  SND_REQUIRE(!st.empty());
  SND_CHECK_EQ(st.back().phase, static_cast<std::uint32_t>(ll::ReloadPhase::Failed));
  SND_CHECK_EQ(st.back().result, SND_ERR_IO);
  SND_CHECK(!HasBank(t, kLive));
  SND_CHECK_EQ(f.server->Stats().reloadsFailed, 1u);
  // a corrupt file
  std::vector<std::uint8_t> junk(256, 0xAB);
  f.fs.AddFile("junk.sndb", junk);
  const std::uint32_t j = t.c->ReloadBank(HashId("Two"), 0, "junk.sndb");
  const std::int32_t jr = t.Ack(j);
  SND_CHECK(jr != SND_OK && jr != 1);
  SND_CHECK(!HasBank(t, HashId("Two")));
}

// G4, G5, D7, F10: during a reload.
SND_TEST(IndepS7Reload, EditsDuringReloadAreNotReady) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(4, 4, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  f.Steps(5);
  const std::uint64_t base = f.server->Stats().framesIn;
  const std::uint32_t reload = t.c->ReloadBank(kLive, 0, "a.sndb");
  const std::uint32_t second = t.c->ReloadBank(kLive, 0, "a.sndb");
  const std::uint32_t sv = t.c->SetValue(lltest::BusVolume("Sfx", -6.0f));
  const std::uint32_t rv = t.c->RevertValue(SfxVolumeCell());
  const std::vector<std::uint8_t> ch = BussChunk(-9.0f);
  const std::uint32_t rc = t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size());
  const std::uint32_t se = t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2);
  const std::uint32_t rd2 = t.c->ReadCell(SfxVolumeCell(), true);
  const std::uint32_t qb = t.c->QueryBanks(true);
  const std::uint32_t op = t.c->OverrideParam(HashId("Knob"), 0.5f, true, true);
  SND_REQUIRE(WaitFramesIn(f, base, 9));  // the game read all of them before any block was rendered
  SND_CHECK_EQ(t.Ack(second), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(t.Ack(sv), SND_ERR_NOT_READY);
  SND_CHECK_EQ(t.Ack(rv), SND_ERR_NOT_READY);
  SND_CHECK_EQ(t.Ack(rc), SND_ERR_NOT_READY);
  SND_CHECK_EQ(t.Ack(se), SND_ERR_NOT_READY);
  SND_CHECK_EQ(ed.Count(), 0u);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 0u);
  SND_CHECK_EQ(t.Ack(op), SND_OK);
  SND_CHECK_EQ(t.Ack(qb), SND_OK);
  const std::int32_t ra = t.Ack(rd2);
  SND_CHECK(ra != 1 && ra != 2);  // answered (value depends on whether the bank was out at that moment)
  SND_CHECK_EQ(t.Ack(reload), SND_OK);
  // once done, edits are taken again
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
}

// G7: a disconnect during the reload does not stop it.
SND_TEST(IndepS7Reload, DisconnectDuringReloadCompletes) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  {
    Tool t(f, NoReconnect(f));
    SND_REQUIRE(t.Ready());
    const std::uint64_t base = f.server->Stats().framesIn;
    SND_REQUIRE(t.c->ReloadBank(kLive, 0, "a.sndb") != 0u);
    SND_REQUIRE(WaitFramesIn(f, base, 1));
    t.c->Disconnect();
    SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  }
  SND_CHECK(f.StepUntil([&] { return f.server->Stats().reloadsDone == 1u; }));
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK(HasBank(t, kLive));
}

// ============================== H: update numbers ==========================

SND_TEST(IndepS7Sequences, MonotonicSinceAndMissed) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.timingHz = 1000;  // every block
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  ll::ClientDesc d = lltest::MemoryClient(f.link);
  d.historySeconds = 1;
  Tool t(f, d);
  SND_REQUIRE(t.Ready());
  SND_REQUIRE(t.c->SetProfilerMask(ll::kProfTiming, 0) != 0u);
  ll::ClientSequences prev = t.c->Sequences();
  bool monotonic = true;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
  while (std::chrono::steady_clock::now() < end) {
    f.Step();
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    const ll::ClientSequences s = t.c->Sequences();
    monotonic = monotonic && s.timings >= prev.timings && s.acks >= prev.acks && s.errors >= prev.errors &&
                s.memory >= prev.memory && s.voiceLists >= prev.voiceLists;
    prev = s;
  }
  SND_CHECK(monotonic);
  // quiet: no block rendered, let the stream drain
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const ll::ClientSequences s = t.c->Sequences();
  SND_REQUIRE(s.timings > 0u);
  std::vector<ll::TimingSample> buf(s.timings + 16u);
  std::uint64_t first = 0;
  const std::uint32_t n = t.c->CopyTimingsSince(0, buf.data(), static_cast<std::uint32_t>(buf.size()), &first);
  SND_CHECK(n > 0u);
  SND_CHECK(first > 1u);                    // the first entries left the 1 s history
  SND_CHECK_EQ(first + n - 1u, s.timings);  // what is kept ends at the last number
  // nothing new after the last number
  std::uint64_t f2 = 0;
  SND_CHECK_EQ(t.c->CopyTimingsSince(s.timings, buf.data(), static_cast<std::uint32_t>(buf.size()), &f2), 0u);
  // a window in the middle: entries numbered after `after`
  const std::uint64_t after = s.timings - 3u;
  std::uint64_t f3 = 0;
  SND_CHECK_EQ(t.c->CopyTimingsSince(after, buf.data(), static_cast<std::uint32_t>(buf.size()), &f3), 3u);
  SND_CHECK_EQ(f3, after + 1u);
  // capacity smaller than what is new: the oldest new ones come first (no skip)
  std::uint64_t f4 = 0;
  SND_CHECK_EQ(t.c->CopyTimingsSince(after, buf.data(), 2, &f4), 2u);
  SND_CHECK_EQ(f4, after + 1u);
}

// ============================== K: budget and RT ==========================

SND_TEST(IndepS7Profiler, BudgetDefinitionAndBlockDetail) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_REQUIRE(t.c->SetProfilerMask(ll::kProfTiming | ll::kProfBlockDetail | ll::kProfStreamList, 30) != 0u);
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(f.StepUntil([&] {
    return t.c->Sequences().timings > 2u && t.c->Sequences().details > 2u && t.c->Sequences().streamLists > 1u;
  }));
  const double us = 1e6 * static_cast<double>(f.sys->BlockFrames()) / static_cast<double>(f.sys->SampleRate()) *
                    static_cast<double>(f.cfg_.tuning.load_target_percent) / 100.0;
  const float expected = static_cast<float>(us);
  ll::TimingSample ts[8];
  const std::uint32_t nt = t.c->CopyTimings(ts, 8);
  SND_REQUIRE(nt > 0u);
  for (std::uint32_t i = 0; i < nt; ++i) SND_CHECK_EQ(ts[i].timing.budgetMicroseconds, expected);
  ll::BlockDetailSample bd[8];
  const std::uint32_t nd = t.c->CopyBlockDetails(bd, 8);
  SND_REQUIRE(nd > 0u);
  for (std::uint32_t i = 0; i < nd; ++i) {
    SND_CHECK_EQ(bd[i].detail.budgetMicroseconds, expected);
    SND_CHECK_EQ(bd[i].detail.lodLevel, 0u);
    SND_CHECK(bd[i].detail.totalMicroseconds >= 0.0f);
    SND_CHECK(bd[i].detail.parallel <= 1u);
  }
  ll::StreamInfo si[16];
  const std::uint32_t ns = t.c->CopyStreams(si, 16);
  SND_CHECK_EQ(ns, f.cfg_.max_streams);
  for (std::uint32_t i = 0; i < ns; ++i) {
    SND_CHECK_EQ(si[i].slot, i);
    SND_CHECK(si[i].state <= 1u);
  }
}

// K1: the audio-thread paths of every 1.1 operation run inside RenderBlock (SND_RT_SCOPE):
// no allocation / lock / IO is recorded while they run.
SND_TEST(IndepS7Rt, NewAudioPathsAreRtSafe) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(4, 4, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  Tool t(f, NoReconnect(f));
  SND_REQUIRE(t.Ready());
  SND_REQUIRE(t.c->SetProfilerMask(ll::ProfilerMaskOf(1), 60) != 0u);
  snd::test::rt::Reset();
  SND_CHECK_EQ(t.Ack(t.c->StartRecording(true)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
  ll::CellValue cv{};
  SND_CHECK(WaitCell(t, t.c->ReadCell(SfxVolumeCell()), &cv));
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(SfxVolumeCell())), SND_OK);
  const std::vector<std::uint8_t> ch = BussChunk(-9.0f);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size())), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->StopAuditions(true)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->StopRecording(true)), SND_OK);
  t.c->Disconnect();
  SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  f.Steps(10);
  using V = snd::rt::Violation;
  std::printf("[indep_s7] RT hooks: new=%d libc=%d alloc=%u free=%u lock=%u io=%u\n",
              snd::test::rt::NewHooksActive() ? 1 : 0, snd::test::rt::LibcHooksActive() ? 1 : 0,
              snd::test::rt::Count(V::Alloc), snd::test::rt::Count(V::Free), snd::test::rt::Count(V::Lock),
              snd::test::rt::Count(V::FileIo));
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
}

// ============================== L: sessions and back-pressure ==========================

// L1, L2: thousands of Ack-requests of the new kinds; one answer each, no disconnect.
SND_TEST(IndepS7Session, ManyAckRequestsAnsweredOnceNoDisconnect) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f, NoReconnect(f));
  SND_REQUIRE(t.Ready());
  const ll::ClientSequences s0 = t.c->Sequences();
  std::uint32_t reads = 0, queries = 0, acks = 0, cells = 0, qAcks = 0, extra = 0;
  for (int batch = 0; batch < 10; ++batch) {  // 10 x (200 ReadCell + 50 QueryBanks), each with an Ack
    std::vector<std::uint32_t> rs, qs;
    for (int i = 0; i < 200; ++i) {
      std::uint32_t q = 0;
      for (int k = 0; k < 2000 && q == 0u; ++k) {
        q = t.c->ReadCell(SfxVolumeCell(), true);
        if (q == 0u) f.Step();
      }
      rs.push_back(q);
      if (i % 4 == 0) {
        std::uint32_t b = 0;
        for (int k = 0; k < 2000 && b == 0u; ++k) {
          b = t.c->QueryBanks(true);
          if (b == 0u) f.Step();
        }
        qs.push_back(b);
      }
    }
    reads += static_cast<std::uint32_t>(rs.size());
    queries += static_cast<std::uint32_t>(qs.size());
    std::vector<std::uint8_t> gotAck(rs.size(), 0), gotCell(rs.size(), 0), gotQ(qs.size(), 0);
    std::uint32_t need = static_cast<std::uint32_t>(2 * rs.size() + qs.size()), have = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (have < need && std::chrono::steady_clock::now() < end) {
      f.Step();
      for (std::size_t i = 0; i < rs.size(); ++i) {
        std::int32_t r = 0;
        ll::CellValue cv{};
        if (!gotAck[i] && t.c->TakeAck(rs[i], &r)) gotAck[i] = 1, ++acks, ++have;
        if (!gotCell[i] && t.c->TakeCell(rs[i], &cv)) gotCell[i] = 1, ++cells, ++have;
      }
      for (std::size_t i = 0; i < qs.size(); ++i) {
        std::int32_t r = 0;
        if (!gotQ[i] && t.c->TakeAck(qs[i], &r)) gotQ[i] = 1, ++qAcks, ++have;
      }
    }
    f.Steps(5);
    for (std::uint32_t q : rs) {
      std::int32_t r = 0;
      ll::CellValue cv{};
      if (t.c->TakeAck(q, &r)) ++extra;
      if (t.c->TakeCell(q, &cv)) ++extra;
    }
  }
  SND_CHECK_EQ(reads, 2000u);
  SND_CHECK_EQ(queries, 500u);
  SND_CHECK_EQ(acks, 2000u);
  SND_CHECK_EQ(cells, 2000u);
  SND_CHECK_EQ(qAcks, 500u);
  SND_CHECK_EQ(extra, 0u);
  // what the client received: exactly one Ack per request and one CellValue per ReadCell
  const ll::ClientSequences s1 = t.c->Sequences();
  SND_CHECK_EQ(s1.acks - s0.acks, 2500u);
  SND_CHECK_EQ(s1.cells - s0.cells, 2000u);
  SND_CHECK_EQ(s1.bankLists - s0.bankLists, 500u);
  SND_CHECK(f.server->Connected());
  SND_CHECK_EQ(f.server->Stats().connections, 1u);
  SND_CHECK_EQ(t.c->Stats().disconnects, 0u);
}

// D-170: the client keeps kClientCellsKept CellValues; one more pushes the oldest out and counts it.
SND_TEST(IndepS7Session, AnswerStoreLimitsAndLostCounts) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f, NoReconnect(f));
  SND_REQUIRE(t.Ready());
  auto burst = [&](std::uint32_t n, std::vector<std::uint32_t>* rs) {
    const std::uint64_t c0 = t.c->Sequences().cells;
    for (std::uint32_t i = 0; i < n; ++i) rs->push_back(t.c->ReadCell(SfxVolumeCell(), false));
    return f.StepUntil([&] { return t.c->Sequences().cells >= c0 + n; });
  };
  {  // exactly the limit
    std::vector<std::uint32_t> rs;
    SND_REQUIRE(burst(ll::kClientCellsKept, &rs));
    std::uint32_t taken = 0;
    for (std::uint32_t q : rs) {
      ll::CellValue cv{};
      taken += t.c->TakeCell(q, &cv) ? 1u : 0u;
    }
    SND_CHECK_EQ(taken, ll::kClientCellsKept);
    SND_CHECK_EQ(t.c->Stats().cellsLost, 0u);
  }
  {  // taken answers pushed out later do not count
    std::vector<std::uint32_t> rs;
    SND_REQUIRE(burst(ll::kClientCellsKept, &rs));
    SND_CHECK_EQ(t.c->Stats().cellsLost, 0u);  // the previous ones were taken
    // one over: the oldest of this burst is lost
    std::vector<std::uint32_t> more;
    SND_REQUIRE(burst(1, &more));
    SND_CHECK_EQ(t.c->Stats().cellsLost, 1u);
    ll::CellValue cv{};
    SND_CHECK(!t.c->TakeCell(rs[0], &cv));
    SND_CHECK(t.c->TakeCell(rs[1], &cv));
    SND_CHECK(t.c->TakeCell(more[0], &cv));
  }
  {  // Acks: 1024 kept, the 1025th pushes one out
    const std::uint32_t lost0 = t.c->Stats().acksLost;
    std::vector<std::uint32_t> rs;
    const std::uint64_t a0 = t.c->Sequences().acks;
    for (std::uint32_t i = 0; i < ll::kClientAcksKept + 1u; ++i) {
      std::uint32_t q = 0;
      for (int k = 0; k < 2000 && q == 0u; ++k) {
        q = t.c->QueryBanks(true);
        if (q == 0u) f.Step();
      }
      rs.push_back(q);
    }
    SND_REQUIRE(f.StepUntil([&] { return t.c->Sequences().acks >= a0 + ll::kClientAcksKept + 1u; }, 20000));
    SND_CHECK_EQ(t.c->Stats().acksLost - lost0, 1u);
    std::int32_t r = 0;
    SND_CHECK(!t.c->TakeAck(rs[0], &r));
    SND_CHECK(t.c->TakeAck(rs.back(), &r));
  }
}

// L3: a RevertValue / ReplaceChunk of an earlier Server's session is not applied. The old
// Server records and publishes them, is destroyed before any block is rendered, and a new
// Server starts (one Server per System at a time, server.hpp).
SND_TEST(IndepS7Session, OldSessionRevertAndReplaceAreNotApplied) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  {
    Tool t(f, NoReconnect(f));
    SND_REQUIRE(t.Ready());
    SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
    const std::uint64_t base = f.server->Stats().framesIn;
    const std::vector<std::uint8_t> ch = BussChunk(-9.0f);
    SND_REQUIRE(t.c->RevertValue(SfxVolumeCell()) != 0u);
    SND_REQUIRE(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size()) != 0u);
    SND_REQUIRE(WaitFramesIn(f, base, 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // a loop or two: recorded and flushed
    f.StopServer();  // no block rendered since
  }
  const std::uint32_t stale0 = f.rt->Drops().staleEdits;
  f.link.Destroy();
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);  // new session (D-87)
  f.Steps(10);
  std::printf("[indep_s7] old-session edits dropped as stale: %u\n", f.rt->Drops().staleEdits - stale0);
  Tool t2(f);
  SND_REQUIRE(t2.Ready());
  ll::CellValue cv{};
  SND_REQUIRE(WaitCell(t2, t2.c->ReadCell(SfxVolumeCell()), &cv));
  SND_CHECK_EQ(cv.current, Db(-6.0f));  // neither the revert nor the replacement of the old session
  SND_CHECK((cv.flags & ll::kCellOverridden) != 0u);
  SND_CHECK_EQ(cv.original, Db(0.0f));
}
