// Unit tests of the live link building blocks: wire codec, framing, queues,
// SPSC channels, the record format, the in-memory transport.
#include <cstring>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/core/core.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/livelink/frame_stream.hpp"
#include "snd/livelink/livelink_module.hpp"
#include "snd/livelink/memory_transport.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/livelink/rings.hpp"
#include "snd/livelink/wire.hpp"
#include "snd/test/test.hpp"

namespace ll = snd::livelink;
namespace bf = snd::bank_format;
using snd::base::DefaultAllocator;

namespace {

std::vector<std::uint8_t> Frame(std::uint16_t type, const void* p, std::uint32_t n, std::uint32_t seq = 7,
                                std::uint16_t flags = 0) {
  std::vector<std::uint8_t> b(ll::kFrameHeaderBytes + n);
  ll::EncodeHeader(b.data(), type, flags, n, seq);
  if (n != 0u) std::memcpy(b.data() + ll::kFrameHeaderBytes, p, n);
  return b;
}

struct Collect final : ll::FrameReader::Handler {
  struct F {
    ll::FrameHeader h;
    std::vector<std::uint8_t> p;
  };
  std::vector<F> frames;
  std::vector<ll::FrameHeader> oversized;
  void OnFrame(const ll::FrameHeader& h, const std::uint8_t* p) noexcept override {
    frames.push_back(F{h, std::vector<std::uint8_t>(p, p + h.payloadBytes)});
  }
  void OnOversized(const ll::FrameHeader& h) noexcept override { oversized.push_back(h); }
};

}  // namespace

SND_TEST(Livelink, ModuleLinks) { SND_CHECK(std::strcmp(ll::ModuleName(), "livelink") == 0); }

SND_TEST(LivelinkWire, HeaderRoundTripAndLimits) {
  std::uint8_t h[16];
  ll::EncodeHeader(h, 0x0010, ll::kFrameWantsAck, 48, 0xABCDEF01u);
  SND_CHECK_EQ(h[0], 'S');
  SND_CHECK_EQ(h[1], 'N');
  SND_CHECK_EQ(h[2], 'L');
  SND_CHECK_EQ(h[3], 'L');
  SND_CHECK_EQ(h[4], 0x10);  // little endian type
  SND_CHECK_EQ(h[8], 48);
  ll::FrameHeader d{};
  SND_CHECK(ll::DecodeHeader(h, &d) == ll::HeaderStatus::Ok);
  SND_CHECK_EQ(d.type, 0x0010);
  SND_CHECK_EQ(d.flags, ll::kFrameWantsAck);
  SND_CHECK_EQ(d.payloadBytes, 48u);
  SND_CHECK_EQ(d.seq, 0xABCDEF01u);
  ll::EncodeHeader(h, 1, 0, ll::kMaxPayloadBytes, 0);
  SND_CHECK(ll::DecodeHeader(h, &d) == ll::HeaderStatus::Ok);
  ll::EncodeHeader(h, 1, 0, ll::kMaxPayloadBytes + 1u, 0);
  SND_CHECK(ll::DecodeHeader(h, &d) == ll::HeaderStatus::TooLarge);
  h[0] = 'X';
  SND_CHECK(ll::DecodeHeader(h, &d) == ll::HeaderStatus::BadMagic);
}

