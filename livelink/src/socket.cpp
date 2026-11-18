// OS socket layer. POSIX (Linux, macOS, iOS) and Winsock 2 (Windows).
#include "socket.hpp"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "snd/platform/clock.hpp"

namespace snd::livelink::net {

namespace {

#if defined(_WIN32)
using Native = SOCKET;
inline Native ToNative(Handle h) noexcept { return static_cast<SOCKET>(h); }
inline Handle FromNative(Native s) noexcept { return s == INVALID_SOCKET ? kInvalid : static_cast<Handle>(s); }
inline bool WouldBlockError() noexcept {
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
}
inline void CloseNative(Native s) noexcept { closesocket(s); }
bool SetNonBlocking(Native s) noexcept {
  u_long on = 1;
  return ioctlsocket(s, FIONBIO, &on) == 0;
}
int PollOne(Native s, short events, std::uint32_t timeoutMs, short* revents) noexcept {
  WSAPOLLFD p{};
  p.fd = s;
  p.events = events;
  const int r = WSAPoll(&p, 1, static_cast<INT>(timeoutMs));
  *revents = p.revents;
  return r;
}
std::atomic<int> gStartups{0};
#else
using Native = int;
inline Native ToNative(Handle h) noexcept { return static_cast<int>(h); }
inline Handle FromNative(Native s) noexcept { return s < 0 ? kInvalid : static_cast<Handle>(s); }
inline bool WouldBlockError() noexcept { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR; }
inline void CloseNative(Native s) noexcept { ::close(s); }
bool SetNonBlocking(Native s) noexcept {
  const int fl = ::fcntl(s, F_GETFL, 0);
  if (fl < 0 || ::fcntl(s, F_SETFL, fl | O_NONBLOCK) != 0) return false;
  const int fd = ::fcntl(s, F_GETFD, 0);
  if (fd >= 0) ::fcntl(s, F_SETFD, fd | FD_CLOEXEC);
  return true;
}
int PollOne(Native s, short events, std::uint32_t timeoutMs, short* revents) noexcept {
  pollfd p{};
  p.fd = s;
  p.events = events;
  int r = 0;
  do {
    r = ::poll(&p, 1, static_cast<int>(timeoutMs));
  } while (r < 0 && errno == EINTR);
  *revents = p.revents;
  return r;
}
#endif

#if defined(_WIN32)
constexpr short kPollIn = POLLRDNORM;
constexpr short kPollOut = POLLWRNORM;
#else
constexpr short kPollIn = POLLIN;
constexpr short kPollOut = POLLOUT;
#endif

void Tune(Native s) noexcept {
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#if defined(SO_NOSIGPIPE)  // macOS / iOS: no SIGPIPE on a closed peer
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
}

sockaddr_in Address(std::uint32_t addressHostOrder, std::uint16_t port) noexcept {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(addressHostOrder);
  return a;
}

}  // namespace

bool Startup() noexcept {
#if defined(_WIN32)
  if (gStartups.fetch_add(1) == 0) {
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
      gStartups.fetch_sub(1);
      return false;
    }
  }
#endif
  return true;
}

void Cleanup() noexcept {
#if defined(_WIN32)
  if (gStartups.fetch_sub(1) == 1) WSACleanup();
#endif
}

bool ParseIpv4(const char* text, std::uint32_t* addressHostOrder) noexcept {
  if (text == nullptr) return false;
  if (std::strcmp(text, "localhost") == 0) {
    *addressHostOrder = 0x7F000001u;
    return true;
  }
  std::uint32_t parts[4] = {};
  int n = 0;
  const char* p = text;
  while (n < 4) {
    if (*p < '0' || *p > '9') return false;
    std::uint32_t v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10u + static_cast<std::uint32_t>(*p - '0');
      if (++digits > 3 || v > 255u) return false;
      ++p;
    }
    parts[n++] = v;
    if (n < 4) {
      if (*p != '.') return false;
      ++p;
    }
  }
  if (*p != '\0') return false;
  *addressHostOrder = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
  return true;
}

