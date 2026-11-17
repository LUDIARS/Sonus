// Live link end to end over the in-memory transport: server and client in one
// process with a real core System. Analytic checks: the bank's "Tone" event
// plays a constant, so the master output scales exactly with the Sfx bus
// volume (dB -> gain) the tool overrides.
#include <cmath>

#include "livelink_fixture.hpp"

using namespace lltest;
using snd::HashId;

namespace {

double Ratio(float db) { return std::pow(10.0, db / 20.0); }

struct Session {
  Fixture f;
  ll::Client* client = nullptr;
  float base = 0;
  explicit Session(ll::ServerDesc sd = {}, std::uint64_t toolHash = kProjectHash) {
    f.Load(DevBank());
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    f.Steps(16);
    base = f.LastLeft();
    SND_REQUIRE(base > 0.05f);
    SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
    ll::ClientDesc cd = MemoryClient(f.link, toolHash);
    SND_REQUIRE_EQ(ll::Client::Create(cd, &client), SND_OK);
    SND_REQUIRE_EQ(client->Start(), SND_OK);
  }
  ~Session() {
    ll::Client::Destroy(client);
    f.StopServer();
  }
  bool Ready() {
    return f.StepUntil([&] { return client->State() == ll::ClientState::Ready && f.server->Connected(); });
  }
  // Waits for the Ack of `seq` while rendering (SetValue is acknowledged once applied).
  std::int32_t Ack(std::uint32_t seq) {
    std::int32_t r = 1;
    if (seq == 0u) return 2;
    f.StepUntil([&] { return client->TakeAck(seq, &r); });
    return r;
  }
};

}  // namespace

SND_TEST(LivelinkOverride, BusVolumeChangesTheLevelAndKeepsItAfterDisconnect) {
  Session s;
  SND_REQUIRE(s.Ready());
  SND_CHECK_EQ(s.client->LastHelloReply().status, static_cast<std::uint32_t>(ll::HelloStatus::Accepted));
  SND_CHECK_EQ(s.client->LastHelloReply().runtimeBuildHash, kProjectHash);
  SND_CHECK_EQ(s.client->LastHelloReply().sampleRate, 48000u);
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -6.0f))), SND_OK);
  s.f.Steps(8);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, Ratio(-6.0f), 1e-4);
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Master", -12.0f))), SND_OK);
  s.f.Steps(8);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, Ratio(-18.0f), 1e-4);
  // 11.2: the overrides stay when the tool goes away (default).
  s.client->Stop();
  SND_CHECK(s.f.StepUntil([&] { return !s.f.server->Connected(); }));
  s.f.Steps(16);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, Ratio(-18.0f), 1e-4);
  SND_CHECK_EQ(s.f.rt->applier().Remembered(), 2u);
}

SND_TEST(LivelinkOverride, RevertOnDisconnectRestoresTheBankValues) {
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  Session s(sd);
  SND_REQUIRE(s.Ready());
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -6.0f))), SND_OK);
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -20.0f))), SND_OK);  // first write keeps the original
  s.f.Steps(8);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, Ratio(-20.0f), 1e-4);
  s.client->Disconnect();  // the server sees the stream close
  SND_CHECK(s.f.StepUntil([&] { return s.f.rt->applier().Remembered() == 0u; }));
  s.f.Steps(8);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, 1.0, 1e-5);
  // The client reconnects by itself and can edit again.
  SND_CHECK(s.f.StepUntil([&] { return s.client->Stats().connects == 2u && s.f.server->Connected(); }));
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -6.0f))), SND_OK);
  s.f.Steps(8);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, Ratio(-6.0f), 1e-4);
}

