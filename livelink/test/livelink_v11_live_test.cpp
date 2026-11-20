// Protocol 1.1 end to end with a real core System (design.md 11.2; livelink-protocol.md
// §8): version negotiation both ways, build-hash edge rules and the bank list, item
// revert and read-back, audition stop-all, structural edits within Config.dev_reserve
// (through a fake editor: core has no structural edit yet, C-01), hot reload, the extra
// profiler streams and the client's update numbers. Levels are analytic: "Tone" plays a
// constant, so the output follows the bus volumes exactly.
#include <thread>

#include "livelink_v11_common.hpp"

using namespace v11;

// --- version negotiation -----------------------------------------------------------------

SND_TEST(LivelinkV11, HandshakeSaysMinorOneBothWays) {
  Session s;
  SND_REQUIRE(s.Ready());
  SND_CHECK_EQ(ll::kProtocolMinor, 1u);
  SND_CHECK_EQ(s.client->LastHelloReply().protocolMinor, 1u);
  SND_CHECK_EQ(s.client->PeerMinor(), 1u);
  SND_CHECK(s.client->Supports(ll::MessageType::ReadCell));
  SND_CHECK(s.client->Supports(ll::MessageType::SetValue));
}

// A 1.1 tool against a 1.0 game: the 1.1 requests are not sent (0), the new mask bits are
// dropped, everything of 1.0 works.
SND_TEST(LivelinkV11, NewToolAgainstOldGameSendsOnlyWhatTheGameKnows) {
  ll::MemoryLink link;
  SND_REQUIRE(link.Init(snd::base::DefaultAllocator(), 1u << 20));
  ll::ClientDesc cd = MemoryClient(link);
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(cd, &c), SND_OK);
  SND_REQUIRE_EQ(c->Start(), SND_OK);
  RawGame g;
  SND_REQUIRE(g.Accept(link.acceptor()));
  const RawGame::Frame* hello = g.WaitFor(ll::MessageType::Hello);
  SND_REQUIRE(hello != nullptr);
  const ll::Hello h = ll::ReadFixed<ll::Hello>(hello->payload.data(), static_cast<std::uint32_t>(hello->payload.size()));
  SND_CHECK_EQ(h.protocolMajor, 1u);
  SND_CHECK_EQ(h.protocolMinor, 1u);
  ll::HelloReply r{};
  r.protocolMajor = 1;
  r.protocolMinor = 0;  // a 1.0 game
  r.status = static_cast<std::uint32_t>(ll::HelloStatus::Accepted);
  r.runtimeBuildHash = kProjectHash;
  SND_REQUIRE(g.Send(ll::MessageType::HelloReply, &r, sizeof(r)));
  SND_REQUIRE(c->WaitReady(2000));
  SND_CHECK_EQ(c->PeerMinor(), 0u);
  SND_CHECK(!c->Supports(ll::MessageType::ReadCell));
  SND_CHECK(c->Supports(ll::MessageType::SetValue));
  SND_CHECK_EQ(c->ReadCell(BusVolumeCell("Sfx")), 0u);
  SND_CHECK_EQ(c->RevertValue(BusVolumeCell("Sfx")), 0u);
  SND_CHECK_EQ(c->QueryBanks(), 0u);
  SND_CHECK_EQ(c->ReloadBank(kBankId, 0, "Live.sndb"), 0u);
  const std::uint32_t seq = c->SetProfilerMask(ll::kProfTiming | ll::kProfStreamList | ll::kProfBlockDetail, 30);
  SND_CHECK(seq != 0u);
  const RawGame::Frame* m = g.WaitFor(ll::MessageType::SetProfilerMask);
  SND_REQUIRE(m != nullptr);
  const ll::SetProfilerMask pm = ll::ReadFixed<ll::SetProfilerMask>(m->payload.data(), 8);
  SND_CHECK_EQ(pm.mask, static_cast<std::uint32_t>(ll::kProfTiming));  // the 1.1 bits are dropped
  SND_CHECK_EQ(ll::CheckSetProfilerMask(pm), SND_OK);                  // what a 1.0 game accepts
  // 1.0 messages still go out.
  SND_CHECK(c->SetValue(BusVolume("Sfx", -6.0f)) != 0u);
  SND_CHECK(g.WaitFor(ll::MessageType::SetValue) != nullptr);
  ll::Client::Destroy(c);
}

