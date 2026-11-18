// Independent tests: transports (TCP and the in-memory link), one connection at a
// time, timeouts, reconnection, the client's buffers and thread safety, and object
// lifetimes across the live link thread and the audio thread (design.md 2, 11.2;
// livelink-protocol.md §1, §4).
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "indep_livelink_common.hpp"
#include "snd/livelink/tcp.hpp"

using namespace indep;
using lltest::Fixture;
using lltest::RawPeer;

namespace {

struct Game {
  Fixture f;
  double base = 0;
  explicit Game(ll::ServerDesc sd = {}, bool tcp = false) {
    f.Load(IndepBank(), "a.sndb");
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    f.Steps(24);
    base = LeftMean(f);
    started = f.StartServer(sd, tcp);
  }
  snd_result started = SND_ERR_NOT_READY;
};

ll::ClientDesc TcpClient(std::uint16_t port) {
  ll::ClientDesc d;
  d.port = port;
  d.projectBuildHash = lltest::kProjectHash;
  d.reconnectDelayMs = 50;
  return d;
}

// Keeps every freed block (contents snapshotted at Free) to detect writes after free.
class QuarantineAllocator final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag tag) override {
    void* p = snd::base::DefaultAllocator().Allocate(bytes, align, tag);
    std::lock_guard<std::mutex> g(m_);
    if (p != nullptr) live_[p] = Block{bytes, tag, {}};
    return p;
  }
  void Free(void* p, snd::MemTag) override {
    if (p == nullptr) return;
    std::lock_guard<std::mutex> g(m_);
    auto it = live_.find(p);
    if (it == live_.end()) return;
    Block b = it->second;
    b.snapshot.assign(static_cast<std::uint8_t*>(p), static_cast<std::uint8_t*>(p) + b.bytes);
    freed_[p] = std::move(b);
    live_.erase(it);
  }
  // Bytes of freed blocks that changed after their Free.
  std::size_t ModifiedAfterFree() {
    std::lock_guard<std::mutex> g(m_);
    std::size_t n = 0;
    for (const auto& [p, b] : freed_) {
      const auto* q = static_cast<const std::uint8_t*>(p);
      for (std::size_t i = 0; i < b.bytes; ++i) n += q[i] != b.snapshot[i] ? 1u : 0u;
    }
    return n;
  }
  ~QuarantineAllocator() override {
    for (auto& [p, b] : freed_) snd::base::DefaultAllocator().Free(p, b.tag);
    for (auto& [p, b] : live_) snd::base::DefaultAllocator().Free(p, b.tag);
  }

 private:
  struct Block {
    std::size_t bytes;
    snd::MemTag tag;
    std::vector<std::uint8_t> snapshot;
  };
  std::mutex m_;
  std::map<void*, Block> live_, freed_;
};

}  // namespace

// B4 / G1 / G3: over real TCP a second connection is accepted and closed at once while
// the first one keeps working; after the first leaves, a new tool is served.
SND_TEST(IndepLivelinkTransport, TcpSecondConnectionIsClosedAndTheFirstKeepsWorking) {
  Game g({}, true);
  SND_REQUIRE_EQ(g.started, SND_OK);
  const std::uint16_t port = g.f.server->Port();
  SND_REQUIRE(port != 0u);
  {
    Tool a(g.f, TcpClient(port));
    SND_REQUIRE(a.Ready());
    ll::TcpConnector second;
    SND_REQUIRE(second.SetTarget("127.0.0.1", port));
    ll::ITransport* t = second.Connect(1000);
    SND_REQUIRE(t != nullptr);
    bool closed = false;
    g.f.StepUntil([&] {
      std::uint8_t b[64];
      std::size_t n = 0;
      t->Wait(true, false, 1);
      const ll::IoStatus st = t->Recv(b, sizeof(b), &n);
      closed = st == ll::IoStatus::Closed || st == ll::IoStatus::Error;
      return closed;
    });
    SND_CHECK(closed);
    second.Release(t);
    SND_CHECK_EQ(a.Ack(a.c->SetValue(lltest::BusVolume("Sfx", -6.0f), true)), SND_OK);
    SND_CHECK_EQ(g.f.server->Stats().connections, 1u);
    SND_CHECK_EQ(a.c->Stats().disconnects, 0u);
  }
  SND_REQUIRE(g.f.StepUntil([&] { return !g.f.server->Connected(); }));
  Tool b(g.f, TcpClient(port));
  SND_REQUIRE(b.Ready());
  SND_CHECK_EQ(b.Ack(b.c->SetValue(lltest::BusVolume("Sfx", -12.0f), true)), SND_OK);
  g.f.Steps(24);
  SND_CHECK_NEAR(LeftMean(g.f) / g.base, DbRatio(-12.0), 1e-3);
}

