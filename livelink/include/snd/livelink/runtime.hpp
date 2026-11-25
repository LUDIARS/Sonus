// Live link, audio-thread side (design.md 2.1, 11.2, 11.3).
//
// LiveRuntime is created before core::System and installed into its CoreDesc
// (Install): core then calls its hooks on the audio thread
//   - liveCommand: LiveSetValue / LiveReplaceChunk -> ValueApplier (override layer);
//   - liveTap.arena: recording (11.3) and "playback requested" log entries;
//   - liveTap.event / ended / stall: event log, LOD changes, stream stalls;
//   - liveTap.block: block timing, voice list, bus meters, statistics, the set of
//     registered banks, and the start / end of a recording.
// Every hook only copies into preallocated SPSC channels (rings.hpp,
// base::SpscRing) and returns: principle 1 holds, and a full channel drops the
// item and counts it (Drops()) instead of waiting. The audio thread never
// touches a socket. The live link thread (Server) is the only consumer.
//
// What the profiler produces is gated by the mask the tool sets
// (SetProfilerMask); with mask 0 the hooks only do the override layer and the
// recording.
//
// 1.1 (protocol §8):
//   - auditions: every LiveAudition the arenas carry is tracked (playback handle)
//     until it ends; the kFieldAuditionStop marker hands the tracked handles to the
//     live link thread (auditionStops()), which stops them with System::Stop. Only
//     auditions are stopped, never the game's playbacks;
//   - cell requests: RevertValue / ReadCell (CellReading through readings());
//   - structural edits (struct_edit.hpp): Config.dev_reserve accounting, the editor
//     seam, the log of applied edits for the disconnect revert;
//   - profiler: StreamList (TripleSlot streams()) and BlockDetail (details());
//   - recording: the starting state (RecordBegin, RecordCell, RecordOverride) after the
//     first Lod and BankMap records, and the data of every LiveReplaceChunk
//     (RecordLiveData, written just before the Arena record that carries the command).
#ifndef SND_LIVELINK_RUNTIME_HPP
#define SND_LIVELINK_RUNTIME_HPP

#include <atomic>
#include <cstdint>

#include "snd/base/spsc.hpp"
#include "snd/config.h"
#include "snd/ext/allocator.hpp"
#include "snd/format/livelink_protocol.hpp"
#include "snd/internal/commands.hpp"
#include "snd/internal/notify.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/livelink/rings.hpp"
#include "snd/livelink/struct_edit.hpp"
#include "snd/livelink/value_applier.hpp"

namespace snd::core {
class System;
class LivePluginPool;
struct CoreDesc;
struct LiveArenaView;
struct LiveBankInfo;
}  // namespace snd::core

