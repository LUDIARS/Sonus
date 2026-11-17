// Independent tests: the live link protocol as seen by a hand-driven tool
// (livelink-protocol.md §2-§5, design.md 14 "ファズ"). Malformed headers, sizes at
// and around every limit, messages out of order, frames split at any byte, and a
// structured mutation fuzzer. After every abuse the server must still serve a
// normal tool (an edit is acknowledged and audible).
#include <chrono>
#include <cstdlib>
#include <limits>
#include <random>
#include <thread>

#include "indep_livelink_common.hpp"

using namespace indep;
using lltest::Fixture;
using lltest::RawPeer;

namespace {

std::uint32_t FuzzIterations(std::uint32_t def) {
  const char* e = std::getenv("SND_FUZZ_ITERATIONS");
  if (e == nullptr) return def;
  const long v = std::strtol(e, nullptr, 10);
  return v > 0 ? static_cast<std::uint32_t>(v) : def;
}

// Steps the audio thread while waiting for a frame of `type`.
const RawPeer::Frame* WaitFrame(Fixture& f, RawPeer& p, ll::MessageType type, std::uint32_t ms = 3000) {
  const RawPeer::Frame* out = nullptr;
  f.StepUntil([&] { return (out = p.WaitFor(type, 1)) != nullptr; }, ms);
  return out;
}

bool WaitClosed(Fixture& f, RawPeer& p, std::uint32_t ms = 3000) {
  return f.StepUntil([&] {
    p.Pump(1);
    return p.closed;
  }, ms);
}

std::vector<std::uint8_t> FrameBytes(std::uint16_t type, std::uint16_t flags, std::uint32_t seq, const void* payload,
                                     std::uint32_t n, std::uint32_t declared) {
  std::vector<std::uint8_t> b(ll::kFrameHeaderBytes + n);
  ll::EncodeHeader(b.data(), type, flags, declared, seq);
  if (n != 0u) std::memcpy(b.data() + ll::kFrameHeaderBytes, payload, n);
  return b;
}

// A normal tool session still works: edit acknowledged, level as expected.
void CheckServesANormalTool(Fixture& f, double base, float db) {
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", db), true)), SND_OK);
  f.Steps(24);
  SND_CHECK_NEAR(LeftMean(f) / base, DbRatio(db), 1e-3);
  t.c->Stop();
  SND_CHECK(f.StepUntil([&] { return !f.server->Connected(); }));
}

struct Game {
  Fixture f;
  double base = 0;
  explicit Game(ll::ServerDesc sd = {}) {
    f.Load(IndepBank(), "a.sndb");
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    f.Steps(24);
    base = LeftMean(f);
    started = f.StartServer(sd);
  }
  snd_result started = SND_ERR_NOT_READY;
};

bool Handshake(Fixture& f, RawPeer& p) {
  if (!p.Hello()) return false;
  const RawPeer::Frame* r = WaitFrame(f, p, ll::MessageType::HelloReply);
  if (r == nullptr) return false;
  const auto hr = ll::ReadFixed<ll::HelloReply>(r->payload.data(), static_cast<std::uint32_t>(r->payload.size()));
  return hr.status == static_cast<std::uint32_t>(ll::HelloStatus::Accepted);
}

bool PingPong(Fixture& f, RawPeer& p, std::uint64_t t) {
  const ll::Ping ping{t};
  if (!p.Send(ll::MessageType::Ping, &ping, sizeof(ping), 0, 77)) return false;
  const RawPeer::Frame* r = WaitFrame(f, p, ll::MessageType::Pong);
  if (r == nullptr) return false;
  const auto pong = ll::ReadFixed<ll::Pong>(r->payload.data(), static_cast<std::uint32_t>(r->payload.size()));
  return pong.senderTimeNs == t && r->header.seq == 77u;
}

}  // namespace