SND_TEST(LivelinkOverride, SetValueErrorsAreAnsweredAndChangeNothing) {
  Session s;
  SND_REQUIRE(s.Ready());
  ll::SetValue v = BusVolume("Sfx", -6.0f);
  v.runtimeId = HashId("NoSuchBus");
  SND_CHECK_EQ(s.Ack(s.client->SetValue(v)), SND_ERR_NOT_FOUND);
  v = BusVolume("Sfx", 0.0f);
  const float nan = std::nanf("");
  std::uint32_t bits = 0;
  std::memcpy(&bits, &nan, 4);
  v.value = bits;
  SND_CHECK_EQ(s.Ack(s.client->SetValue(v)), SND_ERR_INVALID_ARGUMENT);
  v = BusVolume("Sfx", 0.0f);
  v.column = bf::col::BUSS_parent;  // a row reference: not live-editable
  v.valueType = static_cast<std::uint32_t>(ll::ValueType::U32);
  v.value = 0;
  SND_CHECK_EQ(s.Ack(s.client->SetValue(v)), SND_ERR_UNSUPPORTED);
  v.column = 99;
  SND_CHECK_EQ(s.Ack(s.client->SetValue(v)), SND_ERR_NOT_FOUND);
  v = BusVolume("Sfx", -6.0f);
  v.valueType = static_cast<std::uint32_t>(ll::ValueType::U32);  // f32 column needs F32
  SND_CHECK_EQ(s.Ack(s.client->SetValue(v)), SND_ERR_INVALID_ARGUMENT);
  v = BusVolume("Sfx", -6.0f);
  v.index = 1;  // id tables take index 0
  SND_CHECK_EQ(s.Ack(s.client->SetValue(v)), SND_ERR_NOT_FOUND);
  s.f.Steps(4);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, 1.0, 1e-6);
  SND_CHECK_EQ(s.f.rt->applier().Remembered(), 0u);
  // An editable u32 with a range: VLGR has no rows here, EVNT.maxInstances does.
  ll::SetValue m{};
  m.runtimeId = HashId("Tone");
  m.chunk = bf::chunk::EVNT;
  m.column = bf::col::EVNT_maxInstances;
  m.valueType = static_cast<std::uint32_t>(ll::ValueType::U32);
  m.value = 3;
  SND_CHECK_EQ(s.Ack(s.client->SetValue(m)), SND_OK);
  m.column = bf::col::EVNT_virtualReturn;
  m.value = 7;  // 0..2
  SND_CHECK_EQ(s.Ack(s.client->SetValue(m)), SND_ERR_INVALID_ARGUMENT);
  m.column = bf::col::EVNT_priority;  // read by game threads (D-10): refused
  m.value = 1;
  SND_CHECK_EQ(s.Ack(s.client->SetValue(m)), SND_ERR_UNSUPPORTED);
  // List tables through their owner: the clip of track 0 of "Tone".
  ll::SetValue c{};
  c.runtimeId = HashId("Tone");
  c.chunk = bf::chunk::CLIP;
  c.column = bf::col::CLIP_volumeDb;
  c.valueType = static_cast<std::uint32_t>(ll::ValueType::F32);
  const float db = -3.0f;
  std::memcpy(&bits, &db, 4);
  c.value = bits;
  c.index = (0u << 16) | 0u;
  SND_CHECK_EQ(s.Ack(s.client->SetValue(c)), SND_OK);
  c.index = (0u << 16) | 1u;  // no second clip
  SND_CHECK_EQ(s.Ack(s.client->SetValue(c)), SND_ERR_NOT_FOUND);
}

SND_TEST(LivelinkOverride, ClipVolumeEditAffectsNewVoices) {
  Session s;
  SND_REQUIRE(s.Ready());
  ll::SetValue c{};
  c.runtimeId = HashId("Tone");
  c.chunk = bf::chunk::CLIP;
  c.column = bf::col::CLIP_volumeDb;
  c.valueType = static_cast<std::uint32_t>(ll::ValueType::F32);
  const float db = -10.0f;
  std::uint32_t bits = 0;
  std::memcpy(&bits, &db, 4);
  c.value = bits;
  SND_CHECK_EQ(s.Ack(s.client->SetValue(c)), SND_OK);
  // The playing voice keeps its gain; a second "Tone" starts at -10 dB: total 1 + 0.316.
  s.f.Steps(4);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, 1.0, 1e-5);
  s.f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  s.f.Steps(16);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, 1.0 + Ratio(-10.0f), 1e-4);
}

