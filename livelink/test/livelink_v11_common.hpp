// Helpers of the protocol 1.1 tests (livelink/test/livelink_v11_*.cpp). Built on the
// integration fixture (tests/integration/livelink_fixture.hpp: one core System with the
// analytic Sample kind, a dev bank in a memory FS, the in-memory link), included
// unchanged.
#ifndef SND_LIVELINK_TEST_V11_COMMON_HPP
#define SND_LIVELINK_TEST_V11_COMMON_HPP

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "indep_livelink_common.hpp"
#include "livelink_fixture.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/livelink/struct_edit.hpp"

namespace v11 {

using namespace lltest;
namespace cmd = snd::cmd;
using snd::HashId;

inline double Ratio(float db) { return std::pow(10.0, db / 20.0); }
inline std::uint32_t Bits(float f) {
  std::uint32_t b = 0;
  std::memcpy(&b, &f, 4);
  return b;
}

// A cell address for RevertValue / ReadCell (the SetValue form, value ignored).
inline ll::SetValue Cell(snd::Id id, std::uint32_t chunk, std::uint32_t column, std::uint32_t index = 0) {
  ll::SetValue v{};
  v.runtimeId = id;
  v.chunk = chunk;
  v.column = column;
  v.index = index;
  return v;
}
inline ll::SetValue BusVolumeCell(const char* bus) { return Cell(HashId(bus), bf::chunk::BUSS, bf::col::BUSS_volumeDb); }

// A BUSS table chunk with the volumeDb column only (one row per value; the other columns are
// absent, so a ReplaceChunk leaves them as they are). bt::TableBuilder always writes the
// required columns, hence by hand.
inline std::vector<std::uint8_t> BussVolumes(std::initializer_list<float> db) {
  std::vector<std::uint8_t> b(64, 0u);
  const bf::TableChunkHeader th{static_cast<std::uint32_t>(db.size()), 1u, 0u, 0u};
  std::memcpy(b.data(), &th, sizeof(th));
  const bf::ColumnEntry ce{bf::col::BUSS_volumeDb, 4u, 0u, 64u};
  std::memcpy(b.data() + sizeof(th), &ce, sizeof(ce));
  for (float v : db) {
    const std::uint32_t x = Bits(v);
    const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
    b.insert(b.end(), p, p + 4);
  }
  return b;
}

// A structural editor that accepts everything and logs what it was asked (audio thread:
// in these tests the test thread renders the blocks, so the log is read on the same thread).
struct FakeEditor final : ll::IStructEditor {
  struct Call {
    bool undo;
    ll::StructOp op;
    std::uint32_t target;
    std::uint32_t argCount;
    std::uint64_t arg0;
    std::uint32_t session;
  };
  std::vector<Call> calls;
  snd_result answer = SND_OK;
  FakeEditor() { calls.reserve(256); }
  snd_result Apply(snd::core::System&, const ll::StructEditRequest& e) noexcept override {
    if (calls.size() < calls.capacity()) {
      calls.push_back(Call{false, e.op, e.targetId, e.argCount, e.argCount != 0u ? e.args[0] : 0u, e.session});
    }
    return answer;
  }
  snd_result Undo(snd::core::System&, const ll::StructEditRequest& e) noexcept override {
    if (calls.size() < calls.capacity()) {
      calls.push_back(Call{true, e.op, e.targetId, e.argCount, e.argCount != 0u ? e.args[0] : 0u, e.session});
    }
    return SND_OK;
  }
};

// A game, its server and a 1.1 client over the in-memory link. "Tone" plays (base level).
struct Session {
  Fixture f;
  ll::Client* client = nullptr;
  float base = 0;
  explicit Session(const snd_config& cfg = Config(), ll::RuntimeDesc rd = {}, ll::ServerDesc sd = {},
                   std::uint64_t toolHash = kProjectHash, bool deferSync = false, bool tone = true,
                   std::uint32_t historySeconds = 10)
      : f(Hooks::Live, cfg, nullptr, rd) {
    f.Load(DevBank());
    if (tone) {
      f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
      f.Steps(16);
      base = f.LastLeft();
    }
    SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
    ll::ClientDesc cd = MemoryClient(f.link, toolHash);
    cd.deferSyncDecision = deferSync;
    cd.historySeconds = historySeconds;
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
  std::int32_t Ack(std::uint32_t seq) {
    std::int32_t r = 1;
    if (seq == 0u) return 2;
    if (!f.StepUntil([&] { return client->TakeAck(seq, &r); })) return 3;
    return r;
  }
  bool WaitCellValue(std::uint32_t seq, ll::CellValue* out) {
    if (seq == 0u) return false;
    return f.StepUntil([&] { return client->TakeCell(seq, out); });
  }
  double Level() const { return f.LastLeft() / base; }
};

// A hand-driven game end (raw frames) for compatibility tests against an older game.
class RawGame final : public ll::FrameReader::Handler {
 public:
  struct Frame {
    ll::FrameHeader header;
    std::vector<std::uint8_t> payload;
  };
  RawGame() { reader_.Init(snd::base::DefaultAllocator(), 1u << 20); }
  ~RawGame() override {
    if (t_ != nullptr) acceptor_->Release(t_);
  }
  bool Accept(ll::IAcceptor& a, std::uint32_t timeoutMs = 2000) {
    acceptor_ = &a;
    t_ = a.Accept(timeoutMs);
    return t_ != nullptr;
  }
  bool Send(ll::MessageType type, const void* payload, std::uint32_t n, std::uint32_t seq = 1) {
    std::vector<std::uint8_t> b(ll::kFrameHeaderBytes + n);
    ll::EncodeHeader(b.data(), static_cast<std::uint16_t>(type), 0, n, seq);
    if (n != 0u) std::memcpy(b.data() + ll::kFrameHeaderBytes, payload, n);
    std::size_t off = 0;
    for (int i = 0; i < 1000 && off < b.size(); ++i) {
      std::size_t sent = 0;
      const ll::IoStatus st = t_->Send(b.data() + off, b.size() - off, &sent);
      if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) return false;
      off += sent;
      if (off < b.size()) t_->Wait(false, true, 5);
    }
    return off == b.size();
  }
  bool SendRawBytes(const std::vector<std::uint8_t>& b) {
    std::size_t off = 0;
    for (int i = 0; i < 1000 && off < b.size(); ++i) {
      std::size_t sent = 0;
      const ll::IoStatus st = t_->Send(b.data() + off, b.size() - off, &sent);
      if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) return false;
      off += sent;
      if (off < b.size()) t_->Wait(false, true, 5);
    }
    return off == b.size();
  }
  void Pump(std::uint32_t ms) {
    t_->Wait(true, false, ms);
    for (;;) {
      std::uint8_t buf[4096];
      std::size_t got = 0;
      if (t_->Recv(buf, sizeof(buf), &got) != ll::IoStatus::Ok) return;
      reader_.Feed(buf, got, *this);
    }
  }
  const Frame* WaitFor(ll::MessageType type, std::uint32_t timeoutMs = 2000) {
    for (std::uint32_t waited = 0; waited < timeoutMs; waited += 5) {
      for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].header.type == static_cast<std::uint16_t>(type)) {
          last_ = frames[i];
          frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(i));
          return &last_;
        }
      }
      Pump(5);
    }
    return nullptr;
  }
  void OnFrame(const ll::FrameHeader& h, const std::uint8_t* p) noexcept override {
    frames.push_back(Frame{h, std::vector<std::uint8_t>(p, p + h.payloadBytes)});
  }
  void OnOversized(const ll::FrameHeader&) noexcept override {}
  std::vector<Frame> frames;

 private:
  ll::IAcceptor* acceptor_ = nullptr;
  ll::ITransport* t_ = nullptr;
  ll::FrameReader reader_;
  Frame last_{};
};

template <class T>
void Put(std::vector<std::uint8_t>& v, const T& x) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
  v.insert(v.end(), p, p + sizeof(T));
}

}  // namespace v11

#endif  // SND_LIVELINK_TEST_V11_COMMON_HPP
