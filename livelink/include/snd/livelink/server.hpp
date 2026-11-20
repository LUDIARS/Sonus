// Live link, game side (design.md 11.2; protocol §1-§7).
//
// One Server per System. It owns the live link thread (normal priority), which
//   - accepts one tool at a time (TCP, or any IAcceptor such as MemoryLink),
//   - runs the handshake (Hello -> HelloReply [-> SyncDecision]),
//   - turns tool messages into core commands (LiveSetValue, LiveReplaceChunk,
//     LiveOverride, LiveClearOverrides, LiveAudition) recorded from its own
//     registered thread (System::RecordLive, then Flush every loop),
//   - answers Ack / Error (a SetValue / ReplaceChunk is answered once the audio
//     thread applied it: the result comes back through LiveRuntime::results),
//   - streams the profiler (VoiceList, BlockTiming, MemoryUsage, StreamStall,
//     LodChange, EventLog, BusMeters) thinned to the configured rates and
//     capped by a byte budget per second, dropping (and counting) whatever
//     does not fit: the game is never slowed down,
//   - streams recordings (RecordData) and pings the tool (heartbeat); a tool
//     that stays silent for timeoutMs is disconnected, and the server goes back
//     to accepting (the tool reconnects).
// On disconnect the overrides stay (11.2) unless revertOnDisconnect is set:
// then the server records LiveClearOverrides and the revert marker, which puts
// every value edit back to the bank value.
//
// Protocol 1.1 (livelink-protocol.md §8): RevertValue / ReadCell (answered with
// CellValue once applied), QueryBanks (BankList; also before SyncDecision), ReloadBank
// (hot reload: StopAndUnload + the normal load path, ReloadStatus progress, edits
// answered NOT_READY meanwhile), Audition stop-all (stops this runtime's auditions only),
// StructEdit within Config.dev_reserve (struct_edit.hpp seam), StreamList and BlockDetail.
//
// The live link thread never touches runtime data directly and never waits for
// the audio thread (2 thread table). Threads: Create / Start / Stop / Destroy
// on one non-RT thread (normally the owner); Stats / Connected from any thread.
#ifndef SND_LIVELINK_SERVER_HPP
#define SND_LIVELINK_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/livelink/transport.hpp"

namespace snd::core {
class System;
}  // namespace snd::core

namespace snd::livelink {

class LiveRuntime;

struct ServerDesc {
  core::System* system = nullptr;   // required: created with runtime->Install(&coreDesc)
  LiveRuntime* runtime = nullptr;   // required
  IAllocator* allocator = nullptr;  // required (MemTag::LiveLink)
  IAcceptor* acceptor = nullptr;    // null: TCP on bindAddress:port
  const char* bindAddress = "127.0.0.1";  // loopback only by default
  std::int32_t port = -1;                 // -1: Config.livelink.port, 0: any free port
  std::uint32_t sendBufferBytes = 0;      // bulk queue; 0: Config.livelink.send_buffer_bytes
  std::uint32_t controlBufferBytes = 16u << 10;
  std::uint32_t maxReceivePayloadBytes = 1u << 20;  // larger frames are skipped with Error
  std::uint32_t profilerBytesPerSecond = 1u << 20;  // bandwidth cap of the profiler messages
  std::uint32_t meterHz = 0;                        // 0: Config.livelink.meter_hz (tool can change)
  std::uint32_t timingHz = 30;                      // BlockTiming (worst block of each interval)
  std::uint32_t eventLogHz = 10;
  std::uint32_t memoryHz = 1;
  std::uint32_t heartbeatMs = 1000;
  std::uint32_t timeoutMs = 5000;           // silence from the tool -> disconnect
  std::uint32_t handshakeTimeoutMs = 5000;  // no Hello / SyncDecision -> disconnect
  std::uint32_t pollMs = 5;                 // loop period
  bool revertOnDisconnect = false;          // 11.2: false keeps the overrides
  std::uint32_t platform = 0;               // HelloReply.platform; 0: this build's
  // Config.livelink.record_commands != 0: the server records from Start to Stop
  // into this sink (live link thread). A tool's RecordControl is refused meanwhile.
  void (*recordSink)(void* user, const std::uint8_t* bytes, std::size_t n) noexcept = nullptr;
  void* recordUser = nullptr;
};

struct ServerStats {
  std::uint32_t connections = 0;        // accepted
  std::uint32_t handshakesRejected = 0; // protocol major mismatch, bad Hello, SyncDecision Disconnect
  std::uint32_t timeouts = 0;           // heartbeat / handshake timeouts
  std::uint32_t protocolErrors = 0;     // bad magic, oversized, malformed, wrong state
  std::uint64_t framesIn = 0, framesOut = 0;
  std::uint64_t bytesIn = 0, bytesOut = 0;
  std::uint32_t commandsRecorded = 0;   // core commands recorded for the tool
  std::uint32_t commandsRefused = 0;    // SND_ERR_COMMAND_BUFFER_FULL etc.
  std::uint32_t profilerSent = 0;       // profiler messages sent
  std::uint32_t profilerDropped = 0;    // profiler messages dropped (bandwidth / send buffer)
  std::uint32_t recordBytes = 0;        // RecordData bytes produced (tool or sink)
  // From LiveRuntime (audio side): items dropped because a channel was full.
  std::uint32_t runtimeDropped = 0;
  std::uint32_t recordsDropped = 0;
  std::uint32_t pingsAnswered = 0;
  std::uint32_t lastRoundTripUs = 0;    // Ping -> Pong
  std::uint32_t controlDropped = 0;     // server pings that did not fit (the tool reads too slowly)
  std::uint32_t staleResults = 0;       // applied results of an earlier Server's commands (dropped)
  std::uint32_t revertsLost = 0;        // disconnect reverts that could not be recorded (warning)
  // 1.1
  std::uint32_t auditionsStopped = 0;   // audition playbacks stopped by stop-all
  std::uint32_t reloadsDone = 0;        // ReloadBank completed
  std::uint32_t reloadsFailed = 0;
  std::uint32_t structRefused = 0;      // structural edits refused (rules, reserve, editor)
  std::uint32_t readsAnswered = 0;      // CellValue sent
};

class Server {
 public:
  static int Create(const ServerDesc& desc, Server** out) noexcept;  // snd_result
  static void Destroy(Server* server) noexcept;                       // stops first

  // Starts listening and the live link thread.
  //   SND_ERR_IO            could not listen (port in use)
  //   SND_ERR_LIMIT_REACHED the thread could not be created, or (D-170) another Server is
  //                         started on the same LiveRuntime (one consumer of its channels);
  //                         once that one is stopped or destroyed, Start succeeds
  int Start() noexcept;
  void Stop() noexcept;

  std::uint16_t Port() const noexcept;  // TCP port in use (0 with a custom acceptor)
  bool Connected() const noexcept;      // handshake done, tool attached
  ServerStats Stats() const noexcept;

  struct Impl;

 private:
  Server() noexcept = default;
  Impl* impl_ = nullptr;
};

}  // namespace snd::livelink

#endif  // SND_LIVELINK_SERVER_HPP
