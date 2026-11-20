// Independent tests, protocol 1.1 (组 L, indep_s7): compatibility and robustness of the
// new messages (design.md 14; protocol §2, §3). Properties B10, J1-J4, J6.
//   - order violations (before Hello, SyncDecision twice / when Accepted);
//   - short payloads (zero-filled), long payloads (appended fields; trailing data last);
//   - unknown types with the Ack bit (not answered);
//   - lying length fields (argCount * 8 wrapping 32 bits, pathBytes, chunk bytes, list
//     count * elemBytes) on the game and on the tool side;
//   - random mutation of every new message.
// Run under SND_SANITIZE=address,undefined. Scale with SND_FUZZ_ITERATIONS.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "indep_s7_common.hpp"

namespace {

using namespace s7;
using indep::IndepBank;
using indep::Tool;
using lltest::Fixture;
using lltest::RawPeer;
constexpr snd::Id kLive = lltest::kBankId;

std::uint32_t Iterations(std::uint32_t def) {
  const char* e = std::getenv("SND_FUZZ_ITERATIONS");
  if (e == nullptr) return def;
  const long v = std::strtol(e, nullptr, 10);
  return v > 0 ? static_cast<std::uint32_t>(v) : def;
}

bool Handshake(Fixture& f, RawPeer& p) {
  if (!f.StepUntil([&] { return p.Connect(f.link.connector(), 10); })) return false;
  ll::Hello h{};
  h.protocolMajor = ll::kProtocolMajor;
  h.protocolMinor = ll::kProtocolMinor;
  h.role = static_cast<std::uint32_t>(ll::Role::Tool);
  h.projectBuildHash = lltest::kProjectHash;
  if (!p.Send(ll::MessageType::Hello, &h, sizeof(h))) return false;
  return p.WaitFor(ll::MessageType::HelloReply) != nullptr;
}

// Frames of `type` answering `seq` that arrived so far (rendering meanwhile).
std::uint32_t CountAnswers(Fixture& f, RawPeer& p, ll::MessageType type, std::uint32_t seq, std::uint32_t ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    f.Step();
    p.Pump(1);
  }
  std::uint32_t n = 0;
  for (const auto& fr : p.frames)
    if (fr.header.type == static_cast<std::uint16_t>(type) && fr.header.seq == seq) ++n;
  return n;
}
std::int32_t AckResult(const RawPeer& p, std::uint32_t seq) {
  for (const auto& fr : p.frames)
    if (fr.header.type == static_cast<std::uint16_t>(ll::MessageType::Ack) && fr.header.seq == seq)
      return ll::ReadFixed<ll::Ack>(fr.payload.data(), static_cast<std::uint32_t>(fr.payload.size())).result;
  return 12345;
}

// A working tool afterwards, and the Sfx volume cell as the game holds it.
bool StillWorks(Fixture& f, std::uint64_t* sfxBits = nullptr) {
  Tool t(f);
  if (!t.Ready()) return false;
  ll::CellValue cv{};
  if (!WaitCell(t, t.c->ReadCell(SfxVolumeCell()), &cv)) return false;
  if (sfxBits != nullptr) *sfxBits = cv.current;
  return cv.result == SND_OK;
}

}  // namespace

