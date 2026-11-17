// TCP transport: acceptor (game), connector (tool), stream.
#include "snd/livelink/tcp.hpp"

#include "snd/types.h"
#include "socket.hpp"

namespace snd::livelink {

// ---------------------------------------------------------------------------
// TcpTransport
// ---------------------------------------------------------------------------
TcpTransport::~TcpTransport() { Close(); }

void TcpTransport::Adopt(std::intptr_t socket) noexcept {
  Close();
  socket_ = socket;
}

IoStatus TcpTransport::Send(const void* data, std::size_t bytes, std::size_t* sent) noexcept {
  *sent = 0;
  if (socket_ == net::kInvalid) return IoStatus::Closed;
  return net::Send(socket_, data, bytes, sent);
}

IoStatus TcpTransport::Recv(void* data, std::size_t capacity, std::size_t* received) noexcept {
  *received = 0;
  if (socket_ == net::kInvalid) return IoStatus::Closed;
  return net::Recv(socket_, data, capacity, received);
}

bool TcpTransport::Wait(bool wantRead, bool wantWrite, std::uint32_t timeoutMs) noexcept {
  if (socket_ == net::kInvalid) return true;
  return net::Wait(socket_, wantRead, wantWrite, timeoutMs);
}

void TcpTransport::Close() noexcept {
  if (socket_ != net::kInvalid) net::Close(socket_);
  socket_ = net::kInvalid;
}

// ---------------------------------------------------------------------------
// TcpAcceptor
// ---------------------------------------------------------------------------
TcpAcceptor::~TcpAcceptor() { Close(); }

int TcpAcceptor::Listen(const char* bindAddress, std::uint16_t port) noexcept {
  Close();
  std::uint32_t addr = 0;
  if (!net::ParseIpv4(bindAddress != nullptr ? bindAddress : "127.0.0.1", &addr)) return SND_ERR_INVALID_ARGUMENT;
  if (!net::Startup()) return SND_ERR_IO;
  started_ = true;
  listener_ = net::Listen(addr, port, 4, &port_);
  if (listener_ == net::kInvalid) {
    Close();
    return SND_ERR_IO;
  }
  return SND_OK;
}

void TcpAcceptor::Close() noexcept {
  connection_.Close();
  inUse_ = false;
  if (listener_ != net::kInvalid) net::Close(listener_);
  listener_ = net::kInvalid;
  port_ = 0;
  if (started_) net::Cleanup();
  started_ = false;
}

ITransport* TcpAcceptor::Accept(std::uint32_t timeoutMs) noexcept {
  if (listener_ == net::kInvalid || inUse_) return nullptr;
  if (!net::Wait(listener_, true, false, timeoutMs)) return nullptr;
  const net::Handle c = net::AcceptOne(listener_);
  if (c == net::kInvalid) return nullptr;
  connection_.Adopt(c);
  inUse_ = true;
  return &connection_;
}

void TcpAcceptor::Release(ITransport* transport) noexcept {
  if (transport == &connection_) {
    connection_.Close();
    inUse_ = false;
  }
}

void TcpAcceptor::RejectPending() noexcept {
  if (listener_ == net::kInvalid) return;
  for (int i = 0; i < 8; ++i) {
    const net::Handle c = net::AcceptOne(listener_);
    if (c == net::kInvalid) return;
    net::Close(c);  // protocol §1: one connection at a time
  }
}

// ---------------------------------------------------------------------------
// TcpConnector
// ---------------------------------------------------------------------------
TcpConnector::TcpConnector() noexcept { started_ = net::Startup(); }

TcpConnector::~TcpConnector() {
  connection_.Close();
  if (started_) net::Cleanup();
}

bool TcpConnector::SetTarget(const char* address, std::uint16_t port) noexcept {
  std::uint32_t a = 0;
  if (!net::ParseIpv4(address, &a)) return false;
  address_ = a;
  port_ = port;
  return true;
}

ITransport* TcpConnector::Connect(std::uint32_t timeoutMs) noexcept {
  if (!started_ || inUse_) return nullptr;
  const net::Handle s = net::Connect(address_, port_, timeoutMs);
  if (s == net::kInvalid) return nullptr;
  connection_.Adopt(s);
  inUse_ = true;
  return &connection_;
}

void TcpConnector::Release(ITransport* transport) noexcept {
  if (transport == &connection_) {
    connection_.Close();
    inUse_ = false;
  }
}

}  // namespace snd::livelink