// A 1.0 tool against a 1.1 game: accepted, and the game sends nothing of 1.1 (the new
// streams need mask bits a 1.0 tool cannot set; replies only answer 1.1 requests).
SND_TEST(LivelinkV11, OldToolAgainstNewGameSeesOnly10Messages) {
  Fixture f;
  f.Load(DevBank());
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  RawPeer p;
  SND_REQUIRE(f.StepUntil([&] { return p.Connect(f.link.connector(), 10); }));
  ll::Hello h{};
  h.protocolMajor = 1;
  h.protocolMinor = 0;
  h.role = static_cast<std::uint32_t>(ll::Role::Tool);
  h.projectBuildHash = kProjectHash;
  SND_REQUIRE(p.Send(ll::MessageType::Hello, &h, sizeof(h)));
  const RawPeer::Frame* r = nullptr;
  SND_REQUIRE(f.StepUntil([&] { return (r = p.WaitFor(ll::MessageType::HelloReply, 5)) != nullptr; }));
  const auto hr = ll::ReadFixed<ll::HelloReply>(r->payload.data(), static_cast<std::uint32_t>(r->payload.size()));
  SND_CHECK_EQ(hr.status, static_cast<std::uint32_t>(ll::HelloStatus::Accepted));
  SND_CHECK_EQ(hr.protocolMinor, 1u);
  const ll::SetProfilerMask all10{ll::ProfilerMaskOf(0), 60};
  SND_REQUIRE(p.Send(ll::MessageType::SetProfilerMask, &all10, sizeof(all10), ll::kFrameWantsAck, 2));
  // An unknown type with a payload is skipped (protocol §2), the stream stays usable.
  const std::uint8_t junk[40] = {1, 2, 3};
  SND_REQUIRE(p.Send(static_cast<ll::MessageType>(0x0077), junk, sizeof(junk), ll::kFrameWantsAck, 3));
  const RawPeer::Frame* ack = nullptr;
  SND_REQUIRE(f.StepUntil([&] { return (ack = p.WaitFor(ll::MessageType::Ack, 5)) != nullptr; }));
  SND_CHECK_EQ(ack->header.seq, 2u);
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(400)) {
    f.Step();
    p.Pump(1);
  }
  std::uint32_t profiler = 0;
  for (const RawPeer::Frame& fr : p.frames) {
    SND_CHECK_MSG(fr.header.type < static_cast<std::uint16_t>(ll::MessageType::CellValue), "1.1 type %u sent to a 1.0 tool",
                  fr.header.type);
    SND_CHECK_MSG(fr.header.type != static_cast<std::uint16_t>(ll::MessageType::Error), "Error sent");
    profiler += fr.header.type >= static_cast<std::uint16_t>(ll::MessageType::VoiceList) ? 1u : 0u;
  }
  SND_CHECK(profiler > 0u);
  SND_CHECK_EQ(f.server->Stats().protocolErrors, 0u);
}

// --- build hash: edge rules, the bank list, the deferred decision ---------------------------

SND_TEST(LivelinkV11, BuildMismatchBankListThenDecide) {
  {
    Session s(Config(), {}, {}, 0x1111u, true);  // another project's build, deferred decision
    SND_REQUIRE(s.f.StepUntil([&] { return s.client->AwaitingSync(); }));
    SND_CHECK_EQ(s.client->LastHelloReply().status, static_cast<std::uint32_t>(ll::HelloStatus::BuildMismatch));
    SND_CHECK(s.client->State() == ll::ClientState::Handshake);
    const std::uint32_t q = s.client->QueryBanks();
    SND_REQUIRE(q != 0u);
    SND_REQUIRE(s.f.StepUntil([&] { return s.client->Sequences().bankLists >= 1u; }));
    ll::BankEntry b[4];
    std::uint32_t replySeq = 0;
    SND_REQUIRE_EQ(s.client->CopyBanks(b, 4, &replySeq), 1u);
    SND_CHECK_EQ(replySeq, q);
    SND_CHECK_EQ(b[0].bankId, kBankId);
    SND_CHECK_EQ(b[0].languageId, 0u);
    SND_CHECK_EQ(b[0].projectBuildHash, kProjectHash);
    SND_CHECK(b[0].contentHash != 0u);
    SND_CHECK_EQ(b[0].writable, 1u);
    SND_CHECK(s.client->DecideSync(ll::SyncMode::SendDiffs));
    SND_REQUIRE(s.Ready());
    SND_CHECK(!s.client->AwaitingSync());
    SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -6.0f))), SND_OK);  // a diff
    s.f.Steps(8);
    SND_CHECK_NEAR(s.Level(), Ratio(-6.0f), 1e-4);
  }
  {
    Session s(Config(), {}, {}, 0x1111u, true);
    SND_REQUIRE(s.f.StepUntil([&] { return s.client->AwaitingSync(); }));
    SND_CHECK(s.client->DecideSync(ll::SyncMode::Disconnect));
    SND_CHECK(s.client->State() == ll::ClientState::Rejected);
    SND_CHECK(s.f.StepUntil([&] { return s.f.server->Stats().handshakesRejected == 1u; }));
    SND_CHECK(!s.client->DecideSync(ll::SyncMode::SendDiffs));  // nothing waits any more
  }
}

