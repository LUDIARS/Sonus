// Independent tests of the live link protocol 1.1 (组 L, indep_s7): helpers.
//
// Written from livelink-protocol.md (1.1), the contract header and the public
// livelink headers (properties: indep_s7_properties.md). Reuses the integration
// fixture (tests/integration/livelink_fixture.hpp, indep_livelink_common.hpp) only
// for the System / memory link plumbing; nothing there is changed.
//
//   FakeEditor   an IStructEditor (RT-safe: fixed arrays, atomics) that records the
//                order of Apply / Undo and answers a configurable result.
//   RawGame      a hand-driven GAME end (accepts on a MemoryLink) to emulate a 1.0 game.
//   BussChunk    a BUSS table chunk with the IndepBank rows, Sfx volume as given.
#ifndef SND_LIVELINK_TEST_INDEP_S7_COMMON_HPP
#define SND_LIVELINK_TEST_INDEP_S7_COMMON_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "indep_livelink_common.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/livelink/struct_edit.hpp"

namespace s7 {

namespace bf = snd::bank_format;
namespace bt = snd::buildtool;
namespace ll = snd::livelink;
namespace cmd = snd::cmd;
using snd::HashId;
using indep::F32Bits;

// --- structural edit seam ----------------------------------------------------------------
class FakeEditor final : public ll::IStructEditor {
 public:
  static constexpr std::uint32_t kMax = 256;
  struct Entry {
    std::uint32_t op;
    std::uint32_t targetId;
    std::uint32_t argCount;
    std::uint32_t undo;  // 1: Undo call
    std::uint64_t arg0;
  };
  snd_result Apply(snd::core::System&, const ll::StructEditRequest& e) noexcept override {
    Push(e, 0u);
    return static_cast<snd_result>(applyResult.load(std::memory_order_relaxed));
  }
  snd_result Undo(snd::core::System&, const ll::StructEditRequest& e) noexcept override {
    Push(e, 1u);
    return SND_OK;
  }
  std::uint32_t Count() const noexcept { return count.load(std::memory_order_acquire); }
  Entry At(std::uint32_t i) const noexcept { return entries[i]; }
  std::atomic<std::int32_t> applyResult{SND_OK};
  std::atomic<std::uint32_t> count{0};
  Entry entries[kMax] = {};

 private:
  void Push(const ll::StructEditRequest& e, std::uint32_t undo) noexcept {
    const std::uint32_t i = count.load(std::memory_order_relaxed);
    if (i >= kMax) return;
    entries[i] = Entry{static_cast<std::uint32_t>(e.op), e.targetId, e.argCount, undo,
                       (e.argCount != 0u && e.args != nullptr) ? e.args[0] : 0u};
    count.store(i + 1u, std::memory_order_release);
  }
};

// --- a hand-driven game end ----------------------------------------------------------------
class RawGame final : public ll::FrameReader::Handler {
 public:
  struct Frame {
    ll::FrameHeader header;
    std::vector<std::uint8_t> payload;
  };
  RawGame() { reader_.Init(snd::base::DefaultAllocator(), 1u << 20); }
  ~RawGame() override { Close(); }
  bool Accept(ll::IAcceptor& a, std::uint32_t timeoutMs = 3000) {
    acceptor_ = &a;
    t_ = a.Accept(timeoutMs);
    return t_ != nullptr;
  }
  void Close() {
    if (t_ != nullptr) acceptor_->Release(t_);
    t_ = nullptr;
  }
  bool Send(ll::MessageType type, const void* payload, std::uint32_t n, std::uint16_t flags = 0,
            std::uint32_t seq = 1) {
    std::vector<std::uint8_t> b(ll::kFrameHeaderBytes + n);
    ll::EncodeHeader(b.data(), static_cast<std::uint16_t>(type), flags, n, seq);
    if (n != 0u) std::memcpy(b.data() + ll::kFrameHeaderBytes, payload, n);
    const std::uint8_t* p = b.data();
    std::size_t left = b.size();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (left != 0u) {
      std::size_t sent = 0;
      const ll::IoStatus st = t_->Send(p, left, &sent);
      if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) return false;
      p += sent;
      left -= sent;
      if (left != 0u) {
        if (std::chrono::steady_clock::now() > end) return false;
        t_->Wait(false, true, 5);
      }
    }
    return true;
  }
  bool Pump(std::uint32_t ms) {
    if (t_ == nullptr) return false;
    t_->Wait(true, false, ms);
    for (;;) {
      std::uint8_t buf[4096];
      std::size_t got = 0;
      const ll::IoStatus st = t_->Recv(buf, sizeof(buf), &got);
      if (st == ll::IoStatus::Ok) {
        reader_.Feed(buf, got, *this);
        continue;
      }
      if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) {
        closed = true;
        return false;
      }
      return true;
    }
  }
  // Waits for a frame of `type` (consumed). Answers Pings meanwhile. nullptr on timeout.
  const Frame* WaitFor(ll::MessageType type, std::uint32_t timeoutMs = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
      for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].header.type == static_cast<std::uint16_t>(type)) {
          last_ = frames[i];
          frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(i));
          return &last_;
        }
      }
      if (std::chrono::steady_clock::now() > end) return nullptr;
      if (!Pump(5) && closed) return nullptr;
      AnswerPings();
    }
  }
  void AnswerPings() {
    for (std::size_t i = 0; i < frames.size();) {
      if (frames[i].header.type == static_cast<std::uint16_t>(ll::MessageType::Ping)) {
        const auto ping = ll::ReadFixed<ll::Ping>(frames[i].payload.data(),
                                                  static_cast<std::uint32_t>(frames[i].payload.size()));
        ll::Pong pong{ping.senderTimeNs, 0};
        Send(ll::MessageType::Pong, &pong, sizeof(pong), 0, frames[i].header.seq);
        frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
    }
  }
  // Pumps for `ms`, answering pings; keeps every other frame.
  void PumpFor(std::uint32_t ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      if (!Pump(5) && closed) return;
      AnswerPings();
    }
  }
  void OnFrame(const ll::FrameHeader& h, const std::uint8_t* p) noexcept override {
    frames.push_back(Frame{h, std::vector<std::uint8_t>(p, p + h.payloadBytes)});
  }
  void OnOversized(const ll::FrameHeader&) noexcept override {}

  std::vector<Frame> frames;
  bool closed = false;

 private:
  ll::IAcceptor* acceptor_ = nullptr;
  ll::ITransport* t_ = nullptr;
  ll::FrameReader reader_;
  Frame last_{};
};

