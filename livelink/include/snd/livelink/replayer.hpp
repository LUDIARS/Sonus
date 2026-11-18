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
// System), LiveReplaceChunk (its data stays in the recording process),
// PlaySynth / PlayInput / PlayUserBuffer / CreateUserBuffer /
// DestroyUserBuffer (callbacks, device input and game memory: 2.5 does not
// cover them).
//
// Every other command passes the game API's entry rules (command_check.hpp: enums,
// ranges, NaN / Inf, handle shapes, time signatures); a command that fails them is
// left out and counted per type (Info().invalidCommands / invalidByType), like the
// overflow-fallback parameters (invalidFallbackParams).
//
// Starting state: a recording holds no snapshot of the state at its start. A replay
// renders what the recorded run rendered only when it starts from the same state as
// the recording did (banks loaded, sounds playing, overrides, parameter values);
// writing the starting state into the recording is a candidate for contract 1.2.
//
// Create parses and validates the whole recording (malformed input is
// rejected, never crashes). The audio-thread hook only walks prepared arrays.
#ifndef SND_LIVELINK_REPLAYER_HPP
#define SND_LIVELINK_REPLAYER_HPP

#include <atomic>
#include <cstdint>

#include "snd/config.h"
#include "snd/ext/allocator.hpp"
#include "snd/internal/commands.hpp"
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
  // Blocks the replay spans (last - first + 1); render at least this many after Start.
  std::uint64_t Blocks() const noexcept { return info_.arenas == 0u ? 0u : info_.lastBlock - info_.firstBlock + 1u; }
  ReplayInfo Info() const noexcept;

  // Audio thread (installed hooks).
  bool Next(std::uint64_t block, core::LiveArenaView* out) noexcept;
  void OnCommand(cmd::CommandType type, const void* payload) noexcept;

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