SND_TEST(LivelinkV11, BuildHashEdgeRules) {
  // Two banks that disagree: BuildMismatch even for a tool built like the first one; the
  // reported hash is the first registered bank's, the bank list shows both.
  {
    Fixture f;
    f.Load(DevBank());
    indep::BankOpts two;
    two.name = "Two";
    two.hash = 0x9999u;
    f.Load(indep::IndepBank(two), "two.sndb");
    SND_REQUIRE_EQ(f.StartServer(), SND_OK);
    ll::ClientDesc cd = MemoryClient(f.link, kProjectHash);
    cd.deferSyncDecision = true;
    ll::Client* c = nullptr;
    SND_REQUIRE_EQ(ll::Client::Create(cd, &c), SND_OK);
    c->Start();
    SND_REQUIRE(f.StepUntil([&] { return c->AwaitingSync(); }));
    const ll::HelloReply r = c->LastHelloReply();
    SND_CHECK_EQ(r.status, static_cast<std::uint32_t>(ll::HelloStatus::BuildMismatch));
    SND_CHECK_EQ(r.runtimeBuildHash, kProjectHash);
    c->QueryBanks();
    SND_REQUIRE(f.StepUntil([&] { return c->Sequences().bankLists >= 1u; }));
    ll::BankEntry b[4];
    SND_REQUIRE_EQ(c->CopyBanks(b, 4), 2u);
    const bool order = b[0].bankId == kBankId && b[1].bankId == HashId("Two");
    SND_CHECK(order);
    SND_CHECK_EQ(b[1].projectBuildHash, 0x9999u);
    ll::Client::Destroy(c);
  }
  // No bank: the game's hash is 0; a tool hash of 0 is accepted, another one is a mismatch.
  for (std::uint64_t toolHash : {std::uint64_t{0}, std::uint64_t{0x1234}}) {
    Fixture f;
    SND_REQUIRE_EQ(f.StartServer(), SND_OK);
    ll::Client* c = nullptr;
    SND_REQUIRE_EQ(ll::Client::Create(MemoryClient(f.link, toolHash), &c), SND_OK);
    c->Start();
    SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready; }));
    SND_CHECK_EQ(c->LastHelloReply().runtimeBuildHash, 0u);
    SND_CHECK_EQ(c->LastHelloReply().status,
                 static_cast<std::uint32_t>(toolHash == 0u ? ll::HelloStatus::Accepted : ll::HelloStatus::BuildMismatch));
    const std::uint32_t q = c->QueryBanks(true);
    std::int32_t res = 1;
    SND_REQUIRE(f.StepUntil([&] { return c->TakeAck(q, &res); }));
    SND_CHECK_EQ(res, SND_OK);
    SND_CHECK_EQ(c->CopyBanks(nullptr, 0), 0u);
    ll::Client::Destroy(c);
  }
}

// --- item revert and read-back ------------------------------------------------------------------

SND_TEST(LivelinkV11, RevertValuePutsOneCellBack) {
  Session s;
  SND_REQUIRE(s.Ready());
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -6.0f))), SND_OK);
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Master", -3.0f))), SND_OK);
  s.f.Steps(8);
  SND_CHECK_NEAR(s.Level(), Ratio(-9.0f), 1e-4);
  SND_CHECK_EQ(s.f.rt->applier().Remembered(), 2u);
  SND_CHECK_EQ(s.Ack(s.client->RevertValue(BusVolumeCell("Sfx"))), SND_OK);
  s.f.Steps(8);
  SND_CHECK_NEAR(s.Level(), Ratio(-3.0f), 1e-4);  // Master keeps its edit
  SND_CHECK_EQ(s.f.rt->applier().Remembered(), 1u);
  SND_CHECK_EQ(s.Ack(s.client->RevertValue(BusVolumeCell("Sfx"))), SND_OK);  // nothing left to revert
  SND_CHECK_EQ(s.Ack(s.client->RevertValue(BusVolumeCell("Nope"))), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(s.Ack(s.client->RevertValue(Cell(HashId("Sfx"), bf::chunk::BUSS, 0x7777u))), SND_ERR_NOT_FOUND);
  // Editing again after a revert remembers the bank value again.
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -20.0f))), SND_OK);
  SND_CHECK_EQ(s.Ack(s.client->RevertValue(BusVolumeCell("Sfx"))), SND_OK);
  SND_CHECK_EQ(s.Ack(s.client->RevertValue(BusVolumeCell("Master"))), SND_OK);
  s.f.Steps(8);
  SND_CHECK_NEAR(s.Level(), 1.0, 1e-5);
  SND_CHECK_EQ(s.f.rt->applier().Remembered(), 0u);
}

