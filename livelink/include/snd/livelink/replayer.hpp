// Replay of a recording (design.md 2.5, 11.3).
//
// A Replayer is installed into the CoreDesc of a fresh System (Install, before
// System::Create) that has the same Config and the same banks loaded as the
// recorded one. After Start(), the next ApplyCommands of that System is the
// first recorded block, and every recorded arena is applied at the same block
// distance from it, before the System's own producers (CoreDesc::liveTap.replay).
// Value edits (LiveSetValue) go through the Replayer's own override layer.
// Under the conditions of 2.5 (same build, same SIMD path, LOD disabled, no
// stalls) two replays of one recording render bit-identical output, and the
// replay renders what the recorded run rendered from the recording's start.
//
// Commands that point into the recording process or cannot be replayed are
// left out and counted (Info().skippedCommands): BankRegister / BankUnload
// (the replaying System loads the banks itself; BankMap records list them and
// Info().bankMismatches counts recorded banks missing from the replaying
// System), PlaySynth / PlayInput / PlayUserBuffer / CreateUserBuffer /
// DestroyUserBuffer (callbacks, device input and game memory: 2.5 does not
// cover them). 1.1: a LiveReplaceChunk is replayed when the recording carries
// its data (RecordLiveData before its Arena; a 1.0 recording does not: then it
// is skipped); a structural edit (kStructEditTag) goes to ReplayDesc::structEditor
// (none: counted in Info().structEditsNotApplied, outside the 2.5 guarantee).
// A hot reload during the recording appears as BankMap records after the start
// (Info().bankChanges; the replaying System does not reload: outside 2.5).
//
// Every other command passes the game API's entry rules (command_check.hpp: enums,
// ranges, NaN / Inf, handle shapes, time signatures); a command that fails them is
// left out and counted per type (Info().invalidCommands / invalidByType), like the
// overflow-fallback parameters (invalidFallbackParams).
//
// Starting state (1.1): a recording says how it started (RecordBegin: FromCreation /
// MidRun) and carries the live link's part of the state at its start: the override-layer
// cells (RecordCell, written into the replaying System's banks before the first block)
// and the live overrides (RecordOverride, applied as LiveOverride commands in the first
// replayed block); both are applied in a lead block before the first recorded block
// (Info().leadBlocks). What core holds (sounds playing, parameter values, emitters,
// transports) is not in the recording (request C-04): a MidRun recording replays what
// the recorded run rendered only when the replaying System starts from the same core
// state. Info().origin and warnings say so.
//
// LOD and stalls (2.5 conditions): the replay does not reproduce recorded LOD changes or
// stalls (core has no way to force them, request C-05); it reports them
// (Info().conditions: kReplayLodRecorded / kReplayStallRecorded) and counts LOD changes
// and stalls of the replaying System itself (kReplayLodInReplay / kReplayStallInReplay).
// BitExactExpected() is true when none of the conditions that break 2.5 holds.
//
// Comparing replays: the Replayer logs the decisions it can see (decision log:
// Play / LiveAudition / Stop commands applied, playback ends with their reason,
// markers, beats, errors and warnings of the event ring, and a digest of the voice
// list whenever it changes), all stamped with the replay-relative block. Two replays of
// one recording give equal logs (2.5 "logic decisions"); OutputDigest hashes the
// rendered output for the bit-identity check.
//
// Create parses and validates the whole recording (malformed input is
// rejected, never crashes). The audio-thread hook only walks prepared arrays.
#ifndef SND_LIVELINK_REPLAYER_HPP
#define SND_LIVELINK_REPLAYER_HPP

#include <atomic>
#include <cstdint>

#include "snd/config.h"
#include "snd/ext/allocator.hpp"
#include "snd/format/livelink_protocol.hpp"
#include "snd/internal/commands.hpp"
#include "snd/internal/notify.hpp"
#include "snd/livelink/struct_edit.hpp"
#include "snd/livelink/value_applier.hpp"

namespace snd::core {
class System;
struct CoreDesc;
struct LiveArenaView;
}  // namespace snd::core