// B10: order violations.
SND_TEST(IndepS7Fuzz, OrderViolations) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  {  // requests before Hello: never applied
    RawPeer p;
    SND_REQUIRE(f.StepUntil([&] { return p.Connect(f.link.connector(), 10); }));
    const ll::SetValue v = lltest::BusVolume("Sfx", -20.0f);
    SND_REQUIRE(p.Send(ll::MessageType::SetValue, &v, sizeof(v), ll::kFrameWantsAck, 1));
    SND_REQUIRE(p.Send(ll::MessageType::QueryBanks, nullptr, 0, ll::kFrameWantsAck, 2));
    SND_REQUIRE(p.Send(ll::MessageType::ReadCell, &v, sizeof(v), ll::kFrameWantsAck, 3));
    SND_REQUIRE(p.Send(ll::MessageType::RevertValue, &v, sizeof(v), ll::kFrameWantsAck, 4));
    std::vector<std::uint8_t> rb(16 + 6);
    ll::ReloadBank r{kLive, 0, 0, 6};
    std::memcpy(rb.data(), &r, 16);
    std::memcpy(rb.data() + 16, "a.sndb", 6);
    p.Send(ll::MessageType::ReloadBank, rb.data(), static_cast<std::uint32_t>(rb.size()), ll::kFrameWantsAck, 5);
    f.Steps(30);
    p.Pump(20);
    for (const auto& fr : p.frames) {
      SND_CHECK(fr.header.type != static_cast<std::uint16_t>(ll::MessageType::BankList));
      SND_CHECK(fr.header.type != static_cast<std::uint16_t>(ll::MessageType::CellValue));
      SND_CHECK(fr.header.type != static_cast<std::uint16_t>(ll::MessageType::ReloadStatus));
    }
    p.Close();
    SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  }
  std::uint64_t bits = 0;
  SND_REQUIRE(StillWorks(f, &bits));
  SND_CHECK_EQ(bits, F32Bits(0.0f));  // the SetValue before Hello was not applied
  SND_CHECK_EQ(f.server->Stats().reloadsDone, 0u);
  SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  {  // SyncDecision after Accepted, and twice after a mismatch
    RawPeer p;
    SND_REQUIRE(Handshake(f, p));
    ll::SyncDecision sdm{0, 0};
    p.Send(ll::MessageType::SyncDecision, &sdm, sizeof(sdm), ll::kFrameWantsAck, 7);
    f.Steps(20);
    p.Pump(10);
    p.Close();
    SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  }
  {
    RawPeer p;
    SND_REQUIRE(f.StepUntil([&] { return p.Connect(f.link.connector(), 10); }));
    SND_REQUIRE(p.Hello(0x1111u));  // mismatch
    SND_REQUIRE(p.WaitFor(ll::MessageType::HelloReply) != nullptr);
    SND_REQUIRE(p.Send(ll::MessageType::QueryBanks, nullptr, 0, 0, 2));
    SND_CHECK(p.WaitFor(ll::MessageType::BankList) != nullptr);  // allowed while AwaitSync
    ll::SyncDecision sdm{0, 0};
    SND_REQUIRE(p.Send(ll::MessageType::SyncDecision, &sdm, sizeof(sdm), 0, 3));
    p.Send(ll::MessageType::SyncDecision, &sdm, sizeof(sdm), 0, 4);
    ll::SyncDecision bad{7, 0};
    p.Send(ll::MessageType::SyncDecision, &bad, sizeof(bad), 0, 5);
    f.Steps(20);
    p.Pump(10);
    p.Close();
    SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  }
  SND_CHECK(StillWorks(f));
}