SND_TEST(LivelinkV11, ReadCellReturnsCurrentOriginalAndOverridden) {
  Session s;
  SND_REQUIRE(s.Ready());
  ll::CellValue v{};
  SND_REQUIRE(s.WaitCellValue(s.client->ReadCell(BusVolumeCell("Sfx")), &v));
  SND_CHECK_EQ(v.result, SND_OK);
  SND_CHECK_EQ(v.cells, 1u);
  SND_CHECK_EQ(v.flags, 0u);
  SND_CHECK_EQ(v.current, Bits(0.0f));
  SND_CHECK_EQ(v.original, Bits(0.0f));
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -6.0f))), SND_OK);
  const std::uint32_t q = s.client->ReadCell(BusVolumeCell("Sfx"), true);
  SND_REQUIRE(s.WaitCellValue(q, &v));
  SND_CHECK_EQ(s.Ack(q), SND_OK);  // with the Ack bit: CellValue and Ack
  SND_CHECK_EQ(v.requestSeq, q);
  SND_CHECK_EQ(v.current, Bits(-6.0f));
  SND_CHECK_EQ(v.original, Bits(0.0f));
  SND_CHECK_EQ(v.flags, static_cast<std::uint32_t>(ll::kCellOverridden));
  SND_REQUIRE(s.WaitCellValue(s.client->ReadCell(BusVolumeCell("Nope")), &v));
  SND_CHECK_EQ(v.result, SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(v.cells, 0u);
  // A u32 column (channels) reads as its raw value.
  SND_REQUIRE(s.WaitCellValue(s.client->ReadCell(Cell(HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_channels)), &v));
  SND_CHECK_EQ(v.result, SND_OK);
  SND_CHECK_EQ(v.current, 2u);
  // A malformed address is answered at once (CellValue with the refusal).
  SND_REQUIRE(s.WaitCellValue(s.client->ReadCell(Cell(HashId("Sfx"), bf::chunk::WAVE, 1)), &v));
  SND_CHECK_EQ(v.result, SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(s.f.server->Stats().readsAnswered, 4u);  // the refused one is answered at once, not counted
}

// Two banks hold the shared bus "Sfx" (one cell each): a read reports both and whether
// they agree; a ReplaceChunk of one bank makes them differ.
SND_TEST(LivelinkV11, ReadCellAcrossBanks) {
  Session s;
  indep::BankOpts two;
  two.name = "Two";
  s.f.Load(indep::IndepBank(two), "two.sndb");
  SND_REQUIRE(s.Ready());
  ll::CellValue v{};
  SND_REQUIRE(s.WaitCellValue(s.client->ReadCell(BusVolumeCell("Sfx")), &v));
  SND_CHECK_EQ(v.cells, 2u);
  SND_CHECK_EQ(v.flags, 0u);
  bt::TableBuilder bu(bf::chunk::BUSS);
  {
    namespace c = bf::col;
    std::uint32_t r = bu.AddRow();
    bu.U32(c::BUSS_id, r, HashId("Sfx"));
    bu.U32(c::BUSS_parent, r, 1u);
    bu.U32(c::BUSS_channels, r, 2u);
    bu.F32(c::BUSS_volumeDb, r, -10.0f);
    r = bu.AddRow();
    bu.U32(c::BUSS_id, r, HashId("Master"));
    bu.U32(c::BUSS_parent, r, bf::kNone);
    bu.U32(c::BUSS_channels, r, 2u);
    bu.U32(c::BUSS_flags, r, bf::kBusMaster);
  }
  const std::vector<std::uint8_t> chunk = bu.Serialize();
  SND_CHECK_EQ(s.Ack(s.client->ReplaceChunk(HashId("Two"), bf::chunk::BUSS, chunk.data(), chunk.size())), SND_OK);
  SND_REQUIRE(s.WaitCellValue(s.client->ReadCell(BusVolumeCell("Sfx")), &v));
  SND_CHECK_EQ(v.cells, 2u);
  SND_CHECK((v.flags & ll::kCellsDiffer) != 0u);
}

// --- ReplaceChunk: what it accepts (livelink-protocol.md §8.3) ------------------------------------

SND_TEST(LivelinkV11, ReplaceChunkWithFewerColumnsKeepsTheOthersAndIsReverted) {
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  Session s(Config(), {}, sd);
  SND_REQUIRE(s.Ready());
  // Only the volumeDb column: id / parent / channels / flags stay as they are.
  const std::vector<std::uint8_t> chunk = BussVolumes({-6.0f, -6.0f});
  SND_CHECK_EQ(s.Ack(s.client->ReplaceChunk(kBankId, bf::chunk::BUSS, chunk.data(), chunk.size())), SND_OK);
  s.f.Steps(8);
  SND_CHECK_NEAR(s.Level(), Ratio(-12.0f), 1e-4);
  ll::CellValue v{};
  SND_REQUIRE(s.WaitCellValue(s.client->ReadCell(Cell(HashId("Master"), bf::chunk::BUSS, bf::col::BUSS_flags)), &v));
  SND_CHECK_EQ(v.current, static_cast<std::uint64_t>(bf::kBusMaster));
  // Item revert of one replaced cell, then the disconnect revert of the rest.
  SND_CHECK_EQ(s.Ack(s.client->RevertValue(BusVolumeCell("Master"))), SND_OK);
  s.f.Steps(8);
  SND_CHECK_NEAR(s.Level(), Ratio(-6.0f), 1e-4);
  s.client->Disconnect();
  SND_CHECK(s.f.StepUntil([&] { return s.f.rt->applier().Remembered() == 0u; }));
  s.f.Steps(8);
  SND_CHECK_NEAR(s.Level(), 1.0, 1e-5);
}

// --- audition ---------------------------------------------------------------------------------------

SND_TEST(LivelinkV11, StopAllStopsOnlyAuditions) {
  ll::RuntimeDesc rd;
  rd.maxAuditions = 2;
  Session s(Config(), rd);
  SND_REQUIRE(s.Ready());
  SND_CHECK_EQ(s.Ack(s.client->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  SND_CHECK(s.f.StepUntil([&] { return s.Level() > 1.5; }));
  s.f.Steps(4);
  SND_CHECK_NEAR(s.Level(), 2.0, 1e-4);
  SND_CHECK_EQ(s.Ack(s.client->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
  SND_CHECK(s.f.StepUntil([&] { return s.Level() > 2.5; }));
  // The place limit (RuntimeDesc::maxAuditions).
  SND_CHECK_EQ(s.Ack(s.client->Audition(HashId("Tone"), nullptr, false, true)), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(s.f.rt->ActiveAuditions(), 2u);
  SND_CHECK_EQ(s.Ack(s.client->StopAuditions(true)), SND_OK);
  SND_CHECK(s.f.StepUntil([&] { return s.f.rt->ActiveAuditions() == 0u; }));
  s.f.Steps(8);
  SND_CHECK_NEAR(s.Level(), 1.0, 1e-4);  // the game's own "Tone" keeps playing
  SND_CHECK_EQ(s.f.server->Stats().auditionsStopped, 2u);
  // A positioned one-shot after the stop, and a stop with nothing to stop.
  const float pos[3] = {0.0f, 0.0f, 0.0f};
  SND_CHECK_EQ(s.Ack(s.client->Audition(HashId("Blip"), pos, false, true)), SND_OK);
  SND_CHECK(s.f.StepUntil([&] { return s.f.rt->ActiveAuditions() == 0u; }));  // Blip ends by itself
  SND_CHECK_EQ(s.Ack(s.client->StopAuditions(true)), SND_OK);
  SND_CHECK_EQ(s.f.server->Stats().auditionsStopped, 2u);
}

// --- structural edits ------------------------------------------------------------------------------

SND_TEST(LivelinkV11, StructEditWithoutCoreSupportIsUnsupportedAndGivesTheReserveBack) {
  Session s;
  SND_REQUIRE(s.Ready());
  const std::uint64_t addBus[2] = {HashId("Master"), 2u};
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddBus, HashId("New"), addBus, 2)), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(s.f.rt->ReserveUsage().buses, 0u);
  SND_CHECK_EQ(s.f.rt->ReserveUsage().bytes, 0u);
  // The argument rules.
  const std::uint64_t badChannels[2] = {HashId("Master"), 9u};
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddBus, HashId("New"), badChannels, 2)), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::RemoveBus, HashId("Sfx"), addBus, 1)), SND_ERR_INVALID_ARGUMENT);
  // Without the Ack bit the refusal comes as Error with the reason.
  s.client->ClearErrors();
  SND_CHECK(s.client->StructEdit(ll::StructOp::AddBus, HashId("New"), addBus, 2, false) != 0u);
  ll::Error e[4];
  SND_REQUIRE(s.f.StepUntil([&] { return s.client->CopyErrors(e, 4) >= 1u; }));
  SND_CHECK_EQ(e[0].code, SND_ERR_UNSUPPORTED);
  SND_CHECK(std::strstr(e[0].message, "rebuild") != nullptr);
}