namespace snd::livelink {

struct RuntimeDesc {
  const snd_config* config = nullptr;  // required: the System's configuration
  IAllocator* allocator = nullptr;     // required (MemTag::LiveLink)
  std::uint32_t maxEdits = 1024;       // remembered override cells (ValueApplier)
  std::uint32_t recordRingBytes = 4u << 20;  // audio -> live link records (11.3)
  std::uint32_t logEntries = 4096;
  std::uint32_t timingEntries = 1024;
  std::uint32_t stallEntries = 256;
  std::uint32_t lodEntries = 64;
  std::uint32_t resultEntries = 1024;  // applied SetValue / ReplaceChunk results
  std::uint32_t maxVoices = 0;         // 0: max_virtual_voices
  std::uint32_t maxMeters = 0;         // 0: 256 buses
  std::uint32_t chunkSlots = 1;        // ReplaceChunk in flight (PendingChunk pool)
  std::uint32_t chunkBytes = 1u << 20; // largest ReplaceChunk table (data bytes)
  // 1.1
  std::uint32_t maxAuditions = 32;     // auditions tracked at once (more: Audition answers LIMIT_REACHED)
  std::uint32_t detailEntries = 1024;  // BlockDetail channel
  std::uint32_t maxStructLog = 64;     // structural edits remembered for the revert (D-172: an edit that
                                       // would not fit is refused at acceptance, LIMIT_REACHED)
  IStructEditor* structEditor = nullptr;  // null: structural edits answer SND_ERR_UNSUPPORTED (C-01)
};

// LogEntry.kind values the runtime produces (protocol §6, LogEntry):
//   notify::EventType (1..9)              events posted by the systems
//   0x8000 | cmd::CommandType             commands the audio thread took (Play,
//                                         LiveAudition: id = event, handleSlot = playback)
//   kLogPlaybackEnded (0x4000)            a playback ended: id = 0, handleSlot = slot,
//                                         detail = reason << 16 | end detail
inline constexpr std::uint32_t kLogCommandBit = 0x8000u;
inline constexpr std::uint32_t kLogPlaybackEnded = 0x4000u;

// One applied live command (in the order the live link recorded them).
struct ApplyResult {
  std::uint32_t kind;   // cmd::CommandType (LiveSetValue / LiveReplaceChunk)
  std::int32_t result;  // snd_result
  std::uint32_t cells;
  std::uint32_t revert;  // 1: it was the revert marker
  std::uint32_t session = 0;  // session of the Server that recorded it (kFieldSession marker), 0 = unknown
  std::uint32_t reserved = 0;
};

// Record ring messages: records of record_format.hpp, plus this end marker.
inline constexpr std::uint32_t kRecordEndMarker = 0xFFFF0000u;
struct RecordEnd {
  std::uint32_t kind;     // kRecordEndMarker
  std::uint32_t dropped;  // records that did not fit while recording
};

struct RuntimeDrops {
  std::uint32_t log = 0, timing = 0, stalls = 0, lod = 0, results = 0, records = 0;
  std::uint32_t staleEdits = 0;  // edits of an earlier Server's session, not applied (D-87)
  // 1.1
  std::uint32_t readings = 0;    // CellReading channel full
  std::uint32_t auditions = 0;   // auditions not tracked (table / stop channel full)
  std::uint32_t details = 0;     // BlockDetail channel full
  std::uint32_t structLog = 0;   // D-172: never expected (a log place is reserved at acceptance); a bug if not 0
  std::uint32_t structUndo = 0;  // undo refused by the editor during a revert
};

// 1.1: Config.dev_reserve in use by live structural edits (any thread).
struct DevReserveUsage {
  std::uint32_t buses = 0, effects = 0;
  std::uint64_t bytes = 0;
  std::uint32_t busesLimit = 0, effectsLimit = 0;
  std::uint64_t bytesLimit = 0;
};

// Snapshots published through TripleSlots (live link thread reads them).
struct VoiceSnapshotHeader {
  std::uint32_t count;  // entries that follow
  std::uint32_t total;  // voices that existed (count may be smaller)
  std::uint64_t block;
};
struct StatsSnapshot {
  std::uint64_t block;
  snd_stats stats;
};
struct BankSnapshotHeader {
  std::uint32_t count;
  std::uint32_t reserved;
};
struct StreamSnapshotHeader {  // + StreamInfo[count]
  std::uint32_t count;
  std::uint32_t reserved;
  std::uint64_t block;
};

class LiveRuntime {
 public:
  static int Create(const RuntimeDesc& desc, LiveRuntime** out) noexcept;
  static void Destroy(LiveRuntime* rt) noexcept;

  // Before core::System::Create: sets desc->liveCommand / liveUser / liveTap.
  void Install(core::CoreDesc* desc) noexcept;

  // --- controls (any non-RT thread; the live link thread) -----------------------------
  // mask: ProfilerMask. meterHz: rate of voice lists and meters.
  void SetProfiler(std::uint32_t mask, std::uint32_t meterHz) noexcept;
  std::uint32_t ProfilerMask() const noexcept { return mask_.load(std::memory_order_relaxed); }
  // Starts / stops the recording at the next block boundary.
  void RequestRecording(bool on) noexcept { recordRequest_.store(on ? 1u : 0u, std::memory_order_release); }
  bool Recording() const noexcept { return recording_.load(std::memory_order_acquire) != 0u; }
  // A new session number for a Server (never 0, never reused while the runtime lives).
  // The newest session is the only one whose edits are applied: edits recorded by an
  // earlier Server and applied after a newer Server started are dropped (D-87).
  // D-170: one started Server per runtime. Server::Start claims the runtime (false: another
  // Server is started on it); Server::Stop gives it back.
  bool ClaimServer() noexcept {
    std::uint32_t expected = 0u;
    return serverClaimed_.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel);
  }
  void ReleaseServer() noexcept { serverClaimed_.store(0u, std::memory_order_release); }
  std::uint32_t NewSession() noexcept {
    const std::uint32_t s = nextSession_.fetch_add(1u, std::memory_order_relaxed);
    latestSession_.store(s, std::memory_order_release);
    return s;
  }