namespace snd::livelink {

struct ReplayDesc {
  const snd_config* config = nullptr;   // required: the replaying System's configuration
  IAllocator* allocator = nullptr;      // required
  const std::uint8_t* record = nullptr; // the recording (copied)
  std::uint64_t bytes = 0;
  std::uint32_t maxEdits = 1024;        // override cells (ValueApplier)
  // 1.1
  IStructEditor* structEditor = nullptr;  // replays structural edits (null: counted, not applied)
  std::uint32_t maxDecisions = 8192;      // decision log entries kept (0: no log)
  bool applyStartState = true;            // write RecordCell / apply RecordOverride at the start
};

// 1.1: what the replay reports about the 2.5 conditions (ReplayInfo::conditions).
enum ReplayCondition : std::uint32_t {
  kReplayLodRecorded = 1u << 0,      // the recording has a LOD level != 0 or a LOD change
  kReplayStallRecorded = 1u << 1,    // the recording has stalls
  kReplayMidRun = 1u << 2,           // started mid-run: core state before it is not in the recording
  kReplayNoBegin = 1u << 3,          // a 1.0 recording (no starting-state records)
  kReplayStateLost = 1u << 4,        // starting-state items did not fit when recording
  kReplaySkipped = 1u << 5,          // commands left out (not replayable / invalid)
  kReplayStructEdits = 1u << 6,      // structural edits that are not replayed (no editor)
  kReplayBankChanges = 1u << 7,      // banks registered during the recording (hot reload)
  kReplayLodInReplay = 1u << 8,      // the replaying System changed its LOD (enable LOD off)
  kReplayStallInReplay = 1u << 9,    // the replaying System stalled
  kReplayStartStateFailed = 1u << 10,  // a RecordCell could not be written in the replay
};
// Conditions under which 2.5 promises no bit identity (and, for the first ones, no
// identical decisions either).
inline constexpr std::uint32_t kReplayBreaksBitExact = kReplayLodRecorded | kReplayStallRecorded | kReplayStateLost |
                                                       kReplaySkipped | kReplayStructEdits | kReplayBankChanges |
                                                       kReplayLodInReplay | kReplayStallInReplay |
                                                       kReplayStartStateFailed;

// 1.1: one entry of the decision log (block = replay-relative block number).
struct ReplayDecision {
  std::uint64_t block;
  std::uint32_t kind;    // LogEntry kinds (runtime.hpp): 0x8000 | CommandType, kLogPlaybackEnded,
                         // notify::EventType, or kDecisionVoices
  std::uint32_t id;      // event / marker id; kDecisionVoices: number of voices
  std::uint32_t slot;    // playback slot (or the event's object)
  std::uint32_t detail;  // end reason << 16 | detail, event b, kDecisionVoices: digest of the list
};
static_assert(sizeof(ReplayDecision) == 24);
inline constexpr std::uint32_t kDecisionVoices = 0x2000u;

// FNV-1a over the bit patterns of rendered samples (bit identity, 2.5).
struct OutputDigest {
  std::uint64_t value = 0xCBF29CE484222325ull;
  std::uint64_t samples = 0;
  void Add(const float* s, std::size_t n) noexcept;
};

struct ReplayInfo {
  std::uint32_t arenas = 0;           // arena records
  std::uint32_t commands = 0;         // commands kept
  std::uint32_t skippedCommands = 0;  // left out: not replayable (see above)
  std::uint32_t invalidCommands = 0;  // left out: failed the API entry rules (command_check.hpp)
  std::uint32_t invalidByType[static_cast<std::size_t>(cmd::CommandType::Count)] = {};  // per CommandType
  std::uint32_t invalidFallbackParams = 0;
  std::uint32_t lodRecords = 0;
  std::uint32_t stallRecords = 0;
  std::uint32_t bankMaps = 0;
  std::uint64_t firstBlock = 0;       // recorded block numbers
  std::uint64_t lastBlock = 0;
  std::uint32_t bankMismatches = 0;   // after the first replayed block
  // 1.1
  std::uint32_t origin = 0;           // RecordOrigin of the Begin record (0: none, a 1.0 recording)
  std::uint32_t startCells = 0;       // RecordCell records
  std::uint32_t startOverrides = 0;   // RecordOverride records
  // Blocks before the first recorded one that apply the starting state (1 when there is a
  // starting state to apply, else 0). The recorded block firstBlock is rendered leadBlocks
  // blocks after Start: start lead blocks early to keep the block numbering (the value
  // changes of the state ramp in during the lead block, like they did before the recording).
  std::uint32_t leadBlocks = 0;
  std::uint32_t lostState = 0;        // RecordBegin::lostState
  std::uint32_t liveData = 0;         // RecordLiveData records
  std::uint32_t replacedChunks = 0;   // LiveReplaceChunk commands kept (data found)
  std::uint32_t structEdits = 0;      // structural edits kept
  std::uint32_t structEditsNotApplied = 0;  // refused by / no editor (during the replay)
  std::uint32_t bankChanges = 0;      // BankMap records after the first arena
  std::uint32_t startStateFailures = 0;  // RecordCell not written in the replaying System
  std::uint32_t replayLodChanges = 0; // LOD changes of the replaying System
  std::uint32_t replayStalls = 0;     // stalls of the replaying System
  std::uint32_t decisions = 0;        // decision log entries
  std::uint32_t decisionsLost = 0;    // beyond maxDecisions
  std::uint32_t conditions = 0;       // ReplayCondition bits
};

class Replayer {
 public:
  //   SND_ERR_INVALID_ARGUMENT  missing fields
  //   SND_ERR_CORRUPT           the recording is malformed (RecordReader error)
  //   SND_ERR_VERSION_MISMATCH  fallback capacities differ from the Config
  //   SND_ERR_OUT_OF_BUDGET     allocation failed
  static int Create(const ReplayDesc& desc, Replayer** out) noexcept;
  static void Destroy(Replayer* r) noexcept;