// A2 / A3 / A4: unknown types are skipped; a payload exactly at the receive limit is
// read, one byte more is skipped with an Error and the connection stays; above
// 16 MiB the stream is dropped with an Error.
SND_TEST(IndepLivelinkProtocol, PayloadLimitsAndUnknownTypes) {
  ll::ServerDesc sd;
  sd.maxReceivePayloadBytes = 4096;
  Game g(sd);
  SND_REQUIRE_EQ(g.started, SND_OK);
  RawPeer p;
  SND_REQUIRE(p.Connect(g.f.link.connector()));
  SND_REQUIRE(Handshake(g.f, p));
  std::vector<std::uint8_t> junk(4097, 0xA5);
  SND_REQUIRE(p.Send(static_cast<ll::MessageType>(0x7777), junk.data(), 100, ll::kFrameWantsAck, 5));
  SND_CHECK(PingPong(g.f, p, 1));
  SND_CHECK_EQ(g.f.server->Stats().protocolErrors, 0u);
  SND_REQUIRE(p.Send(static_cast<ll::MessageType>(0x7777), junk.data(), 4096, 0, 6));  // == limit
  SND_CHECK(PingPong(g.f, p, 2));
  SND_CHECK_EQ(g.f.server->Stats().protocolErrors, 0u);
  SND_REQUIRE(p.Send(ll::MessageType::SetValue, junk.data(), 4097, ll::kFrameWantsAck, 7));  // limit + 1
  const RawPeer::Frame* e = WaitFrame(g.f, p, ll::MessageType::Error);
  SND_REQUIRE(e != nullptr);
  SND_CHECK_EQ(ll::ReadFixed<ll::Error>(e->payload.data(), 64).seq, 7u);
  SND_CHECK(PingPong(g.f, p, 3));  // still connected, the payload was skipped
  SND_CHECK(!p.closed);
  // A SetValue after the skipped one is applied normally.
  const ll::SetValue v = lltest::BusVolume("Sfx", -6.0f);
  SND_REQUIRE(p.Send(ll::MessageType::SetValue, &v, sizeof(v), ll::kFrameWantsAck, 8));
  const RawPeer::Frame* a = WaitFrame(g.f, p, ll::MessageType::Ack);
  SND_REQUIRE(a != nullptr);
  const auto ack = ll::ReadFixed<ll::Ack>(a->payload.data(), 8);
  SND_CHECK_EQ(ack.seq, 8u);
  SND_CHECK_EQ(ack.result, SND_OK);
  // payloadBytes = 16 MiB + 1: Error, then the server closes.
  const auto big = FrameBytes(0x7777, 0, 9, nullptr, 0, ll::kMaxPayloadBytes + 1u);
  SND_REQUIRE(p.SendBytes(big.data(), big.size()));
  SND_CHECK(WaitFrame(g.f, p, ll::MessageType::Error) != nullptr);
  SND_CHECK(WaitClosed(g.f, p));
  p.Close();
  CheckServesANormalTool(g.f, g.base, -12.0f);
}

// A2 boundary: payloadBytes exactly 16 MiB is within the protocol limit: skipped
// (larger than the receive buffer), not a disconnect.
SND_TEST(IndepLivelinkProtocol, PayloadOfExactly16MiBIsSkippedNotFatal) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  RawPeer p;
  SND_REQUIRE(p.Connect(g.f.link.connector()));
  SND_REQUIRE(Handshake(g.f, p));
  const auto h = FrameBytes(0x7777, 0, 3, nullptr, 0, ll::kMaxPayloadBytes);
  SND_REQUIRE(p.SendBytes(h.data(), h.size()));
  std::vector<std::uint8_t> zeros(1u << 16, 0);
  std::uint32_t left = ll::kMaxPayloadBytes;
  bool ok = true;
  while (left != 0u && ok) {
    const std::uint32_t n = left < zeros.size() ? left : static_cast<std::uint32_t>(zeros.size());
    ok = p.SendBytes(zeros.data(), n, 10000);
    left -= n;
    p.Pump(0);
  }
  SND_CHECK(ok);
  SND_CHECK(PingPong(g.f, p, 42));
  SND_CHECK(!p.closed);
}