SND_TEST(LivelinkV11, StructEditsCountTheDevReserveAndAreRevertedNewestFirst) {
  snd_config cfg = Config();
  cfg.dev_reserve.buses = 1;
  cfg.dev_reserve.effects = 4;
  cfg.dev_reserve.bytes = 4096;
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  Session s(cfg, rd, sd);
  SND_REQUIRE(s.Ready());
  const std::uint64_t addBus[3] = {HashId("Master"), 2u, Bits(-3.0f)};
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddBus, HashId("New"), addBus, 3)), SND_OK);
  ll::DevReserveUsage u = s.f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 1u);
  SND_CHECK_EQ(u.bytes, ll::kStructEditBaseBytes + 24u);
  SND_CHECK_EQ(u.busesLimit, 1u);
  // Over the reserve: OUT_OF_BUDGET (the tool rebuilds and hot reloads); the editor is not asked.
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddBus, HashId("New2"), addBus, 2)), SND_ERR_OUT_OF_BUDGET);
  const std::uint64_t addFx[4] = {0xABCDu, ~0u, Bits(0.5f), Bits(1.0f)};
  // On a bank bus (Sfx), so that removing the live bus "New" does not take them away (D-172).
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddEffect, HashId("Sfx"), addFx, 4)), SND_OK);
  const std::uint64_t send[3] = {HashId("Master"), Bits(-12.0f), 1u};
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddSend, HashId("Sfx"), send, 3)), SND_OK);
  u = s.f.rt->ReserveUsage();
  SND_CHECK_EQ(u.effects, 1u);
  SND_CHECK_EQ(u.bytes, 3u * ll::kStructEditBaseBytes + 8u * (3u + 4u + 3u));
  // Removing the live bus gives its place back; adding it again fits.
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::RemoveBus, HashId("New"), nullptr, 0)), SND_OK);
  SND_CHECK_EQ(s.f.rt->ReserveUsage().buses, 0u);
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddBus, HashId("New3"), addBus, 2)), SND_OK);
  SND_REQUIRE_EQ(ed.calls.size(), 5u);
  SND_CHECK(ed.calls[0].op == ll::StructOp::AddBus && ed.calls[0].target == HashId("New") && ed.calls[0].argCount == 3u &&
            ed.calls[0].arg0 == HashId("Master"));
  SND_CHECK(ed.calls[3].op == ll::StructOp::RemoveBus && !ed.calls[3].undo);
  // An editor refusal gives the reserve back.
  ed.answer = SND_ERR_NOT_FOUND;
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddEffect, HashId("Nope"), addFx, 2)), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(s.f.rt->ReserveUsage().effects, 1u);
  ed.answer = SND_OK;
  // The disconnect revert undoes the edits still standing, newest first: New3, the send, the effect.
  ed.calls.clear();
  s.client->Disconnect();
  SND_CHECK(s.f.StepUntil([&] { return ed.calls.size() == 3u; }));
  SND_REQUIRE_EQ(ed.calls.size(), 3u);
  SND_CHECK(ed.calls[0].undo && ed.calls[0].op == ll::StructOp::AddBus && ed.calls[0].target == HashId("New3"));
  SND_CHECK(ed.calls[1].undo && ed.calls[1].op == ll::StructOp::AddSend);
  SND_CHECK(ed.calls[2].undo && ed.calls[2].op == ll::StructOp::AddEffect);
  u = s.f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 0u);
  SND_CHECK_EQ(u.effects, 0u);
  SND_CHECK_EQ(u.bytes, 0u);
}

