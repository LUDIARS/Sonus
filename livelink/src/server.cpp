// Live link, game side: the live link thread (accept, handshake, tool
// messages -> core commands, profiler / recording streams, heartbeat).
#include "snd/livelink/server.hpp"

#include <atomic>
#include <cstring>
#include <new>

#include "snd/core/core.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/livelink/frame_stream.hpp"
#include "snd/livelink/runtime.hpp"
#include "snd/livelink/struct_edit.hpp"
#include "snd/livelink/tcp.hpp"
#include "snd/livelink/wire.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/thread.hpp"
#include "snd/version.h"

namespace snd::livelink {

namespace {

constexpr std::uint32_t kRecvChunk = 64u << 10;
constexpr std::uint32_t kRecordFrameBytes = 64u << 10;
constexpr std::uint32_t kLogBatch = 512;
constexpr std::uint64_t kMs = 1000000ull;

std::uint32_t ThisPlatform() noexcept {
#if defined(_WIN32)
  return bank_format::kPlatformWindows;
#elif defined(__APPLE__)
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
  return bank_format::kPlatformIos;
#else
  return bank_format::kPlatformMac;
#endif
#elif defined(__ANDROID__)
  return bank_format::kPlatformAndroid;
#else
  return bank_format::kPlatformLinux;
#endif
}

struct Counter {
  std::atomic<std::uint64_t> v{0};
  void Add(std::uint64_t n = 1) noexcept { v.fetch_add(n, std::memory_order_relaxed); }
  std::uint64_t Get() const noexcept { return v.load(std::memory_order_relaxed); }
};

}  // namespace

struct Server::Impl final : FrameReader::Handler {
  enum class Phase : std::uint8_t { AwaitHello, AwaitSync, Ready, Closing };
  enum class RecMode : std::uint8_t { None, Tool, Sink };
  // What a tracked command is, for its answer (1.1).
  enum Op : std::uint16_t { kOpEdit = 0, kOpRead = 1, kOpStopAuditions = 2, kOpStruct = 3, kOpRevert = 4 };
  struct Pending {
    std::uint32_t seq;
    std::uint32_t conn;    // connection generation (replies only to the same one)
    std::uint16_t wantAck;
    std::uint16_t kind;    // cmd::CommandType
    std::uint16_t op;      // Op
    std::uint16_t reserved;
    PendingChunk* chunk;   // ReplaceChunk slot (LiveRuntime pool) or null
  };
  // 1.1: one hot reload at a time (ReloadBank).
  struct Reload {
    bool active = false;
    std::uint32_t seq = 0, conn = 0, bankId = 0, languageId = 0;
    bool wantAck = false;
    snd_bank_handle bank{0};
    bool preexisting = false;  // LoadBankAsync returned a registration the game already had
  };

  ServerDesc d{};
  core::System* sys = nullptr;
  LiveRuntime* rt = nullptr;
  IAllocator* alloc = nullptr;
  TcpAcceptor tcp;
  IAcceptor* acceptor = nullptr;
  ITransport* conn = nullptr;
  FrameReader reader;
  FrameSender sender;
  std::uint8_t* recvBuf = nullptr;
  std::uint8_t* scratch = nullptr;
  std::uint8_t* logBuf = nullptr;  // ListHeader + kLogBatch LogEntry
  std::uint32_t scratchBytes = 0;
  platform::Thread thread;
  std::atomic<std::uint32_t> stop{0};
  std::atomic<std::uint32_t> connected{0};
  std::atomic<std::uint16_t> port{0};
  bool started = false;
  bool registered = false;
  // 0 while the thread has not tried to register, 1 registered, 2 refused (api D-93: Start
  // returns only after the thread holds its registered place, so no game thread takes it).
  std::atomic<std::uint32_t> registration{0};

  // connection
  Phase phase = Phase::AwaitHello;
  std::uint32_t connGen = 0;
  std::uint64_t lastRecvNs = 0, lastPingNs = 0, phaseNs = 0, closingNs = 0;
  std::uint32_t seqOut = 1;
  bool recordedCommands = false;

  // applied-result bookkeeping
  Pending* pending = nullptr;
  std::uint32_t pendingCap = 0, pendingHead = 0, pendingCount = 0;
  std::uint32_t session = 0;       // LiveRuntime::NewSession, announced with kFieldSession
  bool sessionAnnounced = false;
  std::uint32_t revertPending = 0;  // revert marker refused (arena full): retried every loop
  std::uint32_t revertTries = 0;
  std::uint64_t lastSendProgressNs = 0;

  // profiler
  std::uint32_t meterHz = 30;
  std::uint64_t nextTimingNs = 0, nextMetersNs = 0, nextMemoryNs = 0, nextLogNs = 0;
  BlockTiming worst{};
  bool haveWorst = false;
  std::uint32_t lateSum = 0;
  double tokens = 0;
  std::uint64_t tokenNs = 0;

  // recording
  RecMode recMode = RecMode::None;
  bool recStopping = false;
  std::uint64_t recOffset = 0;
  std::uint32_t recStage = 0;  // bytes staged in scratch for the next RecordData

  // registered banks (for HelloReply, BankList)
  core::LiveBankInfo* banks = nullptr;
  std::uint32_t bankCount = 0;

  // 1.1
  std::uint16_t peerMinor = 0;  // Hello.protocolMinor of the tool
  Reload reload;
  char* reloadPath = nullptr;   // kMaxReloadPathBytes + 1
  std::uint8_t* listBuf = nullptr;  // BankList / StreamList payloads
  std::uint32_t listBufBytes = 0;
  BlockDetail worstDetail{};
  bool haveDetail = false;
  std::uint32_t detailLate = 0;
  Counter auditionsStopped, reloadsDone, reloadsFailed, structRefused, readsAnswered;

  // stats
  Counter controlDropped, revertsLost, staleResults;
  Counter connections, handshakesRejected, timeouts, protocolErrors, framesIn, framesOut, bytesIn, bytesOut,
      commandsRecorded, commandsRefused, profilerSent, profilerDropped, recordBytes, pingsAnswered;
  std::atomic<std::uint32_t> lastRttUs{0};

  ~Impl() override { Free(); }

  // --- setup ---------------------------------------------------------------------------
  bool Init(const ServerDesc& desc) noexcept {
    d = desc;
    sys = desc.system;
    rt = desc.runtime;
    alloc = desc.allocator;
    const snd_config& cfg = rt->config();
    const std::uint32_t bulk = desc.sendBufferBytes != 0u ? desc.sendBufferBytes
                                                         : (cfg.livelink.send_buffer_bytes != 0u ? cfg.livelink.send_buffer_bytes
                                                                                                 : (256u << 10));
    meterHz = desc.meterHz != 0u ? desc.meterHz : (cfg.livelink.meter_hz != 0u ? cfg.livelink.meter_hz : 30u);
    const std::uint32_t voiceList = static_cast<std::uint32_t>(sizeof(ListHeader) + sizeof(VoiceInfo) * rt->MaxVoices());
    const std::uint32_t meterList = static_cast<std::uint32_t>(sizeof(ListHeader) + sizeof(MeterEntry) * rt->MaxMeters());
    scratchBytes = kRecordFrameBytes;
    if (voiceList > scratchBytes) scratchBytes = voiceList;
    if (meterList > scratchBytes) scratchBytes = meterList;
    pendingCap = rt->results().Capacity();
    recvBuf = static_cast<std::uint8_t*>(alloc->Allocate(kRecvChunk, 64, MemTag::LiveLink));
    scratch = static_cast<std::uint8_t*>(alloc->Allocate(scratchBytes, 64, MemTag::LiveLink));
    logBuf = static_cast<std::uint8_t*>(alloc->Allocate(sizeof(ListHeader) + sizeof(LogEntry) * kLogBatch, 64, MemTag::LiveLink));
    pending = static_cast<Pending*>(alloc->Allocate(sizeof(Pending) * pendingCap, 64, MemTag::LiveLink));
    banks = static_cast<core::LiveBankInfo*>(
        alloc->Allocate(sizeof(core::LiveBankInfo) * (cfg.max_banks != 0u ? cfg.max_banks : 1u), 64, MemTag::LiveLink));
    reloadPath = static_cast<char*>(alloc->Allocate(kMaxReloadPathBytes + 1u, 64, MemTag::LiveLink));
    {
      const std::uint64_t bl = sizeof(ListHeader) + sizeof(BankEntry) * std::uint64_t{cfg.max_banks != 0u ? cfg.max_banks : 1u};
      const std::uint64_t sl = sizeof(ListHeader) + sizeof(StreamInfo) * std::uint64_t{rt->MaxStreams() != 0u ? rt->MaxStreams() : 1u};
      listBufBytes = static_cast<std::uint32_t>(bl > sl ? bl : sl);
    }
    listBuf = static_cast<std::uint8_t*>(alloc->Allocate(listBufBytes, 64, MemTag::LiveLink));
    if (recvBuf == nullptr || scratch == nullptr || logBuf == nullptr || pending == nullptr || banks == nullptr ||
        reloadPath == nullptr || listBuf == nullptr) {
      return false;
    }
    return reader.Init(*alloc, desc.maxReceivePayloadBytes) && sender.Init(*alloc, desc.controlBufferBytes, bulk);
  }

