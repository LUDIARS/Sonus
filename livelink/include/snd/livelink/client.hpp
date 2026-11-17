// Live link, tool side (design.md 11.1 profiler / mixer, 11.2, 11.3).
//
// A UI-independent client for the authoring tool (and tests). It owns a
// network thread that connects to the game (TCP or any IConnector), runs the
// handshake, answers pings, pings the game, reconnects after a disconnect, and
// buffers what the game streams:
//   - profiler history: the last `historySeconds` of block timings, bus meters
//     (per bus), event log entries, stream stalls and LOD changes, with the
//     local receive time of each; the latest voice list and memory usage;
//   - acknowledgements (by seq) and errors;
//   - the recording (RecordData) in one contiguous buffer.
// Every public function is thread-safe (one internal mutex; the UI thread may
// call them while the network thread runs). Senders return the frame's seq
// (non-zero) or 0 when the message could not be queued (not connected, queue
// full, invalid arguments).
//
// Typical use (tool):
//   ClientDesc d; d.name = "Sonus Tool"; d.projectBuildHash = project.hash;
//   Client* c = nullptr; Client::Create(d, &c); c->Start();
//   c->WaitReady(2000);
//   c->SetProfilerMask(kProfTiming | kProfMeters | kProfVoices, 30);
//   std::uint32_t seq = c->SetValue(BusVolume(bus, -6.0f), true);
//   ... c->TakeAck(seq, &result); c->CopyMeterHistory(bus, samples, n); ...
//   c->Stop(); Client::Destroy(c);
#ifndef SND_LIVELINK_CLIENT_HPP
#define SND_LIVELINK_CLIENT_HPP

#include <cstddef>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/format/livelink_protocol.hpp"
#include "snd/livelink/transport.hpp"

namespace snd::livelink {

struct ClientDesc {
  IAllocator* allocator = nullptr;      // null: the base default allocator
  IConnector* connector = nullptr;      // null: TCP to address:port
  const char* address = "127.0.0.1";
  std::uint16_t port = kDefaultPort;
  const char* name = "sonus-tool";      // Hello.name (up to 31 bytes)
  std::uint64_t projectBuildHash = 0;   // Hello.projectBuildHash
  SyncMode onMismatch = SyncMode::SendDiffs;  // answer to HelloStatus::BuildMismatch
  std::uint32_t historySeconds = 10;    // profiler history kept
  std::uint32_t maxMeterBuses = 64;     // buses kept per meter snapshot
  std::uint32_t maxVoices = 1024;       // voices kept from the latest VoiceList
  std::uint32_t sendBufferBytes = 1u << 20;
  std::uint32_t maxReceivePayloadBytes = 1u << 20;
  std::uint32_t recordBufferBytes = 8u << 20;  // recording kept (larger: overflow flag)
  std::uint32_t heartbeatMs = 1000;
  std::uint32_t timeoutMs = 5000;       // silence from the game -> disconnect
  std::uint32_t connectTimeoutMs = 1000;
  std::uint32_t reconnectDelayMs = 250;
  bool autoReconnect = true;
  std::uint32_t pollMs = 5;
};

enum class ClientState : std::uint8_t {
  Stopped = 0,
  Connecting = 1,  // no connection; trying (or waiting to retry)
  Handshake = 2,   // Hello sent
  Ready = 3,       // commands accepted, profiler streaming
  Rejected = 4,    // protocol major mismatch, or BuildMismatch answered Disconnect; no retry
};

struct TimingSample {
  std::uint64_t receivedNs;  // platform::MonotonicNanoseconds() at receipt
  BlockTiming timing;
};
struct MeterSample {
  std::uint64_t receivedNs;
  MeterEntry meter;
};
struct LogSample {
  std::uint64_t receivedNs;
  LogEntry entry;
};
struct StallSample {
  std::uint64_t receivedNs;
  StreamStall stall;
};
struct LodSample {
  std::uint64_t receivedNs;
  LodChange lod;
};

struct ClientStats {
  std::uint32_t connects = 0;        // handshakes completed (reconnects included)
  std::uint32_t disconnects = 0;
  std::uint32_t timeouts = 0;
  std::uint32_t protocolErrors = 0;  // malformed / unexpected frames from the game
  std::uint64_t framesIn = 0, framesOut = 0, bytesIn = 0, bytesOut = 0;
  std::uint32_t lastRoundTripUs = 0;
  std::uint64_t lastGameClock = 0;   // Pong.gameClockSample
  std::uint32_t errorsReceived = 0;
  std::uint32_t recordOverflow = 0;  // recording bytes that did not fit
};

class Client {
 public:
  static int Create(const ClientDesc& desc, Client** out) noexcept;  // snd_result
  static void Destroy(Client* client) noexcept;