// --- hot reload -----------------------------------------------------------------------------------

SND_TEST(LivelinkV11, ReloadBankReplacesTheRegistrationAndForgetsItsEdits) {
  Session s;
  SND_REQUIRE(s.Ready());
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -6.0f))), SND_OK);
  s.f.Steps(8);
  SND_CHECK_NEAR(s.Level(), Ratio(-6.0f), 1e-4);
  indep::BankOpts rebuilt;  // same bank id ("Live"), another build
  rebuilt.hash = 0xABCDu;
  s.f.fs.AddFile("Live2.sndb", indep::IndepBank(rebuilt));
  // Edits wait while a reload runs (nothing is rendered here, so it cannot finish yet).
  const std::uint32_t q = s.client->ReloadBank(kBankId, 0, "Live2.sndb");
  const std::uint32_t edit = s.client->SetValue(BusVolume("Sfx", -1.0f));
  std::int32_t r = 1;
  SND_REQUIRE(s.client->WaitAck(edit, 2000, &r));
  SND_CHECK_EQ(r, SND_ERR_NOT_READY);
  SND_CHECK_EQ(s.Ack(q), SND_OK);
  ll::ReloadStatus st[8];
  const std::uint32_t n = s.client->CopyReloadStatus(st, 8);
  SND_REQUIRE_EQ(n, 3u);
  SND_CHECK_EQ(st[0].phase, static_cast<std::uint32_t>(ll::ReloadPhase::Unloading));
  SND_CHECK_EQ(st[1].phase, static_cast<std::uint32_t>(ll::ReloadPhase::Loading));
  SND_CHECK_EQ(st[2].phase, static_cast<std::uint32_t>(ll::ReloadPhase::Done));
  SND_CHECK_EQ(st[2].requestSeq, q);
  SND_CHECK_EQ(st[2].bankId, kBankId);
  SND_CHECK(st[2].bank != 0u);
  SND_CHECK_EQ(s.f.sys->GetBankState(snd_bank_handle{st[2].bank}), SND_BANK_STATE_LOADED);
  // The bank's sounds were stopped (StopAndUnload); the new copy carries the rebuilt hash.
  s.f.Steps(8);
  SND_CHECK_NEAR(s.f.LastLeft(), 0.0f, 1e-7);
  s.client->QueryBanks();
  SND_REQUIRE(s.f.StepUntil([&] { return s.client->Sequences().bankLists >= 1u; }));
  ll::BankEntry b[2];
  SND_REQUIRE_EQ(s.client->CopyBanks(b, 2), 1u);
  SND_CHECK_EQ(b[0].projectBuildHash, 0xABCDu);
  // The old registration's edit is gone: the bank value (0 dB) plays.
  ll::CellValue v{};
  SND_REQUIRE(s.WaitCellValue(s.client->ReadCell(BusVolumeCell("Sfx")), &v));
  SND_CHECK_EQ(v.current, Bits(0.0f));
  SND_CHECK_EQ(v.flags, 0u);
  s.f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  s.f.Steps(16);
  SND_CHECK_NEAR(s.Level(), 1.0, 1e-4);
  SND_CHECK_EQ(s.f.server->Stats().reloadsDone, 1u);
}

