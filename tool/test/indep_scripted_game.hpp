// Independent tests of the tool: a scripted game end of the live link over the
// in-memory transport. It speaks the protocol (livelink-protocol.md §2, §4) by
// hand so that a test decides every answer: the HelloReply status, the result
// of each Ack (SND_OK, SND_ERR_COMMAND_BUFFER_FULL, other errors), when acks
// are released, when the connection drops (game restart), and what profiler
// messages arrive. It keeps every frame the tool sent.
//
// Single-threaded: Pump() runs on the caller's thread (the test thread, or a
// "game" thread in the concurrency test); the tool's client runs its own
// network thread on the other end of the MemoryLink.
#ifndef SND_TOOL_TEST_INDEP_SCRIPTED_GAME_HPP
#define SND_TOOL_TEST_INDEP_SCRIPTED_GAME_HPP

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/format/livelink_protocol.hpp"
#include "snd/livelink/frame_stream.hpp"
#include "snd/livelink/memory_transport.hpp"
#include "snd/livelink/wire.hpp"
#include "snd/platform/clock.hpp"
#include "snd/tool/session.hpp"
#include "snd/types.h"
#include "snd/version.h"

namespace indep {

namespace ll = snd::livelink;

inline std::uint64_t NowNs() { return snd::platform::MonotonicNanoseconds(); }

struct GameFrame {
  ll::FrameHeader h{};
  std::vector<std::uint8_t> p;
  std::uint32_t session = 0;  // ScriptedGame::accepts at receipt
  template <class T>
  T As() const {
    T t{};
    std::memcpy(&t, p.data(), p.size() < sizeof(T) ? p.size() : sizeof(T));
    return t;
  }
  bool Is(ll::MessageType t) const { return h.type == static_cast<std::uint16_t>(t); }
};

inline float ValueF32(const ll::SetValue& v) {
  const std::uint32_t bits = static_cast<std::uint32_t>(v.value);
  float f = 0.0f;
  std::memcpy(&f, &bits, 4);
  return f;
}

class ScriptedGame final : public ll::FrameReader::Handler {
 public:
  ScriptedGame() {
    ok = link.Init(snd::base::DefaultAllocator(), 1u << 20) && reader_.Init(snd::base::DefaultAllocator(), 1u << 20);
  }
  ~ScriptedGame() override { Drop(); }
  ScriptedGame(const ScriptedGame&) = delete;
  ScriptedGame& operator=(const ScriptedGame&) = delete;

  // --- script ---------------------------------------------------------------------------
  ll::HelloStatus status = ll::HelloStatus::Accepted;
  std::uint64_t runtimeHash = 0x1234;
  bool autoAccept = true;  // Pump accepts a waiting tool
  bool holdAcks = false;   // keep acks in heldAcks instead of sending them
  // Result of the Ack for a frame that asks for one (null: SND_OK).
  std::function<std::int32_t(const GameFrame&)> ackResult;

  // --- observations ---------------------------------------------------------------------
  bool ok = false;
  std::vector<GameFrame> frames;  // every frame of the tool except Ping / Pong
  struct HeldAck {
    std::uint32_t seq;
    std::int32_t result;
  };
  std::vector<HeldAck> heldAcks;
  std::uint32_t hellos = 0, accepts = 0;
  bool connected() const { return t_ != nullptr; }