  void Free() noexcept {
    reader.Destroy();
    sender.Destroy();
    if (alloc == nullptr) return;
    alloc->Free(recvBuf, MemTag::LiveLink);
    alloc->Free(scratch, MemTag::LiveLink);
    alloc->Free(logBuf, MemTag::LiveLink);
    logBuf = nullptr;
    alloc->Free(pending, MemTag::LiveLink);
    alloc->Free(banks, MemTag::LiveLink);
    alloc->Free(reloadPath, MemTag::LiveLink);
    alloc->Free(listBuf, MemTag::LiveLink);
    reloadPath = nullptr;
    listBuf = nullptr;
    recvBuf = scratch = nullptr;
    pending = nullptr;
    banks = nullptr;
  }

  // --- sending -------------------------------------------------------------------------
  bool Control(MessageType type, std::uint32_t seq, const void* p, std::uint32_t n) noexcept {
    if (conn == nullptr) return false;
    if (!sender.control().Push(static_cast<std::uint16_t>(type), 0, seq, p, n)) {
      // Cannot happen for replies (reading and the result drain stop while the control
      // queue is short of room); a ping that does not fit is dropped. A tool that does
      // not read at all is dropped by the send-progress timeout (Service).
      controlDropped.Add();
      return false;
    }
    framesOut.Add();
    return true;
  }
  void SendError(snd_result code, std::uint32_t seq, const char* msg) noexcept {
    Error e{};
    e.code = code;
    e.seq = seq;
    CopyName(e.message, sizeof(e.message), msg, msg != nullptr ? std::strlen(msg) : 0u);
    Control(MessageType::Error, seq, &e, sizeof(e));
  }
  void Reply(std::uint32_t seq, bool wantAck, snd_result r, const char* msg) noexcept {
    if (wantAck) {
      const Ack a{seq, r};
      Control(MessageType::Ack, seq, &a, sizeof(a));
    } else if (r != SND_OK) {
      SendError(r, seq, msg);
    }
  }
  // Profiler message: bandwidth budget and room in the bulk queue, else dropped.
  bool Profiler(MessageType type, const void* a, std::uint32_t na, const void* b = nullptr, std::uint32_t nb = 0) noexcept {
    const double cost = static_cast<double>(kFrameHeaderBytes + na + nb);
    const std::uint64_t reserve = recMode == RecMode::Tool ? sender.bulk().Capacity() / 4u : 0u;
    if (conn == nullptr || phase != Phase::Ready || tokens < cost ||
        sender.bulk().Free() < static_cast<std::uint64_t>(cost) + reserve ||
        !sender.bulk().Push(static_cast<std::uint16_t>(type), 0, seqOut++, a, na, b, nb)) {
      profilerDropped.Add();
      return false;
    }
    tokens -= cost;
    profilerSent.Add();
    framesOut.Add();
    return true;
  }

  // --- connection ----------------------------------------------------------------------
  void OnConnect(ITransport* t, std::uint64_t now) noexcept {
    conn = t;
    ++connGen;
    connections.Add();
    reader.Reset();
    sender.Clear();
    phase = Phase::AwaitHello;
    phaseNs = lastRecvNs = lastPingNs = lastSendProgressNs = now;
    closingNs = 0;
    tokens = static_cast<double>(d.profilerBytesPerSecond);
    tokenNs = now;
    haveWorst = false;
    lateSum = 0;
    haveDetail = false;
    peerMinor = 0;
  }

  void BeginClose() noexcept {
    if (conn == nullptr || phase == Phase::Closing) return;
    phase = Phase::Closing;
    closingNs = platform::MonotonicNanoseconds();
    connected.store(0, std::memory_order_release);
  }

  void Disconnect() noexcept {
    if (conn == nullptr) return;
    connected.store(0, std::memory_order_release);  // before the peer can see the close
    acceptor->Release(conn);
    conn = nullptr;
    reader.Reset();
    sender.Clear();
    rt->SetProfiler(0u, 0u);
    if (recMode == RecMode::Tool && !recStopping) {
      rt->RequestRecording(false);
      recStopping = true;
    }
    if (d.revertOnDisconnect) {
      revertPending = 7u;
      revertTries = 0;
      TryRevert();
    }
  }

  // Records the disconnect revert; refused parts (arena full) are retried every loop,
  // up to kRevertTries, then counted (ServerStats::revertsLost) and logged.
  static constexpr std::uint32_t kRevertTries = 2000;
  // In order: kFieldRevertBegin (bit0), LiveClearOverrides (bit1), kFieldRevertAll (bit2).
  void TryRevert() noexcept {
    if (revertPending == 0u) return;
    if ((revertPending & 1u) != 0u) {
      cmd::LiveSetValue b{};
      b.field = kFieldRevertBegin;
      if (Record(cmd::CommandType::LiveSetValue, &b, sizeof(b), 0, false, nullptr, false) == SND_OK) revertPending &= ~1u;
    }
    if (revertPending == 6u &&
        Record(cmd::CommandType::LiveClearOverrides, nullptr, 0, 0, false, nullptr, false) == SND_OK) {
      revertPending &= ~2u;
    }
    if (revertPending == 4u) {
      cmd::LiveSetValue rv{};
      rv.field = kFieldRevertAll;
      if (Record(cmd::CommandType::LiveSetValue, &rv, sizeof(rv), 0, false, nullptr, false) == SND_OK) revertPending = 0u;
    }
    if (revertPending != 0u && ++revertTries >= kRevertTries) {
      revertsLost.Add();  // the warning: ServerStats::revertsLost (no tool is attached to tell)
      revertPending = 0u;
    }
  }

  // Records a live command from this (registered) thread. With track, the applied
  // result is matched later (SetValue / ReplaceChunk).
  snd_result Record(cmd::CommandType type, const void* payload, std::uint32_t bytes, std::uint32_t seq, bool wantAck,
                    PendingChunk* chunk, bool reply, std::uint16_t op = kOpEdit) noexcept {
    // Commands with an applied result (the markers other than the revert have none).
    bool track = type == cmd::CommandType::LiveReplaceChunk;
    if (type == cmd::CommandType::LiveSetValue) {
      cmd::LiveSetValue v;
      std::memcpy(&v, payload, sizeof(v));
      track = v.field != kFieldRevertBegin;
    }
    if (track && pendingCount >= pendingCap) {
      commandsRefused.Add();
      return SND_ERR_LIMIT_REACHED;
    }
    if (!sessionAnnounced) {  // before any command, so that every edit carries the session  // results are matched by session (a new Server on the same runtime)
      cmd::LiveSetValue m{};
      m.field = kFieldSession;
      m.value = session;
      const snd_result mr = sys->RecordLive(cmd::CommandType::LiveSetValue, &m, sizeof(m));
      if (mr != SND_OK) {
        commandsRefused.Add();
        return mr;
      }
      sessionAnnounced = true;
    }
    const snd_result r = sys->RecordLive(type, payload, bytes);
    if (r != SND_OK) {
      commandsRefused.Add();
      return r;
    }
    commandsRecorded.Add();
    recordedCommands = true;
    if (track) {
      Pending& p = pending[(pendingHead + pendingCount) % pendingCap];
      p = Pending{seq, reply ? connGen : 0u, static_cast<std::uint16_t>(wantAck ? 1u : 0u),
                  static_cast<std::uint16_t>(type), op, 0u, chunk};
      ++pendingCount;
    }
    return SND_OK;
  }

