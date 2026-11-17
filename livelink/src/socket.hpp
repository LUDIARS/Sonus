// Thin OS socket layer (Linux / macOS / iOS: BSD sockets; Windows: Winsock 2).
// Everything OS specific of the live link is in socket.cpp. Non-RT only.
// Sockets are IPv4 TCP, non-blocking after creation.
#ifndef SND_LIVELINK_SRC_SOCKET_HPP
#define SND_LIVELINK_SRC_SOCKET_HPP

#include <cstddef>
#include <cstdint>

#include "snd/livelink/transport.hpp"

namespace snd::livelink::net {

using Handle = std::intptr_t;
inline constexpr Handle kInvalid = -1;

// Process-wide start (Winsock: WSAStartup, reference counted). false on failure.
bool Startup() noexcept;
void Cleanup() noexcept;

// Parses a dotted IPv4 address ("127.0.0.1", "0.0.0.0"); "localhost" is 127.0.0.1.
bool ParseIpv4(const char* text, std::uint32_t* addressHostOrder) noexcept;

// Listening socket bound to address:port (port 0 = any free port). *boundPort gets the port.
Handle Listen(std::uint32_t addressHostOrder, std::uint16_t port, int backlog, std::uint16_t* boundPort) noexcept;
// Accepts one pending connection (kInvalid if none). The new socket is non-blocking, TCP_NODELAY.
Handle AcceptOne(Handle listener) noexcept;
// Connects with a timeout. kInvalid on failure / timeout.
Handle Connect(std::uint32_t addressHostOrder, std::uint16_t port, std::uint32_t timeoutMs) noexcept;

IoStatus Send(Handle s, const void* data, std::size_t bytes, std::size_t* sent) noexcept;
IoStatus Recv(Handle s, void* data, std::size_t capacity, std::size_t* received) noexcept;
// Waits for readability / writability. Returns true if ready (or error), false on timeout.
bool Wait(Handle s, bool wantRead, bool wantWrite, std::uint32_t timeoutMs) noexcept;
void Close(Handle s) noexcept;

}  // namespace snd::livelink::net

#endif  // SND_LIVELINK_SRC_SOCKET_HPP