// A6: a short fixed payload is zero-filled; a longer one (a newer minor) is read up to
// what this version knows.
SND_TEST(IndepLivelinkProtocol, ShortAndLongFixedPayloads) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  RawPeer p;
  SND_REQUIRE(p.Connect(g.f.link.connector()));
  SND_REQUIRE(Handshake(g.f, p));
  std::vector<std::uint8_t> longer(sizeof(ll::SetValue) + 24, 0xEE);
  const ll::SetValue v = lltest::BusVolume("Sfx", -6.0f);
  std::memcpy(longer.data(), &v, sizeof(v));
  SND_REQUIRE(p.Send(ll::MessageType::SetValue, longer.data(), static_cast<std::uint32_t>(longer.size()),
                     ll::kFrameWantsAck, 11));
  const RawPeer::Frame* a = WaitFrame(g.f, p, ll::MessageType::Ack);
  SND_REQUIRE(a != nullptr);
  SND_CHECK_EQ(ll::ReadFixed<ll::Ack>(a->payload.data(), 8).result, SND_OK);
  // Only the first 20 bytes: chunk / column are zero -> not a known table -> refused.
  SND_REQUIRE(p.Send(ll::MessageType::SetValue, &v, 20, ll::kFrameWantsAck, 12));
  a = WaitFrame(g.f, p, ll::MessageType::Ack);
  SND_REQUIRE(a != nullptr);
  SND_CHECK_EQ(ll::ReadFixed<ll::Ack>(a->payload.data(), 8).seq, 12u);
  SND_CHECK(ll::ReadFixed<ll::Ack>(a->payload.data(), 8).result != SND_OK);
  // An empty ReplaceChunk (no fixed part at all).
  SND_REQUIRE(p.Send(ll::MessageType::ReplaceChunk, nullptr, 0, ll::kFrameWantsAck, 13));
  a = WaitFrame(g.f, p, ll::MessageType::Ack);
  SND_REQUIRE(a != nullptr);
  SND_CHECK(ll::ReadFixed<ll::Ack>(a->payload.data(), 8).result != SND_OK);
  // ReplaceChunk whose `bytes` exceeds the payload / is huge.
  ll::ReplaceChunk rc{lltest::kBankId, bf::chunk::BUSS, ~0ull};
  SND_REQUIRE(p.Send(ll::MessageType::ReplaceChunk, &rc, sizeof(rc), ll::kFrameWantsAck, 14));
  a = WaitFrame(g.f, p, ll::MessageType::Ack);
  SND_REQUIRE(a != nullptr);
  SND_CHECK_EQ(ll::ReadFixed<ll::Ack>(a->payload.data(), 8).result, SND_ERR_INVALID_ARGUMENT);
  // StructEdit with argCount beyond the payload / beyond the cap.
  ll::StructEdit se{0, HashId("Sfx"), 1000000u, 0};
  SND_REQUIRE(p.Send(ll::MessageType::StructEdit, &se, sizeof(se), ll::kFrameWantsAck, 15));
  a = WaitFrame(g.f, p, ll::MessageType::Ack);
  SND_REQUIRE(a != nullptr);
  SND_CHECK_EQ(ll::ReadFixed<ll::Ack>(a->payload.data(), 8).result, SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(PingPong(g.f, p, 5));
  g.f.Steps(24);
  SND_CHECK_NEAR(LeftMean(g.f) / g.base, DbRatio(-6.0), 1e-3);
}