  int Start() noexcept;  // starts the network thread (connects in the background)
  void Stop() noexcept;  // disconnects and joins

  ClientState State() const noexcept;
  bool WaitReady(std::uint32_t timeoutMs) const noexcept;
  HelloReply LastHelloReply() const noexcept;  // of the last handshake
  ClientStats Stats() const noexcept;
  // Drops the connection now (tests: the game sees a disconnect; the client reconnects
  // when autoReconnect is set).
  void Disconnect() noexcept;

  // --- tool -> game (thread-safe; seq or 0) ---------------------------------------------
  std::uint32_t SetValue(const SetValue& value, bool wantAck = true) noexcept;
  // `data`: a table chunk in bank layout (TableChunkHeader + columns).
  std::uint32_t ReplaceChunk(std::uint32_t bankId, std::uint32_t fourcc, const void* data, std::uint64_t bytes,
                             bool wantAck = true) noexcept;
  std::uint32_t Audition(std::uint32_t eventId, const float* position = nullptr, bool stopAll = false,
                         bool wantAck = false) noexcept;
  std::uint32_t OverrideParam(std::uint32_t paramId, float value, bool enable, bool wantAck = false) noexcept;
  std::uint32_t OverrideState(std::uint32_t musicEventId, std::uint32_t stateId, bool enable,
                              bool wantAck = false) noexcept;
  std::uint32_t ClearOverrides(bool wantAck = false) noexcept;
  // Also re-sent automatically after every reconnect.
  std::uint32_t SetProfilerMask(std::uint32_t mask, std::uint32_t meterHz = 0, bool wantAck = false) noexcept;
  std::uint32_t StartRecording(bool wantAck = true) noexcept;  // clears the recording buffer
  std::uint32_t StopRecording(bool wantAck = true) noexcept;
  std::uint32_t StructEdit(StructOp op, std::uint32_t targetId, const std::uint64_t* args, std::uint32_t argCount,
                           bool wantAck = true) noexcept;
  // Any frame (tests, fuzzing). The payload is sent as given.
  std::uint32_t SendRaw(std::uint16_t type, std::uint16_t flags, const void* payload, std::uint32_t bytes) noexcept;

  // --- answers ----------------------------------------------------------------------------
  // true and *result if the game acknowledged `seq` (the Ack is consumed).
  bool TakeAck(std::uint32_t seq, std::int32_t* result) noexcept;
  bool WaitAck(std::uint32_t seq, std::uint32_t timeoutMs, std::int32_t* result) noexcept;
  // Errors received (oldest first, up to `capacity`); they stay until ClearErrors.
  std::uint32_t CopyErrors(Error* out, std::uint32_t capacity) const noexcept;
  void ClearErrors() noexcept;

  // --- profiler (copies; oldest first; only the last historySeconds) --------------------
  std::uint32_t CopyTimings(TimingSample* out, std::uint32_t capacity) const noexcept;
  std::uint32_t CopyMeterHistory(std::uint32_t busId, MeterSample* out, std::uint32_t capacity) const noexcept;
  std::uint32_t CopyEventLog(LogSample* out, std::uint32_t capacity) const noexcept;
  std::uint32_t CopyStalls(StallSample* out, std::uint32_t capacity) const noexcept;
  std::uint32_t CopyLods(LodSample* out, std::uint32_t capacity) const noexcept;
  // Latest voice list / meters / memory (0 / false before the first one).
  std::uint32_t CopyVoices(VoiceInfo* out, std::uint32_t capacity, std::uint64_t* receivedNs = nullptr) const noexcept;
  std::uint32_t CopyMeters(MeterEntry* out, std::uint32_t capacity, std::uint64_t* receivedNs = nullptr) const noexcept;
  bool LatestMemory(MemoryUsage* out) const noexcept;
  // Number of profiler messages of `type` received since Start.
  std::uint64_t Received(MessageType type) const noexcept;

  // --- recording (11.3) ------------------------------------------------------------------
  std::uint64_t RecordingBytes() const noexcept;
  std::uint64_t CopyRecording(std::uint8_t* out, std::uint64_t capacity) const noexcept;

  struct Impl;

 private:
  Client() noexcept = default;
  Impl* impl_ = nullptr;
};

}  // namespace snd::livelink

#endif  // SND_LIVELINK_CLIENT_HPP