// --- data -----------------------------------------------------------------------------------
// BUSS chunk with IndepBank's two rows (Sfx -> Master). extraRow: a third row (row count differs).
inline std::vector<std::uint8_t> BussChunk(float sfxDb, bool extraRow = false, std::uint32_t sfxParent = 1u) {
  namespace c = bf::col;
  bt::TableBuilder bu(bf::chunk::BUSS);
  std::uint32_t r = bu.AddRow();
  bu.U32(c::BUSS_id, r, HashId("Sfx"));
  bu.U32(c::BUSS_parent, r, sfxParent);
  bu.U32(c::BUSS_channels, r, 2u);
  bu.F32(c::BUSS_volumeDb, r, sfxDb);
  r = bu.AddRow();
  bu.U32(c::BUSS_id, r, HashId("Master"));
  bu.U32(c::BUSS_parent, r, bf::kNone);
  bu.U32(c::BUSS_channels, r, 2u);
  bu.U32(c::BUSS_flags, r, bf::kBusMaster);
  if (extraRow) {
    r = bu.AddRow();
    bu.U32(c::BUSS_id, r, HashId("Extra"));
    bu.U32(c::BUSS_parent, r, 1u);
    bu.U32(c::BUSS_channels, r, 2u);
  }
  return bu.Serialize();
}

inline ll::SetValue SfxVolumeCell() { return lltest::BusVolume("Sfx", 0.0f); }

inline std::uint32_t F32Of(std::uint64_t bits) { return static_cast<std::uint32_t>(bits); }

// Waits (rendering) for the CellValue answering `seq`.
inline bool WaitCell(indep::Tool& t, std::uint32_t seq, ll::CellValue* out, std::uint32_t ms = 10000) {
  if (seq == 0u) return false;
  return t.f.StepUntil([&] { return t.c->TakeCell(seq, out); }, ms);
}

// True if `text` appears in any error the client received.
inline bool HasErrorText(ll::Client* c, const char* text) {
  ll::Error e[64];
  const std::uint32_t n = c->CopyErrors(e, 64);
  for (std::uint32_t i = 0; i < n; ++i) {
    char m[57] = {};
    std::memcpy(m, e[i].message, 56);
    if (std::strstr(m, text) != nullptr) return true;
  }
  return false;
}

// Number of voices the core lists right now.
inline std::uint32_t VoiceCount(lltest::Fixture& f) {
  ll::VoiceInfo v[64];
  return f.sys->LiveVoices(v, 64);
}

// A config with a small dev reserve.
inline snd_config ReserveConfig(std::uint32_t buses, std::uint32_t effects, std::uint64_t bytes) {
  snd_config c = lltest::Config();
  c.dev_reserve.buses = buses;
  c.dev_reserve.effects = effects;
  c.dev_reserve.bytes = bytes;
  return c;
}

// Appends a raw record to a byte vector (8-byte padded).
template <class T>
void PutRecord(std::vector<std::uint8_t>& v, const T& rec) {
  const std::size_t at = v.size();
  v.resize(at + ((sizeof(T) + 7u) & ~std::size_t{7}));
  std::memcpy(v.data() + at, &rec, sizeof(T));
}

}  // namespace s7

#endif  // SND_LIVELINK_TEST_INDEP_S7_COMMON_HPP
