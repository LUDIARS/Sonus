// Fuzz (design.md 14) of protocol 1.1: malformed new tool -> game messages against a
// running server with a real core, malformed new game -> tool messages against a client,
// and mutated 1.1 recordings (Begin / Cell / Override / LiveData records) against the
// record reader and the replayer. Nothing may crash or read out of bounds (run under
// ASan / UBSan); afterwards both ends still answer. Deterministic seeds;
// SND_FUZZ_ITERATIONS scales it.
#include <cstdlib>
#include <random>

#include "livelink_v11_common.hpp"

using namespace v11;

namespace {

std::uint32_t Iterations(std::uint32_t base) {
  const char* e = std::getenv("SND_FUZZ_ITERATIONS");
  return e != nullptr ? static_cast<std::uint32_t>(std::strtoul(e, nullptr, 10)) : base;
}

struct Gen {
  std::mt19937 rng;
  explicit Gen(std::uint32_t seed) : rng(seed) {}
  std::uint32_t R() { return static_cast<std::uint32_t>(rng()); }
  std::uint32_t Below(std::uint32_t n) { return n == 0u ? 0u : R() % n; }
  // Mostly plausible values, sometimes extremes.
  std::uint64_t Value() {
    switch (Below(8)) {
      case 0: return 0;
      case 1: return ~0ull;
      case 2: return 0x7FC00000u;  // NaN
      case 3: return 0x7F800000u;  // +Inf
      case 4: return HashId("Sfx");
      case 5: return Below(10);
      default: return (std::uint64_t{R()} << 32) | R();
    }
  }
  void Mutate(std::vector<std::uint8_t>& v) {
    const std::uint32_t n = 1u + Below(6);
    for (std::uint32_t i = 0; i < n && !v.empty(); ++i) {
      switch (Below(4)) {
        case 0: v[Below(static_cast<std::uint32_t>(v.size()))] ^= static_cast<std::uint8_t>(1u << Below(8)); break;
        case 1: v[Below(static_cast<std::uint32_t>(v.size()))] = static_cast<std::uint8_t>(R()); break;
        case 2: v.resize(Below(static_cast<std::uint32_t>(v.size()) + 1u)); break;
        default: {
          const std::uint32_t at = Below(static_cast<std::uint32_t>(v.size()) + 1u);
          v.insert(v.begin() + at, Below(24), static_cast<std::uint8_t>(R()));
          break;
        }
      }
    }
  }
};

// A plausible payload of a 1.1 tool -> game message (then mutated or not).
std::vector<std::uint8_t> ToolPayload(Gen& g, ll::MessageType t) {
  std::vector<std::uint8_t> p;
  switch (t) {
    case ll::MessageType::RevertValue:
    case ll::MessageType::ReadCell:
    case ll::MessageType::SetValue: {
      ll::SetValue v = BusVolume(g.Below(2) == 0u ? "Sfx" : "Master", -3.0f);
      if (g.Below(3) == 0u) v.column = g.R();
      if (g.Below(3) == 0u) v.chunk = g.Below(2) == 0u ? bf::chunk::WAVE : g.R();
      if (g.Below(3) == 0u) v.runtimeId = g.R();
      v.index = g.Below(4) == 0u ? g.R() : 0u;
      Put(p, v);
      break;
    }
    case ll::MessageType::ReloadBank: {
      const char* paths[] = {"missing.sndb", "Live.sndb", "../x", "/abs", "a:b"};
      const char* path = paths[g.Below(5)];
      const ll::ReloadBank m{g.Below(2) == 0u ? kBankId : g.R(), g.Below(4) == 0u ? g.R() : 0u, g.Below(4),
                             static_cast<std::uint32_t>(std::strlen(path))};
      Put(p, m);
      p.insert(p.end(), path, path + std::strlen(path));
      break;
    }
    case ll::MessageType::StructEdit: {
      const std::uint32_t argc = g.Below(6);
      const ll::StructEdit m{g.Below(7), g.Below(3) == 0u ? 0u : g.R(), g.Below(8) == 0u ? g.R() : argc, 0u};
      Put(p, m);
      for (std::uint32_t i = 0; i < argc; ++i) Put(p, g.Value());
      break;
    }
    case ll::MessageType::Audition: {
      ll::Audition a{};
      a.flags = g.Below(4) == 0u ? g.R() : (g.Below(2) == 0u ? static_cast<std::uint32_t>(ll::kAuditionStopAll) : 0u);
      a.eventId = g.Below(2) == 0u ? HashId("Blip") : g.R();
      Put(p, a);
      break;
    }
    case ll::MessageType::SetProfilerMask: {
      const ll::SetProfilerMask m{g.R() & 0x3FFu, g.Below(2000)};
      Put(p, m);
      break;
    }
    default:
      break;  // QueryBanks: empty
  }
  if (g.Below(3) == 0u) g.Mutate(p);
  return p;
}

}  // namespace