SND_TEST(LivelinkOverride, ReplaceChunkRewritesValuesAndRefusesLayoutChanges) {
  Session s;
  SND_REQUIRE(s.Ready());
  // Same BUSS layout, Sfx at -6 dB.
  auto buss = [](float sfxDb, bool extraRow) {
    namespace c = bf::col;
    bt::TableBuilder bu(bf::chunk::BUSS);
    std::uint32_t r = bu.AddRow();
    bu.U32(c::BUSS_id, r, HashId("Sfx"));
    bu.U32(c::BUSS_parent, r, 1u);
    bu.U32(c::BUSS_channels, r, 2u);
    bu.F32(c::BUSS_volumeDb, r, sfxDb);
    r = bu.AddRow();
    bu.U32(c::BUSS_id, r, HashId("Master"));
    bu.U32(c::BUSS_parent, r, bf::kNone);
    bu.U32(c::BUSS_channels, r, 2u);
    bu.U32(c::BUSS_flags, r, bf::kBusMaster);
    if (extraRow) bu.AddRow();
    return bu.Serialize();
  };
  std::vector<std::uint8_t> chunk = buss(-6.0f, false);
  SND_CHECK_EQ(s.Ack(s.client->ReplaceChunk(kBankId, bf::chunk::BUSS, chunk.data(), chunk.size())), SND_OK);
  s.f.Steps(8);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, Ratio(-6.0f), 1e-4);
  chunk = buss(-6.0f, true);
  SND_CHECK_EQ(s.Ack(s.client->ReplaceChunk(kBankId, bf::chunk::BUSS, chunk.data(), chunk.size())), SND_ERR_UNSUPPORTED);
  // A changed reference column is refused as a whole (no partial write).
  chunk = buss(-12.0f, false);
  bt::TableBuilder bad(bf::chunk::BUSS);
  {
    namespace c = bf::col;
    std::uint32_t r = bad.AddRow();
    bad.U32(c::BUSS_id, r, HashId("Sfx"));
    bad.U32(c::BUSS_parent, r, bf::kNone);  // was 1
    bad.F32(c::BUSS_volumeDb, r, -12.0f);
    r = bad.AddRow();
    bad.U32(c::BUSS_id, r, HashId("Master"));
    bad.U32(c::BUSS_parent, r, bf::kNone);
    bad.U32(c::BUSS_flags, r, bf::kBusMaster);
  }
  chunk = bad.Serialize();
  SND_CHECK_EQ(s.Ack(s.client->ReplaceChunk(kBankId, bf::chunk::BUSS, chunk.data(), chunk.size())), SND_ERR_UNSUPPORTED);
  s.f.Steps(4);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, Ratio(-6.0f), 1e-4);
  // Unknown bank, blob chunk, malformed table.
  chunk = buss(0.0f, false);
  SND_CHECK_EQ(s.Ack(s.client->ReplaceChunk(HashId("Other"), bf::chunk::BUSS, chunk.data(), chunk.size())),
               SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(s.Ack(s.client->ReplaceChunk(kBankId, bf::chunk::WAVE, chunk.data(), chunk.size())), SND_ERR_UNSUPPORTED);
  chunk.resize(20);
  SND_CHECK_EQ(s.Ack(s.client->ReplaceChunk(kBankId, bf::chunk::BUSS, chunk.data(), chunk.size())),
               SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(LivelinkOverride, ReleaseBanksHaveNoOverrideLayer) {
  Fixture f;
  f.Load(DevBank(kProjectHash, false));
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(MemoryClient(f.link), &c), SND_OK);
  c->Start();
  SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready; }));
  const std::uint32_t seq = c->SetValue(BusVolume("Sfx", -6.0f));
  std::int32_t r = 1;
  SND_CHECK(f.StepUntil([&] { return c->TakeAck(seq, &r); }));
  SND_CHECK_EQ(r, SND_ERR_UNSUPPORTED);
  ll::Client::Destroy(c);
}

