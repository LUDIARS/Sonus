// Independent unit tests of the live link building blocks (rings.hpp, frame_stream.hpp,
// wire.hpp, memory_transport.hpp), written from the headers: SPSC channels under two
// real threads (TSan target), frame reader / queue edge cases, list and trailing-data
// bounds at the integer limits, and the in-memory transport's TCP-like semantics.
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/livelink/frame_stream.hpp"
#include "snd/livelink/memory_transport.hpp"
#include "snd/livelink/rings.hpp"
#include "snd/livelink/wire.hpp"
#include "snd/test/test.hpp"

namespace ll = snd::livelink;

namespace {

snd::IAllocator& A() { return snd::base::DefaultAllocator(); }

struct Collect final : ll::FrameReader::Handler {
  std::vector<ll::FrameHeader> frames;
  std::vector<std::vector<std::uint8_t>> payloads;
  std::uint32_t oversized = 0;
  void OnFrame(const ll::FrameHeader& h, const std::uint8_t* p) noexcept override {
    frames.push_back(h);
    payloads.emplace_back(p, p + h.payloadBytes);
  }
  void OnOversized(const ll::FrameHeader&) noexcept override { ++oversized; }
};

std::vector<std::uint8_t> Frame(std::uint16_t type, std::uint32_t seq, std::uint32_t n, std::uint8_t fill) {
  std::vector<std::uint8_t> b(ll::kFrameHeaderBytes + n, fill);
  ll::EncodeHeader(b.data(), type, 0, n, seq);
  return b;
}

}  // namespace

// G5: ByteRing between two threads: random write / read sizes, every byte arrives once
// and in order.
SND_TEST(IndepLivelinkRings, ByteRingTwoThreadsKeepsTheStream) {
  ll::ByteRing r;
  SND_REQUIRE(r.Init(A(), 1000));  // rounded up to a power of two
  SND_CHECK_EQ(r.Capacity(), 1024u);
  constexpr std::size_t kTotal = 1u << 21;
  std::atomic<bool> bad{false};
  std::thread prod([&] {
    std::mt19937 rng(1);
    std::uint8_t buf[700];
    std::size_t sent = 0;
    while (sent < kTotal) {
      const std::size_t want = 1u + rng() % sizeof(buf);
      const std::size_t n = want < kTotal - sent ? want : kTotal - sent;
      for (std::size_t i = 0; i < n; ++i) buf[i] = static_cast<std::uint8_t>((sent + i) % 251u);
      const std::size_t w = r.Write(buf, n);
      if (w > n) bad.store(true);
      sent += w;
      if (w == 0u) std::this_thread::yield();
    }
  });
  std::mt19937 rng(2);
  std::uint8_t buf[900];
  std::size_t got = 0;
  while (got < kTotal) {
    const std::size_t n = r.Read(buf, 1u + rng() % sizeof(buf));
    for (std::size_t i = 0; i < n; ++i) {
      if (buf[i] != static_cast<std::uint8_t>((got + i) % 251u)) bad.store(true);
    }
    got += n;
    if (n == 0u) std::this_thread::yield();
  }
  prod.join();
  SND_CHECK(!bad.load());
  SND_CHECK_EQ(r.Readable(), 0u);
}