SND_TEST(LivelinkV11Fuzz, ServerSurvivesMalformed11Messages) {
  Fixture f;
  f.Load(DevBank());
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  RawPeer p;
  SND_REQUIRE(f.StepUntil([&] { return p.Connect(f.link.connector(), 10); }));
  SND_REQUIRE(p.Hello());
  SND_REQUIRE(f.StepUntil([&] { return p.WaitFor(ll::MessageType::HelloReply, 5) != nullptr; }));
  Gen g(0x11F00Du);
  const ll::MessageType types[] = {ll::MessageType::RevertValue, ll::MessageType::ReadCell, ll::MessageType::QueryBanks,
                                   ll::MessageType::ReloadBank, ll::MessageType::StructEdit, ll::MessageType::Audition,
                                   ll::MessageType::SetValue, ll::MessageType::SetProfilerMask};
  const std::uint32_t iters = Iterations(1500);
  std::uint32_t sent = 0;
  for (std::uint32_t i = 0; i < iters; ++i) {
    const ll::MessageType t = types[g.Below(8)];
    const std::vector<std::uint8_t> payload = ToolPayload(g, t);
    const std::uint16_t flags = static_cast<std::uint16_t>(g.Below(2) == 0u ? 1u : 0u);  // kFrameWantsAck or none
    if (!p.Send(t, payload.data(), static_cast<std::uint32_t>(payload.size()), flags, i + 2u)) break;
    ++sent;
    if (i % 4u == 0u) f.Step();
    p.Pump(0);
    p.frames.clear();
  }
  SND_CHECK_EQ(sent, iters);
  // Still alive: a ping is answered.
  const ll::Ping ping{12345u};
  SND_REQUIRE(p.Send(ll::MessageType::Ping, &ping, sizeof(ping), 0, 99999u));
  const RawPeer::Frame* pong = nullptr;
  SND_CHECK(f.StepUntil([&] {
    pong = p.WaitFor(ll::MessageType::Pong, 5);
    return pong != nullptr;
  }, 10000));
  f.Steps(16);
  SND_CHECK(f.server->Connected());
}