  // --- channels (consumer: the live link thread) ---------------------------------------
  base::SpscRing<LogEntry>& log() noexcept { return log_; }
  base::SpscRing<BlockTiming>& timing() noexcept { return timing_; }
  base::SpscRing<StreamStall>& stalls() noexcept { return stalls_; }
  base::SpscRing<LodChange>& lods() noexcept { return lods_; }
  base::SpscRing<ApplyResult>& results() noexcept { return results_; }
  MessageRing& records() noexcept { return records_; }
  TripleSlot& voices() noexcept { return voices_; }   // VoiceSnapshotHeader + VoiceInfo[]
  TripleSlot& meters() noexcept { return meters_; }   // VoiceSnapshotHeader + MeterEntry[]
  TripleSlot& stats() noexcept { return stats_; }     // StatsSnapshot
  TripleSlot& banks() noexcept { return banks_; }     // BankSnapshotHeader + core::LiveBankInfo[]
  RuntimeDrops Drops() const noexcept;
  ValueApplier& applier() noexcept { return applier_; }
  // 1.1 channels
  base::SpscRing<CellReading>& readings() noexcept { return readings_; }
  base::SpscRing<std::uint64_t>& auditionStops() noexcept { return auditionStops_; }  // playback handle values
  base::SpscRing<BlockDetail>& details() noexcept { return details_; }
  TripleSlot& streams() noexcept { return streams_; }  // StreamSnapshotHeader + StreamInfo[]
  std::uint32_t MaxStreams() const noexcept { return maxStreams_; }

  // --- 1.1 auditions (live link thread) -------------------------------------------------
  // Takes one place among maxAuditions before an Audition is recorded (false: full).
  // ReleaseAudition gives it back when the command could not be recorded; the audio thread
  // gives it back when the audition ends.
  bool ReserveAudition() noexcept;
  void ReleaseAudition() noexcept;
  std::uint32_t ActiveAuditions() const noexcept { return auditionsActive_.load(std::memory_order_relaxed); }

  // --- 1.1 structural edits (live link thread; usage from any thread) --------------------
  // Takes the edit's cost from Config.dev_reserve. SND_ERR_OUT_OF_BUDGET when it does not fit
  // (the tool rebuilds and hot reloads). The audio thread gives it back if the edit is not
  // applied, and when a revert undoes it or a remove takes away what a live edit added.
  snd_result ReserveStruct(const StructCost& cost) noexcept;
  void ReleaseStruct(const StructCost& cost) noexcept;
  DevReserveUsage ReserveUsage() const noexcept;
  // D-172: one place of the structural-edit log (maxStructLog), taken by the live link thread
  // when it accepts an edit (false: full, the edit is refused with SND_ERR_LIMIT_REACHED and
  // takes nothing; a remove goes on without one). The audio thread gives it back unless the
  // applied edit stays in the log (a refused / stale / cancelled edit, a remove that cancels
  // a live add, an undo); it takes one itself for a remove of a bank object accepted without.
  bool ReserveStructLog() noexcept;
  void ReleaseStructLog(std::uint32_t n = 1) noexcept;
  std::uint32_t StructLogPlaces() const noexcept { return structLogTaken_.load(std::memory_order_relaxed); }
  bool HasStructEditor() const noexcept { return structEditor_ != nullptr; }

