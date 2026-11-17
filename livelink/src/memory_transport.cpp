// In-memory loopback transport.
//
// Pipe states: Free -(Connect)-> Pending -(Accept)-> Active, or
// Pending -(RejectPending)-> Rejected -(Connect gives up)-> Free. An Active pipe
// returns to Free when both ends were released (the last one resets the rings).
#include "snd/livelink/memory_transport.hpp"

#include "snd/platform/clock.hpp"

namespace snd::livelink {

namespace {
ByteRing& Outgoing(Pipe& p, std::uint32_t side) noexcept { return side == 0u ? p.toTool : p.toGame; }
ByteRing& Incoming(Pipe& p, std::uint32_t side) noexcept { return side == 0u ? p.toGame : p.toTool; }
}  // namespace

// ---------------------------------------------------------------------------
// MemoryEnd
// ---------------------------------------------------------------------------
IoStatus MemoryEnd::Send(const void* data, std::size_t bytes, std::size_t* sent) noexcept {
  *sent = 0;
  Pipe& p = *pipe_;
  if (p.closed[side_].load(std::memory_order_acquire) != 0u || p.closed[1u - side_].load(std::memory_order_acquire) != 0u) {
    return IoStatus::Closed;
  }
  if (bytes == 0u) return IoStatus::Ok;
  const std::size_t n = Outgoing(p, side_).Write(data, bytes);
  if (n == 0u) return IoStatus::WouldBlock;
  *sent = n;
  p.wake[1u - side_].Post();
  return IoStatus::Ok;
}

IoStatus MemoryEnd::Recv(void* data, std::size_t capacity, std::size_t* received) noexcept {
  *received = 0;
  Pipe& p = *pipe_;
  if (p.closed[side_].load(std::memory_order_acquire) != 0u) return IoStatus::Closed;
  const bool peerClosed = p.closed[1u - side_].load(std::memory_order_acquire) != 0u;
  const std::size_t n = Incoming(p, side_).Read(data, capacity);
  if (n != 0u) {
    *received = n;
    p.wake[1u - side_].Post();
    return IoStatus::Ok;
  }
  return peerClosed ? IoStatus::Closed : IoStatus::WouldBlock;
}

bool MemoryEnd::Wait(bool wantRead, bool wantWrite, std::uint32_t timeoutMs) noexcept {
  Pipe& p = *pipe_;
  auto ready = [&] {
    if (p.closed[side_].load(std::memory_order_acquire) != 0u ||
        p.closed[1u - side_].load(std::memory_order_acquire) != 0u) {
      return true;
    }
    if (wantRead && Incoming(p, side_).Readable() != 0u) return true;
    if (wantWrite && Outgoing(p, side_).Writable() != 0u) return true;
    return false;
  };
  if (ready()) return true;
  const std::uint64_t deadline = platform::MonotonicNanoseconds() + std::uint64_t{timeoutMs} * 1000000u;
  for (;;) {
    const std::uint64_t now = platform::MonotonicNanoseconds();
    if (now >= deadline) return ready();
    p.wake[side_].WaitFor(deadline - now);
    if (ready()) return true;
  }
}

void MemoryEnd::Close() noexcept {
  Pipe& p = *pipe_;
  p.closed[side_].store(1u, std::memory_order_release);
  p.wake[1u - side_].Post();
  p.wake[side_].Post();
}

// ---------------------------------------------------------------------------
// MemoryLink
// ---------------------------------------------------------------------------
MemoryLink::MemoryLink() noexcept {
  acceptor_.link = this;
  connector_.link = this;
  for (Pipe& p : pipes_) {
    for (std::uint32_t s = 0; s < 2; ++s) {
      p.ends[s].pipe_ = &p;
      p.ends[s].side_ = s;
    }
  }
}

MemoryLink::~MemoryLink() { Destroy(); }

bool MemoryLink::Init(IAllocator& allocator, std::uint32_t bytesPerDirection) noexcept {
  Destroy();
  for (Pipe& p : pipes_) {
    if (!p.toTool.Init(allocator, bytesPerDirection) || !p.toGame.Init(allocator, bytesPerDirection)) {
      Destroy();
      return false;
    }
    p.state.store(Pipe::kFree, std::memory_order_relaxed);
  }
  ready_ = true;
  return true;
}

void MemoryLink::Destroy() noexcept {
  for (Pipe& p : pipes_) {
    p.toTool.Destroy();
    p.toGame.Destroy();
  }
  ready_ = false;
}

void MemoryLink::ReleaseEnd(MemoryEnd* end) noexcept {
  Pipe& p = *end->pipe_;
  end->Close();
  if (end->side_ == 0u) activeGame_.store(0u, std::memory_order_release);
  if (p.releasedCount.fetch_add(1u, std::memory_order_acq_rel) == 1u) {  // last end: recycle
    p.toTool.Reset();
    p.toGame.Reset();
    p.closed[0].store(0u, std::memory_order_relaxed);
    p.closed[1].store(0u, std::memory_order_relaxed);
    p.releasedCount.store(0u, std::memory_order_relaxed);
    p.state.store(Pipe::kFree, std::memory_order_release);
  }
}

ITransport* MemoryLink::Acceptor::Accept(std::uint32_t timeoutMs) noexcept {
  MemoryLink& l = *link;
  if (!l.ready_ || l.activeGame_.load(std::memory_order_acquire) != 0u) return nullptr;
  for (int attempt = 0; attempt < 2; ++attempt) {
    for (Pipe& p : l.pipes_) {
      std::uint32_t expected = Pipe::kPending;
      if (p.state.compare_exchange_strong(expected, Pipe::kActive, std::memory_order_acq_rel)) {
        l.activeGame_.store(1u, std::memory_order_release);
        p.wake[1].Post();
        return &p.ends[0];
      }
    }
    if (attempt == 0) pending.WaitFor(std::uint64_t{timeoutMs} * 1000000u);
  }
  return nullptr;
}

void MemoryLink::Acceptor::Release(ITransport* transport) noexcept {
  if (transport != nullptr) link->ReleaseEnd(static_cast<MemoryEnd*>(transport));
}

void MemoryLink::Acceptor::RejectPending() noexcept {
  MemoryLink& l = *link;
  if (l.activeGame_.load(std::memory_order_acquire) == 0u) return;
  for (Pipe& p : l.pipes_) {
    std::uint32_t expected = Pipe::kPending;
    if (p.state.compare_exchange_strong(expected, Pipe::kRejected, std::memory_order_acq_rel)) {
      l.rejected_.fetch_add(1u, std::memory_order_relaxed);
      p.wake[1].Post();
    }
  }
}

ITransport* MemoryLink::Connector::Connect(std::uint32_t timeoutMs) noexcept {
  MemoryLink& l = *link;
  if (!l.ready_) return nullptr;
  for (Pipe& p : l.pipes_) {
    std::uint32_t expected = Pipe::kFree;
    if (!p.state.compare_exchange_strong(expected, Pipe::kPending, std::memory_order_acq_rel)) continue;
    while (p.wake[1].TryWait()) {
    }
    l.acceptor_.pending.Post();
    const std::uint64_t deadline = platform::MonotonicNanoseconds() + std::uint64_t{timeoutMs} * 1000000u;
    for (;;) {
      const std::uint32_t st = p.state.load(std::memory_order_acquire);
      if (st == Pipe::kActive) return &p.ends[1];
      if (st == Pipe::kRejected) {
        p.state.store(Pipe::kFree, std::memory_order_release);
        return nullptr;
      }
      const std::uint64_t now = platform::MonotonicNanoseconds();
      if (now >= deadline) {
        std::uint32_t pend = Pipe::kPending;
        if (p.state.compare_exchange_strong(pend, Pipe::kFree, std::memory_order_acq_rel)) return nullptr;
        continue;  // accepted or rejected meanwhile
      }
      p.wake[1].WaitFor(deadline - now);
    }
  }
  return nullptr;
}

void MemoryLink::Connector::Release(ITransport* transport) noexcept {
  if (transport != nullptr) link->ReleaseEnd(static_cast<MemoryEnd*>(transport));
}

}  // namespace snd::livelink
