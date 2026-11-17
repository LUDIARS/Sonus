// Live link, tool side: network thread, handshake, reconnect, history buffers.
#include "snd/livelink/client.hpp"

#include <cstring>
#include <new>

#include "snd/base/allocator.hpp"
#include "snd/base/mutex.hpp"
#include "snd/livelink/frame_stream.hpp"
#include "snd/livelink/tcp.hpp"
#include "snd/livelink/wire.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/thread.hpp"
#include "snd/version.h"

namespace snd::livelink {

namespace {

constexpr std::uint64_t kMs = 1000000ull;
constexpr std::uint32_t kRecvChunk = 64u << 10;

// Fixed-capacity ring that overwrites its oldest entry.
template <class T>
struct History {
  T* items = nullptr;
  std::uint32_t capacity = 0;
  std::uint64_t written = 0;
  bool Init(IAllocator& a, std::uint32_t cap) noexcept {
    capacity = cap < 1u ? 1u : cap;
    items = static_cast<T*>(a.Allocate(sizeof(T) * capacity, 64, MemTag::LiveLink));
    return items != nullptr;
  }
  void Free(IAllocator& a) noexcept {
    a.Free(items, MemTag::LiveLink);
    items = nullptr;
  }
  void Push(const T& v) noexcept { items[written++ % capacity] = v; }
  std::uint32_t Size() const noexcept {
    return written < capacity ? static_cast<std::uint32_t>(written) : capacity;
  }
  // i-th oldest kept entry
  const T& At(std::uint32_t i) const noexcept { return items[(written - Size() + i) % capacity]; }
};

struct AckRec {
  std::uint32_t seq;
  std::int32_t result;
  std::uint32_t used;
};

}  // namespace

struct Client::Impl final : FrameReader::Handler {
  ClientDesc d{};
  IAllocator* alloc = nullptr;
  TcpConnector tcp;
  IConnector* connector = nullptr;
  ITransport* conn = nullptr;
  FrameReader reader;
  FrameSender sender;
  std::uint8_t* recvBuf = nullptr;
  platform::Thread thread;
  std::atomic<std::uint32_t> stop{0};
  std::atomic<std::uint32_t> state{static_cast<std::uint32_t>(ClientState::Stopped)};
  std::atomic<std::uint32_t> dropRequest{0};
  bool started = false;
  mutable base::Mutex mutex;  // everything below

  std::uint32_t seqOut = 1;
  std::uint64_t lastRecvNs = 0, lastPingNs = 0, nextAttemptNs = 0, handshakeNs = 0;
  HelloReply reply{};
  std::uint32_t profilerMask = 0, profilerHz = 0;
  bool haveMask = false;

  History<AckRec> acks;
  History<Error> errors;
  History<TimingSample> timings;
  History<MeterSample> meters;
  History<LogSample> log;
  History<StallSample> stalls;
  History<LodSample> lods;
  VoiceInfo* voices = nullptr;
  std::uint32_t voiceCount = 0;
  std::uint64_t voicesNs = 0;
  MeterEntry* lastMeters = nullptr;
  std::uint32_t meterCount = 0;
  std::uint64_t metersNs = 0;
  MemoryUsage memory{};
  bool haveMemory = false;
  std::uint64_t received[8] = {};
  std::uint8_t* record = nullptr;
  std::uint64_t recordBytes = 0;
  ClientStats stats{};

  ~Impl() override { Free(); }