  // --- receiving -----------------------------------------------------------------------
  void OnOversized(const FrameHeader& h) noexcept override {
    framesIn.Add();
    protocolErrors.Add();
    SendError(SND_ERR_OUT_OF_BUDGET, h.seq, "message larger than the receive buffer");
  }

  void OnFrame(const FrameHeader& h, const std::uint8_t* p) noexcept override {
    framesIn.Add();
    if (phase == Phase::Closing) return;
    const bool wantAck = (h.flags & kFrameWantsAck) != 0u;
    const std::uint32_t n = h.payloadBytes;
    switch (static_cast<MessageType>(h.type)) {
      case MessageType::Hello: return OnHello(h, ReadFixed<Hello>(p, n));
      case MessageType::SyncDecision: {
        const SyncDecision s = ReadFixed<SyncDecision>(p, n);
        if (phase != Phase::AwaitSync || CheckSyncDecision(s) != SND_OK) return Fatal(h.seq, "unexpected SyncDecision");
        if (s.mode == static_cast<std::uint32_t>(SyncMode::Disconnect)) {
          handshakesRejected.Add();
          BeginClose();
          return;
        }
        return Ready();
      }
      case MessageType::Ping: {
        const Ping ping = ReadFixed<Ping>(p, n);
        snd_stats st{};
        sys->GetStats(&st);
        const Pong pong{ping.senderTimeNs, st.clock_sample};
        if (Control(MessageType::Pong, h.seq, &pong, sizeof(pong))) pingsAnswered.Add();
        return;
      }
      case MessageType::Pong: {
        const Pong pong = ReadFixed<Pong>(p, n);
        const std::uint64_t now = platform::MonotonicNanoseconds();
        if (pong.senderTimeNs <= now) lastRttUs.store(static_cast<std::uint32_t>((now - pong.senderTimeNs) / 1000u));
        return;
      }
      case MessageType::Ack:
      case MessageType::Error:
        return;  // the tool's answers to our pings / data: nothing to do
      default:
        break;
    }
    if (!IsKnown(h.type)) return;  // protocol §2: unknown types are skipped
    if (h.type >= static_cast<std::uint16_t>(MessageType::VoiceList) || h.type == static_cast<std::uint16_t>(MessageType::HelloReply)) {
      protocolErrors.Add();
      SendError(SND_ERR_INVALID_ARGUMENT, h.seq, "game -> tool message sent by the tool");
      return;
    }
    // 1.1: the bank list may be asked for before SyncDecision (the tool decides with it).
    if (h.type == static_cast<std::uint16_t>(MessageType::QueryBanks) && (phase == Phase::AwaitSync || phase == Phase::Ready)) {
      return OnQueryBanks(h, wantAck);
    }
    if (phase != Phase::Ready) return Fatal(h.seq, "handshake first");
    switch (static_cast<MessageType>(h.type)) {
      case MessageType::SetValue: return OnSetValue(h, ReadFixed<SetValue>(p, n), wantAck);
      case MessageType::RevertValue: return OnCellRequest(h, ReadFixed<SetValue>(p, n), wantAck, false);
      case MessageType::ReadCell: return OnCellRequest(h, ReadFixed<SetValue>(p, n), wantAck, true);
      case MessageType::ReloadBank: return OnReloadBank(h, p, wantAck);
      case MessageType::ReplaceChunk: return OnReplaceChunk(h, p, wantAck);
      case MessageType::Audition: return OnAudition(h, ReadFixed<Audition>(p, n), wantAck);
      case MessageType::OverrideParam: {
        const OverrideParam m = ReadFixed<OverrideParam>(p, n);
        snd_result r = CheckOverrideParam(m);
        if (r == SND_OK) {
          cmd::LiveOverride c{};
          c.kind = static_cast<std::uint32_t>(cmd::LiveOverrideKind::Param);
          c.id = m.paramId;
          std::memcpy(&c.value, &m.value, 4);
          c.enable = m.enable;
          r = Record(cmd::CommandType::LiveOverride, &c, sizeof(c), h.seq, wantAck, nullptr, true);
        }
        return Reply(h.seq, wantAck, r, "OverrideParam");
      }
      case MessageType::OverrideState: {
        const OverrideState m = ReadFixed<OverrideState>(p, n);
        snd_result r = CheckOverrideState(m);
        if (r == SND_OK) {
          cmd::LiveOverride c{};
          c.kind = static_cast<std::uint32_t>(cmd::LiveOverrideKind::MusicState);
          c.id = m.musicEventId;
          c.value = m.stateId;
          c.enable = m.enable;
          r = Record(cmd::CommandType::LiveOverride, &c, sizeof(c), h.seq, wantAck, nullptr, true);
        }
        return Reply(h.seq, wantAck, r, "OverrideState");
      }
      case MessageType::ClearOverrides:
        return Reply(h.seq, wantAck, Record(cmd::CommandType::LiveClearOverrides, nullptr, 0, h.seq, wantAck, nullptr, true),
                     "ClearOverrides");
      case MessageType::SetProfilerMask: {
        const SetProfilerMask m = ReadFixed<SetProfilerMask>(p, n);
        const snd_result r = CheckSetProfilerMask(m, kProtocolMinor);
        if (r == SND_OK) {
          if (m.meterHz != 0u) meterHz = m.meterHz;
          rt->SetProfiler(m.mask, meterHz);
        }
        return Reply(h.seq, wantAck, r, "SetProfilerMask");
      }
      case MessageType::RecordControl: return OnRecordControl(h, ReadFixed<RecordControl>(p, n), wantAck);
      case MessageType::StructEdit: return OnStructEdit(h, p, wantAck);
      default:
        return;
    }
  }

  void Fatal(std::uint32_t seq, const char* msg) noexcept {
    protocolErrors.Add();
    SendError(SND_ERR_INVALID_ARGUMENT, seq, msg);
    BeginClose();
  }

  void OnHello(const FrameHeader& h, const Hello& m) noexcept {
    if (phase != Phase::AwaitHello) return Fatal(h.seq, "Hello twice");
    HelloReply r{};
    r.protocolMajor = kProtocolMajor;
    r.protocolMinor = kProtocolMinor;
    r.contractVersion = SND_CONTRACT_VERSION;
    r.sampleRate = rt->config().sample_rate;
    r.blockSize = rt->config().block_size;
    r.platform = d.platform != 0u ? d.platform : ThisPlatform();
    if (m.protocolMajor != kProtocolMajor) {
      r.status = static_cast<std::uint32_t>(HelloStatus::RejectedProtocol);
      Control(MessageType::HelloReply, h.seq, &r, sizeof(r));
      handshakesRejected.Add();
      BeginClose();
      return;
    }
    if (CheckHello(m) != SND_OK) {
      handshakesRejected.Add();
      return Fatal(h.seq, "Hello.role must be Tool");
    }
    peerMinor = m.protocolMinor;
    // runtimeBuildHash: projectBuildHash of the loaded banks; all must agree (protocol §4).
    // Edge rules (1.1, §4): no bank -> 0 (Accepted only for a tool hash of 0); banks that
    // disagree -> BuildMismatch with the first registered bank's hash, even when the tool's
    // hash equals one of them (the tool lists them with QueryBanks).
    RefreshBanks();
    std::uint64_t hash = 0;
    bool agree = true;
    for (std::uint32_t i = 0; i < bankCount; ++i) {
      if (i == 0u) hash = banks[i].projectBuildHash;
      agree = agree && banks[i].projectBuildHash == hash;
    }
    r.runtimeBuildHash = hash;
    const bool match = agree && hash == m.projectBuildHash;
    r.status = static_cast<std::uint32_t>(match ? HelloStatus::Accepted : HelloStatus::BuildMismatch);
    Control(MessageType::HelloReply, h.seq, &r, sizeof(r));
    if (match) {
      Ready();
    } else {
      phase = Phase::AwaitSync;
      phaseNs = platform::MonotonicNanoseconds();
    }
  }

