// Live link over real loopback TCP sockets: connect, edit, disconnect,
// reconnect, the heartbeat timeout, and the one-connection rule.
#include <cmath>

#include "livelink_fixture.hpp"
#include "snd/livelink/tcp.hpp"

using namespace lltest;
using snd::HashId;

namespace {
ll::ClientDesc TcpClient(std::uint16_t port) {
  ll::ClientDesc d;
  d.port = port;
  d.address = "127.0.0.1";
  d.projectBuildHash = kProjectHash;
  return d;
}
}  // namespace

SND_TEST(LivelinkTcp, ConnectEditDisconnectReconnect) {
  Fixture f;
  f.Load(DevBank());
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(8);
  const float base = f.LastLeft();
  SND_REQUIRE_EQ(f.StartServer({}, true), SND_OK);
  const std::uint16_t port = f.server->Port();
  SND_REQUIRE(port != 0u);
  ll::Client* c = nullptr;
  ll::ClientDesc cd = TcpClient(port);
  cd.heartbeatMs = 50;
  SND_REQUIRE_EQ(ll::Client::Create(cd, &c), SND_OK);
  SND_REQUIRE_EQ(c->Start(), SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready && f.server->Connected(); }));
  const std::uint32_t seq = c->SetValue(BusVolume("Sfx", -6.0f));
  std::int32_t r = 1;
  SND_CHECK(f.StepUntil([&] { return c->TakeAck(seq, &r); }));
  SND_CHECK_EQ(r, SND_OK);
  f.Steps(8);
  SND_CHECK_NEAR(f.LastLeft() / base, std::pow(10.0, -6.0 / 20.0), 1e-4);
  // Heartbeat both ways.
  SND_CHECK(f.StepUntil([&] { return c->Stats().lastGameClock != 0u && f.server->Stats().pingsAnswered > 0u; }));
  // Disconnect -> the server goes back to accepting -> the client reconnects.
  c->Disconnect();
  SND_CHECK(f.StepUntil([&] { return c->Stats().connects >= 2u && f.server->Connected(); }));
  SND_CHECK_EQ(f.server->Stats().connections, 2u);
  const std::uint32_t seq2 = c->SetValue(BusVolume("Sfx", 0.0f));
  SND_CHECK(f.StepUntil([&] { return c->TakeAck(seq2, &r); }));
  SND_CHECK_EQ(r, SND_OK);
  // A second tool is refused while one is attached (protocol §1).
  {
    ll::TcpConnector other;
    SND_REQUIRE(other.SetTarget("127.0.0.1", port));
    ll::ITransport* t = other.Connect(1000);
    SND_REQUIRE(t != nullptr);
    bool closed = false;
    f.StepUntil([&] {
      std::uint8_t b[16];
      std::size_t n = 0;
      t->Wait(true, false, 1);
      const ll::IoStatus st = t->Recv(b, sizeof(b), &n);
      closed = st == ll::IoStatus::Closed || st == ll::IoStatus::Error;
      return closed;
    });
    SND_CHECK(closed);
    other.Release(t);
  }
  SND_CHECK(f.server->Connected());
  // The game stops the server: the client keeps retrying without errors.
  f.StopServer();
  SND_CHECK(f.StepUntil([&] { return c->State() == ll::ClientState::Connecting; }));
  ll::Client::Destroy(c);
}

SND_TEST(LivelinkTcp, SilentToolTimesOutAndTheServerAcceptsAgain) {
  Fixture f;
  f.Load(DevBank());
  ll::ServerDesc sd;
  sd.timeoutMs = 300;
  sd.heartbeatMs = 50;
  SND_REQUIRE_EQ(f.StartServer(sd, true), SND_OK);
  ll::TcpConnector con;
  SND_REQUIRE(con.SetTarget("127.0.0.1", f.server->Port()));
  RawPeer p;
  SND_REQUIRE(p.Connect(con));
  SND_REQUIRE(p.Hello());
  SND_REQUIRE(p.WaitFor(ll::MessageType::HelloReply) != nullptr);
  SND_REQUIRE(f.StepUntil([&] { return f.server->Connected(); }));
  // The peer reads (it sees the server's pings) but never answers.
  SND_CHECK(p.WaitFor(ll::MessageType::Ping) != nullptr);
  SND_CHECK(f.StepUntil([&] { p.Pump(1); return p.closed; }, 3000));
  SND_CHECK_EQ(f.server->Stats().timeouts, 1u);
  SND_CHECK(!f.server->Connected());
  p.Close();
  RawPeer q;
  SND_REQUIRE(q.Connect(con));
  SND_REQUIRE(q.Hello());
  SND_CHECK(q.WaitFor(ll::MessageType::HelloReply) != nullptr);
}

SND_TEST(LivelinkTcp, HandshakeTimeoutAndListenErrors) {
  Fixture f;
  f.Load(DevBank());
  ll::ServerDesc sd;
  sd.handshakeTimeoutMs = 200;
  SND_REQUIRE_EQ(f.StartServer(sd, true), SND_OK);
  ll::TcpConnector con;
  SND_REQUIRE(con.SetTarget("localhost", f.server->Port()));
  RawPeer p;
  SND_REQUIRE(p.Connect(con));
  SND_CHECK(p.WaitFor(ll::MessageType::Error, 3000) != nullptr);  // no Hello: "handshake timeout"
  SND_CHECK(f.StepUntil([&] { p.Pump(1); return p.closed; }));
  // The port is taken: a second listener fails cleanly; bad addresses are refused.
  ll::TcpAcceptor a;
  SND_CHECK_EQ(a.Listen("127.0.0.1", f.server->Port()), SND_ERR_IO);
  SND_CHECK_EQ(a.Listen("300.1.1.1", 0), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(a.Listen("not an address", 0), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(a.Listen("127.0.0.1", 0), SND_OK);
  SND_CHECK(a.Port() != 0u);
  ll::TcpConnector bad;
  SND_CHECK(!bad.SetTarget("1.2.3", 1));
}