  bool Init(const ClientDesc& desc) noexcept {
    d = desc;
    alloc = desc.allocator != nullptr ? desc.allocator : &base::DefaultAllocator();
    if (desc.connector != nullptr) {
      connector = desc.connector;
    } else {
      if (!tcp.SetTarget(desc.address != nullptr ? desc.address : "127.0.0.1", desc.port)) return false;
      connector = &tcp;
    }
    const std::uint32_t secs = desc.historySeconds < 1u ? 1u : desc.historySeconds;
    recvBuf = static_cast<std::uint8_t*>(alloc->Allocate(kRecvChunk, 64, MemTag::LiveLink));
    voices = static_cast<VoiceInfo*>(alloc->Allocate(sizeof(VoiceInfo) * (desc.maxVoices + 1u), 64, MemTag::LiveLink));
    lastMeters =
        static_cast<MeterEntry*>(alloc->Allocate(sizeof(MeterEntry) * (desc.maxMeterBuses + 1u), 64, MemTag::LiveLink));
    record = static_cast<std::uint8_t*>(alloc->Allocate(desc.recordBufferBytes != 0u ? desc.recordBufferBytes : 64u, 64,
                                                        MemTag::LiveLink));
    return recvBuf != nullptr && voices != nullptr && lastMeters != nullptr && record != nullptr &&
           acks.Init(*alloc, 1024) && errors.Init(*alloc, 256) && timings.Init(*alloc, secs * 120u) &&
           meters.Init(*alloc, secs * 60u * (desc.maxMeterBuses < 1u ? 1u : desc.maxMeterBuses)) &&
           log.Init(*alloc, secs * 2000u) && stalls.Init(*alloc, 1024) && lods.Init(*alloc, 1024) &&
           reader.Init(*alloc, desc.maxReceivePayloadBytes) && sender.Init(*alloc, 64u << 10, desc.sendBufferBytes);
  }

  void Free() noexcept {
    if (alloc == nullptr) return;
    reader.Destroy();
    sender.Destroy();
    acks.Free(*alloc);
    errors.Free(*alloc);
    timings.Free(*alloc);
    meters.Free(*alloc);
    log.Free(*alloc);
    stalls.Free(*alloc);
    lods.Free(*alloc);
    alloc->Free(recvBuf, MemTag::LiveLink);
    alloc->Free(voices, MemTag::LiveLink);
    alloc->Free(lastMeters, MemTag::LiveLink);
    alloc->Free(record, MemTag::LiveLink);
    recvBuf = nullptr;
    voices = nullptr;
    lastMeters = nullptr;
    record = nullptr;
    alloc = nullptr;
  }

  ClientState St() const noexcept { return static_cast<ClientState>(state.load(std::memory_order_acquire)); }
  void SetSt(ClientState s) noexcept { state.store(static_cast<std::uint32_t>(s), std::memory_order_release); }

  // --- sending (mutex held) -----------------------------------------------------------
  std::uint32_t NextSeq() noexcept {
    if (seqOut == 0u) seqOut = 1u;  // 0 means "no seq" in the API
    return seqOut++;
  }
  std::uint32_t QueueLocked(bool control, MessageType type, bool wantAck, const void* a, std::uint32_t na,
                            const void* b = nullptr, std::uint32_t nb = 0) noexcept {
    if (conn == nullptr) return 0;
    const std::uint32_t seq = NextSeq();
    FrameQueue& q = control ? sender.control() : sender.bulk();
    if (!q.Push(static_cast<std::uint16_t>(type), wantAck ? kFrameWantsAck : 0u, seq, a, na, b, nb)) return 0;
    ++stats.framesOut;
    return seq;
  }
  std::uint32_t Command(MessageType type, bool wantAck, const void* a, std::uint32_t na, const void* b = nullptr,
                        std::uint32_t nb = 0) noexcept {
    base::LockGuard g(mutex);
    if (St() != ClientState::Ready) return 0;
    return QueueLocked(false, type, wantAck, a, na, b, nb);
  }

  // --- receiving (mutex held) ---------------------------------------------------------
  void OnOversized(const FrameHeader&) noexcept override { ++stats.protocolErrors; }