  void Ready() noexcept {
    phase = Phase::Ready;
    connected.store(1, std::memory_order_release);
    rt->SetProfiler(rt->ProfilerMask(), meterHz);
  }

  void RefreshBanks() noexcept {
    TripleSlot& s = rt->banks();
    if (!s.Take()) return;
    BankSnapshotHeader hdr;
    std::memcpy(&hdr, s.Front(), sizeof(hdr));
    const std::uint32_t cap = rt->config().max_banks;
    bankCount = hdr.count < cap ? hdr.count : cap;
    std::memcpy(static_cast<void*>(banks), s.Front() + sizeof(hdr), sizeof(core::LiveBankInfo) * bankCount);
  }

  void OnSetValue(const FrameHeader& h, const SetValue& m, bool wantAck) noexcept {
    snd_result r = CheckSetValue(m);
    if (r == SND_OK && reload.active) r = SND_ERR_NOT_READY;  // 1.1: a hot reload is running
    if (r == SND_OK) {
      cmd::LiveSetValue c{};
      c.objectId = m.runtimeId;
      c.field = EncodeField(m.chunk, m.column);
      c.valueType = m.valueType;
      c.index = m.index;
      c.value = m.value;
      r = Record(cmd::CommandType::LiveSetValue, &c, sizeof(c), h.seq, wantAck, nullptr, true);
    }
    if (r != SND_OK) Reply(h.seq, wantAck, r, "SetValue");  // success: answered when applied
  }

  void OnReplaceChunk(const FrameHeader& h, const std::uint8_t* p, bool wantAck) noexcept {
    const ReplaceChunk m = ReadFixed<ReplaceChunk>(p, h.payloadBytes);
    const std::uint8_t* data = nullptr;
    std::uint32_t rows = 0;
    snd_result r = TrailingData(p, h.payloadBytes, sizeof(ReplaceChunk), m.bytes, &data) ? SND_OK : SND_ERR_INVALID_ARGUMENT;
    if (r == SND_OK) r = CheckTableChunk(m.fourcc, data, m.bytes, &rows);
    if (r == SND_OK && reload.active) r = SND_ERR_NOT_READY;  // 1.1: a hot reload is running
    if (r == SND_ERR_UNSUPPORTED) return Reply(h.seq, wantAck, r, "blob chunk: rebuild and hot reload");
    PendingChunk* slot = nullptr;
    if (r == SND_OK && m.bytes > rt->ChunkBytes()) r = SND_ERR_OUT_OF_BUDGET;
    if (r == SND_OK) {
      slot = rt->AcquireChunk();
      if (slot == nullptr) r = SND_ERR_LIMIT_REACHED;
    }
    if (r == SND_OK) {
      PendingChunk& c = *slot;
      std::memcpy(c.data, data, static_cast<std::size_t>(m.bytes));
      c.bytes = m.bytes;
      c.bankId = m.bankId;
      c.fourcc = m.fourcc;
      c.rows = rows;
      c.seq = h.seq;
      c.result = 0;
      c.cells = 0;
      c.costBuses = c.costEffects = 0u;
      c.costBytes = 0u;
      c.structLogPlace = 0u;
      c.state.store(PendingChunk::kQueued, std::memory_order_release);
      const cmd::LiveReplaceChunk cc{m.bankId, m.fourcc, &c};
      r = Record(cmd::CommandType::LiveReplaceChunk, &cc, sizeof(cc), h.seq, wantAck, slot, true);
      if (r != SND_OK) c.state.store(PendingChunk::kFree, std::memory_order_release);
    }
    if (r != SND_OK) Reply(h.seq, wantAck, r, "ReplaceChunk");
  }

  // 1.1: stop-all stops the auditions this live link started before it (the marker is
  // answered once the audio thread handed them over and they were stopped); a play
  // takes one of RuntimeDesc::maxAuditions places (LIMIT_REACHED when none is free).
  void OnAudition(const FrameHeader& h, const Audition& m, bool wantAck) noexcept {
    snd_result r = CheckAudition(m);
    if (r == SND_OK && (m.flags & kAuditionStopAll) != 0u) {
      cmd::LiveSetValue mk{};
      mk.field = kFieldAuditionStop;
      r = Record(cmd::CommandType::LiveSetValue, &mk, sizeof(mk), h.seq, wantAck, nullptr, true, kOpStopAuditions);
      if (r != SND_OK) Reply(h.seq, wantAck, r, "Audition stop");
      return;
    }
    if (r == SND_OK && !rt->ReserveAudition()) r = SND_ERR_LIMIT_REACHED;
    if (r == SND_OK) {
      cmd::LiveAudition c{};
      c.event = m.eventId;
      if ((m.flags & kAuditionAtPosition) != 0u) {
        c.flags = cmd::kPlayHasPosition;
        c.position = Vec3{m.position[0], m.position[1], m.position[2]};
      }
      r = Record(cmd::CommandType::LiveAudition, &c, sizeof(c), h.seq, wantAck, nullptr, true);
      if (r != SND_OK) rt->ReleaseAudition();
    }
    Reply(h.seq, wantAck, r, "Audition");
  }

  // Stops the auditions the audio thread handed over (System::Stop, short fade). Only
  // handles of LiveAudition playbacks ever reach this channel.
  void DrainStops() noexcept {
    std::uint64_t v = 0;
    const float fade = rt->config().tuning.short_fade_ms / 1000.0f;
    while (rt->auditionStops().TryPop(v)) {
      if (registered && sys->Stop(snd_playback_handle{v}, fade, nullptr) == SND_OK) auditionsStopped.Add();
    }
  }

  // --- 1.1 requests --------------------------------------------------------------------------
  void OnCellRequest(const FrameHeader& h, const SetValue& m, bool wantAck, bool read) noexcept {
    snd_result r = CheckCellAddress(m);
    if (r == SND_OK && !read && reload.active) r = SND_ERR_NOT_READY;
    if (r == SND_OK) {
      cmd::LiveSetValue c{};
      c.objectId = m.runtimeId;
      c.field = EncodeField(m.chunk, m.column);
      c.valueType = read ? kValueTypeReadCell : kValueTypeRevertCell;
      c.index = m.index;
      c.value = read ? h.seq : 0u;
      r = Record(cmd::CommandType::LiveSetValue, &c, sizeof(c), h.seq, wantAck, nullptr, true, read ? kOpRead : kOpRevert);
    }
    if (r == SND_OK) return;  // answered when applied
    if (read) {
      const CellValue cv{h.seq, r, 0u, 0u, 0u, 0u};
      Control(MessageType::CellValue, h.seq, &cv, sizeof(cv));
      if (wantAck) Reply(h.seq, true, r, "ReadCell");
      return;
    }
    Reply(h.seq, wantAck, r, "RevertValue");
  }

  void OnQueryBanks(const FrameHeader& h, bool wantAck) noexcept {
    RefreshBanks();
    const ListHeader lh{bankCount, sizeof(BankEntry)};
    std::memcpy(listBuf, &lh, sizeof(lh));
    for (std::uint32_t i = 0; i < bankCount; ++i) {
      const core::LiveBankInfo& b = banks[i];
      const BankEntry e{b.bankId, b.languageId, b.projectBuildHash, b.contentHash, b.writable, b.handle.slot};
      std::memcpy(listBuf + sizeof(lh) + std::size_t{i} * sizeof(e), &e, sizeof(e));
    }
    const std::uint32_t bytes = static_cast<std::uint32_t>(sizeof(lh) + std::size_t{bankCount} * sizeof(BankEntry));
    bool sent = sender.control().Free() >= kFrameHeaderBytes + bytes + 2u * kReplyRoom &&
                Control(MessageType::BankList, h.seq, listBuf, bytes);
    if (!sent && conn != nullptr && sender.bulk().Push(static_cast<std::uint16_t>(MessageType::BankList), 0, h.seq, listBuf, bytes)) {
      framesOut.Add();
      sent = true;
    }
    Reply(h.seq, wantAck, sent ? SND_OK : SND_ERR_OUT_OF_BUDGET, "BankList did not fit the send buffer");
  }