SND_TEST(LivelinkV11Fuzz, ClientSurvivesMalformed11Messages) {
  ll::MemoryLink link;
  SND_REQUIRE(link.Init(snd::base::DefaultAllocator(), 1u << 20));
  ll::ClientDesc cd = MemoryClient(link);
  cd.timeoutMs = 60000;
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(cd, &c), SND_OK);
  SND_REQUIRE_EQ(c->Start(), SND_OK);
  RawGame game;
  SND_REQUIRE(game.Accept(link.acceptor()));
  SND_REQUIRE(game.WaitFor(ll::MessageType::Hello) != nullptr);
  ll::HelloReply r{};
  r.protocolMajor = 1;
  r.protocolMinor = 1;
  r.status = static_cast<std::uint32_t>(ll::HelloStatus::Accepted);
  SND_REQUIRE(game.Send(ll::MessageType::HelloReply, &r, sizeof(r)));
  SND_REQUIRE(c->WaitReady(2000));
  Gen g(0xC11E47u);
  const ll::MessageType types[] = {ll::MessageType::CellValue, ll::MessageType::BankList, ll::MessageType::StreamList,
                                   ll::MessageType::BlockDetail, ll::MessageType::ReloadStatus};
  const std::uint32_t iters = Iterations(3000);
  for (std::uint32_t i = 0; i < iters; ++i) {
    const ll::MessageType t = types[g.Below(5)];
    std::vector<std::uint8_t> p;
    if (t == ll::MessageType::BankList || t == ll::MessageType::StreamList) {
      const std::uint32_t count = g.Below(4) == 0u ? g.R() : g.Below(8);
      const std::uint32_t elem = g.Below(4) == 0u ? g.Below(64) : (t == ll::MessageType::BankList ? 32u : 16u);
      Put(p, ll::ListHeader{count, elem});
      const std::uint32_t body = g.Below(4) == 0u ? g.Below(300) : count * elem;
      for (std::uint32_t k = 0; k < body && k < 4096u; ++k) p.push_back(static_cast<std::uint8_t>(g.R()));
    } else {
      p.resize(g.Below(64));
      for (auto& b : p) b = static_cast<std::uint8_t>(g.R());
    }
    std::vector<std::uint8_t> frame(ll::kFrameHeaderBytes + p.size());
    ll::EncodeHeader(frame.data(), static_cast<std::uint16_t>(t), static_cast<std::uint16_t>(g.Below(2)),
                     static_cast<std::uint32_t>(p.size()), g.R());
    if (!p.empty()) std::memcpy(frame.data() + ll::kFrameHeaderBytes, p.data(), p.size());
    SND_REQUIRE(game.SendRawBytes(frame));
    if (i % 64u == 0u) {
      ll::BankEntry b[8];
      ll::StreamInfo s[8];
      ll::CellValue v{};
      c->CopyBanks(b, 8);
      c->CopyStreams(s, 8);
      c->TakeCell(g.R(), &v);
      (void)c->Sequences();
      game.Pump(0);
      game.frames.clear();
    }
  }
  const ll::Ping ping{777u};
  SND_REQUIRE(game.Send(ll::MessageType::Ping, &ping, sizeof(ping)));
  const RawGame::Frame* pong = game.WaitFor(ll::MessageType::Pong, 5000);
  SND_REQUIRE(pong != nullptr);
  SND_CHECK(c->State() == ll::ClientState::Ready);
  SND_CHECK(c->Sequences().cells + c->Sequences().bankLists + c->Sequences().streamLists > 0u);
  ll::Client::Destroy(c);
}