// A9 / A13 / A14 / B1: out-of-order handshake messages close the connection; a newer
// minor is accepted; HelloReply describes the runtime.
SND_TEST(IndepLivelinkProtocol, HandshakeOrderAndVersions) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  {
    RawPeer p;  // SyncDecision first
    SND_REQUIRE(p.Connect(g.f.link.connector()));
    const ll::SyncDecision s{0, 0};
    SND_REQUIRE(p.Send(ll::MessageType::SyncDecision, &s, sizeof(s)));
    SND_CHECK(WaitFrame(g.f, p, ll::MessageType::Error) != nullptr);
    SND_CHECK(WaitClosed(g.f, p));
  }
  {
    RawPeer p;  // Hello twice
    SND_REQUIRE(g.f.StepUntil([&] { return p.Connect(g.f.link.connector(), 10); }));
    SND_REQUIRE(Handshake(g.f, p));
    SND_REQUIRE(p.Hello());
    SND_CHECK(WaitClosed(g.f, p));
  }
  {
    RawPeer p;  // role Game
    SND_REQUIRE(g.f.StepUntil([&] { return p.Connect(g.f.link.connector(), 10); }));
    ll::Hello h{};
    h.protocolMajor = ll::kProtocolMajor;
    h.role = static_cast<std::uint32_t>(ll::Role::Game);
    h.projectBuildHash = lltest::kProjectHash;
    SND_REQUIRE(p.Send(ll::MessageType::Hello, &h, sizeof(h)));
    SND_CHECK(WaitClosed(g.f, p));
  }
  {
    RawPeer p;  // a newer minor, a 3-byte-short Hello name: accepted
    SND_REQUIRE(g.f.StepUntil([&] { return p.Connect(g.f.link.connector(), 10); }));
    ll::Hello h{};
    h.protocolMajor = ll::kProtocolMajor;
    h.protocolMinor = 9;
    h.role = static_cast<std::uint32_t>(ll::Role::Tool);
    h.projectBuildHash = lltest::kProjectHash;
    std::memset(h.name, 'x', sizeof(h.name));  // not NUL terminated
    SND_REQUIRE(p.Send(ll::MessageType::Hello, &h, sizeof(h) - 3));
    const RawPeer::Frame* r = WaitFrame(g.f, p, ll::MessageType::HelloReply);
    SND_REQUIRE(r != nullptr);
    const auto hr = ll::ReadFixed<ll::HelloReply>(r->payload.data(), static_cast<std::uint32_t>(r->payload.size()));
    SND_CHECK_EQ(hr.status, static_cast<std::uint32_t>(ll::HelloStatus::Accepted));
    SND_CHECK_EQ(hr.protocolMajor, 1u);
    SND_CHECK_EQ(hr.protocolMinor, 0u);
    SND_CHECK_EQ(hr.blockSize, g.f.cfg_.block_size);
    SND_CHECK_EQ(hr.sampleRate, g.f.cfg_.sample_rate);
    SND_CHECK_EQ(hr.runtimeBuildHash, lltest::kProjectHash);
    SND_CHECK_EQ(r->payload.size(), sizeof(ll::HelloReply));
    SND_CHECK(PingPong(g.f, p, 9));
    p.Close();
  }
  {
    RawPeer p;  // BuildMismatch, then a SyncDecision with an unknown mode: closed
    SND_REQUIRE(g.f.StepUntil([&] { return p.Connect(g.f.link.connector(), 10); }));
    SND_REQUIRE(p.Hello(0x99u));
    const RawPeer::Frame* r = WaitFrame(g.f, p, ll::MessageType::HelloReply);
    SND_REQUIRE(r != nullptr);
    SND_CHECK_EQ(ll::ReadFixed<ll::HelloReply>(r->payload.data(), 32).status,
                 static_cast<std::uint32_t>(ll::HelloStatus::BuildMismatch));
    // A command before the decision is not applied.
    const ll::SetValue v = lltest::BusVolume("Sfx", -30.0f);
    SND_REQUIRE(p.Send(ll::MessageType::SetValue, &v, sizeof(v), ll::kFrameWantsAck, 3));
    SND_CHECK(WaitClosed(g.f, p));
    g.f.Steps(24);
    SND_CHECK_NEAR(LeftMean(g.f) / g.base, 1.0, 1e-4);
  }
  {
    RawPeer p;
    SND_REQUIRE(g.f.StepUntil([&] { return p.Connect(g.f.link.connector(), 10); }));
    SND_REQUIRE(p.Hello(0x99u));
    SND_REQUIRE(WaitFrame(g.f, p, ll::MessageType::HelloReply) != nullptr);
    const ll::SyncDecision s{7, 0};
    SND_REQUIRE(p.Send(ll::MessageType::SyncDecision, &s, sizeof(s)));
    SND_CHECK(WaitClosed(g.f, p));
  }
  CheckServesANormalTool(g.f, g.base, -3.0f);
}