  void SendReloadStatus(ReloadPhase ph, snd_result r) noexcept {
    if (conn == nullptr || reload.conn != connGen) return;
    const ReloadStatus st{reload.seq, static_cast<std::uint32_t>(ph), r, reload.bankId,
                          ph == ReloadPhase::Loading || ph == ReloadPhase::Done ? reload.bank.value : 0u};
    Control(MessageType::ReloadStatus, reload.seq, &st, sizeof(st));
  }

  // 1.1 hot reload (11.2 大きな差し替え): StopAndUnload the loaded copy (its sounds stop
  // with a short fade), then load the file again through the normal path; the bank
  // manager registers the new copy once the old one is released (epoch, 10.3). The
  // override layer forgets the old registration's edits. Edits wait (NOT_READY) until done.
  void OnReloadBank(const FrameHeader& h, const std::uint8_t* p, bool wantAck) noexcept {
    const ReloadBank m = ReadFixed<ReloadBank>(p, h.payloadBytes);
    const std::uint8_t* path = nullptr;
    snd_result r = TrailingData(p, h.payloadBytes, sizeof(ReloadBank), m.pathBytes, &path) ? SND_OK : SND_ERR_INVALID_ARGUMENT;
    if (r == SND_OK) r = CheckReloadBank(m, path);
    if (r == SND_OK && reload.active) r = SND_ERR_LIMIT_REACHED;  // one at a time
    if (r == SND_OK && !registered) r = SND_ERR_THREAD_NOT_REGISTERED;
    const core::LiveBankInfo* old = nullptr;
    if (r == SND_OK) {
      RefreshBanks();
      for (std::uint32_t i = 0; i < bankCount && old == nullptr; ++i) {
        if (banks[i].bankId == m.bankId && banks[i].languageId == m.languageId) old = &banks[i];
      }
      if (old == nullptr && (m.flags & kReloadLoadIfAbsent) == 0u) r = SND_ERR_NOT_FOUND;
    }
    if (r != SND_OK) return Reply(h.seq, wantAck, r, "ReloadBank");
    std::memcpy(reloadPath, path, m.pathBytes);
    reloadPath[m.pathBytes] = '\0';
    reload = Reload{};
    reload.seq = h.seq;
    reload.conn = connGen;
    reload.bankId = m.bankId;
    reload.languageId = m.languageId;
    reload.wantAck = wantAck;
    if (old != nullptr) {
      r = sys->UnloadBank(snd_bank_handle{old->handle.Value()}, SND_UNLOAD_STOP_AND_UNLOAD);
      if (r != SND_OK) return Reply(h.seq, wantAck, r, "ReloadBank: unload refused");
      reload.active = true;
      SendReloadStatus(ReloadPhase::Unloading, SND_OK);
    }
    RefreshBanks();  // D-173: the registrations before the load (the same path returns the game's own)
    reload.bank = sys->LoadBankAsync(reloadPath);
    reload.active = true;
    for (std::uint32_t i = 0; i < bankCount && !reload.preexisting; ++i) {
      reload.preexisting = banks[i].handle.Value() == reload.bank.value;
    }
    if (reload.bank.value == 0u) {
      FinishReload(ReloadPhase::Failed, SND_ERR_INVALID_ARGUMENT);
      return;
    }
    SendReloadStatus(ReloadPhase::Loading, SND_OK);
  }

  void FinishReload(ReloadPhase ph, snd_result r) noexcept {
    SendReloadStatus(ph, r);
    (ph == ReloadPhase::Done ? reloadsDone : reloadsFailed).Add();
    if (conn != nullptr && reload.conn == connGen) {
      Reply(reload.seq, reload.wantAck, r,
            r == SND_ERR_INVALID_ARGUMENT ? "ReloadBank: the file holds another bank" : "ReloadBank failed");
    }
    reload.active = false;
  }

  // Every loop: the new copy's state (GetBankState: any thread).
  void PollReload() noexcept {
    if (!reload.active) return;
    switch (sys->GetBankState(reload.bank)) {
      case SND_BANK_STATE_LOADED: {
        // Done once the audio side published the new registration too (BankList / the
        // override layer see it from then on).
        RefreshBanks();
        for (std::uint32_t i = 0; i < bankCount; ++i) {
          if (banks[i].handle.Value() != reload.bank.value) continue;
          // D-173: the file held another bank (id or language): it is not kept (unloaded again);
          // the bank asked for stays unloaded, as when the load fails.
          if (banks[i].bankId != reload.bankId || banks[i].languageId != reload.languageId) {
            // Only a registration this reload made; the game's own bank stays (D-173).
            if (registered && !reload.preexisting) sys->UnloadBank(reload.bank, SND_UNLOAD_STOP_AND_UNLOAD);
            return FinishReload(ReloadPhase::Failed, SND_ERR_INVALID_ARGUMENT);
          }
          return FinishReload(ReloadPhase::Done, SND_OK);
        }
        return;
      }
      case SND_BANK_STATE_FAILED: return FinishReload(ReloadPhase::Failed, SND_ERR_IO);
      case SND_BANK_STATE_UNLOADED:
      case SND_BANK_STATE_UNLOADING: return FinishReload(ReloadPhase::Failed, SND_ERR_INVALID_HANDLE);
      default: return;  // loading (or waiting for the old copy's release)
    }
  }

  // 1.1 structural edit (§8.4): rules, Config.dev_reserve, then the editor seam on the
  // audio thread (struct_edit.hpp). Refusals tell the tool to rebuild and hot reload.
  void OnStructEdit(const FrameHeader& h, const std::uint8_t* p, bool wantAck) noexcept {
    const StructEdit m = ReadFixed<StructEdit>(p, h.payloadBytes);
    const std::uint8_t* raw = nullptr;
    snd_result r = CheckStructEdit(m);
    if (r == SND_OK && !TrailingData(p, h.payloadBytes, sizeof(StructEdit), std::uint64_t{m.argCount} * 8u, &raw)) {
      r = SND_ERR_INVALID_ARGUMENT;
    }
    std::uint64_t args[64 + 2];
    if (r == SND_OK && m.argCount != 0u) std::memcpy(args, raw, std::size_t{m.argCount} * 8u);
    if (r == SND_OK) r = CheckStructArgs(m, args);
    if (r == SND_OK && reload.active) r = SND_ERR_NOT_READY;
    const StructCost cost = StructCostOf(static_cast<StructOp>(m.op), m.argCount);
    bool reserved = false;
    if (r == SND_OK) {
      r = rt->ReserveStruct(cost);
      reserved = r == SND_OK;
    }
    bool logPlace = false;  // D-172: an edit the revert log cannot hold is refused here
    if (r == SND_OK) {
      logPlace = rt->ReserveStructLog();
      // A remove goes on without a place: if it cancels a live add it needs none, else the
      // audio thread takes one when it applies it (or refuses it, LIMIT_REACHED).
      const bool remove = m.op == static_cast<std::uint32_t>(StructOp::RemoveBus) ||
                          m.op == static_cast<std::uint32_t>(StructOp::RemoveEffect);
      if (!logPlace && !remove) r = SND_ERR_LIMIT_REACHED;
    }
    PendingChunk* slot = nullptr;
    const std::uint64_t bytes = sizeof(StructEdit) + std::uint64_t{m.argCount} * 8u;
    if (r == SND_OK && bytes > rt->ChunkBytes()) r = SND_ERR_OUT_OF_BUDGET;
    if (r == SND_OK) {
      slot = rt->AcquireChunk();
      if (slot == nullptr) r = SND_ERR_LIMIT_REACHED;
    }
    if (r == SND_OK) {
      PendingChunk& c = *slot;
      std::memcpy(c.data, &m, sizeof(m));
      if (m.argCount != 0u) std::memcpy(c.data + sizeof(m), args, std::size_t{m.argCount} * 8u);
      c.bytes = bytes;
      c.bankId = 0;
      c.fourcc = kStructEditTag;
      c.rows = 0;
      c.seq = h.seq;
      c.result = 0;
      c.cells = 0;
      c.costBuses = cost.buses;
      c.costEffects = cost.effects;
      c.costBytes = cost.bytes;
      c.structLogPlace = logPlace ? 1u : 0u;
      c.state.store(PendingChunk::kQueued, std::memory_order_release);
      const cmd::LiveReplaceChunk cc{0u, kStructEditTag, &c};
      r = Record(cmd::CommandType::LiveReplaceChunk, &cc, sizeof(cc), h.seq, wantAck, slot, true, kOpStruct);
      if (r != SND_OK) c.state.store(PendingChunk::kFree, std::memory_order_release);
    }
    if (r == SND_OK) return;  // answered when applied (the audio thread gives the reserve back on failure)
    if (reserved) rt->ReleaseStruct(cost);
    if (logPlace) rt->ReleaseStructLog();
    structRefused.Add();
    if (!wantAck) return SendError(r, h.seq, StructWhy(r));
    Reply(h.seq, true, r, "StructEdit");
  }