SND_TEST(LivelinkWire, FixedPayloadsAreZeroFilledOrTruncated) {
  ll::Hello full{};
  full.protocolMajor = 1;
  full.role = 1;
  full.projectBuildHash = 0x1122334455667788ull;
  std::vector<std::uint8_t> bytes(sizeof(full) + 8, 0xEE);  // a newer minor: longer
  std::memcpy(bytes.data(), &full, sizeof(full));
  ll::Hello r = ll::ReadFixed<ll::Hello>(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  SND_CHECK_EQ(r.projectBuildHash, full.projectBuildHash);
  r = ll::ReadFixed<ll::Hello>(bytes.data(), 8);  // an older / shorter one: rest is 0
  SND_CHECK_EQ(r.protocolMajor, 1u);
  SND_CHECK_EQ(r.projectBuildHash, 0u);
  r = ll::ReadFixed<ll::Hello>(bytes.data(), 0);
  SND_CHECK_EQ(r.role, 0u);
}

SND_TEST(LivelinkWire, ListsAndTrailingDataAreBoundsChecked) {
  std::vector<std::uint8_t> p(8 + 3 * 40, 0);
  const ll::ListHeader lh{3, 40};  // elements larger than known (minor growth)
  std::memcpy(p.data(), &lh, 8);
  for (std::uint32_t i = 0; i < 3; ++i) {
    ll::VoiceInfo v{};
    v.eventId = 100 + i;
    std::memcpy(p.data() + 8 + i * 40, &v, sizeof(v));
  }
  ll::ListView lv;
  SND_REQUIRE(ll::ParseList(p.data(), static_cast<std::uint32_t>(p.size()), 16, &lv));
  SND_CHECK_EQ(lv.At<ll::VoiceInfo>(2).eventId, 102u);
  SND_CHECK(!ll::ParseList(p.data(), static_cast<std::uint32_t>(p.size()) - 1u, 16, &lv));  // short
  SND_CHECK(!ll::ParseList(p.data(), static_cast<std::uint32_t>(p.size()), 2, &lv));        // over max
  SND_CHECK(!ll::ParseList(p.data(), 7, 16, &lv));
  const ll::ListHeader zero{5, 0};
  std::memcpy(p.data(), &zero, 8);
  SND_CHECK(!ll::ParseList(p.data(), static_cast<std::uint32_t>(p.size()), 16, &lv));
  const ll::ListHeader huge{0xFFFFFFFFu, 0xFFFFFFFFu};
  std::memcpy(p.data(), &huge, 8);
  SND_CHECK(!ll::ParseList(p.data(), static_cast<std::uint32_t>(p.size()), 0xFFFFFFFFu, &lv));
  // trailing data: the last `bytes` of the payload
  const std::uint8_t* d = nullptr;
  std::uint8_t q[32] = {};
  q[31] = 9;
  SND_CHECK(ll::TrailingData(q, 32, 16, 16, &d));
  SND_CHECK(d == q + 16);
  SND_CHECK(ll::TrailingData(q, 32, 16, 1, &d) && *d == 9);
  SND_CHECK(!ll::TrailingData(q, 32, 16, 17, &d));
  SND_CHECK(!ll::TrailingData(q, 32, 16, ~0ull, &d));
  SND_CHECK(ll::TrailingData(q, 8, 16, 0, &d));
  SND_CHECK(!ll::TrailingData(q, 8, 16, 1, &d));
}

SND_TEST(LivelinkWire, ValidatorsRejectBadEnumsAndFloats) {
  ll::SetValue v{};
  v.chunk = bf::chunk::BUSS;
  v.column = bf::col::BUSS_volumeDb;
  v.valueType = 0;
  SND_CHECK_EQ(ll::CheckSetValue(v), SND_OK);
  v.valueType = 4;
  SND_CHECK_EQ(ll::CheckSetValue(v), SND_ERR_INVALID_ARGUMENT);
  v.valueType = 0;
  v.value = 0x7FC00000u;  // NaN
  SND_CHECK_EQ(ll::CheckSetValue(v), SND_ERR_INVALID_ARGUMENT);
  v.value = 1ull << 40;
  SND_CHECK_EQ(ll::CheckSetValue(v), SND_ERR_INVALID_ARGUMENT);
  v.value = 0;
  v.chunk = bf::chunk::WAVE;  // a blob
  SND_CHECK_EQ(ll::CheckSetValue(v), SND_ERR_NOT_FOUND);
  v.chunk = snd::Fourcc('Z', 'Z', 'Z', 'Z');
  SND_CHECK_EQ(ll::CheckSetValue(v), SND_ERR_NOT_FOUND);
  v.chunk = bf::chunk::ASST;
  v.column = bf::col::ASST_totalFrames;  // u64 column
  v.valueType = 1;
  SND_CHECK_EQ(ll::CheckSetValue(v), SND_ERR_INVALID_ARGUMENT);
  ll::Hello h{};
  SND_CHECK_EQ(ll::CheckHello(h), SND_ERR_INVALID_ARGUMENT);
  h.role = 1;
  SND_CHECK_EQ(ll::CheckHello(h), SND_OK);
  SND_CHECK_EQ(ll::CheckSyncDecision(ll::SyncDecision{2, 0}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckSyncDecision(ll::SyncDecision{1, 0}), SND_OK);
  ll::Audition a{};
  a.eventId = 1;
  SND_CHECK_EQ(ll::CheckAudition(a), SND_OK);
  a.flags = 4;
  SND_CHECK_EQ(ll::CheckAudition(a), SND_ERR_INVALID_ARGUMENT);
  a.flags = ll::kAuditionAtPosition;
  a.position[1] = 1.0f / 0.0f;
  SND_CHECK_EQ(ll::CheckAudition(a), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckOverrideParam(ll::OverrideParam{1, 2, 0.0f, 0}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckOverrideParam(ll::OverrideParam{1, 1, 0.5f, 0}), SND_OK);
  SND_CHECK_EQ(ll::CheckOverrideState(ll::OverrideState{1, 0, 1, 0}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckSetProfilerMask(ll::SetProfilerMask{0x80, 0}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckSetProfilerMask(ll::SetProfilerMask{0x7F, 5000}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckRecordControl(ll::RecordControl{2, 0}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckStructEdit(ll::StructEdit{5, 0, 0, 0}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckStructEdit(ll::StructEdit{4, 0, 65, 0}), SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(LivelinkWire, TableChunkValidation) {
  // Header + one column entry + data, hand-made.
  std::vector<std::uint8_t> c(128 + 64, 0);
  bf::TableChunkHeader th{2, 1, 0, 0};
  bf::ColumnEntry ce{static_cast<std::uint16_t>(bf::col::BUSS_volumeDb), 4, 0, 64};
  std::memcpy(c.data(), &th, sizeof(th));
  std::memcpy(c.data() + 16, &ce, sizeof(ce));
  std::uint32_t rows = 0;
  SND_CHECK_EQ(ll::CheckTableChunk(bf::chunk::BUSS, c.data(), 72, &rows), SND_OK);
  SND_CHECK_EQ(rows, 2u);
  SND_CHECK_EQ(ll::CheckTableChunk(bf::chunk::BUSS, c.data(), 71, &rows), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckTableChunk(bf::chunk::WAVE, c.data(), 72, &rows), SND_ERR_UNSUPPORTED);
  ce.offset = 32;  // inside the directory / not 64-aligned
  std::memcpy(c.data() + 16, &ce, sizeof(ce));
  SND_CHECK_EQ(ll::CheckTableChunk(bf::chunk::BUSS, c.data(), 192, &rows), SND_ERR_INVALID_ARGUMENT);
  ce.offset = 64;
  ce.elemBytes = 8;  // f32 column with 8-byte elements
  std::memcpy(c.data() + 16, &ce, sizeof(ce));
  SND_CHECK_EQ(ll::CheckTableChunk(bf::chunk::BUSS, c.data(), 192, &rows), SND_ERR_INVALID_ARGUMENT);
  th.rowCount = 0x40000000u;  // rows * elem beyond the data
  ce.elemBytes = 4;
  std::memcpy(c.data(), &th, sizeof(th));
  std::memcpy(c.data() + 16, &ce, sizeof(ce));
  SND_CHECK_EQ(ll::CheckTableChunk(bf::chunk::BUSS, c.data(), 192, &rows), SND_ERR_INVALID_ARGUMENT);
  th.columnCount = 0xFFFFFFFFu;
  std::memcpy(c.data(), &th, sizeof(th));
  SND_CHECK_EQ(ll::CheckTableChunk(bf::chunk::BUSS, c.data(), 192, &rows), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(ll::CheckTableChunk(bf::chunk::BUSS, c.data(), 3, &rows), SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(LivelinkWire, FieldEncodingRoundTripsEveryColumn) {
  for (const bf::ColumnInfo& col : bf::kColumns) {
    const std::uint32_t f = ll::EncodeField(col.chunk, col.columnId);
    SND_REQUIRE(f != ll::kFieldRevertAll);
    std::uint32_t ch = 0, co = 0;
    SND_CHECK(ll::DecodeField(f, &ch, &co));
    SND_CHECK_EQ(ch, col.chunk);
    SND_CHECK_EQ(co, static_cast<std::uint32_t>(col.columnId));
  }
  std::uint32_t ch = 0, co = 0;
  SND_CHECK(!ll::DecodeField(ll::kFieldRevertAll, &ch, &co));
  SND_CHECK(!ll::DecodeField(0x7FFF0000u, &ch, &co));
  SND_CHECK_EQ(ll::EncodeField(bf::chunk::BUSS, 999), ll::kFieldRevertAll);
}

SND_TEST(LivelinkFrames, ReaderCutsFramesFromAnySplit) {
  std::vector<std::uint8_t> stream;
  const ll::Ping ping{42};
  for (int i = 0; i < 5; ++i) {
    auto f = Frame(0x0004, &ping, sizeof(ping), static_cast<std::uint32_t>(i));
    stream.insert(stream.end(), f.begin(), f.end());
  }
  auto empty = Frame(0x0015, nullptr, 0, 99);
  stream.insert(stream.end(), empty.begin(), empty.end());
  std::vector<std::uint8_t> big(5000, 3);
  auto over = Frame(0x0011, big.data(), static_cast<std::uint32_t>(big.size()), 100);  // > reader buffer
  stream.insert(stream.end(), over.begin(), over.end());
  auto tail = Frame(0x0004, &ping, sizeof(ping), 101);
  stream.insert(stream.end(), tail.begin(), tail.end());
  for (std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{17}, stream.size()}) {
    ll::FrameReader r;
    SND_REQUIRE(r.Init(DefaultAllocator(), 4096));
    Collect c;
    for (std::size_t at = 0; at < stream.size(); at += chunk) {
      const std::size_t n = stream.size() - at < chunk ? stream.size() - at : chunk;
      SND_REQUIRE(r.Feed(stream.data() + at, n, c) == ll::HeaderStatus::Ok);
    }
    SND_CHECK(r.AtBoundary());
    SND_REQUIRE_EQ(c.frames.size(), 7u);
    SND_CHECK_EQ(c.frames[2].h.seq, 2u);
    SND_CHECK_EQ(ll::ReadFixed<ll::Ping>(c.frames[4].p.data(), 8).senderTimeNs, 42u);
    SND_CHECK_EQ(c.frames[5].h.payloadBytes, 0u);
    SND_CHECK_EQ(c.frames[6].h.seq, 101u);
    SND_REQUIRE_EQ(c.oversized.size(), 1u);
    SND_CHECK_EQ(c.oversized[0].seq, 100u);
  }
}

SND_TEST(LivelinkFrames, QueuesKeepFramesWholeAndControlFirst) {
  ll::MemoryLink link;
  SND_REQUIRE(link.Init(DefaultAllocator(), 64));  // tiny pipe: many partial sends
  ll::ITransport* tool = nullptr;
  std::thread t([&] { tool = link.connector().Connect(2000); });
  ll::ITransport* game = nullptr;
  for (int i = 0; i < 200 && game == nullptr; ++i) game = link.acceptor().Accept(10);
  t.join();
  SND_REQUIRE(game != nullptr && tool != nullptr);
  ll::FrameSender s;
  SND_REQUIRE(s.Init(DefaultAllocator(), 1024, 4096));
  std::vector<std::uint8_t> payload(300);
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i);
  SND_CHECK(s.bulk().Push(0x0107, 0, 1, payload.data(), 100, payload.data() + 100, 200));
  SND_CHECK(s.bulk().Push(0x0107, 0, 2, payload.data(), 300));
  while (s.bulk().Push(0x0107, 0, 3, payload.data(), 300)) {
  }
  SND_CHECK(s.bulk().Free() < 316u);  // all or nothing
  ll::FrameReader r;
  SND_REQUIRE(r.Init(DefaultAllocator(), 1024));
  Collect c;
  bool pushedControl = false;
  for (int guard = 0; guard < 100000 && !s.Idle(); ++guard) {
    std::uint64_t sent = 0;
    s.Pump(*game, &sent);
    if (!pushedControl && sent != 0u) {  // mid-frame: the control frame waits for the boundary
      const ll::Ack a{9, 0};
      SND_CHECK(s.control().Push(0x0006, 0, 9, &a, sizeof(a)));
      pushedControl = true;
    }
    std::uint8_t buf[64];
    std::size_t n = 0;
    while (tool->Recv(buf, sizeof(buf), &n) == ll::IoStatus::Ok) r.Feed(buf, n, c);
  }
  SND_CHECK(s.Idle());
  std::uint8_t buf[64];
  std::size_t n = 0;
  while (tool->Recv(buf, sizeof(buf), &n) == ll::IoStatus::Ok) r.Feed(buf, n, c);
  SND_REQUIRE(c.frames.size() >= 3u);
  SND_CHECK_EQ(c.frames[0].h.seq, 1u);
  SND_CHECK_EQ(c.frames[1].h.type, 0x0006);  // the control frame jumped ahead of the queued bulk
  SND_CHECK(std::memcmp(c.frames[0].p.data(), payload.data(), 300) == 0);
  for (const auto& f : c.frames) SND_CHECK(f.h.type == 0x0107 ? f.p.size() == 300u : f.h.type == 0x0006);
  link.acceptor().Release(game);
  link.connector().Release(tool);
}

SND_TEST(LivelinkRings, ByteRingMessageRingTripleSlot) {
  ll::ByteRing br;
  SND_REQUIRE(br.Init(DefaultAllocator(), 100));
  SND_CHECK_EQ(br.Capacity(), 128u);
  std::uint8_t in[200], out[200];
  for (int i = 0; i < 200; ++i) in[i] = static_cast<std::uint8_t>(i);
  SND_CHECK_EQ(br.Write(in, 200), 128u);
  SND_CHECK_EQ(br.Read(out, 50), 50u);
  SND_CHECK_EQ(br.Write(in + 128, 72), 50u);  // wraps
  SND_CHECK_EQ(br.Read(out + 50, 200), 128u);
  SND_CHECK(std::memcmp(in, out, 178) == 0);
  ll::MessageRing mr;
  SND_REQUIRE(mr.Init(DefaultAllocator(), 64));
  SND_CHECK(mr.Push(in, 20));        // 8 + 24
  SND_CHECK(mr.Push(in + 20, 24));   // 8 + 24 = 64 total
  SND_CHECK(!mr.Push(in, 1));        // full: refused, nothing changes
  std::uint32_t n = 0;
  SND_REQUIRE(mr.Next(&n));
  SND_CHECK_EQ(n, 20u);
  mr.Copy(0, out, 20);
  SND_CHECK(std::memcmp(out, in, 20) == 0);
  mr.Pop();
  SND_CHECK(mr.Push(in + 50, 24));  // wraps
  SND_REQUIRE(mr.Next(&n));
  SND_CHECK_EQ(n, 24u);
  mr.Pop();
  SND_REQUIRE(mr.Next(&n));
  mr.Copy(0, out, 24);
  SND_CHECK(std::memcmp(out, in + 50, 24) == 0);
  mr.Pop();
  SND_CHECK(!mr.Next(&n));
  // Concurrent producer / consumer: every message arrives intact, in order.
  ll::MessageRing cr;
  SND_REQUIRE(cr.Init(DefaultAllocator(), 4096));
  std::thread prod([&] {
    for (std::uint32_t i = 0; i < 20000; ++i) {
      std::uint32_t msg[5] = {i, i * 3u, i ^ 0xABCDu, i + 7u, i * i};
      const std::uint32_t len = 4u * (1u + i % 5u);
      while (!cr.Push(msg, len)) std::this_thread::yield();
    }
  });
  std::uint32_t expect = 0;
  bool ok = true;
  while (expect < 20000u) {
    if (!cr.Next(&n)) {
      std::this_thread::yield();
      continue;
    }
    std::uint32_t msg[5] = {};
    cr.Copy(0, msg, n);
    ok = ok && n == 4u * (1u + expect % 5u) && msg[0] == expect && (n < 8u || msg[1] == expect * 3u);
    cr.Pop();
    ++expect;
  }
  prod.join();
  SND_CHECK(ok);
  // TripleSlot: the consumer always gets the newest complete value.
  ll::TripleSlot ts;
  SND_REQUIRE(ts.Init(DefaultAllocator(), 8));
  SND_CHECK(!ts.Take());
  std::uint64_t v = 1;
  std::memcpy(ts.Back(), &v, 8);
  ts.Publish();
  v = 2;
  std::memcpy(ts.Back(), &v, 8);
  ts.Publish();
  SND_REQUIRE(ts.Take());
  std::memcpy(&v, ts.Front(), 8);
  SND_CHECK_EQ(v, 2u);
  SND_CHECK(!ts.Take());
}

SND_TEST(LivelinkRecordFormat, ArenaRecordRoundTripWithFallback) {
  snd_config cfg;
  snd_config_init(&cfg);
  cfg.max_playbacks = 100;
  cfg.max_emitters = 10;
  cfg.max_transports = 4;
  cfg.max_user_buffers = 2;
  cfg.max_banks = 8;
  const ll::FallbackCaps caps = ll::CapsOf(cfg);
  // Two commands in the normal region, one in the reserve.
  std::vector<std::uint8_t> normal, reserve;
  auto put = [](std::vector<std::uint8_t>& v, snd::cmd::CommandType t, const void* p, std::size_t n, std::uint32_t wire) {
    const std::size_t at = v.size();
    v.resize(at + wire, 0);
    const snd::cmd::CommandHeader h{static_cast<std::uint16_t>(t), static_cast<std::uint16_t>(wire), 1};
    std::memcpy(v.data() + at, &h, 8);
    std::memcpy(v.data() + at + 8, p, n);
  };
  snd::cmd::SetParam sp{};
  sp.param = 77;
  sp.value = 0.5f;
  put(normal, snd::cmd::CommandType::SetParam, &sp, sizeof(sp), snd::cmd::kWireSize<snd::cmd::SetParam>);
  snd::cmd::Stop st{};
  st.playback = snd::HandleRef{3, 1};
  put(normal, snd::cmd::CommandType::Stop, &st, sizeof(st), snd::cmd::kWireSize<snd::cmd::Stop>);
  put(reserve, snd::cmd::CommandType::Stop, &st, sizeof(st), snd::cmd::kWireSize<snd::cmd::Stop>);
  std::uint64_t pb[2] = {0x5u, 0x8u}, em[1] = {1}, tr[1] = {0}, ub[1] = {0}, bk[1] = {0x80};
  snd::cmd::OverflowParam params[3] = {};
  params[1].used = 1;
  params[1].id = 55;
  params[1].value = 9;
  snd::cmd::OverflowFallback fb{pb, em, tr, ub, bk, params, 3, 2};
  snd::core::LiveArenaView a{};
  a.producer = 2;
  a.appliedBlock = 1234;
  a.flags = snd::cmd::kArenaHasFallback;
  a.normal = normal.data();
  a.normalBytes = static_cast<std::uint32_t>(normal.size());
  a.reserve = reserve.data();
  a.reserveBytes = static_cast<std::uint32_t>(reserve.size());
  a.fallback = &fb;
  ll::MessageRing ring;
  SND_REQUIRE(ring.Init(DefaultAllocator(), 4096));
  SND_REQUIRE(ll::WriteArenaRecord(ring, a, caps));
  a.fallback = nullptr;
  SND_REQUIRE(ll::WriteArenaRecord(ring, a, caps));
  std::vector<std::uint8_t> stream;
  std::uint32_t n = 0;
  while (ring.Next(&n)) {
    const std::size_t at = stream.size();
    stream.resize(at + n);
    ring.Copy(0, stream.data() + at, n);
    ring.Pop();
  }
  const ll::RecordLod lod{2, 1, 1230};
  stream.insert(stream.begin(), reinterpret_cast<const std::uint8_t*>(&lod), reinterpret_cast<const std::uint8_t*>(&lod) + 16);
  ll::RecordReader rr(stream.data(), stream.size());
  ll::RecordItem it;
  SND_REQUIRE(rr.Next(&it));
  SND_CHECK(it.kind == ll::RecordKind::Lod);
  SND_CHECK_EQ(it.lod.block, 1230u);
  SND_REQUIRE(rr.Next(&it));
  SND_CHECK(it.kind == ll::RecordKind::Arena);
  SND_CHECK_EQ(it.arena.producer, 2u);
  SND_CHECK_EQ(it.arena.appliedBlock, 1234u);
  SND_CHECK_EQ(it.sections.normalBytes, normal.size());
  SND_CHECK(std::memcmp(it.normal, normal.data(), normal.size()) == 0);
  SND_CHECK(std::memcmp(it.reserve, reserve.data(), reserve.size()) == 0);
  SND_REQUIRE(it.fallback != nullptr);
  ll::FallbackHeader fh;
  std::memcpy(&fh, it.fallback, sizeof(fh));
  SND_CHECK_EQ(fh.playbackBits, 100u);
  SND_CHECK_EQ(fh.paramCount, 1u);
  SND_CHECK_EQ(fh.lostParams, 2u);
  std::uint64_t w[2];
  std::memcpy(w, it.fallback + sizeof(fh), 16);
  SND_CHECK_EQ(w[0], 5u);
  SND_CHECK_EQ(w[1], 8u);
  SND_REQUIRE(rr.Next(&it));
  SND_CHECK(it.fallback == nullptr);
  SND_CHECK(!rr.Next(&it));
  SND_CHECK(rr.Error() == ll::RecordError::None);
}