  // --- ReplaceChunk slots (live link thread) -------------------------------------------
  // The pool outlives any Server: the audio thread writes results into it even if the
  // Server that queued a chunk is gone. Acquire: a Free slot (nullptr if none), its
  // `data` holds ChunkBytes(). Finish: Done -> Free after the result was read.
  // CancelAll (Server::Destroy): Queued -> Cancelled (the audio thread frees it without
  // applying), Done -> Free.
  snd_result PreparePlugin(PendingChunk& p) noexcept;  // network thread, before queuing
  PendingChunk* AcquireChunk() noexcept;
  void FinishChunk(PendingChunk* c) noexcept;
  void CancelAllChunks() noexcept;
  std::uint32_t ChunkBytes() const noexcept { return chunkBytes_; }

  const snd_config& config() const noexcept { return config_; }
  std::uint32_t MaxVoices() const noexcept { return maxVoices_; }
  std::uint32_t MaxMeters() const noexcept { return maxMeters_; }

  // --- audio thread (core's hooks call these) ------------------------------------------
  void OnCommand(core::System& sys, cmd::CommandType type, const void* payload) noexcept;
  void OnArena(const core::LiveArenaView& arena) noexcept;
  void OnEvent(const notify::EventRecord& record) noexcept;
  void OnEnded(std::uint32_t slot, std::uint32_t generation, std::uint32_t reason, std::uint32_t detail) noexcept;
  void OnStall(std::uint32_t slot, Id eventId, std::uint32_t count, BlockIndex block) noexcept;
  void OnBlock(core::System& sys) noexcept;

  // The System bound to the hooks (set by Bind, after System::Create).
  void Bind(core::System* sys) noexcept { system_.store(sys, std::memory_order_release); }
  core::System* system() const noexcept { return system_.load(std::memory_order_acquire); }