  void Install(core::CoreDesc* desc) noexcept;  // liveTap.replay + liveCommand
  void Bind(core::System* sys) noexcept { system_.store(sys, std::memory_order_release); }
  // Arms the replay: the next block of the bound System is the first recorded block.
  void Start() noexcept { armed_.store(1u, std::memory_order_release); }
  bool Finished() const noexcept { return finished_.load(std::memory_order_acquire) != 0u; }
  // Blocks the replay spans (last - first + 1, plus the lead blocks); render at least this
  // many after Start.
  std::uint64_t Blocks() const noexcept {
    return (info_.arenas == 0u ? 0u : info_.lastBlock - info_.firstBlock + 1u) + info_.leadBlocks;
  }
  ReplayInfo Info() const noexcept;
  // 1.1: the 2.5 conditions hold (no kReplayBreaksBitExact condition, so far).
  bool BitExactExpected() const noexcept { return (Info().conditions & kReplayBreaksBitExact) == 0u; }
  // 1.1: decision log (oldest first). Read it after the replay (Finished) or from the
  // thread that renders; returns the number copied.
  std::uint32_t CopyDecisions(ReplayDecision* out, std::uint32_t capacity) const noexcept;

  // Audio thread (installed hooks).
  bool Next(std::uint64_t block, core::LiveArenaView* out) noexcept;
  void OnCommand(cmd::CommandType type, const void* payload) noexcept;
  void OnEvent(const notify::EventRecord& record) noexcept;
  void OnEnded(std::uint32_t slot, std::uint32_t generation, std::uint32_t reason, std::uint32_t detail) noexcept;
  void OnStall(std::uint32_t slot) noexcept;
  void OnBlock(core::System& sys) noexcept;

 private:
  struct Arena {
    std::uint64_t relBlock;
    std::uint32_t normalOffset, normalBytes;
    std::uint32_t reserveOffset, reserveBytes;
    std::uint32_t flags;
    std::uint32_t fallback;  // index into fallbacks_, or ~0u
  };
  struct BankRef {
    Id bankId;
    std::uint32_t reserved;
    std::uint64_t contentHash;
  };
  Replayer() noexcept = default;
  ~Replayer();
  int Init(const ReplayDesc& desc) noexcept;
  void CheckBanks(core::System& sys) noexcept;
  void Log(std::uint32_t kind, std::uint32_t id, std::uint32_t slot, std::uint32_t detail) noexcept;
  void ApplyStartState(core::System& sys) noexcept;
  void LogArena(const Arena& a) noexcept;

  IAllocator* alloc_ = nullptr;
  snd_config config_{};
  ValueApplier applier_;
  std::uint8_t* commands_ = nullptr;  // filtered command bytes of every arena
  Arena* arenas_ = nullptr;
  std::uint32_t arenaCount_ = 0;
  cmd::OverflowFallback* fallbacks_ = nullptr;
  std::uint32_t fallbackCount_ = 0;
  std::uint8_t* fallbackMemory_ = nullptr;  // bitsets + params of every fallback
  BankRef* banks_ = nullptr;
  std::uint32_t bankCount_ = 0;
  // 1.1
  IStructEditor* structEditor_ = nullptr;
  PendingChunk* chunks_ = nullptr;  // [chunkCount_] the recorded LiveReplaceChunk data
  std::uint8_t* chunkMemory_ = nullptr;
  std::uint32_t chunkCount_ = 0;
  RecordCell* startCells_ = nullptr;
  std::uint32_t startCellCount_ = 0;
  bool applyStart_ = true;
  bool prologue_ = false;  // arenas_[0] holds the starting overrides
  ReplayDecision* decisions_ = nullptr;
  std::uint32_t maxDecisions_ = 0;
  std::atomic<std::uint32_t> decisionCount_{0};
  std::atomic<std::uint32_t> decisionsLost_{0};
  std::atomic<std::uint32_t> replayLod_{0}, replayStalls_{0}, structNotApplied_{0}, startFailures_{0};
  livelink::VoiceInfo* voiceScratch_ = nullptr;
  std::uint32_t voiceCap_ = 0;
  std::uint32_t lastVoiceDigest_ = 0, lastVoiceCount_ = 0xFFFFFFFFu;
  std::uint64_t rel_ = 0;  // audio thread: replay-relative block being rendered
  ReplayInfo info_{};
  std::atomic<core::System*> system_{nullptr};
  std::atomic<std::uint32_t> armed_{0};
  std::atomic<std::uint32_t> finished_{0};
  std::atomic<std::uint32_t> bankMismatches_{0};
  // audio thread
  bool begun_ = false;
  std::uint64_t base_ = 0;
  std::uint32_t cursor_ = 0;
};

}  // namespace snd::livelink

#endif  // SND_LIVELINK_REPLAYER_HPP
