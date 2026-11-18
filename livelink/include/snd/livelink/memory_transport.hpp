// In-memory loopback transport (tests, and a tool running in the game's
// process). A MemoryLink owns a few pipes; each pipe is two ByteRings (one per
// direction) with closed flags. The game side uses link.acceptor(), the tool
// side link.connector(); Connect waits until the acceptor takes the pipe. The
// two ends may be used from two different threads (one thread per end).
// Behaves like TCP: Recv reports Closed once the peer closed and every byte
// it sent was read; Send to a closed peer reports Closed.
#ifndef SND_LIVELINK_MEMORY_TRANSPORT_HPP
#define SND_LIVELINK_MEMORY_TRANSPORT_HPP

#include <atomic>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/livelink/rings.hpp"
#include "snd/livelink/transport.hpp"
#include "snd/platform/semaphore.hpp"

namespace snd::livelink {

class MemoryLink;

class MemoryEnd final : public ITransport {
 public:
  IoStatus Send(const void* data, std::size_t bytes, std::size_t* sent) noexcept override;
  IoStatus Recv(void* data, std::size_t capacity, std::size_t* received) noexcept override;
  bool Wait(bool wantRead, bool wantWrite, std::uint32_t timeoutMs) noexcept override;
  void Close() noexcept override;

 private:
  friend class MemoryLink;
  struct Pipe* pipe_ = nullptr;
  std::uint32_t side_ = 0;  // 0 = game (acceptor), 1 = tool (connector)
};

struct Pipe {
  enum State : std::uint32_t { kFree = 0, kPending = 1, kActive = 2, kRejected = 3 };
  ByteRing toTool, toGame;
  std::atomic<std::uint32_t> state{kFree};
  std::atomic<std::uint32_t> closed[2] = {};    // per side: Close() called
  std::atomic<std::uint32_t> releasedCount{0};  // ends given back to their owners
  platform::Semaphore wake[2];                  // per side: something changed for it
  MemoryEnd ends[2];
};

class MemoryLink {
 public:
  static constexpr std::uint32_t kMaxPipes = 2;

  MemoryLink() noexcept;
  ~MemoryLink();
  MemoryLink(const MemoryLink&) = delete;
  MemoryLink& operator=(const MemoryLink&) = delete;

  // bytesPerDirection: ring size of each direction of each pipe.
  bool Init(IAllocator& allocator, std::uint32_t bytesPerDirection) noexcept;
  void Destroy() noexcept;

  IAcceptor& acceptor() noexcept { return acceptor_; }
  IConnector& connector() noexcept { return connector_; }

  // Tests: number of connections refused because one was active.
  std::uint32_t Rejected() const noexcept { return rejected_.load(std::memory_order_relaxed); }

 private:
  class Acceptor final : public IAcceptor {
   public:
    ITransport* Accept(std::uint32_t timeoutMs) noexcept override;
    void Release(ITransport* transport) noexcept override;
    void RejectPending() noexcept override;
    MemoryLink* link = nullptr;
    platform::Semaphore pending;
  };
  class Connector final : public IConnector {
   public:
    ITransport* Connect(std::uint32_t timeoutMs) noexcept override;
    void Release(ITransport* transport) noexcept override;
    MemoryLink* link = nullptr;
  };
  void ReleaseEnd(MemoryEnd* end) noexcept;

  Pipe pipes_[kMaxPipes];
  Acceptor acceptor_;
  Connector connector_;
  std::atomic<std::uint32_t> activeGame_{0};  // the acceptor holds a connection
  std::atomic<std::uint32_t> rejected_{0};
  bool ready_ = false;
};

}  // namespace snd::livelink

#endif  // SND_LIVELINK_MEMORY_TRANSPORT_HPP