// J1: short payloads of the new requests (zero-filled): one answer each, nothing applied.
SND_TEST(IndepS7Fuzz, ShortPayloadsAreZeroFilled) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  RawPeer p;
  SND_REQUIRE(Handshake(f, p));
  const ll::SetValue v = SfxVolumeCell();
  std::uint32_t seq = 100;
  std::vector<std::uint32_t> seqs;
  for (std::uint32_t n : {0u, 1u, 12u, 47u}) {
    SND_REQUIRE(p.Send(ll::MessageType::RevertValue, &v, n, ll::kFrameWantsAck, ++seq));
    seqs.push_back(seq);
    SND_REQUIRE(p.Send(ll::MessageType::ReadCell, &v, n, ll::kFrameWantsAck, ++seq));
    seqs.push_back(seq);
  }
  const ll::ReloadBank rb{kLive, 0, 0, 6};
  for (std::uint32_t n : {0u, 4u, 15u}) {
    SND_REQUIRE(p.Send(ll::MessageType::ReloadBank, &rb, n, ll::kFrameWantsAck, ++seq));
    seqs.push_back(seq);
  }
  const ll::StructEdit se{static_cast<std::uint32_t>(ll::StructOp::AddBus), HashId("B1"), 2, 0};
  for (std::uint32_t n : {0u, 8u, 15u}) {
    SND_REQUIRE(p.Send(ll::MessageType::StructEdit, &se, n, ll::kFrameWantsAck, ++seq));
    seqs.push_back(seq);
  }
  // the counts
  f.Steps(5);
  CountAnswers(f, p, ll::MessageType::Ack, 0, 500);
  for (std::uint32_t s : seqs) {
    std::uint32_t acks = 0;
    for (const auto& fr : p.frames)
      if (fr.header.type == static_cast<std::uint16_t>(ll::MessageType::Ack) && fr.header.seq == s) ++acks;
    SND_CHECK_MSG(acks == 1u, "seq %u: %u acks", s, acks);
    const std::int32_t r = AckResult(p, s);
    const bool fullAddress = s == 107u || s == 108u;  // 47 of 48 bytes: the whole address (value ignored)
    SND_CHECK_MSG((r == SND_OK) == fullAddress, "seq %u answered %d", s, r);  // zero-filled: no table, no path, no args
  }
  // zero-filled RevertValue / ReadCell address no table: NOT_FOUND
  for (std::size_t i = 0; i < 6; ++i) SND_CHECK_MSG(AckResult(p, seqs[i]) == SND_ERR_NOT_FOUND, "seq %u", seqs[i]);
  SND_CHECK(p.frames.size() > 0u);
  SND_CHECK_EQ(f.server->Stats().reloadsDone + f.server->Stats().reloadsFailed, 0u);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 0u);
  p.Close();
  SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  std::uint64_t bits = 0;
  SND_CHECK(StillWorks(f, &bits));
  SND_CHECK_EQ(bits, F32Bits(0.0f));
}

// J2: long payloads: appended fields are ignored; the trailing path is the LAST pathBytes.
SND_TEST(IndepS7Fuzz, LongPayloadsAreRead) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  RawPeer p;
  SND_REQUIRE(Handshake(f, p));
  std::vector<std::uint8_t> rc(48 + 24, 0xEE);
  const ll::SetValue v = SfxVolumeCell();
  std::memcpy(rc.data(), &v, sizeof(v));
  SND_REQUIRE(p.Send(ll::MessageType::ReadCell, rc.data(), static_cast<std::uint32_t>(rc.size()), 0, 11));
  const RawPeer::Frame* cvf = nullptr;
  SND_CHECK(f.StepUntil([&] {
    p.Pump(1);
    for (const auto& fr : p.frames)
      if (fr.header.type == static_cast<std::uint16_t>(ll::MessageType::CellValue)) return true;
    return false;
  }));
  cvf = p.WaitFor(ll::MessageType::CellValue, 10);
  SND_REQUIRE(cvf != nullptr);
  const auto cv = ll::ReadFixed<ll::CellValue>(cvf->payload.data(), static_cast<std::uint32_t>(cvf->payload.size()));
  SND_CHECK_EQ(cvf->header.seq, 11u);
  SND_CHECK_EQ(cv.requestSeq, 11u);
  SND_CHECK_EQ(cv.result, SND_OK);
  SND_CHECK_EQ(cv.current, F32Bits(0.0f));
  // ReloadBank with 8 unknown bytes between the fixed part and the path
  std::vector<std::uint8_t> rb(16 + 8 + 6, 0);
  const ll::ReloadBank r{kLive, 0, 0, 6};
  std::memcpy(rb.data(), &r, 16);
  std::memset(rb.data() + 16, 'Z', 8);
  std::memcpy(rb.data() + 24, "a.sndb", 6);
  SND_REQUIRE(p.Send(ll::MessageType::ReloadBank, rb.data(), static_cast<std::uint32_t>(rb.size()),
                     ll::kFrameWantsAck, 12));
  SND_CHECK(f.StepUntil([&] {
    p.Pump(1);
    return AckResult(p, 12) != 12345;
  }));
  SND_CHECK_EQ(AckResult(p, 12), SND_OK);
  // QueryBanks with a payload (0-byte fixed part)
  std::uint8_t junk[40] = {};
  SND_REQUIRE(p.Send(ll::MessageType::QueryBanks, junk, sizeof(junk), 0, 13));
  SND_CHECK(f.StepUntil([&] {
    p.Pump(1);
    for (const auto& fr : p.frames)
      if (fr.header.type == static_cast<std::uint16_t>(ll::MessageType::BankList) && fr.header.seq == 13u) return true;
    return false;
  }));
}