  void Drop() {
    if (t_ != nullptr) link.acceptor().Release(t_);
    t_ = nullptr;
    pendingDrop_ = false;
  }
  bool Accept(std::uint32_t timeoutMs) {
    if (t_ != nullptr) return true;
    t_ = link.acceptor().Accept(timeoutMs);
    if (t_ == nullptr) return false;
    reader_.Reset();
    ++accepts;
    lastPingNs_ = NowNs();
    return true;
  }
  // Reads and answers what the tool sent; keeps the connection alive.
  void Pump() {
    if (t_ == nullptr && autoAccept) Accept(0);
    if (t_ == nullptr) return;
    for (int guard = 0; guard < 64 && t_ != nullptr; ++guard) {
      std::uint8_t buf[8192];
      std::size_t got = 0;
      const ll::IoStatus st = t_->Recv(buf, sizeof buf, &got);
      if (st == ll::IoStatus::Ok) {
        reader_.Feed(buf, got, *this);
        if (pendingDrop_) Drop();
        continue;
      }
      if (st == ll::IoStatus::Closed || st == ll::IoStatus::Error) Drop();
      break;
    }
    if (t_ != nullptr && NowNs() - lastPingNs_ > 200000000ull) {
      lastPingNs_ = NowNs();
      const ll::Ping ping{NowNs()};
      Send(ll::MessageType::Ping, &ping, sizeof ping);
    }
  }
  bool Send(ll::MessageType type, const void* payload, std::uint32_t n, std::uint16_t flags = 0) {
    if (t_ == nullptr) return false;
    std::vector<std::uint8_t> b(ll::kFrameHeaderBytes + n);
    ll::EncodeHeader(b.data(), static_cast<std::uint16_t>(type), flags, n, ++seqOut_);
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
        t_->Wait(false, true, 2);
      }
    }
    return true;
  }
  void SendAck(std::uint32_t seq, std::int32_t result) {
    const ll::Ack a{seq, result};
    Send(ll::MessageType::Ack, &a, sizeof a);
  }
  void ReleaseAcks() {
    for (const HeldAck& a : heldAcks) SendAck(a.seq, a.result);
    heldAcks.clear();
  }
  // Sends a list message (ListHeader + elements).
  template <class T>
  bool SendList(ll::MessageType type, const std::vector<T>& items) {
    std::vector<std::uint8_t> b(sizeof(ll::ListHeader) + items.size() * sizeof(T));
    const ll::ListHeader lh{static_cast<std::uint32_t>(items.size()), static_cast<std::uint32_t>(sizeof(T))};
    std::memcpy(b.data(), &lh, sizeof lh);
    if (!items.empty()) std::memcpy(b.data() + sizeof lh, items.data(), items.size() * sizeof(T));
    return Send(type, b.data(), static_cast<std::uint32_t>(b.size()));
  }

  // Frames of one type (optionally from index `from` of frames).
  std::vector<GameFrame> Of(ll::MessageType t, std::size_t from = 0) const {
    std::vector<GameFrame> out;
    for (std::size_t i = from; i < frames.size(); ++i) {
      if (frames[i].Is(t)) out.push_back(frames[i]);
    }
    return out;
  }

  void OnFrame(const ll::FrameHeader& h, const std::uint8_t* payload) noexcept override {
    GameFrame f;
    f.h = h;
    f.p.assign(payload, payload + h.payloadBytes);
    f.session = accepts;
    if (f.Is(ll::MessageType::Ping)) {
      const ll::Ping ping = f.As<ll::Ping>();
      const ll::Pong pong{ping.senderTimeNs, 0u};
      Send(ll::MessageType::Pong, &pong, sizeof pong);
      return;
    }
    if (f.Is(ll::MessageType::Pong)) return;
    frames.push_back(f);
    if (f.Is(ll::MessageType::Hello)) {
      ++hellos;
      ll::HelloReply r{};
      r.protocolMajor = ll::kProtocolMajor;
      r.protocolMinor = ll::kProtocolMinor;
      r.status = static_cast<std::uint32_t>(status);
      r.runtimeBuildHash = runtimeHash;
      r.contractVersion = SND_CONTRACT_VERSION;
      r.sampleRate = 48000;
      r.blockSize = 256;
      Send(ll::MessageType::HelloReply, &r, sizeof r);
      if (status == ll::HelloStatus::RejectedProtocol) pendingDrop_ = true;
      return;
    }
    if (f.Is(ll::MessageType::SyncDecision)) {
      if (f.As<ll::SyncDecision>().mode == static_cast<std::uint32_t>(ll::SyncMode::Disconnect)) pendingDrop_ = true;
      return;
    }
    if ((h.flags & ll::kFrameWantsAck) != 0u) {
      const std::int32_t result = ackResult ? ackResult(f) : SND_OK;
      if (holdAcks) {
        heldAcks.push_back(HeldAck{h.seq, result});
      } else {
        SendAck(h.seq, result);
      }
    }
  }
  void OnOversized(const ll::FrameHeader&) noexcept override {}

  ll::MemoryLink link;

 private:
  ll::ITransport* t_ = nullptr;
  ll::FrameReader reader_;
  std::uint32_t seqOut_ = 0;
  std::uint64_t lastPingNs_ = 0;
  bool pendingDrop_ = false;
};

// Ticks the session (and pumps the game) until pred() or the timeout.
template <class Pred>
bool Drive(snd::tool::Session& s, ScriptedGame& g, Pred&& pred, std::uint32_t timeoutMs = 4000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    g.Pump();
    s.Tick(NowNs());
    if (pred()) return true;
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(300));
  }
}
// Ticks for a while (no condition).
inline void DriveFor(snd::tool::Session& s, ScriptedGame& g, std::uint32_t ms) {
  Drive(s, g, [] { return false; }, ms);
}

inline snd::tool::ConnectionDesc GameConn(ScriptedGame& g, std::uint64_t hash) {
  snd::tool::ConnectionDesc d;
  d.connector = &g.link.connector();
  d.projectBuildHash = hash;
  d.reconnectDelayMs = 20;
  d.name = "indep-tool";
  return d;
}

}  // namespace indep

#endif  // SND_TOOL_TEST_INDEP_SCRIPTED_GAME_HPP