// B4 / G3 / G4: the in-memory link refuses a second connection like TCP.
SND_TEST(IndepLivelinkTransport, MemoryLinkRefusesASecondConnection) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  Tool a(g.f);
  SND_REQUIRE(a.Ready());
  RawPeer p;
  const bool connected = p.Connect(g.f.link.connector(), 300);
  if (connected) {  // accepted at the transport level: then it must be closed at once
    SND_CHECK(g.f.StepUntil([&] {
      p.Pump(1);
      return p.closed;
    }));
  }
  SND_CHECK(g.f.link.Rejected() >= 1u || connected);
  SND_CHECK_EQ(a.Ack(a.c->SetValue(lltest::BusVolume("Sfx", -6.0f), true)), SND_OK);
  SND_CHECK_EQ(a.c->Stats().disconnects, 0u);
}

// A12 / G2: a tool that never says Hello is dropped after handshakeTimeoutMs with an
// Error; one that goes silent after the handshake is dropped after timeoutMs; the
// server serves the next tool in both cases.
SND_TEST(IndepLivelinkTransport, HandshakeAndHeartbeatTimeoutsOverTheMemoryLink) {
  ll::ServerDesc sd;
  sd.handshakeTimeoutMs = 200;
  sd.timeoutMs = 300;
  sd.heartbeatMs = 50;
  Game g(sd);
  SND_REQUIRE_EQ(g.started, SND_OK);
  {
    RawPeer p;
    SND_REQUIRE(p.Connect(g.f.link.connector()));
    const auto t0 = std::chrono::steady_clock::now();
    const RawPeer::Frame* e = nullptr;
    g.f.StepUntil([&] { return (e = p.WaitFor(ll::MessageType::Error, 1)) != nullptr; });
    SND_REQUIRE(e != nullptr);
    SND_CHECK_EQ(ll::ReadFixed<ll::Error>(e->payload.data(), 64).code, SND_ERR_NOT_READY);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    SND_CHECK_MSG(ms >= 150.0, "handshake timeout after %.0f ms", ms);
    SND_CHECK(g.f.StepUntil([&] {
      p.Pump(1);
      return p.closed;
    }));
  }
  {
    RawPeer p;
    SND_REQUIRE(g.f.StepUntil([&] { return p.Connect(g.f.link.connector(), 10); }));
    SND_REQUIRE(p.Hello());
    SND_REQUIRE(g.f.StepUntil([&] { return p.WaitFor(ll::MessageType::HelloReply, 1) != nullptr; }));
    // The game pings; we never answer and never send.
    SND_CHECK(g.f.StepUntil([&] { return p.WaitFor(ll::MessageType::Ping, 1) != nullptr; }));
    SND_CHECK(g.f.StepUntil([&] {
      p.Pump(1);
      return p.closed;
    }));
  }
  SND_CHECK(g.f.server->Stats().timeouts >= 2u);
  Tool t(g.f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f), true)), SND_OK);
}