// J3: unknown types with the Ack bit are skipped without an answer.
SND_TEST(IndepS7Fuzz, UnknownTypesAreNotAnswered) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  RawPeer p;
  SND_REQUIRE(Handshake(f, p));
  std::uint8_t pay[64] = {};
  for (std::uint16_t t : {std::uint16_t{0x001D}, std::uint16_t{0x00FE}, std::uint16_t{0x010D}, std::uint16_t{0x7FFF}}) {
    std::vector<std::uint8_t> b(ll::kFrameHeaderBytes + sizeof(pay));
    ll::EncodeHeader(b.data(), t, ll::kFrameWantsAck, sizeof(pay), 900u + t);
    std::memcpy(b.data() + ll::kFrameHeaderBytes, pay, sizeof(pay));
    SND_REQUIRE(p.SendBytes(b.data(), b.size()));
  }
  ll::Ping ping{42};
  SND_REQUIRE(p.Send(ll::MessageType::Ping, &ping, sizeof(ping), 0, 5));
  SND_CHECK(f.StepUntil([&] {
    p.Pump(1);
    for (const auto& fr : p.frames)
      if (fr.header.type == static_cast<std::uint16_t>(ll::MessageType::Pong)) return true;
    return false;
  }));
  f.Steps(10);
  p.Pump(10);
  for (const auto& fr : p.frames) {
    SND_CHECK(fr.header.type != static_cast<std::uint16_t>(ll::MessageType::Ack));
    SND_CHECK(fr.header.type != static_cast<std::uint16_t>(ll::MessageType::Error));
  }
  SND_CHECK(f.server->Connected());
}