  static const char* StructWhy(snd_result r) noexcept {
    switch (r) {
      case SND_ERR_OUT_OF_BUDGET: return "dev reserve exhausted: rebuild and hot reload";
      case SND_ERR_UNSUPPORTED: return "structural edit: rebuild and hot reload";
      case SND_ERR_NOT_READY: return "hot reload running";
      case SND_ERR_LIMIT_REACHED: return "structural edit log or chunk slots full";
      default: return "structural edit refused";
    }
  }

  void OnRecordControl(const FrameHeader& h, const RecordControl& m, bool wantAck) noexcept {
    snd_result r = CheckRecordControl(m);
    if (r == SND_OK && m.start != 0u) {
      if (recMode != RecMode::None) {
        r = SND_ERR_LIMIT_REACHED;  // already recording (sink or a stop still draining)
      } else {
        recMode = RecMode::Tool;
        recStopping = false;
        recOffset = 0;
        recStage = 0;
        rt->RequestRecording(true);
      }
    } else if (r == SND_OK) {
      if (recMode == RecMode::Tool && !recStopping) {
        rt->RequestRecording(false);
        recStopping = true;
      } else if (recMode != RecMode::Tool) {
        r = SND_ERR_INVALID_ARGUMENT;
      }
    }
    Reply(h.seq, wantAck, r, "RecordControl");
  }

  // --- runtime channels ----------------------------------------------------------------
  // Back-pressure (acceptance decision D2): a result is taken from the ring only when its
  // answer fits in the control queue; the rest waits in the ring for the next loop, so
  // no Ack is lost and the order is kept.
  static constexpr std::uint64_t kReplyRoom = kFrameHeaderBytes + sizeof(Error);
  void DrainResults() noexcept {
    ApplyResult ar{};
    for (;;) {
      if (conn != nullptr && sender.control().Free() < kReplyRoom) return;
      if (!rt->results().TryPop(ar)) return;
      if (ar.session != session) {  // recorded by an earlier Server on this runtime
        staleResults.Add();
        continue;
      }
      if (pendingCount == 0u) continue;  // results of commands recorded by someone else (none)
      const Pending pd = pending[pendingHead];
      pendingHead = (pendingHead + 1u) % pendingCap;
      --pendingCount;
      if (pd.chunk != nullptr) rt->FinishChunk(pd.chunk);
      CellReading rd{};
      if (pd.op == kOpRead) {  // the reading was pushed before the result (same audio thread)
        bool found = false;
        CellReading x{};
        while (!found && rt->readings().TryPop(x)) found = x.seq == pd.seq && x.session == session;
        if (found) rd = x;
        if (!found) rd.result = ar.result != SND_OK ? ar.result : SND_ERR_NOT_FOUND;
      }
      if (pd.op == kOpStopAuditions) DrainStops();  // the stops were handed over before the result
      if (pd.conn == 0u || pd.conn != connGen || conn == nullptr) continue;
      switch (pd.op) {
        case kOpRead: {
          const CellValue cv{pd.seq, rd.result, rd.cells, rd.flags, rd.current, rd.original};
          if (Control(MessageType::CellValue, pd.seq, &cv, sizeof(cv))) readsAnswered.Add();
          if (pd.wantAck != 0u) Reply(pd.seq, true, ar.result, "ReadCell");
          break;
        }
        case kOpStruct:
          if (ar.result != SND_OK) structRefused.Add();
          if (pd.wantAck == 0u && ar.result != SND_OK) {
            SendError(ar.result, pd.seq, StructWhy(ar.result));
          } else {
            Reply(pd.seq, pd.wantAck != 0u, ar.result, StructWhy(ar.result));
          }
          break;
        case kOpStopAuditions: Reply(pd.seq, pd.wantAck != 0u, ar.result, "Audition stop not applied"); break;
        case kOpRevert: Reply(pd.seq, pd.wantAck != 0u, ar.result, "RevertValue not applied"); break;
        default:
          Reply(pd.seq, pd.wantAck != 0u, ar.result,
                pd.kind == static_cast<std::uint16_t>(cmd::CommandType::LiveReplaceChunk) ? "ReplaceChunk not applied"
                                                                                           : "SetValue not applied");
          break;
      }
    }
  }

  void FlushRecordStage() noexcept {
    if (recStage == 0u) return;
    const RecordData rd{recOffset, recStage};
    if (recMode == RecMode::Sink) {
      if (d.recordSink != nullptr) d.recordSink(d.recordUser, scratch, recStage);
    } else if (conn != nullptr && phase == Phase::Ready) {
      if (!sender.bulk().Push(static_cast<std::uint16_t>(MessageType::RecordData), 0, seqOut++, &rd, sizeof(rd), scratch,
                              recStage)) {
        return;  // retried when the queue drains
      }
      framesOut.Add();
    }
    recordBytes.Add(recStage);
    recOffset += recStage;
    recStage = 0;
  }

  void DrainRecords() noexcept {
    MessageRing& ring = rt->records();
    std::uint32_t bytes = 0;
    for (int guard = 0; guard < 4096 && ring.Next(&bytes); ++guard) {
      if (bytes >= sizeof(RecordEnd)) {
        std::uint32_t kind = 0;
        ring.Copy(0, &kind, 4);
        if (kind == kRecordEndMarker) {
          RecordEnd e{};
          ring.Copy(0, &e, sizeof(e));
          ring.Pop();
          FlushRecordStage();
          if (recStage != 0u) return;  // could not send: keep the marker handled next loop
          if (e.dropped != 0u && recMode == RecMode::Tool) SendError(SND_ERR_OUT_OF_BUDGET, 0, "recording incomplete");
          recMode = RecMode::None;
          recStopping = false;
          continue;
        }
      }
      if (recMode == RecMode::None) {  // nobody wants it any more
        ring.Pop();
        continue;
      }
      // Stage the record (split across RecordData frames when larger than the stage).
      if (recMode == RecMode::Tool && (conn == nullptr || phase != Phase::Ready)) {
        ring.Pop();
        continue;
      }
      std::uint32_t done = 0;
      while (done < bytes) {
        if (recStage == scratchBytes || recStage >= kRecordFrameBytes) {
          FlushRecordStage();
          if (recStage != 0u) return;  // bulk queue full: resume later (the record stays)
        }
        const std::uint32_t room = (kRecordFrameBytes < scratchBytes ? kRecordFrameBytes : scratchBytes) - recStage;
        const std::uint32_t take = bytes - done < room ? bytes - done : room;
        ring.Copy(done, scratch + recStage, take);
        recStage += take;
        done += take;
      }
      ring.Pop();
    }
    FlushRecordStage();
  }