 private:
  LiveRuntime() noexcept = default;
  ~LiveRuntime();
  bool Init(const RuntimeDesc& desc) noexcept;
  void PushLog(std::uint32_t kind, std::uint32_t id, std::uint32_t handleSlot, std::uint32_t detail) noexcept;
  void PushRecord(const void* rec, std::uint32_t bytes) noexcept;
  void UpdateBanks(core::System& sys) noexcept;
  // 1.1
  void ScanArena(const core::LiveArenaView& a) noexcept;           // auditions, stop markers, recorded chunk data
  void WriteLiveData(const cmd::LiveReplaceChunk& c) noexcept;
  void WriteInitialRoutes() noexcept;
  void RecordStartState(core::System& sys) noexcept;
  void TrackAudition(HandleRef h) noexcept;
  void ApplyStruct(core::System& sys, PendingChunk& p, std::uint32_t session, ApplyResult* r) noexcept;
  void UndoStructs(core::System& sys, std::uint32_t session) noexcept;
  void PublishStreams(core::System& sys, std::uint64_t block) noexcept;
  IAllocator* alloc_ = nullptr;
  snd_config config_{};
  std::atomic<core::System*> system_{nullptr};
  FallbackCaps caps_{};
  std::uint32_t maxVoices_ = 0, maxMeters_ = 0, maxBanks_ = 0;
  ValueApplier applier_;
  base::SpscRing<LogEntry> log_;
  base::SpscRing<BlockTiming> timing_;
  base::SpscRing<StreamStall> stalls_;
  base::SpscRing<LodChange> lods_;
  base::SpscRing<ApplyResult> results_;
  base::SpscRing<CellReading> readings_;
  base::SpscRing<std::uint64_t> auditionStops_;
  base::SpscRing<BlockDetail> details_;
  MessageRing records_;
  TripleSlot voices_, meters_, stats_, banks_, streams_;
  // controls
  std::atomic<std::uint32_t> mask_{0};
  std::atomic<std::uint32_t> meterEvery_{1};
  std::atomic<std::uint32_t> recordRequest_{0};
  std::atomic<std::uint32_t> recording_{0};
  std::atomic<std::uint32_t> serverClaimed_{0};
  std::atomic<std::uint32_t> nextSession_{1};
  std::atomic<std::uint32_t> latestSession_{0};
  std::atomic<std::uint32_t> dropStale_{0};
  bool StaleProducer() const noexcept;  // audio thread: the arena being applied is of an older session
  bool Admit(cmd::CommandType type, const void* payload) noexcept;  // LiveTap::admit
  // Who set each live override last (D-87: an older session's revert removes only its own).
  struct OverrideOwner {
    std::uint32_t kind;
    Id id;
    std::uint32_t session;
    std::uint32_t used;
    std::uint32_t value = 0;     // 1.1: for the recording's starting state
    std::uint32_t reserved = 0;
    std::uint64_t target = 0;
  };
  static constexpr std::uint32_t kMaxOverrideOwners = 256;
  OverrideOwner overrideOwners_[kMaxOverrideOwners] = {};
  std::uint8_t* revertOpen_ = nullptr;  // [producerCount_] audio thread: kFieldRevertBegin seen, revert not done
  std::uint32_t* producerSession_ = nullptr;  // [max_registered_threads] audio thread: last marker per producer
  std::uint32_t producerCount_ = 0;
  std::uint32_t currentProducer_ = 0xFFFFFFFFu;  // producer of the arena being applied
  // drops
  std::atomic<std::uint32_t> dropLog_{0}, dropTiming_{0}, dropStalls_{0}, dropLod_{0}, dropResults_{0},
      dropRecords_{0};
  std::atomic<std::uint32_t> dropReadings_{0}, dropAuditions_{0}, dropDetails_{0}, dropStructLog_{0}, dropStructUndo_{0};
  // 1.1 auditions: places taken (live link thread reserves, audio thread releases at the end)
  std::atomic<std::uint32_t> auditionsActive_{0};
  std::uint32_t maxAuditions_ = 0;
  std::uint64_t* auditions_ = nullptr;  // [maxAuditions_] audio thread: tracked handle values (0 = empty)
  // 1.1 structural edits
  core::LivePluginPool* pluginPool_ = nullptr;
  IStructEditor* structEditor_ = nullptr;
  std::atomic<std::uint32_t> usedBuses_{0}, usedEffects_{0};
  std::atomic<std::uint64_t> usedBytes_{0};
  struct StructLogEntry {
    std::uint32_t used;
    std::uint32_t op;
    std::uint32_t targetId;
    std::uint32_t argCount;  // of the original edit (args beyond kLoggedArgs are not kept)
    std::uint32_t session;
    std::uint32_t buses, effects;  // cost (given back by the undo)
    std::uint32_t producer;
    std::uint64_t bytes;
    std::uint64_t bankHandle, targetBankHandle;
    std::uint64_t token;
    std::uint64_t args[4];
  };
  std::uint64_t nextStructToken_ = 0;
  StructLogEntry* structLog_ = nullptr;  // [structLogCap_] audio thread, oldest first
  std::atomic<std::uint32_t> structLogTaken_{0};  // places taken (accepted edits in flight + entries)
  std::uint32_t structLogCap_ = 0, structLogCount_ = 0;
  // 1.1 profiler / recording (audio thread)
  std::uint32_t maxStreams_ = 0;
  std::uint32_t* streamStalls_ = nullptr;  // [maxStreams_]
  std::uint64_t arenasSeen_ = 0;           // non-empty arenas applied before (FromCreation / MidRun)
  EditView* editScratch_ = nullptr;        // [maxEdits] starting state of a recording
  std::uint32_t editScratchCap_ = 0;
  // audio thread private
  std::uint32_t recordDroppedSession_ = 0;
  std::uint32_t lod_ = 0;
  BlockIndex block_ = 0;      // block being processed (ApplyCommands .. end)
  ClockSample clock_ = 0;     // clock at the start of block_
  std::uint32_t blocksSinceMeters_ = 0, blocksSinceStats_ = 0, blocksPerSecond_ = 1;
  const void** rejectedStructs_ = nullptr;  // payloads in current arena, bounded by queued slots
  std::uint32_t rejectedStructCount_ = 0;
  PendingChunk* chunks_ = nullptr;  // [chunkCount_]
  std::uint8_t* chunkMemory_ = nullptr;
  std::uint32_t chunkCount_ = 0, chunkBytes_ = 0;
  core::LiveBankInfo* knownBanks_ = nullptr;  // [maxBanks] registered at the last block
  core::LiveBankInfo* scratchBanks_ = nullptr;
  std::uint32_t knownBankCount_ = 0;
};

}  // namespace snd::livelink

#endif  // SND_LIVELINK_RUNTIME_HPP
