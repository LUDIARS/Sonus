// Byte-stream transports of the live link (design.md 11.2).
//
// The game listens (IAcceptor) and the tool connects (IConnector); both get an
// ITransport: a non-blocking, ordered, reliable byte stream (TCP, or the
// in-memory loopback pair of memory_transport.hpp for tests and in-process
// tools). All functions are non-RT: the live link thread and the tool's
// network thread call them, never the audio thread.
//
// Ownership: a transport returned by Accept / Connect belongs to its acceptor /
// connector and is handed back with Release (no allocation per connection).
#ifndef SND_LIVELINK_TRANSPORT_HPP
#define SND_LIVELINK_TRANSPORT_HPP

#include <cstddef>
#include <cstdint>

namespace snd::livelink {

enum class IoStatus : std::uint8_t {
  Ok = 0,          // some bytes were transferred (*n > 0)
  WouldBlock = 1,  // nothing transferred now; wait and retry
  Closed = 2,      // the peer closed the stream (Recv: orderly end)
  Error = 3,       // the stream is broken
};

class ITransport {
 public:
  virtual ~ITransport() = default;
  // Sends up to `bytes`; *sent = bytes taken.
  virtual IoStatus Send(const void* data, std::size_t bytes, std::size_t* sent) noexcept = 0;
  // Receives up to `capacity`; *received = bytes stored.
  virtual IoStatus Recv(void* data, std::size_t capacity, std::size_t* received) noexcept = 0;
  // Waits until readable (and, with wantWrite, writable) or the timeout. true if
  // something is ready (or the stream closed / failed: the next call says).
  virtual bool Wait(bool wantRead, bool wantWrite, std::uint32_t timeoutMs) noexcept = 0;
  // Closes the stream (both directions). Idempotent.
  virtual void Close() noexcept = 0;
};

class IAcceptor {
 public:
  virtual ~IAcceptor() = default;
  // Waits up to timeoutMs for a peer. Returns the connection (owned by the acceptor) or null.
  virtual ITransport* Accept(std::uint32_t timeoutMs) noexcept = 0;
  // Gives a connection back (closes it).
  virtual void Release(ITransport* transport) noexcept = 0;
  // Rejects connections that arrive while one is active (protocol §1: one connection).
  // Called periodically by the server; default: nothing.
  virtual void RejectPending() noexcept {}
};

class IConnector {
 public:
  virtual ~IConnector() = default;
  // Connects (up to timeoutMs). Returns the connection (owned by the connector) or null.
  virtual ITransport* Connect(std::uint32_t timeoutMs) noexcept = 0;
  virtual void Release(ITransport* transport) noexcept = 0;
};

}  // namespace snd::livelink

#endif  // SND_LIVELINK_TRANSPORT_HPP
