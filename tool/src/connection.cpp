// Live connection of the tool (design.md 11.2).
#include "snd/tool/connection.hpp"

#include "snd/livelink/client.hpp"
#include "snd/types.h"

namespace snd::tool {

const char* LinkStateName(LinkState s) {
  switch (s) {
    case LinkState::Offline: return "offline";
    case LinkState::Connecting: return "connecting";
    case LinkState::Handshake: return "handshake";
    case LinkState::Online: return "online";
    case LinkState::Rejected: return "rejected";
  }
  return "?";
}

Connection::~Connection() { Close(); }

int Connection::Open(const ConnectionDesc& desc) {
  Close();
  desc_ = desc;
  livelink::ClientDesc d;
  d.connector = desc.connector;
  d.address = desc_.address.c_str();
  d.port = desc.port;
  d.name = desc_.name.c_str();
  d.projectBuildHash = desc.projectBuildHash;
  d.onMismatch = desc.policy == MismatchPolicy::SendDiffs ? livelink::SyncMode::SendDiffs : livelink::SyncMode::Disconnect;
  d.historySeconds = desc.historySeconds;
  d.autoReconnect = desc.autoReconnect;
  d.reconnectDelayMs = desc.reconnectDelayMs;
  d.timeoutMs = desc.timeoutMs;
  d.deferSyncDecision = desc.deferSyncDecision;
  livelink::Client* c = nullptr;
  int r = livelink::Client::Create(d, &c);
  if (r != SND_OK) return r;
  r = c->Start();
  if (r != SND_OK) {
    livelink::Client::Destroy(c);
    return r;
  }
  if (desc.profilerMask != 0u) c->SetProfilerMask(desc.profilerMask, desc.meterHz, false);
  client_ = c;
  ++opens_;
  state_ = LinkState::Connecting;
  mismatch_ = false;
  sessions_ = 0;
  seenConnects_ = 0;
  seenDisconnects_ = 0;
  hello_ = livelink::HelloReply{};
  return SND_OK;
}

void Connection::Close() {
  if (client_ == nullptr) return;
  client_->Stop();
  livelink::Client::Destroy(client_);
  client_ = nullptr;
  state_ = LinkState::Offline;
  mismatch_ = false;
}

void Connection::Drop() {
  if (client_ != nullptr) client_->Disconnect();
}

ConnectionEvents Connection::Poll() {
  ConnectionEvents ev;
  if (client_ == nullptr) {
    state_ = LinkState::Offline;
    return ev;
  }
  const livelink::ClientStats st = client_->Stats();
  const livelink::ClientState cs = client_->State();
  switch (cs) {
    case livelink::ClientState::Stopped: state_ = LinkState::Offline; break;
    case livelink::ClientState::Connecting: state_ = LinkState::Connecting; break;
    case livelink::ClientState::Handshake: state_ = LinkState::Handshake; break;
    case livelink::ClientState::Ready: state_ = LinkState::Online; break;
    case livelink::ClientState::Rejected: state_ = LinkState::Rejected; break;
  }
  if (st.disconnects != seenDisconnects_) {
    seenDisconnects_ = st.disconnects;
    ev.sessionLost = true;
  }
  // A session starts when the client reports Ready with a handshake we have not seen.
  if (state_ == LinkState::Online && st.connects != seenConnects_) {
    seenConnects_ = st.connects;
    ++sessions_;
    ev.sessionStarted = true;
    hello_ = client_->LastHelloReply();
    mismatch_ = hello_.status == static_cast<std::uint32_t>(livelink::HelloStatus::BuildMismatch);
  }
  if (state_ == LinkState::Handshake && client_->AwaitingSync()) {
    hello_ = client_->LastHelloReply();  // the mismatch the tool is asked about
    mismatch_ = true;
  }
  if (state_ == LinkState::Rejected) {
    hello_ = client_->LastHelloReply();
    mismatch_ = hello_.status == static_cast<std::uint32_t>(livelink::HelloStatus::BuildMismatch);
  }
  roundTripUs_ = st.lastRoundTripUs;
  return ev;
}

void Connection::SetProjectBuildHash(std::uint64_t hash) {
  if (desc_.projectBuildHash == hash) return;
  ConnectionDesc d = desc_;
  d.projectBuildHash = hash;
  if (client_ == nullptr) {
    desc_ = d;
    return;
  }
  const std::uint32_t sessions = sessions_;
  if (Open(d) == SND_OK) sessions_ = sessions;
}

bool Connection::awaitingSync() const { return client_ != nullptr && client_->AwaitingSync(); }

bool Connection::DecideSync(MismatchPolicy answer) {
  if (client_ == nullptr) return false;
  return client_->DecideSync(answer == MismatchPolicy::Refuse ? livelink::SyncMode::Disconnect
                                                             : livelink::SyncMode::SendDiffs);
}

std::string Connection::Target() const {
  if (desc_.connector != nullptr) return "in-process";
  return desc_.address + ":" + std::to_string(desc_.port);
}

}  // namespace snd::tool
