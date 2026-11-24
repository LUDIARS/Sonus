// Live connection of the tool to a game (design.md 11.2).
//
// Owns one livelink::Client: the target (TCP address and port, or any
// IConnector), the state shown to the user, reconnection and the answer to a
// build hash mismatch:
//   - MismatchPolicy::SendDiffs: the client answers SyncDecision "send diffs";
//     the session is usable and Connection::mismatch() is true. The mixer then
//     sends every value it manages (MixerModel, CellSync::SendAll), so the
//     game's mixer matches the tool's project (the tool's data is the truth).
//   - MismatchPolicy::Refuse: the client answers "disconnect"; the state stays
//     Rejected (no retry) until Open is called again.
// Poll() runs on the UI thread once per frame; it reports the start of each
// new session (first handshake and every reconnect) so that the models can
// re-send their state.
#ifndef SND_TOOL_CONNECTION_HPP
#define SND_TOOL_CONNECTION_HPP

#include <cstdint>
#include <string>

#include "snd/format/livelink_protocol.hpp"

namespace snd::livelink {
class Client;
class IConnector;
}  // namespace snd::livelink

namespace snd::tool {

enum class MismatchPolicy : std::uint8_t { SendDiffs = 0, Refuse = 1 };

enum class LinkState : std::uint8_t {
  Offline = 0,     // not opened (or closed)
  Connecting = 1,  // trying / waiting to retry
  Handshake = 2,
  Online = 3,      // commands accepted
  Rejected = 4,    // protocol mismatch or mismatch refused (no retry)
};
const char* LinkStateName(LinkState s);

struct ConnectionDesc {
  std::string address = "127.0.0.1";
  std::uint16_t port = livelink::kDefaultPort;
  livelink::IConnector* connector = nullptr;  // overrides address/port (tests, in-process)
  std::string name = "sonus-tool";
  std::uint64_t projectBuildHash = 0;
  MismatchPolicy policy = MismatchPolicy::SendDiffs;
  bool autoReconnect = true;
  std::uint32_t reconnectDelayMs = 250;
  std::uint32_t timeoutMs = 5000;
  std::uint32_t historySeconds = 10;
  std::uint32_t profilerMask = 0x7Fu;  // every ProfilerMask bit
  std::uint32_t meterHz = 30;
  // 1.1 (group P): on BuildMismatch stay in Handshake (awaitingSync()) until DecideSync is
  // called (livelink ClientDesc::deferSyncDecision); `policy` is then not used. The game waits
  // for the answer for its handshake timeout (5 s by default), then drops the connection; the
  // client reconnects and asks again.
  bool deferSyncDecision = false;
};

struct ConnectionEvents {
  bool sessionStarted = false;  // a handshake completed since the last Poll (resend state)
  bool sessionLost = false;     // the connection dropped since the last Poll
};

class Connection {
 public:
  Connection() = default;
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // Creates and starts the client (closes a previous one). snd_result.
  int Open(const ConnectionDesc& desc);
  void Close();
  // 1.1 (group P, D-182): the Hello hash of the next handshakes. Re-creates the client when it
  // changes (the client sends the hash it was created with); the session count and the open state
  // carry on (a reconnect, not a new Open).
  void SetProjectBuildHash(std::uint64_t hash);
  // Drops the current connection; the client reconnects (autoReconnect).
  void Drop();
  bool IsOpen() const { return client_ != nullptr; }
  // Group P: incremented by every Open (a new client: models drop what they knew of the old one).
  std::uint32_t opens() const { return opens_; }

  ConnectionEvents Poll();
  LinkState state() const { return state_; }
  bool mismatch() const { return mismatch_; }  // last handshake: BuildMismatch
  std::uint32_t sessions() const { return sessions_; }  // handshakes completed since Open
  const livelink::HelloReply& hello() const { return hello_; }
  const ConnectionDesc& desc() const { return desc_; }
  std::string Target() const;  // "127.0.0.1:27460" or "in-process"
  std::uint32_t roundTripUs() const { return roundTripUs_; }
  // 1.1 (group P): the game's protocol minor of the last handshake (0 = a 1.0 game, or none yet).
  std::uint16_t peerMinor() const { return hello_.protocolMinor; }
  // deferSyncDecision: true while a BuildMismatch waits for DecideSync (state Handshake).
  bool awaitingSync() const;
  // Answers the waiting BuildMismatch: SendDiffs -> Online (mismatch() true), Refuse -> Rejected.
  bool DecideSync(MismatchPolicy answer);

  livelink::Client* client() { return client_; }
  const livelink::Client* client() const { return client_; }

 private:
  ConnectionDesc desc_;
  livelink::Client* client_ = nullptr;
  LinkState state_ = LinkState::Offline;
  bool mismatch_ = false;
  std::uint32_t sessions_ = 0;
  std::uint32_t opens_ = 0;
  std::uint32_t seenConnects_ = 0;
  std::uint32_t seenDisconnects_ = 0;
  std::uint32_t roundTripUs_ = 0;
  livelink::HelloReply hello_{};
};

}  // namespace snd::tool

#endif  // SND_TOOL_CONNECTION_HPP