// I3 / B5: the client reconnects after a server restart (new Server on the same
// link) and after repeated drops; edits keep working and the server ends connected.
SND_TEST(IndepLivelinkTransport, ClientReconnectsAfterServerRestartAndRepeatedDrops) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  ll::ClientDesc cd = lltest::MemoryClient(g.f.link);
  cd.reconnectDelayMs = 10;
  Tool t(g.f, cd);
  SND_REQUIRE(t.Ready());
  ll::Server::Destroy(g.f.server);
  g.f.server = nullptr;
  ll::ServerDesc sd;
  sd.system = g.f.sys;
  sd.runtime = g.f.rt;
  sd.allocator = &snd::base::DefaultAllocator();
  sd.acceptor = &g.f.link.acceptor();
  SND_REQUIRE_EQ(ll::Server::Create(sd, &g.f.server), SND_OK);
  SND_REQUIRE_EQ(g.f.server->Start(), SND_OK);
  SND_REQUIRE(g.f.StepUntil([&] { return t.c->State() == ll::ClientState::Ready && g.f.server->Connected(); }));
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f), true)), SND_OK);
  for (int i = 0; i < 15; ++i) {
    const std::uint32_t before = t.c->Stats().connects;
    t.c->SetValue(lltest::BusVolume("Sfx", -1.0f - static_cast<float>(i)), false);
    t.c->Disconnect();
    SND_REQUIRE(g.f.StepUntil([&] {
      return t.c->Stats().connects > before && t.c->State() == ll::ClientState::Ready && g.f.server->Connected();
    }));
  }
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -20.0f), true)), SND_OK);
  g.f.Steps(24);
  SND_CHECK_NEAR(LeftMean(g.f) / g.base, DbRatio(-20.0), 1e-3);
  SND_CHECK_EQ(t.c->Stats().protocolErrors, 0u);
}