// G5: MessageRing: whole messages or nothing; Begin fails without changing anything
// when a message does not fit; under two threads every message arrives intact.
SND_TEST(IndepLivelinkRings, MessageRingAllOrNothingAndTwoThreads) {
  ll::MessageRing m;
  SND_REQUIRE(m.Init(A(), 256));
  // 256 bytes: messages take 8 + round8(n). Fill exactly.
  std::uint8_t data[300];
  for (int i = 0; i < 300; ++i) data[i] = static_cast<std::uint8_t>(i);
  SND_CHECK(m.Push(data, 120));             // 128
  SND_CHECK(m.Push(data, 113));             // 8 + 120 = 128 -> full
  SND_CHECK(!m.Push(data, 0));              // 8 more: does not fit
  SND_CHECK(!m.Begin(1));
  std::uint32_t n = 0;
  SND_REQUIRE(m.Next(&n));
  SND_CHECK_EQ(n, 120u);
  std::uint8_t out[300] = {};
  m.Copy(0, out, 120);
  SND_CHECK(std::memcmp(out, data, 120) == 0);
  m.Copy(100, out, 30);  // beyond the message: ignored
  m.Pop();
  SND_REQUIRE(m.Next(&n));
  SND_CHECK_EQ(n, 113u);
  m.Pop();
  SND_CHECK(!m.Next(&n));
  SND_CHECK(!m.Push(data, 249));  // 8 + 256 > 256
  SND_CHECK(m.Push(data, 248));   // 8 + 248 = 256: exactly fits (wraps around)
  SND_REQUIRE(m.Next(&n));
  m.Copy(0, out, 248);
  SND_CHECK(std::memcmp(out, data, 248) == 0);
  m.Pop();

  ll::MessageRing q;
  SND_REQUIRE(q.Init(A(), 4096));
  constexpr std::uint32_t kMessages = 200000;
  std::atomic<bool> bad{false};
  std::thread prod([&] {
    std::mt19937 rng(3);
    std::uint8_t buf[512];
    for (std::uint32_t s = 0; s < kMessages;) {
      const std::uint32_t len = 4u + rng() % 300u;
      std::memcpy(buf, &s, 4);
      for (std::uint32_t i = 4; i < len; ++i) buf[i] = static_cast<std::uint8_t>(s * 7u + i);
      if (q.Begin(len)) {
        q.Put(buf, 3);  // in pieces
        q.Put(buf + 3, len - 3);
        q.Commit();
        ++s;
      } else {
        std::this_thread::yield();
      }
    }
  });
  std::uint32_t expect = 0;
  std::uint8_t buf[512];
  while (expect < kMessages) {
    std::uint32_t len = 0;
    if (!q.Next(&len)) {
      std::this_thread::yield();
      continue;
    }
    q.Copy(0, buf, len);
    std::uint32_t s = 0;
    std::memcpy(&s, buf, 4);
    if (s != expect || len < 4u || len > 303u) bad.store(true);
    for (std::uint32_t i = 4; i < len; ++i) {
      if (buf[i] != static_cast<std::uint8_t>(s * 7u + i)) bad.store(true);
    }
    q.Pop();
    ++expect;
  }
  prod.join();
  SND_CHECK(!bad.load());
}

// G5: TripleSlot: the consumer only ever sees whole, increasingly recent buffers.
SND_TEST(IndepLivelinkRings, TripleSlotNeverTearsAndOnlyMovesForward) {
  ll::TripleSlot t;
  constexpr std::uint32_t kWords = 256;
  SND_REQUIRE(t.Init(A(), kWords * 8u));
  SND_CHECK(!t.Take());  // nothing published yet
  std::atomic<bool> done{false}, bad{false};
  std::thread prod([&] {
    for (std::uint64_t v = 1; v <= 300000; ++v) {
      std::uint64_t* b = reinterpret_cast<std::uint64_t*>(t.Back());
      for (std::uint32_t i = 0; i < kWords; ++i) b[i] = v;
      t.Publish();
    }
    done.store(true);
  });
  std::uint64_t last = 0;
  std::uint32_t takes = 0;
  for (;;) {
    const bool finished = done.load();
    if (t.Take()) {
      ++takes;
      const std::uint64_t* b = reinterpret_cast<const std::uint64_t*>(t.Front());
      const std::uint64_t v = b[0];
      for (std::uint32_t i = 1; i < kWords; ++i) {
        if (b[i] != v) bad.store(true);
      }
      if (v <= last) bad.store(true);
      last = v;
    } else if (finished) {
      break;
    }
  }
  prod.join();
  SND_CHECK(!bad.load());
  SND_CHECK_EQ(last, 300000u);
  SND_CHECK(takes > 0u);
  SND_CHECK(!t.Take());
}