SND_TEST(LivelinkOverride, ParameterAndStateOverridesAuditionAndClear) {
  Session s;
  SND_REQUIRE(s.Ready());
  SND_CHECK_EQ(s.Ack(s.client->OverrideParam(HashId("Knob"), 0.75f, true, true)), SND_OK);
  SND_CHECK_EQ(s.Ack(s.client->OverrideState(HashId("Music"), HashId("Calm"), true, true)), SND_OK);
  SND_CHECK_EQ(s.Ack(s.client->OverrideParam(0, 0.75f, true, true)), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s.Ack(s.client->ClearOverrides(true)), SND_OK);
  // Audition: one-shot playback of "Blip" (2400 frames) adds kDc on top of "Tone", then ends.
  SND_CHECK_EQ(s.Ack(s.client->Audition(HashId("Blip"), nullptr, false, true)), SND_OK);
  SND_CHECK(s.f.StepUntil([&] { return s.f.LastLeft() / s.base > 1.5; }));
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, 2.0, 1e-4);
  s.f.Steps(20);
  SND_CHECK_NEAR(s.f.LastLeft() / s.base, 1.0, 1e-4);
  SND_CHECK_EQ(s.Ack(s.client->Audition(0, nullptr, true, true)), SND_ERR_UNSUPPORTED);  // stop all: no core command
  const std::uint64_t args[2] = {1, 2};
  SND_CHECK_EQ(s.Ack(s.client->StructEdit(ll::StructOp::AddBus, HashId("Sfx"), args, 2)), SND_ERR_UNSUPPORTED);
}

SND_TEST(LivelinkOverride, BuildMismatchAndProtocolMismatch) {
  {
    Session s({}, 0x1111u);  // tool built another project
    SND_REQUIRE(s.Ready());  // SendDiffs (default)
    SND_CHECK_EQ(s.client->LastHelloReply().status, static_cast<std::uint32_t>(ll::HelloStatus::BuildMismatch));
    SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -6.0f))), SND_OK);
  }
  {
    Fixture f;
    f.Load(DevBank());
    SND_REQUIRE_EQ(f.StartServer(), SND_OK);
    ll::ClientDesc cd = MemoryClient(f.link, 0x2222u);
    cd.onMismatch = ll::SyncMode::Disconnect;
    ll::Client* c = nullptr;
    SND_REQUIRE_EQ(ll::Client::Create(cd, &c), SND_OK);
    c->Start();
    SND_CHECK(f.StepUntil([&] { return c->State() == ll::ClientState::Rejected; }));
    SND_CHECK(f.StepUntil([&] { return f.server->Stats().handshakesRejected == 1u; }));
    ll::Client::Destroy(c);
  }
  {
    Fixture f;
    f.Load(DevBank());
    SND_REQUIRE_EQ(f.StartServer(), SND_OK);
    RawPeer p;
    SND_REQUIRE(p.Connect(f.link.connector()));
    SND_REQUIRE(p.Hello(kProjectHash, 2));
    const RawPeer::Frame* r = p.WaitFor(ll::MessageType::HelloReply);
    SND_REQUIRE(r != nullptr);
    const auto hr = ll::ReadFixed<ll::HelloReply>(r->payload.data(), static_cast<std::uint32_t>(r->payload.size()));
    SND_CHECK_EQ(hr.status, static_cast<std::uint32_t>(ll::HelloStatus::RejectedProtocol));
    SND_CHECK_EQ(hr.protocolMajor, ll::kProtocolMajor);
    SND_CHECK(f.StepUntil([&] { p.Pump(1); return p.closed; }));
  }
}