// J4: lying lengths on the game side.
SND_TEST(IndepS7Fuzz, LyingLengthsOnTheGameSide) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  RawPeer p;
  SND_REQUIRE(Handshake(f, p));
  std::uint32_t seq = 200;
  // argCount * 8 wraps 32 bits to 0 / to 8, argCount huge, args shorter than announced
  for (std::uint32_t argCount : {0x20000000u, 0x20000001u, 0xFFFFFFFFu, 3u, 40u}) {
    for (std::uint32_t op = 0; op < 6; ++op) {
      std::vector<std::uint8_t> b(16 + 16, 0x11);
      const ll::StructEdit se{op, HashId("B1"), argCount, 0};
      std::memcpy(b.data(), &se, 16);
      SND_REQUIRE(p.Send(ll::MessageType::StructEdit, b.data(), static_cast<std::uint32_t>(b.size()),
                         ll::kFrameWantsAck, ++seq));
    }
  }
  // pathBytes lying
  for (std::uint32_t pathBytes : {7u, 0xFFFFFFFFu, 0x80000000u, 1025u}) {
    std::vector<std::uint8_t> b(16 + 6, 0);
    const ll::ReloadBank r{kLive, 0, 0, pathBytes};
    std::memcpy(b.data(), &r, 16);
    std::memcpy(b.data() + 16, "a.sndb", 6);
    SND_REQUIRE(p.Send(ll::MessageType::ReloadBank, b.data(), static_cast<std::uint32_t>(b.size()),
                       ll::kFrameWantsAck, ++seq));
  }
  // ReplaceChunk bytes lying
  for (std::uint64_t bytes : {std::uint64_t{0}, std::uint64_t{17}, ~std::uint64_t{0}, std::uint64_t{1} << 32,
                              (std::uint64_t{1} << 32) + 8u}) {
    std::vector<std::uint8_t> b(16 + 16, 0);
    const ll::ReplaceChunk rc{kLive, bf::chunk::BUSS, bytes};
    std::memcpy(b.data(), &rc, 16);
    SND_REQUIRE(p.Send(ll::MessageType::ReplaceChunk, b.data(), static_cast<std::uint32_t>(b.size()),
                       ll::kFrameWantsAck, ++seq));
  }
  // a table chunk whose header lies (rowCount / columnCount huge)
  {
    std::vector<std::uint8_t> ch = BussChunk(-1.0f);
    bf::TableChunkHeader th{};
    std::memcpy(&th, ch.data(), 16);
    for (std::uint32_t cols : {0xFFFFFFFFu, 0x10000000u, 0u}) {
      bf::TableChunkHeader h2 = th;
      h2.columnCount = cols;
      std::vector<std::uint8_t> c2 = ch;
      std::memcpy(c2.data(), &h2, 16);
      std::vector<std::uint8_t> b(16 + c2.size());
      const ll::ReplaceChunk rc{kLive, bf::chunk::BUSS, c2.size()};
      std::memcpy(b.data(), &rc, 16);
      std::memcpy(b.data() + 16, c2.data(), c2.size());
      SND_REQUIRE(p.Send(ll::MessageType::ReplaceChunk, b.data(), static_cast<std::uint32_t>(b.size()),
                         ll::kFrameWantsAck, ++seq));
    }
    bf::TableChunkHeader h3 = th;
    h3.rowCount = 0x40000000u;
    std::memcpy(ch.data(), &h3, 16);
    std::vector<std::uint8_t> b(16 + ch.size());
    const ll::ReplaceChunk rc{kLive, bf::chunk::BUSS, ch.size()};
    std::memcpy(b.data(), &rc, 16);
    std::memcpy(b.data() + 16, ch.data(), ch.size());
    SND_REQUIRE(p.Send(ll::MessageType::ReplaceChunk, b.data(), static_cast<std::uint32_t>(b.size()),
                       ll::kFrameWantsAck, ++seq));
  }
  const std::uint32_t last = seq;
  CountAnswers(f, p, ll::MessageType::Ack, 0, 800);
  std::uint32_t failed = 0, answered = 0;
  for (std::uint32_t s = 201; s <= last; ++s) {
    const std::int32_t r = AckResult(p, s);
    if (r != 12345) ++answered;
    if (r != 12345 && r != SND_OK) ++failed;
  }
  SND_CHECK_EQ(answered, last - 200u);  // each one answered once (D-86)
  // and none of them taken, except the chunk with 0 columns (seq 242): every column is missing from
  // the replacement, so every cell keeps its value (protocol §8.3) and the replacement is a no-op
  SND_CHECK_EQ(failed, last - 200u - 1u);
  SND_CHECK_EQ(AckResult(p, 242), SND_OK);
  SND_CHECK_EQ(f.rt->ReserveUsage().buses, 0u);
  SND_CHECK_EQ(f.server->Stats().reloadsDone, 0u);
  p.Close();
  SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  std::uint64_t bits = 0;
  SND_CHECK(StillWorks(f, &bits));
  SND_CHECK_EQ(bits, F32Bits(0.0f));
}