  void DrainProfiler(std::uint64_t now) noexcept {
    const bool live = conn != nullptr && phase == Phase::Ready;
    const std::uint32_t mask = rt->ProfilerMask();
    // refill the byte budget
    const double dt = static_cast<double>(now - tokenNs) / 1e9;
    tokenNs = now;
    tokens += dt * d.profilerBytesPerSecond;
    if (tokens > d.profilerBytesPerSecond) tokens = d.profilerBytesPerSecond;
    // block timing: worst block of each interval
    BlockTiming t{};
    while (rt->timing().TryPop(t)) {
      lateSum += t.lateChunks;
      if (!haveWorst || t.logicMicroseconds + t.renderMicroseconds > worst.logicMicroseconds + worst.renderMicroseconds) {
        worst = t;
        haveWorst = true;
      }
    }
    const bool timingDue = now >= nextTimingNs;
    if (timingDue) {
      nextTimingNs = now + 1000000000ull / (d.timingHz != 0u ? d.timingHz : 1u);
      if (haveWorst && live && (mask & kProfTiming) != 0u) {
        BlockTiming w = worst;
        w.lateChunks = lateSum;
        Profiler(MessageType::BlockTiming, &w, sizeof(w));
      }
      haveWorst = false;
      lateSum = 0;
    }
    // 1.1: block detail (worst block of each timing interval, like BlockTiming)
    BlockDetail bd{};
    while (rt->details().TryPop(bd)) {
      detailLate += bd.lateChunks;
      if (!haveDetail || bd.totalMicroseconds > worstDetail.totalMicroseconds) {
        worstDetail = bd;
        haveDetail = true;
      }
    }
    if (timingDue) {
      if (haveDetail && live && (mask & kProfBlockDetail) != 0u) Profiler(MessageType::BlockDetail, &worstDetail, sizeof(worstDetail));
      haveDetail = false;
      detailLate = 0;
    }
    // voices and meters at meterHz
    if (now >= nextMetersNs) {
      nextMetersNs = now + 1000000000ull / (meterHz != 0u ? meterHz : 1u);
      if (rt->voices().Take() && live && (mask & kProfVoices) != 0u) {
        VoiceSnapshotHeader vh;
        std::memcpy(&vh, rt->voices().Front(), sizeof(vh));
        const ListHeader lh{vh.count, sizeof(VoiceInfo)};
        Profiler(MessageType::VoiceList, &lh, sizeof(lh), rt->voices().Front() + sizeof(vh),
                 static_cast<std::uint32_t>(sizeof(VoiceInfo) * vh.count));
      }
      if (rt->streams().Take() && live && (mask & kProfStreamList) != 0u) {  // 1.1
        StreamSnapshotHeader sh;
        std::memcpy(&sh, rt->streams().Front(), sizeof(sh));
        const std::uint32_t cnt = sh.count < rt->MaxStreams() ? sh.count : rt->MaxStreams();
        const ListHeader lh{cnt, sizeof(StreamInfo)};
        Profiler(MessageType::StreamList, &lh, sizeof(lh), rt->streams().Front() + sizeof(sh),
                 static_cast<std::uint32_t>(sizeof(StreamInfo) * cnt));
      }
      if (rt->meters().Take() && live && (mask & kProfMeters) != 0u) {
        VoiceSnapshotHeader mh;
        std::memcpy(&mh, rt->meters().Front(), sizeof(mh));
        const ListHeader lh{mh.count, sizeof(MeterEntry)};
        Profiler(MessageType::BusMeters, &lh, sizeof(lh), rt->meters().Front() + sizeof(mh),
                 static_cast<std::uint32_t>(sizeof(MeterEntry) * mh.count));
      }
    }
    // memory usage
    if (now >= nextMemoryNs) {
      nextMemoryNs = now + 1000000000ull / (d.memoryHz != 0u ? d.memoryHz : 1u);
      if (rt->stats().Take() && live && (mask & kProfMemory) != 0u) {
        StatsSnapshot ss;
        std::memcpy(&ss, rt->stats().Front(), sizeof(ss));
        MemoryUsage mu{};
        for (std::uint32_t k = 0; k < SND_MEM_TAG_COUNT; ++k) {
          mu.used[k] = ss.stats.memory_used[k];
          mu.budget[k] = ss.stats.memory_budget[k];
        }
        Profiler(MessageType::MemoryUsage, &mu, sizeof(mu));
      }
    }
    // events as they come
    StreamStall st{};
    while (rt->stalls().TryPop(st)) {
      if (live && (mask & kProfStreams) != 0u) Profiler(MessageType::StreamStall, &st, sizeof(st));
    }
    LodChange lc{};
    while (rt->lods().TryPop(lc)) {
      if (live && (mask & kProfLod) != 0u) Profiler(MessageType::LodChange, &lc, sizeof(lc));
    }
    // event log in batches
    const bool due = now >= nextLogNs || rt->log().SizeApprox() >= kLogBatch;
    if (due) {
      nextLogNs = now + 1000000000ull / (d.eventLogHz != 0u ? d.eventLogHz : 1u);
      for (int round = 0; round < 8; ++round) {
        std::uint32_t n = 0;
        LogEntry e{};
        while (n < kLogBatch && rt->log().TryPop(e)) {
          std::memcpy(logBuf + sizeof(ListHeader) + std::size_t{n} * sizeof(LogEntry), &e, sizeof(e));
          ++n;
        }
        if (n == 0u) break;
        if (live && (mask & kProfEventLog) != 0u) {
          const ListHeader lh{n, sizeof(LogEntry)};
          std::memcpy(logBuf, &lh, sizeof(lh));
          Profiler(MessageType::EventLog, logBuf, static_cast<std::uint32_t>(sizeof(lh) + n * sizeof(LogEntry)));
        }
      }
    }
  }

  // --- the thread ------------------------------------------------------------------------
  static void Main(void* user) noexcept { static_cast<Impl*>(user)->Run(); }

  void Run() noexcept {
    platform::SetCurrentThreadName("snd-livelink");
    registered = sys->RegisterThread() == SND_OK;
    registration.store(registered ? 1u : 2u, std::memory_order_release);

    if (rt->config().livelink.record_commands != 0u && d.recordSink != nullptr) {
      recMode = RecMode::Sink;
      rt->RequestRecording(true);
    }
    while (stop.load(std::memory_order_acquire) == 0u) {
      const std::uint64_t now = platform::MonotonicNanoseconds();
      if (conn == nullptr) {
        ITransport* t = acceptor->Accept(d.pollMs);
        if (t != nullptr) OnConnect(t, platform::MonotonicNanoseconds());
      } else {
        Service(now);
      }
      TryRevert();
      DrainStops();
      PollReload();
      DrainResults();
      DrainRecords();
      DrainProfiler(platform::MonotonicNanoseconds());
      if (conn != nullptr) PumpSend();
      // Every loop, not only after recording: a publish that found no free arena
      // (the audio thread still holds them) must be retried, or the last commands
      // would wait for the next tool message (empty arenas are not published).
      if (registered) sys->Flush();
      recordedCommands = false;
    }
    // shutdown: stop recording, drain what is left to the sink
    if (recMode == RecMode::Sink) {
      rt->RequestRecording(false);
      recStopping = true;
    }
    Disconnect();
    DrainResults();
    DrainRecords();
    if (recordedCommands && registered) sys->Flush();
    if (registered) sys->UnregisterThread();
    registered = false;
  }

  void PumpSend() noexcept {
    std::uint64_t sent = 0;
    const IoStatus st = sender.Pump(*conn, &sent);
    bytesOut.Add(sent);
    if (sent != 0u || sender.Idle()) lastSendProgressNs = platform::MonotonicNanoseconds();
    if (st == IoStatus::Closed || st == IoStatus::Error) Disconnect();
  }

  // Bytes that may be read now (acceptance decision D2): every tracked command (>= 32
  // bytes on the wire) needs a slot of the result FIFO, every frame (>= 16 bytes) may
  // need a reply (<= kReplyRoom bytes) in the control queue. 0: stop reading; the
  // tool's frames wait in the transport (TCP flow control).
  std::size_t ReadBudget() const noexcept {
    std::uint64_t b = kRecvChunk;
    const std::uint64_t byResults = std::uint64_t{pendingCap - pendingCount} * 32u;
    if (byResults < b) b = byResults;
    const std::uint64_t cf = sender.control().Free();
    const std::uint64_t byControl = cf > 2u * kReplyRoom ? (cf - 2u * kReplyRoom) * kFrameHeaderBytes / kReplyRoom : 0u;
    if (byControl < b) b = byControl;
    return static_cast<std::size_t>(b);
  }