// I2: the client is used from several threads at once while it receives the profiler
// stream (TSan target). Every edit is recorded by the server; nothing is lost or torn.
SND_TEST(IndepLivelinkTransport, ClientApiFromManyThreads) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  Tool t(g.f);
  SND_REQUIRE(t.Ready());
  t.c->SetProfilerMask(ll::kProfMeters | ll::kProfVoices | ll::kProfTiming | ll::kProfEventLog, 200);
  constexpr int kThreads = 3, kEach = 150;
  std::atomic<int> sent{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> th;
  for (int k = 0; k < kThreads; ++k) {
    th.emplace_back([&, k] {
      for (int i = 0; i < kEach; ++i) {
        std::uint32_t q = 0;
        while ((q = t.c->SetValue(lltest::BusVolume(k == 0 ? "Sfx" : "Master", -1.0f), false)) == 0u) {
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        sent.fetch_add(1);
      }
    });
  }
  th.emplace_back([&] {
    ll::MeterEntry m[16];
    ll::VoiceInfo v[16];
    ll::TimingSample ts[64];
    ll::LogSample ls[64];
    while (!stop.load()) {
      t.c->CopyMeters(m, 16);
      t.c->CopyVoices(v, 16);
      t.c->CopyTimings(ts, 64);
      t.c->CopyEventLog(ls, 64);
      t.c->Stats();
      std::int32_t r = 0;
      t.c->TakeAck(12345, &r);
    }
  });
  g.f.StepUntil([&] { return sent.load() == kThreads * kEach; }, 20000);
  for (int k = 0; k < kThreads; ++k) th[static_cast<std::size_t>(k)].join();
  SND_CHECK(g.f.StepUntil([&] {
    return g.f.server->Stats().commandsRecorded >= static_cast<std::uint32_t>(kThreads * kEach);
  }));
  SND_CHECK(g.f.StepUntil([&] { return t.c->Received(ll::MessageType::BusMeters) > 0u; }));
  stop.store(true);
  th.back().join();
  SND_CHECK_EQ(g.f.server->Stats().commandsRecorded, static_cast<std::uint32_t>(kThreads * kEach));
  SND_CHECK_EQ(t.c->Stats().protocolErrors, 0u);
  SND_CHECK(t.c->Received(ll::MessageType::BusMeters) > 0u);
}

// Client record buffer (11.3): a recording larger than recordBufferBytes keeps the
// first bytes and counts the rest as overflow; nothing else is reported as an error.
SND_TEST(IndepLivelinkTransport, ClientRecordOverflowIsCountedAsOverflow) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  ll::ClientDesc cd = lltest::MemoryClient(g.f.link);
  cd.recordBufferBytes = 4096;
  Tool t(g.f, cd);
  SND_REQUIRE(t.Ready());
  SND_REQUIRE_EQ(t.Ack(t.c->StartRecording(true)), SND_OK);
  for (int i = 0; i < 60; ++i) {
    g.f.sys->SetParamGlobal(HashId("Knob"), static_cast<float>(i) / 60.0f);
    g.f.Step();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  SND_REQUIRE_EQ(t.Ack(t.c->StopRecording(true)), SND_OK);
  SND_REQUIRE(g.f.StepUntil([&] { return !g.f.rt->Recording(); }));
  std::uint64_t produced = 0;
  g.f.StepUntil([&] {
    const ll::ClientStats st = t.c->Stats();
    produced = g.f.server->Stats().recordBytes;
    return produced > 4096u && st.recordOverflow + t.c->RecordingBytes() >= produced;
  }, 3000);
  const ll::ClientStats st = t.c->Stats();
  SND_REQUIRE(produced > 4096u);
  SND_CHECK_EQ(t.c->RecordingBytes(), 4096u);
  SND_CHECK_MSG(st.recordOverflow == produced - 4096u, "recordOverflow %u, expected %llu", st.recordOverflow,
                static_cast<unsigned long long>(produced - 4096u));
  SND_CHECK_MSG(st.protocolErrors == 0u, "%u protocol errors while the record buffer was full", st.protocolErrors);
}

// Lifetime: destroying the Server while one of its ReplaceChunk commands is recorded but
// not yet applied must not let the audio thread use the Server's memory afterwards.
SND_TEST(IndepLivelinkTransport, ServerDestroyDoesNotLeaveReplaceChunkDangling) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(24);
  QuarantineAllocator qa;
  SND_REQUIRE(f.link.Init(snd::base::DefaultAllocator(), 1u << 20));
  ll::ServerDesc sd;
  sd.system = f.sys;
  sd.runtime = f.rt;
  sd.allocator = &qa;
  sd.acceptor = &f.link.acceptor();
  SND_REQUIRE_EQ(ll::Server::Create(sd, &f.server), SND_OK);
  SND_REQUIRE_EQ(f.server->Start(), SND_OK);
  {
    Tool t(f);
    SND_REQUIRE(t.Ready());
    bt::TableBuilder bu(bf::chunk::BUSS);
    std::uint32_t r = bu.AddRow();
    bu.U32(bf::col::BUSS_id, r, HashId("Sfx"));
    bu.U32(bf::col::BUSS_parent, r, 1u);
    bu.U32(bf::col::BUSS_channels, r, 2u);
    bu.F32(bf::col::BUSS_volumeDb, r, -6.0f);
    r = bu.AddRow();
    bu.U32(bf::col::BUSS_id, r, HashId("Master"));
    bu.U32(bf::col::BUSS_parent, r, bf::kNone);
    bu.U32(bf::col::BUSS_channels, r, 2u);
    bu.U32(bf::col::BUSS_flags, r, bf::kBusMaster);
    const std::vector<std::uint8_t> chunk = bu.Serialize();
    SND_REQUIRE(t.c->ReplaceChunk(lltest::kBankId, bf::chunk::BUSS, chunk.data(), chunk.size(), true) != 0u);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.server->Stats().commandsRecorded == 0u && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    SND_REQUIRE_EQ(f.server->Stats().commandsRecorded, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // published by the server's Flush
    ll::Server::Destroy(f.server);  // the audio thread has not run since
    f.server = nullptr;
    f.Steps(8);  // applies the recorded LiveReplaceChunk
  }
  const std::size_t modified = qa.ModifiedAfterFree();
  SND_CHECK_MSG(modified == 0u, "%zu bytes of the destroyed Server's memory were written afterwards", modified);
}