  template <class T, class Fn>
  void List(const std::uint8_t* p, std::uint32_t n, std::uint32_t maxCount, Fn&& fn) noexcept {
    ListView lv;
    if (!ParseList(p, n, maxCount, &lv)) {
      ++stats.protocolErrors;
      return;
    }
    for (std::uint32_t i = 0; i < lv.count; ++i) fn(lv.At<T>(i), i, lv.count);
  }

  void Count(std::uint16_t type) noexcept {
    const std::uint32_t i = type - static_cast<std::uint16_t>(MessageType::VoiceList);
    if (type >= static_cast<std::uint16_t>(MessageType::VoiceList) && i < 8u) ++received[i];
  }

  void OnFrame(const FrameHeader& h, const std::uint8_t* p) noexcept override {
    ++stats.framesIn;
    const std::uint32_t n = h.payloadBytes;
    const std::uint64_t now = platform::MonotonicNanoseconds();
    Count(h.type);
    switch (static_cast<MessageType>(h.type)) {
      case MessageType::HelloReply: {
        if (St() != ClientState::Handshake) {
          ++stats.protocolErrors;
          return;
        }
        reply = ReadFixed<HelloReply>(p, n);
        if (reply.status == static_cast<std::uint32_t>(HelloStatus::Accepted)) {
          OnReady();
        } else if (reply.status == static_cast<std::uint32_t>(HelloStatus::BuildMismatch)) {
          const SyncDecision s{static_cast<std::uint32_t>(d.onMismatch), 0u};
          QueueLocked(true, MessageType::SyncDecision, false, &s, sizeof(s));
          if (d.onMismatch == SyncMode::SendDiffs) {
            OnReady();
          } else {
            SetSt(ClientState::Rejected);
          }
        } else {
          SetSt(ClientState::Rejected);
        }
        return;
      }
      case MessageType::Ping: {
        const Ping ping = ReadFixed<Ping>(p, n);
        const Pong pong{ping.senderTimeNs, 0u};
        QueueLocked(true, MessageType::Pong, false, &pong, sizeof(pong));
        return;
      }
      case MessageType::Pong: {
        const Pong pong = ReadFixed<Pong>(p, n);
        if (pong.senderTimeNs <= now) stats.lastRoundTripUs = static_cast<std::uint32_t>((now - pong.senderTimeNs) / 1000u);
        stats.lastGameClock = pong.gameClockSample;
        return;
      }
      case MessageType::Ack: {
        const Ack a = ReadFixed<Ack>(p, n);
        acks.Push(AckRec{a.seq, a.result, 1u});
        return;
      }
      case MessageType::Error: {
        Error e = ReadFixed<Error>(p, n);
        e.message[sizeof(e.message) - 1] = '\0';
        errors.Push(e);
        ++stats.errorsReceived;
        return;
      }
      case MessageType::VoiceList:
        voiceCount = 0;
        voicesNs = now;
        List<VoiceInfo>(p, n, 1u << 20, [&](const VoiceInfo& v, std::uint32_t, std::uint32_t) {
          if (voiceCount < d.maxVoices) voices[voiceCount++] = v;
        });
        return;
      case MessageType::BlockTiming:
        timings.Push(TimingSample{now, ReadFixed<BlockTiming>(p, n)});
        return;
      case MessageType::MemoryUsage:
        memory = ReadFixed<MemoryUsage>(p, n);
        haveMemory = true;
        return;
      case MessageType::StreamStall:
        stalls.Push(StallSample{now, ReadFixed<StreamStall>(p, n)});
        return;
      case MessageType::LodChange:
        lods.Push(LodSample{now, ReadFixed<LodChange>(p, n)});
        return;
      case MessageType::EventLog:
        List<LogEntry>(p, n, 1u << 20, [&](const LogEntry& e, std::uint32_t, std::uint32_t) { log.Push(LogSample{now, e}); });
        return;
      case MessageType::BusMeters:
        meterCount = 0;
        metersNs = now;
        List<MeterEntry>(p, n, 1u << 16, [&](const MeterEntry& m, std::uint32_t, std::uint32_t) {
          if (meterCount < d.maxMeterBuses) {
            lastMeters[meterCount++] = m;
            meters.Push(MeterSample{now, m});
          }
        });
        return;
      case MessageType::RecordData: {
        const RecordData rd = ReadFixed<RecordData>(p, n);
        const std::uint8_t* data = nullptr;
        if (!TrailingData(p, n, sizeof(RecordData), rd.bytes, &data)) {
          ++stats.protocolErrors;
          return;
        }
        if (rd.offset != recordBytes) {  // a gap: keep what is contiguous
          ++stats.protocolErrors;
          return;
        }
        const std::uint64_t room = d.recordBufferBytes - recordBytes;
        const std::uint64_t take = rd.bytes < room ? rd.bytes : room;
        std::memcpy(record + recordBytes, data, static_cast<std::size_t>(take));
        recordBytes += take;
        stats.recordOverflow += static_cast<std::uint32_t>(rd.bytes - take);
        return;
      }
      default:
        if (IsKnown(h.type)) ++stats.protocolErrors;  // tool -> game type sent by the game
        return;                                       // unknown: skipped (protocol §2)
    }
  }

