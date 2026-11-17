// TCP transport of the live link (design.md 11.2, protocol §1).
//
// TcpAcceptor: the game side. Listens on bindAddress:port (default loopback
// only, 127.0.0.1:27460) and serves one connection at a time; connections that
// arrive while one is active are accepted and closed at once (RejectPending).
// TcpConnector: the tool side.
// The OS specifics (BSD sockets / Winsock) are in src/socket.cpp. Non-RT.
#ifndef SND_LIVELINK_TCP_HPP
#define SND_LIVELINK_TCP_HPP

#include <cstdint>

#include "snd/livelink/transport.hpp"

namespace snd::livelink {

class TcpTransport final : public ITransport {
 public:
  TcpTransport() noexcept = default;
  ~TcpTransport() override;
  TcpTransport(const TcpTransport&) = delete;
  TcpTransport& operator=(const TcpTransport&) = delete;

  IoStatus Send(const void* data, std::size_t bytes, std::size_t* sent) noexcept override;
  IoStatus Recv(void* data, std::size_t capacity, std::size_t* received) noexcept override;
  bool Wait(bool wantRead, bool wantWrite, std::uint32_t timeoutMs) noexcept override;
  void Close() noexcept override;

  bool Open() const noexcept { return socket_ != -1; }
  void Adopt(std::intptr_t socket) noexcept;  // takes ownership of an OS socket

 private:
  std::intptr_t socket_ = -1;
};

class TcpAcceptor final : public IAcceptor {
 public:
  TcpAcceptor() noexcept = default;
  ~TcpAcceptor() override;
  TcpAcceptor(const TcpAcceptor&) = delete;
  TcpAcceptor& operator=(const TcpAcceptor&) = delete;

  // Starts listening. bindAddress: dotted IPv4 ("127.0.0.1" default, "0.0.0.0" all
  // interfaces). port 0 picks a free port (Port() tells which).
  //   SND_ERR_INVALID_ARGUMENT  bad address
  //   SND_ERR_IO                the OS refused (port in use, no permission)
  int Listen(const char* bindAddress, std::uint16_t port) noexcept;
  void Close() noexcept;
  std::uint16_t Port() const noexcept { return port_; }
  bool Listening() const noexcept { return listener_ != -1; }

  ITransport* Accept(std::uint32_t timeoutMs) noexcept override;
  void Release(ITransport* transport) noexcept override;
  void RejectPending() noexcept override;

 private:
  std::intptr_t listener_ = -1;
  std::uint16_t port_ = 0;
  bool started_ = false;
  bool inUse_ = false;
  TcpTransport connection_;
};

class TcpConnector final : public IConnector {
 public:
  TcpConnector() noexcept;
  ~TcpConnector() override;
  TcpConnector(const TcpConnector&) = delete;
  TcpConnector& operator=(const TcpConnector&) = delete;

  // Target address (dotted IPv4 or "localhost") and port. false on a bad address.
  bool SetTarget(const char* address, std::uint16_t port) noexcept;
  ITransport* Connect(std::uint32_t timeoutMs) noexcept override;
  void Release(ITransport* transport) noexcept override;

 private:
  std::uint32_t address_ = 0x7F000001u;
  std::uint16_t port_ = 27460;
  bool started_ = false;
  bool inUse_ = false;
  TcpTransport connection_;
};

}  // namespace snd::livelink

#endif  // SND_LIVELINK_TCP_HPP