// A5: the same session cut into pieces of every small size gives the same answers.
SND_TEST(IndepLivelinkProtocol, AnySplitOfTheStreamGivesTheSameAnswers) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  std::vector<std::uint8_t> stream;
  auto add = [&](ll::MessageType t, const void* pl, std::uint32_t n, std::uint16_t fl, std::uint32_t seq) {
    const auto b = FrameBytes(static_cast<std::uint16_t>(t), fl, seq, pl, n, n);
    stream.insert(stream.end(), b.begin(), b.end());
  };
  ll::Hello h{};
  h.protocolMajor = ll::kProtocolMajor;
  h.role = static_cast<std::uint32_t>(ll::Role::Tool);
  h.projectBuildHash = lltest::kProjectHash;
  add(ll::MessageType::Hello, &h, sizeof(h), 0, 1);
  const ll::SetValue v = lltest::BusVolume("Sfx", -6.0f);
  add(ll::MessageType::SetValue, &v, sizeof(v), ll::kFrameWantsAck, 2);
  add(static_cast<ll::MessageType>(0x4242), stream.data(), 37, 0, 3);
  const ll::Ping ping{1234};
  add(ll::MessageType::Ping, &ping, sizeof(ping), 0, 4);
  const ll::SetValue back = lltest::BusVolume("Sfx", 0.0f);
  add(ll::MessageType::SetValue, &back, sizeof(back), ll::kFrameWantsAck, 5);
  for (const std::size_t piece : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{5}, std::size_t{7},
                                  std::size_t{15}, std::size_t{16}, std::size_t{17}, std::size_t{63}}) {
    RawPeer p;
    SND_REQUIRE(g.f.StepUntil([&] { return p.Connect(g.f.link.connector(), 10); }));
    for (std::size_t at = 0; at < stream.size(); at += piece) {
      const std::size_t n = stream.size() - at < piece ? stream.size() - at : piece;
      SND_REQUIRE(p.SendBytes(stream.data() + at, n));
      if ((at / piece) % 4u == 0u) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    SND_CHECK_MSG(WaitFrame(g.f, p, ll::MessageType::HelloReply) != nullptr, "piece %zu", piece);
    const RawPeer::Frame* a = WaitFrame(g.f, p, ll::MessageType::Ack);
    SND_REQUIRE(a != nullptr);
    SND_CHECK_EQ(ll::ReadFixed<ll::Ack>(a->payload.data(), 8).seq, 2u);
    SND_CHECK_EQ(ll::ReadFixed<ll::Ack>(a->payload.data(), 8).result, SND_OK);
    const RawPeer::Frame* po = WaitFrame(g.f, p, ll::MessageType::Pong);
    SND_REQUIRE(po != nullptr);
    SND_CHECK_EQ(ll::ReadFixed<ll::Pong>(po->payload.data(), 16).senderTimeNs, 1234u);
    a = WaitFrame(g.f, p, ll::MessageType::Ack);
    SND_REQUIRE(a != nullptr);
    SND_CHECK_EQ(ll::ReadFixed<ll::Ack>(a->payload.data(), 8).seq, 5u);
    SND_CHECK_EQ(g.f.server->Stats().protocolErrors, 0u);
    p.Close();
    SND_REQUIRE(g.f.StepUntil([&] { return !g.f.server->Connected(); }));
  }
}

// A17: a frame cut in the middle by a disconnect leaves nothing behind: the next
// connection starts at a frame boundary and the half command is never applied.
SND_TEST(IndepLivelinkProtocol, TruncatedFrameThenDisconnectLeavesNoTrace) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  for (std::uint32_t cut : {3u, 16u, 16u + 20u, 16u + 47u}) {
    RawPeer p;
    SND_REQUIRE(g.f.StepUntil([&] { return p.Connect(g.f.link.connector(), 10); }));
    SND_REQUIRE(Handshake(g.f, p));
    const ll::SetValue v = lltest::BusVolume("Sfx", -40.0f);
    const auto b = FrameBytes(static_cast<std::uint16_t>(ll::MessageType::SetValue), ll::kFrameWantsAck, 9, &v,
                              sizeof(v), sizeof(v));
    SND_REQUIRE(p.SendBytes(b.data(), cut));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    p.Close();
    SND_REQUIRE(g.f.StepUntil([&] { return !g.f.server->Connected(); }));
  }
  g.f.Steps(24);
  SND_CHECK_NEAR(LeftMean(g.f) / g.base, 1.0, 1e-5);
  SND_CHECK_EQ(g.f.rt->applier().Remembered(), 0u);
  CheckServesANormalTool(g.f, g.base, -6.0f);
}

