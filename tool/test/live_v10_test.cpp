// A protocol 1.0 game (group P): the live link tests' hand-driven game side (RawGame) answers
// Hello with minor 0. LiveSync, the audition router and CellSync must show the 1.1 features as
// unavailable with the reason, keep using the 1.0 messages, and never wait for a 1.1 answer.
#include <chrono>
#include <thread>

#include "live_fixture.hpp"
#include "livelink_v11_common.hpp"
#include "snd/buildtool/json.hpp"
#include "snd/tool/cell_sync.hpp"
#include "snd/tool/preview.hpp"

using namespace snd::tool;
namespace ll = snd::livelink;
namespace json = snd::buildtool::json;

SND_TEST(ToolLiveV10, OneZeroGameGetsValuesAndSaysWhatIsMissing) {
  ProjectDocument doc;
  livetest::LoadLive(doc);
  LiveSyncOptions o = livetest::MakeOptions(LiveDecision::SendDiffs);
  o.writer = [](const BankImage&, std::string* path, std::string*) {
    *path = "x.sndb";
    return true;
  };
  LiveSync sync(doc, o);
  ll::MemoryLink link;
  SND_REQUIRE(link.Init(snd::base::DefaultAllocator(), 1u << 20));
  Connection conn;
  ConnectionDesc d;
  d.connector = &link.connector();
  d.profilerMask = 0;
  d.autoReconnect = false;
  SND_REQUIRE_EQ(conn.Open(sync.Prepare(d)), SND_OK);
  v11::RawGame g;
  SND_REQUIRE(g.Accept(link.acceptor()));
  const v11::RawGame::Frame* hello = g.WaitFor(ll::MessageType::Hello);
  SND_REQUIRE(hello != nullptr);
  const ll::Hello h = ll::ReadFixed<ll::Hello>(hello->payload.data(), static_cast<std::uint32_t>(hello->payload.size()));
  ll::HelloReply r{};
  r.protocolMajor = 1;
  r.protocolMinor = 0;  // a 1.0 game
  r.status = static_cast<std::uint32_t>(ll::HelloStatus::Accepted);
  r.runtimeBuildHash = h.projectBuildHash;
  r.sampleRate = 48000;
  r.blockSize = 256;
  SND_REQUIRE(g.Send(ll::MessageType::HelloReply, &r, sizeof(r)));
  // The game answers every SetValue / ReplaceChunk with Ack OK.
  std::uint32_t setValues = 0;
  auto frame = [&] {
    const ConnectionEvents ev = conn.Poll();
    sync.Tick(conn, ev, livetest::Now());
    g.Pump(1);
    for (std::size_t i = 0; i < g.frames.size();) {
      const v11::RawGame::Frame f = g.frames[i];
      g.frames.erase(g.frames.begin() + static_cast<std::ptrdiff_t>(i));
      if (f.header.type == static_cast<std::uint16_t>(ll::MessageType::SetValue) ||
          f.header.type == static_cast<std::uint16_t>(ll::MessageType::ReplaceChunk)) {
        setValues += f.header.type == static_cast<std::uint16_t>(ll::MessageType::SetValue) ? 1u : 0u;
        // A 1.1 message would be a protocol error for a 1.0 game.
        const ll::Ack a{f.header.seq, SND_OK};
        g.Send(ll::MessageType::Ack, &a, sizeof(a), 0);
      }
      SND_CHECK_MSG(f.header.type < static_cast<std::uint16_t>(ll::MessageType::RevertValue) ||
                        f.header.type >= 0x0100u,
                    "1.1 request %u sent to a 1.0 game", f.header.type);
    }
  };
  auto run = [&](auto pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > end) return false;
      frame();
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return true;
  };
  SND_REQUIRE(run([&] { return sync.phase() == LivePhase::Live; }));
  SND_CHECK_EQ(conn.peerMinor(), 0u);
  SND_CHECK(!sync.check().banksKnown);
  SND_CHECK(!sync.check().note.empty());
  bool saidWhy = false;
  for (const LiveFeature& f : sync.features()) {
    if (!f.available) saidWhy = saidWhy || f.reason.find("1.0") != std::string::npos;
  }
  SND_CHECK(saidWhy);
  // A value edit goes as SetValue (1.0 has it).
  SND_REQUIRE(doc.SetNumber(livetest::SfxGuid(), "volumeDb", -6.0));
  frame();
  SND_REQUIRE(run([&] { return sync.phase() == LivePhase::Live; }));
  SND_CHECK_EQ(setValues, 1u);
  const LiveItem* it = sync.Find(livetest::SfxGuid(), "volumeDb");
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->state == LiveState::Live);
  // A new event needs a hot reload: not in 1.0, listed with the reason, nothing waits for it.
  json::Value init;
  json::ParseError pe{};
  SND_REQUIRE(json::Parse("{\"bank\": \"Main\", \"bus\": \"Sfx\"}", &init, &pe));
  SND_REQUIRE(!doc.Create("event", "Tone2", &init).empty());
  frame();
  SND_REQUIRE(run([&] { return sync.phase() == LivePhase::Live; }));
  bool listed = false;
  for (const UnsupportedItem& u : sync.unsupported()) listed = listed || u.reason.find("1.0") != std::string::npos;
  for (const LiveProblem& p : sync.problems()) std::printf("problem: %s\n", p.message.c_str());
  SND_CHECK(listed);
  SND_CHECK(!sync.reloading());
  // The audition router: no stop-all for a 1.0 game (said, not sent).
  AuditionRouter router;
  router.Bind(&conn, nullptr);
  router.SetTarget(AuditionTarget::Game);
  const AuditionResult stop = router.StopAll();
  SND_CHECK(!stop.ok);
  SND_CHECK(!stop.unsupported.empty());
  // Read-back asks nothing of a 1.0 game.
  CellSync cs;
  std::vector<Cell> cells(1);
  cells[0].address.chunk = snd::bank_format::chunk::BUSS;
  cells[0].address.runtimeId = snd::HashId("Sfx");
  cells[0].address.column = snd::bank_format::col::BUSS_volumeDb;
  cs.Update(conn.client(), cells, {}, false, false, 0);
  SND_CHECK(cs.RequestReadBack());
  cs.Update(conn.client(), cells, {}, false, false, 0);
  SND_CHECK(cs.cells()[0].readState == CellStatus::Read::None);
  // An item revert on a 1.0 game is the bank value by SetValue (as before).
  cells[0].value = -6.0f;
  cs.Update(conn.client(), cells, {}, false, false, 0);
  cells[0].value = 0.0f;
  SND_CHECK(cs.RequestRevert(cells[0].address));
  cs.Update(conn.client(), cells, {}, false, false, 0);
  SND_CHECK_EQ(cs.reverts(), 0u);
  conn.Close();
}