SND_TEST(LivelinkV11, ReloadBankFailuresAndLoadIfAbsent) {
  Session s;
  SND_REQUIRE(s.Ready());
  SND_CHECK_EQ(s.Ack(s.client->ReloadBank(HashId("Two"), 0, "two.sndb")), SND_ERR_NOT_FOUND);
  // The file is missing: unloaded, then the load fails (the decided behaviour: the bank is gone).
  SND_CHECK_EQ(s.Ack(s.client->ReloadBank(kBankId, 0, "missing.sndb")), SND_ERR_IO);
  ll::ReloadStatus st[8];
  const std::uint32_t n = s.client->CopyReloadStatus(st, 8);
  SND_REQUIRE(n >= 1u);
  SND_CHECK_EQ(st[n - 1].phase, static_cast<std::uint32_t>(ll::ReloadPhase::Failed));
  SND_CHECK_EQ(st[n - 1].result, SND_ERR_IO);
  // Load a bank that is not loaded (kReloadLoadIfAbsent).
  indep::BankOpts two;
  two.name = "Two";
  s.f.fs.AddFile("two.sndb", indep::IndepBank(two));
  SND_CHECK_EQ(s.Ack(s.client->ReloadBank(HashId("Two"), 0, "two.sndb", true)), SND_OK);
  SND_CHECK_EQ(s.f.server->Stats().reloadsDone, 1u);
  SND_CHECK_EQ(s.f.server->Stats().reloadsFailed, 1u);
  // Malformed requests.
  const ll::ReloadBank bad{kBankId, 0, 0, 0};
  const std::uint32_t q = s.client->SendRaw(static_cast<std::uint16_t>(ll::MessageType::ReloadBank), ll::kFrameWantsAck, &bad,
                                            sizeof(bad));
  SND_CHECK_EQ(s.Ack(q), SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(LivelinkV11, ReloadFinishesWhenTheToolGoesAway) {
  Session s;
  SND_REQUIRE(s.Ready());
  indep::BankOpts rebuilt;
  rebuilt.hash = 0x77u;
  s.f.fs.AddFile("Live2.sndb", indep::IndepBank(rebuilt));
  SND_REQUIRE(s.client->ReloadBank(kBankId, 0, "Live2.sndb") != 0u);
  ll::ReloadStatus st[4];
  SND_REQUIRE(s.f.StepUntil([&] { return s.client->CopyReloadStatus(st, 4) >= 1u; }));  // the game started it
  s.client->Disconnect();
  SND_REQUIRE(s.f.StepUntil([&] { return !s.f.server->Connected(); }));
  SND_CHECK(s.f.StepUntil([&] { return s.f.server->Stats().reloadsDone == 1u; }));
  SND_REQUIRE(s.Ready());  // the client reconnects
  s.client->QueryBanks();
  SND_REQUIRE(s.f.StepUntil([&] {
    ll::BankEntry b[2];
    return s.client->CopyBanks(b, 2) == 1u && b[0].projectBuildHash == 0x77u;
  }));
}

// --- profiler streams and update numbers -------------------------------------------------------

SND_TEST(LivelinkV11, StreamListBlockDetailAndTheBudgetDefinition) {
  Session s;
  SND_REQUIRE(s.Ready());
  s.client->SetProfilerMask(ll::kProfTiming | ll::kProfStreamList | ll::kProfBlockDetail, 60);
  SND_REQUIRE(s.f.StepUntil([&] {
    const ll::ClientSequences q = s.client->Sequences();
    return q.streamLists >= 2u && q.details >= 2u && q.timings >= 2u;
  }));
  ll::StreamInfo si[8];
  const std::uint32_t n = s.client->CopyStreams(si, 8);
  SND_REQUIRE_EQ(n, Config().max_streams);
  for (std::uint32_t i = 0; i < n; ++i) {
    SND_CHECK_EQ(si[i].slot, i);
    SND_CHECK_EQ(si[i].state, static_cast<std::uint32_t>(ll::StreamState::Idle));  // resident asset: no stream
    SND_CHECK_EQ(si[i].stalls, 0u);
  }
  // budgetMicroseconds = block duration * tuning.load_target_percent / 100 (2.4).
  const snd_config cfg = Config();
  const float budget = static_cast<float>(1e6 * cfg.block_size / cfg.sample_rate * cfg.tuning.load_target_percent / 100.0);
  ll::BlockDetailSample d[4];
  SND_REQUIRE(s.client->CopyBlockDetails(d, 4) >= 1u);
  SND_CHECK_EQ(d[0].detail.budgetMicroseconds, budget);
  SND_CHECK(d[0].detail.totalMicroseconds >= d[0].detail.logicMicroseconds);
  SND_CHECK_EQ(d[0].detail.lodLevel, 0u);
  SND_CHECK(d[0].detail.renderedVoices >= 1u);
  ll::TimingSample t[4];
  SND_REQUIRE(s.client->CopyTimings(t, 4) >= 1u);
  SND_CHECK_EQ(t[0].timing.budgetMicroseconds, budget);
}

SND_TEST(LivelinkV11, UpdateNumbersTellNewAndMissedEntries) {
  ll::ServerDesc sd;
  sd.timingHz = 200;
  Session s(Config(), {}, sd, kProjectHash, false, true, 1);  // 1 s of history: 120 timings kept
  SND_REQUIRE(s.Ready());
  const ll::ClientSequences before = s.client->Sequences();
  SND_CHECK_EQ(before.timings, 0u);
  s.client->SetProfilerMask(ll::kProfTiming, 30);
  SND_REQUIRE(s.f.StepUntil([&] { return s.client->Sequences().timings >= 5u; }));
  ll::TimingSample t[256];
  std::uint64_t first = 0;
  const std::uint64_t seen = s.client->Sequences().timings;
  std::uint32_t n = s.client->CopyTimingsSince(0, t, 256, &first);
  SND_CHECK_EQ(first, 1u);
  SND_CHECK(n >= seen);
  // Nothing new since the last number copied.
  const std::uint64_t last = first + n - 1u;
  const std::uint32_t again = s.client->CopyTimingsSince(last, t, 256, &first);
  SND_CHECK(again == 0u || first == last + 1u);
  // More than the history holds (120): the oldest are missed.
  SND_REQUIRE(s.f.StepUntil([&] { return s.client->Sequences().timings > last + 130u; }, 30000));
  const std::uint64_t now = s.client->Sequences().timings;
  n = s.client->CopyTimingsSince(last, t, 256, &first);
  SND_CHECK(first > last + 1u);           // missed entries
  SND_CHECK(first + 120u >= now + 1u);    // the oldest one kept
  SND_CHECK(n >= 120u);
}

// D-172: removing a live bus also takes away the live effects on it and the live sends from it
// (their whole cost comes back, they leave the revert log, the editor gets no undo for them);
// an edit the revert log cannot hold is refused at acceptance (LIMIT_REACHED, takes nothing).
SND_TEST(LivelinkV11, RemovingALiveBusTakesItsLiveEffectsAndTheLogIsReserved) {
  snd_config cfg = Config();
  cfg.dev_reserve.buses = 4;
  cfg.dev_reserve.effects = 4;
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  rd.maxStructLog = 3;
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  Session s(cfg, rd, sd);
  SND_REQUIRE(s.Ready());
  const std::uint64_t addBus[2] = {HashId("Master"), 2u};
  const std::uint64_t fx[2] = {0xABCDu, ~0u};
  const std::uint64_t send[2] = {HashId("Master"), Bits(-6.0f)};
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddBus, HashId("B1"), addBus, 2)), SND_OK);
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddEffect, HashId("B1"), fx, 2)), SND_OK);
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddSend, HashId("B1"), send, 2)), SND_OK);
  SND_CHECK_EQ(s.f.rt->StructLogPlaces(), 3u);
  // The log is full: refused before the reserve and the editor.
  const ll::DevReserveUsage full = s.f.rt->ReserveUsage();
  const std::size_t calls = ed.calls.size();
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddBus, HashId("B2"), addBus, 2)), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(ed.calls.size(), calls);
  SND_CHECK_EQ(s.f.rt->ReserveUsage().buses, full.buses);
  SND_CHECK_EQ(s.f.rt->ReserveUsage().bytes, full.bytes);
  SND_CHECK_EQ(s.f.rt->Drops().structLog, 0u);
  // Make room: the log holds 3, a remove needs a place too, so first an edit is refused...
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::RemoveBus, HashId("B1"), nullptr, 0)), SND_ERR_LIMIT_REACHED);
  // ...with a larger log, removing B1 frees everything that went with it.
  ll::RuntimeDesc rd2 = rd;
  rd2.maxStructLog = 4;
  Session t(cfg, rd2, sd);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.client->StructEdit(ll::StructOp::AddBus, HashId("B1"), addBus, 2)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.client->StructEdit(ll::StructOp::AddEffect, HashId("B1"), fx, 2)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.client->StructEdit(ll::StructOp::AddSend, HashId("B1"), send, 2)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.client->StructEdit(ll::StructOp::RemoveBus, HashId("B1"), nullptr, 0)), SND_OK);
  const ll::DevReserveUsage u = t.f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 0u);
  SND_CHECK_EQ(u.effects, 0u);
  SND_CHECK_EQ(u.bytes, 0u);
  SND_CHECK_EQ(t.f.rt->StructLogPlaces(), 0u);
  ed.calls.clear();
  t.client->Disconnect();
  SND_REQUIRE(t.f.StepUntil([&] { return !t.f.server->Connected(); }));
  t.f.Steps(16);
  SND_CHECK_EQ(ed.calls.size(), 0u);  // nothing left to undo
  SND_CHECK_EQ(t.f.rt->Drops().structLog, 0u);
}