// A8 / A18 and a game -> tool type sent by the tool: answered, not fatal.
SND_TEST(IndepLivelinkProtocol, WrongDirectionMessagesAreRefusedButNotFatal) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  RawPeer p;
  SND_REQUIRE(p.Connect(g.f.link.connector()));
  SND_REQUIRE(Handshake(g.f, p));
  const ll::ListHeader lh{0xFFFFFFFFu, 0xFFFFFFFFu};
  SND_REQUIRE(p.Send(ll::MessageType::BusMeters, &lh, sizeof(lh), 0, 21));
  SND_CHECK(WaitFrame(g.f, p, ll::MessageType::Error) != nullptr);
  const ll::HelloReply hr{};
  SND_REQUIRE(p.Send(ll::MessageType::HelloReply, &hr, sizeof(hr), 0, 22));
  SND_CHECK(WaitFrame(g.f, p, ll::MessageType::Error) != nullptr);
  SND_CHECK(PingPong(g.f, p, 7));
}

// A15 / A16: structured mutation fuzzing of whole sessions. Frames of every tool ->
// game type (and others) with sizes at the boundaries of each fixed part, fields
// drawn from boundary values (0, 1, ~0, sign bit, NaN, +-Inf, -0, kNone, valid ids),
// random flags and seqs, plus occasional broken headers. The server must keep
// running and serve a normal tool afterwards; no memory error under ASan / UBSan.
SND_TEST(IndepLivelinkProtocol, StructuredSessionFuzz) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  std::mt19937 rng(0x5EEDu);
  auto R = [&]() -> std::uint32_t { return static_cast<std::uint32_t>(rng()); };
  const std::uint32_t rounds = FuzzIterations(60);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  auto fbits = [](float f) {
    std::uint32_t b = 0;
    std::memcpy(&b, &f, 4);
    return b;
  };
  const std::uint32_t interesting[] = {0u,
                                       1u,
                                       2u,
                                       7u,
                                       0xFFFFu,
                                       0x10000u,
                                       0x7FFFFFFFu,
                                       0x80000000u,
                                       0xFFFFFFFFu,
                                       fbits(nan),
                                       fbits(inf),
                                       fbits(-inf),
                                       fbits(-0.0f),
                                       fbits(-6.0f),
                                       fbits(1e30f),
                                       HashId("Sfx"),
                                       HashId("Master"),
                                       HashId("Tone"),
                                       HashId("Knob"),
                                       lltest::kBankId,
                                       bf::chunk::BUSS,
                                       bf::chunk::EVNT,
                                       bf::chunk::CLIP,
                                       bf::chunk::EFPV,
                                       bf::chunk::DUCK,
                                       bf::chunk::WAVE,
                                       bf::col::BUSS_volumeDb,
                                       bf::col::EVNT_priority};
  auto word = [&]() -> std::uint32_t {
    return (R() % 3u) == 0u ? static_cast<std::uint32_t>(R())
                              : interesting[R() % (sizeof(interesting) / sizeof(interesting[0]))];
  };
  const std::uint16_t types[] = {0x0001, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0010, 0x0011, 0x0012,
                                 0x0013, 0x0014, 0x0015, 0x0016, 0x0017, 0x0018, 0x0100, 0x0105, 0x0107,
                                 0x0000, 0x00FF, 0xFFFF};
  const std::uint32_t sizes[] = {0, 1, 4, 7, 8, 15, 16, 17, 23, 24, 31, 32, 47, 48, 49, 55, 56, 57, 64, 200, 4096};
  std::uint32_t frames = 0;
  for (std::uint32_t round = 0; round < rounds; ++round) {
    RawPeer p;
    if (!g.f.StepUntil([&] { return p.Connect(g.f.link.connector(), 10); }, 8000)) {
      SND_CHECK_MSG(false, "round %u: the server stopped accepting", round);
      break;
    }
    if (R() % 8u != 0u && !Handshake(g.f, p)) {
      p.Close();
      continue;
    }
    const std::uint32_t n = 4u + R() % 40u;
    for (std::uint32_t i = 0; i < n && !p.closed; ++i) {
      const std::uint16_t type = types[R() % (sizeof(types) / sizeof(types[0]))];
      std::uint32_t size = sizes[R() % (sizeof(sizes) / sizeof(sizes[0]))];
      std::vector<std::uint8_t> payload((size + 3u) & ~3u);
      for (std::size_t k = 0; k + 4 <= payload.size(); k += 4) {
        const std::uint32_t w = word();
        std::memcpy(payload.data() + k, &w, 4);
      }
      // ReplaceChunk: sometimes a plausible table chunk (header + column entries).
      if (type == 0x0011 && R() % 2u == 0u) {
        const bf::TableChunkHeader th{R() % 4u, 1u + R() % 3u, 0u, 0u};
        std::vector<std::uint8_t> chunk(sizeof(th) + 3 * sizeof(bf::ColumnEntry) + 64 * 4);
        std::memcpy(chunk.data(), &th, sizeof(th));
        for (std::uint32_t c = 0; c < 3; ++c) {
          bf::ColumnEntry ce{};
          ce.columnId = static_cast<std::uint16_t>(R() % 12u);
          ce.elemBytes = static_cast<std::uint16_t>((R() % 3u) * 4u);
          ce.offset = 64u * (1u + R() % 4u);
          std::memcpy(chunk.data() + sizeof(th) + c * sizeof(ce), &ce, sizeof(ce));
        }
        for (std::size_t k = sizeof(th) + 3 * sizeof(bf::ColumnEntry); k + 4 <= chunk.size(); k += 4) {
          const std::uint32_t w = word();
          std::memcpy(chunk.data() + k, &w, 4);
        }
        const ll::ReplaceChunk rc{R() % 2u == 0u ? lltest::kBankId : word(), R() % 2u == 0u ? bf::chunk::BUSS : word(),
                                  R() % 4u == 0u ? static_cast<std::uint64_t>(word()) : chunk.size()};
        payload.assign(reinterpret_cast<const std::uint8_t*>(&rc), reinterpret_cast<const std::uint8_t*>(&rc) + sizeof(rc));
        payload.insert(payload.end(), chunk.begin(), chunk.end());
      }
      size = static_cast<std::uint32_t>(payload.size());
      std::uint32_t declared = size;
      std::uint32_t magicBreak = 0;
      const std::uint32_t mode = R() % 64u;
      if (mode == 0u) declared = ll::kMaxPayloadBytes + 1u + R() % 1000u;  // fatal
      if (mode == 1u) magicBreak = 1u;                                      // fatal
      if (mode == 2u && size != 0u) declared = size - 1u;                   // desync: next header is garbage
      auto b = FrameBytes(type, static_cast<std::uint16_t>(R()), static_cast<std::uint32_t>(R()), payload.data(), size,
                          declared);
      if (magicBreak != 0u) b[R() % 4u] ^= 0x5Au;
      if (!p.SendBytes(b.data(), b.size(), 2000)) break;
      ++frames;
      if (R() % 4u == 0u) {
        p.Pump(0);
        g.f.Step();
      }
    }
    g.f.Steps(2);
    p.Pump(2);
    p.Close();
    if (!g.f.StepUntil([&] { return !g.f.server->Connected(); }, 8000)) {
      SND_CHECK_MSG(false, "round %u: the server kept a closed connection", round);
      break;
    }
  }
  SND_CHECK(frames > rounds);
  // The fuzz may have edited values: put them back, then a normal tool works.
  RevertDirect(g.f);
  ll::ApplyResult ar{};
  while (g.f.rt->results().TryPop(ar)) {
  }
  g.f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);  // in case an Audition / limit stopped it
  g.f.Steps(32);
  Tool t(g.f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f), true)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", 0.0f), true)), SND_OK);
  const ll::ServerStats st = g.f.server->Stats();
  SND_CHECK(st.connections >= rounds);
  SND_CHECK(st.protocolErrors > 0u);
}