  void Service(std::uint64_t now) noexcept {
    const bool canRead = ReadBudget() != 0u;
    conn->Wait(canRead, !sender.Idle(), d.pollMs);
    if (!canRead || phase == Phase::Closing) lastRecvNs = platform::MonotonicNanoseconds();  // not listening
    // receive
    for (int round = 0; round < 64 && conn != nullptr; ++round) {
      const std::size_t budget = ReadBudget();
      if (budget == 0u) break;
      std::size_t got = 0;
      const IoStatus st = conn->Recv(recvBuf, budget, &got);
      if (st == IoStatus::Ok) {
        bytesIn.Add(got);
        lastRecvNs = platform::MonotonicNanoseconds();
        if (phase == Phase::Closing) continue;  // discard
        const HeaderStatus hs = reader.Feed(recvBuf, got, *this);
        if (hs != HeaderStatus::Ok) {
          protocolErrors.Add();
          SendError(SND_ERR_CORRUPT, 0, hs == HeaderStatus::BadMagic ? "bad magic" : "payload too large");
          BeginClose();
        }
        continue;
      }
      if (st == IoStatus::Closed || st == IoStatus::Error) {
        Disconnect();
        return;
      }
      break;  // WouldBlock
    }
    if (conn == nullptr) return;
    acceptor->RejectPending();
    now = platform::MonotonicNanoseconds();
    if (phase == Phase::Closing) {
      PumpSend();
      if (conn != nullptr && (sender.Idle() || now - closingNs > 500u * kMs)) Disconnect();
      return;
    }
    if ((phase == Phase::AwaitHello || phase == Phase::AwaitSync) && now - phaseNs > d.handshakeTimeoutMs * kMs) {
      timeouts.Add();
      SendError(SND_ERR_NOT_READY, 0, "handshake timeout");
      BeginClose();
      return;
    }
    if (now - lastRecvNs > d.timeoutMs * kMs) {
      timeouts.Add();
      Disconnect();
      return;
    }
    // A tool that reads nothing for timeoutMs (the only reason to drop a tool for its pace).
    if (!sender.Idle() && now - lastSendProgressNs > d.timeoutMs * kMs) {
      timeouts.Add();
      Disconnect();
      return;
    }
    if (phase == Phase::Ready && now - lastPingNs >= d.heartbeatMs * kMs) {
      lastPingNs = now;
      const Ping ping{now};
      Control(MessageType::Ping, seqOut++, &ping, sizeof(ping));
    }
  }
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
int Server::Create(const ServerDesc& desc, Server** out) noexcept {
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  *out = nullptr;
  if (desc.system == nullptr || desc.runtime == nullptr || desc.allocator == nullptr ||
      desc.maxReceivePayloadBytes < 4096u || desc.controlBufferBytes < 1024u) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  IAllocator& a = *desc.allocator;
  void* sm = a.Allocate(sizeof(Server), 16, MemTag::LiveLink);
  void* im = a.Allocate(sizeof(Impl), alignof(Impl) < 16 ? 16 : alignof(Impl), MemTag::LiveLink);
  if (sm == nullptr || im == nullptr) {
    a.Free(sm, MemTag::LiveLink);
    a.Free(im, MemTag::LiveLink);
    return SND_ERR_OUT_OF_BUDGET;
  }
  Server* s = new (sm) Server();
  s->impl_ = new (im) Impl();
  if (!s->impl_->Init(desc)) {
    Destroy(s);
    return SND_ERR_OUT_OF_BUDGET;
  }
  desc.runtime->Bind(desc.system);
  *out = s;
  return SND_OK;
}

void Server::Destroy(Server* server) noexcept {
  if (server == nullptr) return;
  IAllocator* a = server->impl_ != nullptr ? server->impl_->alloc : nullptr;
  if (server->impl_ != nullptr) {
    server->Stop();
    server->impl_->~Impl();
    if (a != nullptr) a->Free(server->impl_, MemTag::LiveLink);
  }
  server->~Server();
  if (a != nullptr) a->Free(server, MemTag::LiveLink);
}

int Server::Start() noexcept {
  Impl& im = *impl_;
  if (im.started) return SND_OK;
  if (!im.rt->ClaimServer()) return SND_ERR_LIMIT_REACHED;  // D-170: one started Server per runtime
  if (im.d.acceptor != nullptr) {
    im.acceptor = im.d.acceptor;
  } else {
    const std::int32_t cfgPort = static_cast<std::int32_t>(im.rt->config().livelink.port);
    const std::int32_t port = im.d.port >= 0 ? im.d.port : cfgPort;
    if (port < 0 || port > 65535) {
      im.rt->ReleaseServer();
      return SND_ERR_INVALID_ARGUMENT;
    }
    const int r = im.tcp.Listen(im.d.bindAddress, static_cast<std::uint16_t>(port));
    if (r != SND_OK) {
      im.rt->ReleaseServer();
      return r;
    }
    im.port.store(im.tcp.Port(), std::memory_order_relaxed);
    im.acceptor = &im.tcp;
  }
  // D-87: the session starts here, so that once Start returns no edit an earlier Server
  // accepted is applied any more.
  im.session = im.rt->NewSession();
  im.sessionAnnounced = false;
  im.stop.store(0, std::memory_order_release);
  platform::ThreadDesc td;
  td.name = "snd-livelink";
  td.priority = platform::ThreadPriority::Normal;
  im.registration.store(0, std::memory_order_relaxed);
  if (!im.thread.Start(td, &Impl::Main, &im)) {
    im.tcp.Close();
    im.rt->ReleaseServer();
    return SND_ERR_LIMIT_REACHED;
  }
  im.started = true;
  // The thread registers itself first thing: wait for it, so that its registered place
  // is taken before Start returns (the api reserves one place for it, D-93).
  std::uint32_t reg = 0;
  while ((reg = im.registration.load(std::memory_order_acquire)) == 0u) platform::YieldThread();
  if (reg != 1u) {
    Stop();
    return SND_ERR_LIMIT_REACHED;
  }
  return SND_OK;
}

void Server::Stop() noexcept {
  Impl& im = *impl_;
  if (!im.started) return;
  im.stop.store(1, std::memory_order_release);
  im.thread.Join();
  // Chunks still queued are cancelled: the audio thread frees them without applying
  // (their memory belongs to the LiveRuntime, which outlives this Server).
  im.rt->CancelAllChunks();
  im.tcp.Close();
  im.started = false;
  im.rt->ReleaseServer();
}

std::uint16_t Server::Port() const noexcept { return impl_->port.load(std::memory_order_relaxed); }

bool Server::Connected() const noexcept { return impl_->connected.load(std::memory_order_acquire) != 0u; }

ServerStats Server::Stats() const noexcept {
  const Impl& im = *impl_;
  ServerStats s;
  s.connections = static_cast<std::uint32_t>(im.connections.Get());
  s.handshakesRejected = static_cast<std::uint32_t>(im.handshakesRejected.Get());
  s.timeouts = static_cast<std::uint32_t>(im.timeouts.Get());
  s.protocolErrors = static_cast<std::uint32_t>(im.protocolErrors.Get());
  s.framesIn = im.framesIn.Get();
  s.framesOut = im.framesOut.Get();
  s.bytesIn = im.bytesIn.Get();
  s.bytesOut = im.bytesOut.Get();
  s.commandsRecorded = static_cast<std::uint32_t>(im.commandsRecorded.Get());
  s.commandsRefused = static_cast<std::uint32_t>(im.commandsRefused.Get());
  s.profilerSent = static_cast<std::uint32_t>(im.profilerSent.Get());
  s.profilerDropped = static_cast<std::uint32_t>(im.profilerDropped.Get());
  s.recordBytes = static_cast<std::uint32_t>(im.recordBytes.Get());
  const RuntimeDrops dr = im.rt->Drops();
  s.runtimeDropped = dr.log + dr.timing + dr.stalls + dr.lod + dr.results;
  s.recordsDropped = dr.records;
  s.pingsAnswered = static_cast<std::uint32_t>(im.pingsAnswered.Get());
  s.controlDropped = static_cast<std::uint32_t>(im.controlDropped.Get());
  s.revertsLost = static_cast<std::uint32_t>(im.revertsLost.Get());
  s.staleResults = static_cast<std::uint32_t>(im.staleResults.Get());
  s.lastRoundTripUs = im.lastRttUs.load(std::memory_order_relaxed);
  s.auditionsStopped = static_cast<std::uint32_t>(im.auditionsStopped.Get());
  s.reloadsDone = static_cast<std::uint32_t>(im.reloadsDone.Get());
  s.reloadsFailed = static_cast<std::uint32_t>(im.reloadsFailed.Get());
  s.structRefused = static_cast<std::uint32_t>(im.structRefused.Get());
  s.readsAnswered = static_cast<std::uint32_t>(im.readsAnswered.Get());
  return s;
}

}  // namespace snd::livelink