SND_TEST(LivelinkV11Fuzz, Mutated11RecordingsAreRejectedOrReplayed) {
  // A valid 1.1 recording: Lod, BankMap, Begin(MidRun), a cell, an override, then a block
  // with a LiveReplaceChunk (its data first) and a SetParam, and a block with a stop marker.
  std::vector<std::uint8_t> base;
  const std::uint64_t first = 40;
  Put(base, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, first});
  Put(base, ll::RecordBankMap{static_cast<std::uint32_t>(ll::RecordKind::BankMap), kBankId, 1u});
  Put(base, ll::RecordBegin{ll::kRecordBegin, static_cast<std::uint16_t>(ll::RecordOrigin::MidRun), 0u, first});
  Put(base, ll::RecordCell{ll::kRecordCell, kBankId, bf::chunk::BUSS, bf::col::BUSS_volumeDb, 0u, 0u, Bits(-6.0f), Bits(0.0f)});
  Put(base, ll::RecordOverride{ll::kRecordOverride, 0u, HashId("Knob"), Bits(0.5f), 0u});
  bt::TableBuilder bu(bf::chunk::BUSS);
  std::uint32_t row = bu.AddRow();
  bu.U32(bf::col::BUSS_id, row, HashId("Sfx"));
  bu.F32(bf::col::BUSS_volumeDb, row, -3.0f);
  row = bu.AddRow();
  bu.U32(bf::col::BUSS_id, row, HashId("Master"));
  const std::vector<std::uint8_t> table = bu.Serialize();
  Put(base, ll::RecordLiveData{ll::kRecordLiveData, bf::chunk::BUSS, 0x5000u, kBankId, static_cast<std::uint32_t>(table.size())});
  base.insert(base.end(), table.begin(), table.end());
  base.resize((base.size() + 7u) & ~std::size_t{7}, 0u);
  auto command = [](std::vector<std::uint8_t>& v, cmd::CommandType t, const void* payload, std::uint32_t bytes,
                    std::uint32_t wire) {
    const std::size_t at = v.size();
    v.resize(at + wire, 0u);
    const cmd::CommandHeader h{static_cast<std::uint16_t>(t), static_cast<std::uint16_t>(wire), 0u};
    std::memcpy(v.data() + at, &h, sizeof(h));
    std::memcpy(v.data() + at + sizeof(h), payload, bytes);
  };
  auto arena = [&](std::vector<std::uint8_t>& v, std::uint64_t block, const std::vector<std::uint8_t>& normal) {
    Put(v, ll::RecordArena{static_cast<std::uint32_t>(ll::RecordKind::Arena), 0u, block,
                           static_cast<std::uint32_t>(sizeof(ll::ArenaSections) + normal.size()), 0u});
    Put(v, ll::ArenaSections{static_cast<std::uint32_t>(normal.size()), 0u, 0u, 0u});
    v.insert(v.end(), normal.begin(), normal.end());
  };
  {
    std::vector<std::uint8_t> normal;
    cmd::LiveReplaceChunk rc{};
    rc.bankId = kBankId;
    rc.fourcc = bf::chunk::BUSS;
    rc.prepared = reinterpret_cast<const void*>(std::uintptr_t{0x5000});
    command(normal, cmd::CommandType::LiveReplaceChunk, &rc, sizeof(rc), cmd::kWireSize<cmd::LiveReplaceChunk>);
    cmd::SetParam sp{};
    sp.param = HashId("Knob");
    sp.value = 0.25f;
    command(normal, cmd::CommandType::SetParam, &sp, sizeof(sp), cmd::kWireSize<cmd::SetParam>);
    arena(base, first, normal);
    normal.clear();
    cmd::LiveSetValue mk{};
    mk.field = ll::kFieldAuditionStop;
    command(normal, cmd::CommandType::LiveSetValue, &mk, sizeof(mk), cmd::kWireSize<cmd::LiveSetValue>);
    arena(base, first + 3u, normal);
  }
  const snd_config cfg = Config();
  auto create = [&](const std::vector<std::uint8_t>& rec, ll::Replayer** out) {
    ll::ReplayDesc d;
    d.config = &cfg;
    d.allocator = &snd::base::DefaultAllocator();
    d.record = rec.data();
    d.bytes = rec.size();
    return ll::Replayer::Create(d, out);
  };
  {
    ll::Replayer* r = nullptr;
    SND_REQUIRE_EQ(create(base, &r), SND_OK);
    SND_CHECK_EQ(r->Info().replacedChunks, 1u);
    SND_CHECK_EQ(r->Info().startCells, 1u);
    SND_CHECK_EQ(r->Info().startOverrides, 1u);
    ll::Replayer::Destroy(r);
  }
  Gen g(0x5EC0DEu);
  const std::uint32_t iters = Iterations(20000);
  std::uint32_t accepted = 0, replayed = 0;
  for (std::uint32_t i = 0; i < iters; ++i) {
    std::vector<std::uint8_t> m = base;
    g.Mutate(m);
    ll::RecordReader rr(m.data(), m.size());
    ll::RecordItem it;
    while (rr.Next(&it)) {
    }
    ll::Replayer* r = nullptr;
    const int res = create(m, &r);
    SND_CHECK(res == SND_OK || res == SND_ERR_CORRUPT || res == SND_ERR_VERSION_MISMATCH || res == SND_ERR_OUT_OF_BUDGET);
    if (res == SND_OK) {
      ++accepted;
      // Replay some of the accepted ones on a real System (the start state and the chunk data
      // go into core; nothing may crash).
      if (replayed < 12u && r->Blocks() < 64u && g.Below(50) == 0u) {
        ++replayed;
        Fixture f(Hooks::Replay, cfg, r);
        f.Load(DevBank());
        r->Start();
        for (std::uint64_t b = 0; b < r->Blocks() + 2u; ++b) f.sys->RenderBlock(f.out.data());
        ll::ReplayDecision d[64];
        r->CopyDecisions(d, 64);
      }
      ll::Replayer::Destroy(r);
    }
  }
  SND_CHECK(accepted > 0u);
}