Handle Listen(std::uint32_t addressHostOrder, std::uint16_t port, int backlog, std::uint16_t* boundPort) noexcept {
  const Native s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (FromNative(s) == kInvalid) return kInvalid;
  int one = 1;
#if defined(_WIN32)
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
  sockaddr_in a = Address(addressHostOrder, port);
  if (bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0 || listen(s, backlog) != 0 || !SetNonBlocking(s)) {
    CloseNative(s);
    return kInvalid;
  }
  sockaddr_in got{};
#if defined(_WIN32)
  int len = sizeof(got);
#else
  socklen_t len = sizeof(got);
#endif
  if (getsockname(s, reinterpret_cast<sockaddr*>(&got), &len) != 0) {
    CloseNative(s);
    return kInvalid;
  }
  if (boundPort != nullptr) *boundPort = ntohs(got.sin_port);
  return FromNative(s);
}

Handle AcceptOne(Handle listener) noexcept {
  const Native c = accept(ToNative(listener), nullptr, nullptr);
  if (FromNative(c) == kInvalid) return kInvalid;
  if (!SetNonBlocking(c)) {
    CloseNative(c);
    return kInvalid;
  }
  Tune(c);
  return FromNative(c);
}

Handle Connect(std::uint32_t addressHostOrder, std::uint16_t port, std::uint32_t timeoutMs) noexcept {
  const Native s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (FromNative(s) == kInvalid) return kInvalid;
  if (!SetNonBlocking(s)) {
    CloseNative(s);
    return kInvalid;
  }
  Tune(s);
  sockaddr_in a = Address(addressHostOrder, port);
  if (connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
    if (!WouldBlockError()) {
      CloseNative(s);
      return kInvalid;
    }
    short re = 0;
    if (PollOne(s, kPollOut, timeoutMs, &re) <= 0) {
      CloseNative(s);
      return kInvalid;
    }
    int err = 0;
#if defined(_WIN32)
    int len = sizeof(err);
#else
    socklen_t len = sizeof(err);
#endif
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0 || err != 0) {
      CloseNative(s);
      return kInvalid;
    }
  }
  return FromNative(s);
}

IoStatus Send(Handle s, const void* data, std::size_t bytes, std::size_t* sent) noexcept {
  *sent = 0;
  if (bytes == 0u) return IoStatus::Ok;
  if (bytes > (1u << 30)) bytes = 1u << 30;
#if defined(_WIN32)
  const int r = ::send(ToNative(s), static_cast<const char*>(data), static_cast<int>(bytes), 0);
#elif defined(MSG_NOSIGNAL)
  const ssize_t r = ::send(ToNative(s), data, bytes, MSG_NOSIGNAL);
#else
  const ssize_t r = ::send(ToNative(s), data, bytes, 0);
#endif
  if (r > 0) {
    *sent = static_cast<std::size_t>(r);
    return IoStatus::Ok;
  }
  if (r < 0 && WouldBlockError()) return IoStatus::WouldBlock;
  return IoStatus::Error;
}

IoStatus Recv(Handle s, void* data, std::size_t capacity, std::size_t* received) noexcept {
  *received = 0;
  if (capacity == 0u) return IoStatus::WouldBlock;
  if (capacity > (1u << 30)) capacity = 1u << 30;
#if defined(_WIN32)
  const int r = ::recv(ToNative(s), static_cast<char*>(data), static_cast<int>(capacity), 0);
#else
  const ssize_t r = ::recv(ToNative(s), data, capacity, 0);
#endif
  if (r > 0) {
    *received = static_cast<std::size_t>(r);
    return IoStatus::Ok;
  }
  if (r == 0) return IoStatus::Closed;
  if (WouldBlockError()) return IoStatus::WouldBlock;
  return IoStatus::Error;
}

bool Wait(Handle s, bool wantRead, bool wantWrite, std::uint32_t timeoutMs) noexcept {
  short ev = 0;
  if (wantRead) ev = static_cast<short>(ev | kPollIn);
  if (wantWrite) ev = static_cast<short>(ev | kPollOut);
  if (ev == 0) {
    platform::SleepFor(std::uint64_t{timeoutMs} * 1000000u);
    return false;
  }
  short re = 0;
  const int r = PollOne(ToNative(s), ev, timeoutMs, &re);
  return r != 0;
}

void Close(Handle s) noexcept {
  if (s != kInvalid) CloseNative(ToNative(s));
}

}  // namespace snd::livelink::net