  void OnReady() noexcept {
    SetSt(ClientState::Ready);
    ++stats.connects;
    if (haveMask) {
      const SetProfilerMask m{profilerMask, profilerHz};
      QueueLocked(false, MessageType::SetProfilerMask, false, &m, sizeof(m));
    }
  }

  // --- the thread ------------------------------------------------------------------------
  static void Main(void* u) noexcept { static_cast<Impl*>(u)->Run(); }

  void DropConnection() noexcept {
    if (conn == nullptr) return;
    connector->Release(conn);
    conn = nullptr;
    reader.Reset();
    sender.Clear();
    ++stats.disconnects;
    if (St() != ClientState::Rejected) SetSt(ClientState::Connecting);
    nextAttemptNs = platform::MonotonicNanoseconds() + std::uint64_t{d.reconnectDelayMs} * kMs;
  }

  void Run() noexcept {
    platform::SetCurrentThreadName("snd-livelink-tool");
    bool first = true;
    while (stop.load(std::memory_order_acquire) == 0u) {
      if (conn == nullptr) {
        const std::uint64_t now = platform::MonotonicNanoseconds();
        const bool retry = (first || d.autoReconnect) && St() != ClientState::Rejected && now >= nextAttemptNs;
        if (!retry) {
          platform::SleepFor(std::uint64_t{d.pollMs} * kMs);
          continue;
        }
        first = false;
        ITransport* t = connector->Connect(d.connectTimeoutMs);
        if (t == nullptr) {
          nextAttemptNs = platform::MonotonicNanoseconds() + std::uint64_t{d.reconnectDelayMs} * kMs;
          continue;
        }
        base::LockGuard g(mutex);
        conn = t;
        reader.Reset();
        sender.Clear();
        lastRecvNs = lastPingNs = handshakeNs = platform::MonotonicNanoseconds();
        Hello hello{};
        hello.protocolMajor = kProtocolMajor;
        hello.protocolMinor = kProtocolMinor;
        hello.role = static_cast<std::uint32_t>(Role::Tool);
        hello.projectBuildHash = d.projectBuildHash;
        hello.contractVersion = SND_CONTRACT_VERSION;
        CopyName(hello.name, sizeof(hello.name), d.name, d.name != nullptr ? std::strlen(d.name) : 0u);
        SetSt(ClientState::Handshake);
        QueueLocked(true, MessageType::Hello, false, &hello, sizeof(hello));
        continue;
      }
      bool wantWrite = false;
      {
        base::LockGuard g(mutex);
        wantWrite = !sender.Idle();
      }
      conn->Wait(true, wantWrite, d.pollMs);
      base::LockGuard g(mutex);
      if (dropRequest.exchange(0u) != 0u) {
        DropConnection();
        continue;
      }
      for (int round = 0; round < 64 && conn != nullptr; ++round) {
        std::size_t got = 0;
        const IoStatus st = conn->Recv(recvBuf, kRecvChunk, &got);
        if (st == IoStatus::Ok) {
          stats.bytesIn += got;
          lastRecvNs = platform::MonotonicNanoseconds();
          if (reader.Feed(recvBuf, got, *this) != HeaderStatus::Ok) {
            ++stats.protocolErrors;
            DropConnection();
          }
          continue;
        }
        if (st == IoStatus::Closed || st == IoStatus::Error) DropConnection();
        break;
      }
      if (conn == nullptr) continue;
      const std::uint64_t now = platform::MonotonicNanoseconds();
      if (St() == ClientState::Rejected) {
        std::uint64_t sent = 0;
        sender.Pump(*conn, &sent);
        DropConnection();
        continue;
      }
      if (now - lastRecvNs > std::uint64_t{d.timeoutMs} * kMs) {
        ++stats.timeouts;
        DropConnection();
        continue;
      }
      if (St() == ClientState::Ready && now - lastPingNs >= std::uint64_t{d.heartbeatMs} * kMs) {
        lastPingNs = now;
        const Ping ping{now};
        QueueLocked(true, MessageType::Ping, false, &ping, sizeof(ping));
      }
      std::uint64_t sent = 0;
      const IoStatus st = sender.Pump(*conn, &sent);
      stats.bytesOut += sent;
      if (st == IoStatus::Closed || st == IoStatus::Error) DropConnection();
    }
    base::LockGuard g(mutex);
    DropConnection();
    SetSt(ClientState::Stopped);
  }
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
int Client::Create(const ClientDesc& desc, Client** out) noexcept {
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  *out = nullptr;
  IAllocator& a = desc.allocator != nullptr ? *desc.allocator : base::DefaultAllocator();
  if (desc.sendBufferBytes < 4096u || desc.maxReceivePayloadBytes < 4096u) return SND_ERR_INVALID_ARGUMENT;
  void* cm = a.Allocate(sizeof(Client), 16, MemTag::LiveLink);
  void* im = a.Allocate(sizeof(Impl), alignof(Impl) < 16 ? 16 : alignof(Impl), MemTag::LiveLink);
  if (cm == nullptr || im == nullptr) {
    a.Free(cm, MemTag::LiveLink);
    a.Free(im, MemTag::LiveLink);
    return SND_ERR_OUT_OF_BUDGET;
  }
  Client* c = new (cm) Client();
  c->impl_ = new (im) Impl();
  if (!c->impl_->Init(desc)) {
    c->impl_->alloc = &a;
    Destroy(c);
    return SND_ERR_INVALID_ARGUMENT;
  }
  *out = c;
  return SND_OK;
}

void Client::Destroy(Client* c) noexcept {
  if (c == nullptr) return;
  IAllocator* a = c->impl_->alloc != nullptr ? c->impl_->alloc : &base::DefaultAllocator();
  c->Stop();
  c->impl_->~Impl();
  a->Free(c->impl_, MemTag::LiveLink);
  c->~Client();
  a->Free(c, MemTag::LiveLink);
}

int Client::Start() noexcept {
  Impl& im = *impl_;
  if (im.started) return SND_OK;
  im.stop.store(0u);
  im.SetSt(ClientState::Connecting);
  platform::ThreadDesc td;
  td.name = "snd-livelink-tool";
  if (!im.thread.Start(td, &Impl::Main, &im)) {
    im.SetSt(ClientState::Stopped);
    return SND_ERR_LIMIT_REACHED;
  }
  im.started = true;
  return SND_OK;
}

void Client::Stop() noexcept {
  Impl& im = *impl_;
  if (!im.started) return;
  im.stop.store(1u, std::memory_order_release);
  im.thread.Join();
  im.started = false;
}

ClientState Client::State() const noexcept { return impl_->St(); }

bool Client::WaitReady(std::uint32_t timeoutMs) const noexcept {
  const std::uint64_t end = platform::MonotonicNanoseconds() + std::uint64_t{timeoutMs} * kMs;
  while (impl_->St() != ClientState::Ready) {
    if (impl_->St() == ClientState::Rejected || platform::MonotonicNanoseconds() >= end) return false;
    platform::SleepFor(kMs);
  }
  return true;
}

HelloReply Client::LastHelloReply() const noexcept {
  base::LockGuard g(impl_->mutex);
  return impl_->reply;
}

ClientStats Client::Stats() const noexcept {
  base::LockGuard g(impl_->mutex);
  return impl_->stats;
}

void Client::Disconnect() noexcept { impl_->dropRequest.store(1u, std::memory_order_release); }

std::uint32_t Client::SetValue(const livelink::SetValue& v, bool wantAck) noexcept {
  return impl_->Command(MessageType::SetValue, wantAck, &v, sizeof(v));
}

std::uint32_t Client::ReplaceChunk(std::uint32_t bankId, std::uint32_t fourcc, const void* data, std::uint64_t bytes,
                                   bool wantAck) noexcept {
  if (bytes > kMaxPayloadBytes - sizeof(livelink::ReplaceChunk) || (bytes != 0u && data == nullptr)) return 0;
  const livelink::ReplaceChunk h{bankId, fourcc, bytes};
  return impl_->Command(MessageType::ReplaceChunk, wantAck, &h, sizeof(h), data, static_cast<std::uint32_t>(bytes));
}

std::uint32_t Client::Audition(std::uint32_t eventId, const float* position, bool stopAll, bool wantAck) noexcept {
  livelink::Audition a{};
  a.eventId = eventId;
  if (stopAll) a.flags |= kAuditionStopAll;
  if (position != nullptr) {
    a.flags |= kAuditionAtPosition;
    a.position[0] = position[0];
    a.position[1] = position[1];
    a.position[2] = position[2];
  }
  return impl_->Command(MessageType::Audition, wantAck, &a, sizeof(a));
}

std::uint32_t Client::OverrideParam(std::uint32_t paramId, float value, bool enable, bool wantAck) noexcept {
  const livelink::OverrideParam m{paramId, enable ? 1u : 0u, value, 0u};
  return impl_->Command(MessageType::OverrideParam, wantAck, &m, sizeof(m));
}

std::uint32_t Client::OverrideState(std::uint32_t musicEventId, std::uint32_t stateId, bool enable, bool wantAck) noexcept {
  const livelink::OverrideState m{musicEventId, stateId, enable ? 1u : 0u, 0u};
  return impl_->Command(MessageType::OverrideState, wantAck, &m, sizeof(m));
}

std::uint32_t Client::ClearOverrides(bool wantAck) noexcept {
  return impl_->Command(MessageType::ClearOverrides, wantAck, nullptr, 0);
}

std::uint32_t Client::SetProfilerMask(std::uint32_t mask, std::uint32_t meterHz, bool wantAck) noexcept {
  base::LockGuard g(impl_->mutex);
  impl_->profilerMask = mask;
  impl_->profilerHz = meterHz;
  impl_->haveMask = true;
  if (impl_->St() != ClientState::Ready) return 0;
  const livelink::SetProfilerMask m{mask, meterHz};
  return impl_->QueueLocked(false, MessageType::SetProfilerMask, wantAck, &m, sizeof(m));
}

std::uint32_t Client::StartRecording(bool wantAck) noexcept {
  {
    base::LockGuard g(impl_->mutex);
    impl_->recordBytes = 0;
  }
  const RecordControl m{1u, 0u};
  return impl_->Command(MessageType::RecordControl, wantAck, &m, sizeof(m));
}

std::uint32_t Client::StopRecording(bool wantAck) noexcept {
  const RecordControl m{0u, 0u};
  return impl_->Command(MessageType::RecordControl, wantAck, &m, sizeof(m));
}

std::uint32_t Client::StructEdit(StructOp op, std::uint32_t targetId, const std::uint64_t* args, std::uint32_t argCount,
                                 bool wantAck) noexcept {
  if (argCount > 64u || (argCount != 0u && args == nullptr)) return 0;
  const livelink::StructEdit m{static_cast<std::uint32_t>(op), targetId, argCount, 0u};
  return impl_->Command(MessageType::StructEdit, wantAck, &m, sizeof(m), args, argCount * 8u);
}

std::uint32_t Client::SendRaw(std::uint16_t type, std::uint16_t flags, const void* payload, std::uint32_t bytes) noexcept {
  base::LockGuard g(impl_->mutex);
  Impl& im = *impl_;
  if (im.conn == nullptr) return 0;
  const std::uint32_t seq = im.NextSeq();
  if (!im.sender.bulk().Push(type, flags, seq, payload, bytes)) return 0;
  ++im.stats.framesOut;
  return seq;
}

bool Client::TakeAck(std::uint32_t seq, std::int32_t* result) noexcept {
  base::LockGuard g(impl_->mutex);
  History<AckRec>& h = impl_->acks;
  for (std::uint32_t i = 0; i < h.Size(); ++i) {
    AckRec& a = h.items[(h.written - h.Size() + i) % h.capacity];
    if (a.used != 0u && a.seq == seq) {
      if (result != nullptr) *result = a.result;
      a.used = 0u;
      return true;
    }
  }
  return false;
}

bool Client::WaitAck(std::uint32_t seq, std::uint32_t timeoutMs, std::int32_t* result) noexcept {
  const std::uint64_t end = platform::MonotonicNanoseconds() + std::uint64_t{timeoutMs} * kMs;
  while (!TakeAck(seq, result)) {
    if (platform::MonotonicNanoseconds() >= end) return false;
    platform::SleepFor(kMs / 2u);
  }
  return true;
}

std::uint32_t Client::CopyErrors(Error* out, std::uint32_t capacity) const noexcept {
  base::LockGuard g(impl_->mutex);
  const History<Error>& h = impl_->errors;
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < h.Size() && n < capacity; ++i) out[n++] = h.At(i);
  return n;
}

void Client::ClearErrors() noexcept {
  base::LockGuard g(impl_->mutex);
  impl_->errors.written = 0;
}

namespace {
template <class T>
std::uint32_t CopyRecent(const History<T>& h, std::uint64_t cutoffNs, T* out, std::uint32_t capacity) noexcept {
  std::uint32_t first = 0;
  while (first < h.Size() && h.At(first).receivedNs < cutoffNs) ++first;
  std::uint32_t n = h.Size() - first;
  if (n > capacity) {
    first += n - capacity;  // keep the newest
    n = capacity;
  }
  for (std::uint32_t i = 0; i < n; ++i) out[i] = h.At(first + i);
  return n;
}
}  // namespace

#define SND_CLIENT_CUTOFF                                                                                          \
  const std::uint64_t nowNs = platform::MonotonicNanoseconds();                                                    \
  const std::uint64_t span = std::uint64_t{impl_->d.historySeconds} * 1000000000ull;                               \
  const std::uint64_t cutoff = nowNs > span ? nowNs - span : 0u

std::uint32_t Client::CopyTimings(TimingSample* out, std::uint32_t capacity) const noexcept {
  SND_CLIENT_CUTOFF;
  base::LockGuard g(impl_->mutex);
  return CopyRecent(impl_->timings, cutoff, out, capacity);
}

std::uint32_t Client::CopyMeterHistory(std::uint32_t busId, MeterSample* out, std::uint32_t capacity) const noexcept {
  SND_CLIENT_CUTOFF;
  base::LockGuard g(impl_->mutex);
  const History<MeterSample>& h = impl_->meters;
  std::uint32_t total = 0;
  for (std::uint32_t i = 0; i < h.Size(); ++i) {
    const MeterSample& s = h.At(i);
    total += s.receivedNs >= cutoff && s.meter.busId == busId ? 1u : 0u;
  }
  std::uint32_t skip = total > capacity ? total - capacity : 0u, n = 0;
  for (std::uint32_t i = 0; i < h.Size() && n < capacity; ++i) {
    const MeterSample& s = h.At(i);
    if (s.receivedNs < cutoff || s.meter.busId != busId) continue;
    if (skip != 0u) {
      --skip;
      continue;
    }
    out[n++] = s;
  }
  return n;
}

std::uint32_t Client::CopyEventLog(LogSample* out, std::uint32_t capacity) const noexcept {
  SND_CLIENT_CUTOFF;
  base::LockGuard g(impl_->mutex);
  return CopyRecent(impl_->log, cutoff, out, capacity);
}

std::uint32_t Client::CopyStalls(StallSample* out, std::uint32_t capacity) const noexcept {
  SND_CLIENT_CUTOFF;
  base::LockGuard g(impl_->mutex);
  return CopyRecent(impl_->stalls, cutoff, out, capacity);
}

std::uint32_t Client::CopyLods(LodSample* out, std::uint32_t capacity) const noexcept {
  SND_CLIENT_CUTOFF;
  base::LockGuard g(impl_->mutex);
  return CopyRecent(impl_->lods, cutoff, out, capacity);
}

#undef SND_CLIENT_CUTOFF

std::uint32_t Client::CopyVoices(VoiceInfo* out, std::uint32_t capacity, std::uint64_t* receivedNs) const noexcept {
  base::LockGuard g(impl_->mutex);
  const std::uint32_t n = impl_->voiceCount < capacity ? impl_->voiceCount : capacity;
  for (std::uint32_t i = 0; i < n; ++i) out[i] = impl_->voices[i];
  if (receivedNs != nullptr) *receivedNs = impl_->voicesNs;
  return n;
}

std::uint32_t Client::CopyMeters(MeterEntry* out, std::uint32_t capacity, std::uint64_t* receivedNs) const noexcept {
  base::LockGuard g(impl_->mutex);
  const std::uint32_t n = impl_->meterCount < capacity ? impl_->meterCount : capacity;
  for (std::uint32_t i = 0; i < n; ++i) out[i] = impl_->lastMeters[i];
  if (receivedNs != nullptr) *receivedNs = impl_->metersNs;
  return n;
}

bool Client::LatestMemory(MemoryUsage* out) const noexcept {
  base::LockGuard g(impl_->mutex);
  if (!impl_->haveMemory || out == nullptr) return false;
  *out = impl_->memory;
  return true;
}

std::uint64_t Client::Received(MessageType type) const noexcept {
  const std::uint32_t i = static_cast<std::uint32_t>(type) - static_cast<std::uint32_t>(MessageType::VoiceList);
  if (static_cast<std::uint32_t>(type) < static_cast<std::uint32_t>(MessageType::VoiceList) || i >= 8u) return 0;
  base::LockGuard g(impl_->mutex);
  return impl_->received[i];
}

std::uint64_t Client::RecordingBytes() const noexcept {
  base::LockGuard g(impl_->mutex);
  return impl_->recordBytes;
}

std::uint64_t Client::CopyRecording(std::uint8_t* out, std::uint64_t capacity) const noexcept {
  base::LockGuard g(impl_->mutex);
  const std::uint64_t n = impl_->recordBytes < capacity ? impl_->recordBytes : capacity;
  if (n != 0u) std::memcpy(out, impl_->record, static_cast<std::size_t>(n));
  return n;
}

}  // namespace snd::livelink