// J4 (tool side): lying 1.1 replies from a game.
SND_TEST(IndepS7Fuzz, LyingRepliesOnTheToolSide) {
  ll::MemoryLink link;
  SND_REQUIRE(link.Init(snd::base::DefaultAllocator(), 1u << 20));
  ll::ClientDesc d;
  d.connector = &link.connector();
  d.autoReconnect = false;
  d.maxBanks = 4;
  d.maxStreams = 4;
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(d, &c), SND_OK);
  SND_REQUIRE_EQ(c->Start(), SND_OK);
  RawGame g;
  SND_REQUIRE(g.Accept(link.acceptor()));
  const RawGame::Frame* h = g.WaitFor(ll::MessageType::Hello);
  SND_REQUIRE(h != nullptr);
  ll::HelloReply hr{1, 1, 0, 0, 0, 48000, 256, 0};
  SND_REQUIRE(g.Send(ll::MessageType::HelloReply, &hr, sizeof(hr), 0, h->header.seq));
  SND_REQUIRE(c->WaitReady(3000));
  auto list = [&](ll::MessageType t, std::uint32_t count, std::uint32_t elem, std::uint32_t actual, std::uint32_t seq) {
    std::vector<std::uint8_t> b(8 + actual, 0x5A);
    const ll::ListHeader lh{count, elem};
    std::memcpy(b.data(), &lh, 8);
    g.Send(t, b.data(), static_cast<std::uint32_t>(b.size()), 0, seq);
  };
  for (ll::MessageType t : {ll::MessageType::BankList, ll::MessageType::StreamList}) {
    list(t, 0x10000000u, 16u, 64u, 1);  // count * elemBytes wraps 32 bits to 0
    list(t, 0x10000001u, 16u, 64u, 2);  // wraps to 16
    list(t, 3u, 0u, 64u, 3);            // elemBytes 0
    list(t, 2u, 1000u, 64u, 4);         // longer than the payload
    list(t, 0xFFFFFFFFu, 0xFFFFFFFFu, 64u, 5);
    list(t, 100u, 32u, 100u * 32u, 6);  // more than maxBanks / maxStreams: kept up to the capacity
  }
  std::uint8_t z[64] = {};
  for (ll::MessageType t : {ll::MessageType::CellValue, ll::MessageType::ReloadStatus, ll::MessageType::BlockDetail}) {
    for (std::uint32_t n : {0u, 3u, 31u, 64u}) g.Send(t, z, n, 0, 77);
  }
  g.PumpFor(200);
  SND_CHECK(c->State() == ll::ClientState::Ready);
  ll::BankEntry be[8];
  const std::uint32_t nb = c->CopyBanks(be, 8);
  SND_CHECK(nb <= 4u);
  ll::StreamInfo si[8];
  SND_CHECK(c->CopyStreams(si, 8) <= 4u);
  // a well-formed list with longer elements (minor growth): the known part is read
  std::vector<std::uint8_t> b(8 + 2 * 40, 0);
  const ll::ListHeader lh{2, 40};
  std::memcpy(b.data(), &lh, 8);
  for (std::uint32_t i = 0; i < 2; ++i) {
    ll::BankEntry e{};
    e.bankId = 0x100u + i;
    e.languageId = 7;
    e.projectBuildHash = 0xAAu;
    e.contentHash = 0xBBu;
    e.writable = 1;
    e.slot = i;
    std::memcpy(b.data() + 8 + i * 40, &e, sizeof(e));
  }
  SND_REQUIRE(g.Send(ll::MessageType::BankList, b.data(), static_cast<std::uint32_t>(b.size()), 0, 99));
  std::uint32_t rs = 0;
  bool got = false;
  for (int i = 0; i < 200 && !got; ++i) {
    g.PumpFor(5);
    got = c->CopyBanks(be, 8, &rs) == 2u && rs == 99u;
  }
  SND_CHECK(got);
  SND_CHECK_EQ(be[1].bankId, 0x101u);
  SND_CHECK_EQ(be[1].slot, 1u);
  SND_CHECK_EQ(be[0].contentHash, 0xBBu);
  c->Stop();
  ll::Client::Destroy(c);
  g.Close();
}