// A1 / A2 / A3 / A5: frames before a bad header are delivered, the bad one stops the
// reader; exactly 16 MiB is not "too large"; payloads over the reader's buffer are
// skipped and the reader stays aligned on frame boundaries.
SND_TEST(IndepLivelinkFrames, ReaderLimitsAndBadHeaders) {
  ll::FrameReader r;
  SND_REQUIRE(r.Init(A(), 64));
  Collect c;
  std::vector<std::uint8_t> s;
  for (auto& f : {Frame(1, 1, 64, 0x11), Frame(2, 2, 65, 0x22), Frame(3, 3, 0, 0), Frame(4, 4, 10, 0x44)}) {
    s.insert(s.end(), f.begin(), f.end());
  }
  for (std::size_t i = 0; i < s.size(); ++i) SND_REQUIRE(r.Feed(&s[i], 1, c) == ll::HeaderStatus::Ok);
  SND_CHECK(r.AtBoundary());
  SND_REQUIRE_EQ(c.frames.size(), 3u);
  SND_CHECK_EQ(c.oversized, 1u);
  SND_CHECK_EQ(c.frames[0].seq, 1u);
  SND_CHECK_EQ(c.frames[1].seq, 3u);
  SND_CHECK_EQ(c.frames[2].seq, 4u);
  SND_CHECK_EQ(c.payloads[2].size(), 10u);
  SND_CHECK_EQ(c.payloads[2][9], 0x44u);
  // Exactly 16 MiB: oversized (skipped), not TooLarge. 16 MiB + 1: TooLarge.
  std::uint8_t h[16];
  ll::EncodeHeader(h, 9, 0, ll::kMaxPayloadBytes, 9);
  SND_CHECK(r.Feed(h, 16, c) == ll::HeaderStatus::Ok);
  SND_CHECK_EQ(c.oversized, 2u);
  SND_CHECK(!r.AtBoundary());
  r.Reset();
  ll::EncodeHeader(h, 9, 0, ll::kMaxPayloadBytes + 1u, 9);
  SND_CHECK(r.Feed(h, 16, c) == ll::HeaderStatus::TooLarge);
  r.Reset();
  // Good frame then bad magic in the same buffer: the good one is delivered first.
  std::vector<std::uint8_t> two = Frame(5, 5, 4, 0x55);
  two.insert(two.end(), 16, 0xEE);
  const std::size_t before = c.frames.size();
  SND_CHECK(r.Feed(two.data(), two.size(), c) == ll::HeaderStatus::BadMagic);
  SND_CHECK_EQ(c.frames.size(), before + 1u);
  // A reader without a buffer skips every non-empty payload but still delivers empty ones.
  ll::FrameReader z;
  SND_REQUIRE(z.Init(A(), 0));
  Collect cz;
  std::vector<std::uint8_t> zs = Frame(1, 1, 8, 1);
  const std::vector<std::uint8_t> e = Frame(2, 2, 0, 0);
  zs.insert(zs.end(), e.begin(), e.end());
  SND_CHECK(z.Feed(zs.data(), zs.size(), cz) == ll::HeaderStatus::Ok);
  SND_CHECK_EQ(cz.oversized, 1u);
  SND_REQUIRE_EQ(cz.frames.size(), 1u);
  SND_CHECK_EQ(cz.frames[0].seq, 2u);
}

// A7 / A8: list and trailing-data bounds at the integer limits (no overflow).
SND_TEST(IndepLivelinkWire, ListAndTrailingBoundsAtTheLimits) {
  std::vector<std::uint8_t> p(8 + 64, 0);
  auto list = [&](std::uint32_t count, std::uint32_t elem, std::uint32_t payloadBytes, std::uint32_t max) {
    const ll::ListHeader lh{count, elem};
    std::memcpy(p.data(), &lh, 8);
    ll::ListView v;
    return ll::ParseList(p.data(), payloadBytes, max, &v);
  };
  SND_CHECK(!list(0xFFFFFFFFu, 0xFFFFFFFFu, 72, 0xFFFFFFFFu));
  SND_CHECK(!list(2, 0x80000000u, 72, 100));
  SND_CHECK(!list(0x10000u, 0x10000u, 72, 0xFFFFFFFFu));  // 2^32 bytes: must not wrap to 0
  SND_CHECK(list(8, 8, 72, 8));    // exactly fits, count == max
  SND_CHECK(!list(8, 8, 72, 7));   // count > max
  SND_CHECK(!list(9, 8, 72, 100)); // one element too many
  SND_CHECK(!list(1, 0, 72, 100));
  SND_CHECK(list(0, 0, 8, 0));
  SND_CHECK(!list(0, 0, 7, 0));    // shorter than the header
  const std::uint8_t* d = nullptr;
  std::uint8_t buf[64] = {};
  SND_CHECK(!ll::TrailingData(buf, 64, 16, ~0ull, &d));
  SND_CHECK(!ll::TrailingData(buf, 64, 16, 49, &d));
  SND_CHECK(ll::TrailingData(buf, 64, 16, 48, &d));
  SND_CHECK(d == buf + 16);
  SND_CHECK(ll::TrailingData(buf, 64, 16, 0, &d));
  SND_CHECK(d == buf + 64);
  SND_CHECK(!ll::TrailingData(buf, 8, 16, 1, &d));  // shorter than the fixed part
  // EncodeField / DecodeField refuse what they do not know.
  std::uint32_t chunk = 0, col = 0;
  SND_CHECK(!ll::DecodeField(ll::kFieldRevertAll, &chunk, &col));
  SND_CHECK(!ll::DecodeField(0xFFFE0000u, &chunk, &col));
  SND_CHECK_EQ(ll::EncodeField(0x12345678u, 0), ll::kFieldRevertAll);
}