SND_TEST(LivelinkOverride, CommandsBeforeTheHandshakeCloseTheConnection) {
  Fixture f;
  f.Load(DevBank());
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  RawPeer p;
  SND_REQUIRE(p.Connect(f.link.connector()));
  const ll::SetValue v = BusVolume("Sfx", -6.0f);
  SND_REQUIRE(p.Send(ll::MessageType::SetValue, &v, sizeof(v)));
  const RawPeer::Frame* e = p.WaitFor(ll::MessageType::Error);
  SND_REQUIRE(e != nullptr);
  SND_CHECK_EQ(ll::ReadFixed<ll::Error>(e->payload.data(), 64).code, SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(f.StepUntil([&] { p.Pump(1); return p.closed; }));
  // Bad magic: Error, then closed; the server accepts the next tool.
  RawPeer q;
  SND_REQUIRE(f.StepUntil([&] { return q.Connect(f.link.connector(), 10); }));
  const std::uint8_t junk[32] = {1, 2, 3, 4};
  SND_REQUIRE(q.SendBytes(junk, sizeof(junk)));
  SND_CHECK(q.WaitFor(ll::MessageType::Error) != nullptr);
  SND_CHECK(f.StepUntil([&] { q.Pump(1); return q.closed; }));
  SND_CHECK(f.server->Stats().protocolErrors >= 2u);
}

// D-87: a Server destroyed with edits accepted but not applied; the next Server's tool
// edits the same bus before any block runs. Whatever order the two producers' arenas
// are applied in, the old session's edits are dropped (counted) and the new value wins.
SND_TEST(LivelinkOverride, OldSessionEditsNeverOverwriteTheNewServersValues) {
  Fixture f;
  f.Load(DevBank());
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(16);
  const float base = f.LastLeft();
  SND_REQUIRE(f.link.Init(snd::base::DefaultAllocator(), 1u << 20));
  auto newServer = [&] {
    ll::ServerDesc sd;
    sd.system = f.sys;
    sd.runtime = f.rt;
    sd.allocator = &snd::base::DefaultAllocator();
    sd.acceptor = &f.link.acceptor();
    ll::Server* s = nullptr;
    if (ll::Server::Create(sd, &s) != SND_OK || s->Start() != SND_OK) {
      ll::Server::Destroy(s);
      return static_cast<ll::Server*>(nullptr);
    }
    return s;
  };
  auto readyWithoutRendering = [&](ll::Client* c, ll::Server* s) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!(c->State() == ll::ClientState::Ready && s->Connected()) && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return s->Connected();
  };
  auto waitRecorded = [&](ll::Server* s, std::uint32_t n) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (s->Stats().commandsRecorded < n && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // published by the server's Flush
    return s->Stats().commandsRecorded >= n;
  };
  const ll::RuntimeDrops before = f.rt->Drops();
  // Old Server: two edits accepted, never applied while it lives.
  ll::Server* a = newServer();
  SND_REQUIRE(a != nullptr);
  ll::Client* ca = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(MemoryClient(f.link), &ca), SND_OK);
  ca->Start();
  SND_REQUIRE(readyWithoutRendering(ca, a));
  SND_REQUIRE(ca->SetValue(BusVolume("Sfx", -20.0f), true) != 0u);
  SND_REQUIRE(ca->OverrideParam(HashId("Knob"), 0.9f, true, false) != 0u);
  SND_REQUIRE(waitRecorded(a, 2u));
  ll::Server::Destroy(a);
  ll::Client::Destroy(ca);
  // New Server: its own edit of the same bus, still before any block.
  ll::Server* b = newServer();
  SND_REQUIRE(b != nullptr);
  ll::Client* cb = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(MemoryClient(f.link), &cb), SND_OK);
  cb->Start();
  SND_REQUIRE(readyWithoutRendering(cb, b));
  const std::uint32_t seq = cb->SetValue(BusVolume("Sfx", -6.0f), true);
  SND_REQUIRE(seq != 0u);
  SND_REQUIRE(waitRecorded(b, 1u));
  std::int32_t r = 1;
  SND_CHECK(f.StepUntil([&] { return cb->TakeAck(seq, &r); }));
  SND_CHECK_EQ(r, SND_OK);
  f.Steps(8);
  SND_CHECK_NEAR(f.LastLeft() / base, std::pow(10.0, -6.0 / 20.0), 1e-4);
  const ll::RuntimeDrops after = f.rt->Drops();
  SND_CHECK_EQ(after.staleEdits - before.staleEdits, 2u);  // the old SetValue and OverrideParam
  SND_CHECK_EQ(b->Stats().staleResults, 1u);               // the old SetValue's result, dropped
  SND_CHECK_EQ(f.rt->applier().Remembered(), 1u);          // only the new edit's cell
  ll::Client::Destroy(cb);
  ll::Server::Destroy(b);
}