// J6: random mutations of every new message, then the game still serves a tool.
SND_TEST(IndepS7Fuzz, RandomMutationsOfNewMessages) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.timeoutMs = 60000;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  std::mt19937 rng(0x5EED57u);
  const std::uint32_t iters = Iterations(3000);
  // seeds: valid 1.1 requests
  std::vector<std::pair<ll::MessageType, std::vector<std::uint8_t>>> seeds;
  {
    const ll::SetValue v = SfxVolumeCell();
    std::vector<std::uint8_t> b(sizeof(v));
    std::memcpy(b.data(), &v, sizeof(v));
    seeds.push_back({ll::MessageType::RevertValue, b});
    seeds.push_back({ll::MessageType::ReadCell, b});
    seeds.push_back({ll::MessageType::QueryBanks, {}});
    std::vector<std::uint8_t> r(16 + 7);
    const ll::ReloadBank rb{0x12345u, 0, 0, 7};  // not loaded: NOT_FOUND
    std::memcpy(r.data(), &rb, 16);
    std::memcpy(r.data() + 16, "x.sndb0", 7);
    seeds.push_back({ll::MessageType::ReloadBank, r});
    std::vector<std::uint8_t> s(16 + 16);
    const ll::StructEdit se{static_cast<std::uint32_t>(ll::StructOp::AddBus), HashId("B1"), 2, 0};
    std::memcpy(s.data(), &se, 16);
    const std::uint64_t a[2] = {HashId("Master"), 2};
    std::memcpy(s.data() + 16, a, 16);
    seeds.push_back({ll::MessageType::StructEdit, s});
    std::vector<std::uint8_t> au(sizeof(ll::Audition), 0);
    const ll::Audition aud{0, ll::kAuditionStopAll, {0, 0, 0}, 0};
    std::memcpy(au.data(), &aud, sizeof(aud));
    seeds.push_back({ll::MessageType::Audition, au});
    const std::vector<std::uint8_t> ch = BussChunk(-2.0f);
    std::vector<std::uint8_t> rc(16 + ch.size());
    const ll::ReplaceChunk rch{kLive, bf::chunk::BUSS, ch.size()};
    std::memcpy(rc.data(), &rch, 16);
    std::memcpy(rc.data() + 16, ch.data(), ch.size());
    seeds.push_back({ll::MessageType::ReplaceChunk, rc});
    const ll::SetProfilerMask pm{0x1FFu, 30};
    std::vector<std::uint8_t> pmb(8);
    std::memcpy(pmb.data(), &pm, 8);
    seeds.push_back({ll::MessageType::SetProfilerMask, pmb});
  }
  RawPeer* p = new RawPeer;
  SND_REQUIRE(Handshake(f, *p));
  std::uint32_t reconnects = 0;
  for (std::uint32_t i = 0; i < iters; ++i) {
    const auto& seed = seeds[rng() % seeds.size()];
    std::vector<std::uint8_t> b = seed.second;
    const std::uint32_t mode = rng() % 6u;
    if (mode == 0 && !b.empty()) b.resize(rng() % b.size());          // cut
    if (mode == 1) b.resize(b.size() + rng() % 64u, static_cast<std::uint8_t>(rng()));  // grow
    if (mode >= 2 && !b.empty()) {
      const std::uint32_t flips = 1u + rng() % 8u;
      for (std::uint32_t k = 0; k < flips; ++k) b[rng() % b.size()] = static_cast<std::uint8_t>(rng());
    }
    if (mode == 5 && b.size() >= 16u) {  // a length field set to an extreme
      const std::uint32_t ex[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x20000000u};
      const std::uint32_t at = (rng() % (static_cast<std::uint32_t>(b.size()) / 4u)) * 4u;
      std::memcpy(b.data() + at, &ex[rng() % 6u], 4);
    }
    const std::uint16_t flags = (rng() & 1u) ? std::uint16_t{ll::kFrameWantsAck} : std::uint16_t{0};
    if (!p->Send(seed.first, b.data(), static_cast<std::uint32_t>(b.size()), flags, i + 1u) || p->closed) {
      delete p;
      p = new RawPeer;
      ++reconnects;
      SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
      SND_REQUIRE(Handshake(f, *p));
    }
    if ((i & 15u) == 0u) {
      f.Step();
      p->Pump(0);
      p->frames.clear();
    }
  }
  f.Steps(50);
  p->Pump(10);
  delete p;
  SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  std::printf("[indep_s7] fuzz: %u iterations, %u reconnects, protocolErrors %u\n", iters, reconnects,
              f.server->Stats().protocolErrors);
  SND_CHECK(StillWorks(f));
}