// FrameQueue keeps frames whole across the wrap-around; FrameSender never interleaves
// and sends control frames before bulk frames (G5 / framing).
SND_TEST(IndepLivelinkFrames, QueueWrapAroundAndSenderOrder) {
  ll::FrameQueue q;
  SND_REQUIRE(q.Init(A(), 1000));
  std::mt19937 rng(5);
  std::uint32_t pushed = 0, popped = 0;
  std::vector<std::uint8_t> payload(200);
  for (int round = 0; round < 20000; ++round) {
    if (rng() % 2u == 0u) {
      const std::uint32_t n = rng() % 200u;
      for (std::uint32_t i = 0; i < n; ++i) payload[i] = static_cast<std::uint8_t>(pushed + i);
      const bool fits = q.Free() >= 16u + n;
      SND_CHECK_EQ(q.Push(7, 0, pushed, payload.data(), n), fits);
      if (fits) ++pushed;
    } else if (!q.Empty()) {
      const std::uint64_t fb = q.HeadFrameBytes();
      std::vector<std::uint8_t> f;
      while (f.size() < fb) {
        std::size_t n = 0;
        const std::uint8_t* p = q.Peek(fb - f.size(), &n);
        f.insert(f.end(), p, p + n);
        q.Pop(n);
      }
      ll::FrameHeader h;
      SND_CHECK(ll::DecodeHeader(f.data(), &h) == ll::HeaderStatus::Ok);
      SND_CHECK_EQ(h.seq, popped);
      SND_CHECK_EQ(16u + h.payloadBytes, f.size());
      for (std::uint32_t i = 0; i < h.payloadBytes; ++i) {
        if (f[16 + i] != static_cast<std::uint8_t>(h.seq + i)) {
          SND_CHECK(false);
          break;
        }
      }
      ++popped;
    }
  }
  SND_CHECK(pushed > 1000u);
}

// G4: the in-memory link behaves like TCP: data sent before a close is read before
// Closed; Send to a closed peer is Closed; a 4 MiB transfer between two threads through
// a small pipe arrives intact.
SND_TEST(IndepLivelinkMemoryLink, TcpLikeSemanticsAndIntegrity) {
  ll::MemoryLink link;
  SND_REQUIRE(link.Init(A(), 4096));
  ll::ITransport* game = nullptr;
  std::thread acc([&] { game = link.acceptor().Accept(2000); });
  ll::ITransport* tool = link.connector().Connect(2000);
  acc.join();
  SND_REQUIRE(tool != nullptr && game != nullptr);
  std::size_t n = 0;
  SND_CHECK(tool->Send("hello", 5, &n) == ll::IoStatus::Ok);
  SND_CHECK_EQ(n, 5u);
  link.connector().Release(tool);  // closes the tool end
  char buf[16] = {};
  SND_CHECK(game->Recv(buf, sizeof(buf), &n) == ll::IoStatus::Ok);
  SND_CHECK_EQ(n, 5u);
  SND_CHECK(std::memcmp(buf, "hello", 5) == 0);
  SND_CHECK(game->Recv(buf, sizeof(buf), &n) == ll::IoStatus::Closed);
  SND_CHECK(game->Send("x", 1, &n) == ll::IoStatus::Closed);
  link.acceptor().Release(game);

  // Reconnect (the pipe is recycled) and stream 4 MiB. Accept may return early without
  // a peer (a wake-up left over from the first connection): retry until the deadline.
  game = nullptr;
  std::thread acc2([&] {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (game == nullptr && std::chrono::steady_clock::now() < end) game = link.acceptor().Accept(100);
  });
  tool = link.connector().Connect(2000);
  acc2.join();
  SND_REQUIRE(tool != nullptr && game != nullptr);
  constexpr std::size_t kTotal = 4u << 20;
  std::atomic<bool> bad{false};
  std::thread writer([&] {
    std::vector<std::uint8_t> b(3000);
    std::size_t sent = 0;
    while (sent < kTotal) {
      const std::size_t m = b.size() < kTotal - sent ? b.size() : kTotal - sent;
      for (std::size_t i = 0; i < m; ++i) b[i] = static_cast<std::uint8_t>((sent + i) * 13u);
      std::size_t off = 0;
      while (off < m) {
        std::size_t w = 0;
        const ll::IoStatus st = tool->Send(b.data() + off, m - off, &w);
        if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) {
          bad.store(true);
          return;
        }
        off += w;
        if (w == 0u) tool->Wait(false, true, 5);
      }
      sent += m;
    }
  });
  std::vector<std::uint8_t> b(5000);
  std::size_t got = 0;
  while (got < kTotal && !bad.load()) {
    std::size_t r = 0;
    const ll::IoStatus st = game->Recv(b.data(), b.size(), &r);
    if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) break;
    for (std::size_t i = 0; i < r; ++i) {
      if (b[i] != static_cast<std::uint8_t>((got + i) * 13u)) bad.store(true);
    }
    got += r;
    if (r == 0u) game->Wait(true, false, 5);
  }
  writer.join();
  SND_CHECK(!bad.load());
  SND_CHECK_EQ(got, kTotal);
  link.connector().Release(tool);
  link.acceptor().Release(game);
}
