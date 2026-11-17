// Override layer, audio-thread side (design.md 11.2 "上書き層").
//
// The live link thread records LiveSetValue / LiveReplaceChunk commands; core's
// ApplyCommands hands them to CoreDesc::liveCommand at a block boundary, which
// calls this applier. It writes the cells of the dev banks' writable copies
// through core (System::LiveWriteCell) and remembers the original value of
// every cell it changed (first write wins), so that the edits can be put back:
// RevertAll (LiveSetValue with field kFieldRevertAll) restores every remembered
// cell whose bank is still the same registration, then forgets them.
//
// A command is applied completely or not at all: every cell is validated
// first. A command that would need more remembered cells than the table holds
// is refused (SND_ERR_LIMIT_REACHED) so that a revert is always complete.
//
// Audio thread only (principle 1): no allocation, no lock. Init / Destroy are
// non-RT.
#ifndef SND_LIVELINK_VALUE_APPLIER_HPP
#define SND_LIVELINK_VALUE_APPLIER_HPP

#include <atomic>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/internal/commands.hpp"
#include "snd/types.h"

namespace snd::core {
class System;
struct LiveCell;
struct LiveBankInfo;
}  // namespace snd::core

namespace snd::livelink {

// What LiveReplaceChunk::prepared points to: a validated table chunk owned by the
// live link thread until `state` says the audio thread is done with it.
struct PendingChunk {
  enum State : std::uint32_t { kFree = 0, kQueued = 1, kDone = 2 };
  std::atomic<std::uint32_t> state{kFree};
  std::int32_t result = 0;        // snd_result, written before state = kDone
  std::uint32_t cells = 0;        // cells changed
  std::uint32_t seq = 0;          // live link bookkeeping (frame seq)
  Id bankId = 0;
  std::uint32_t fourcc = 0;
  std::uint32_t rows = 0;
  std::uint32_t reserved = 0;
  const std::uint8_t* data = nullptr;  // TableChunkHeader + ColumnEntry[] + columns
  std::uint64_t bytes = 0;
};

struct ApplyOutcome {
  snd_result result = SND_OK;
  std::uint32_t cells = 0;  // cells written (or restored)
};

class ValueApplier {
 public:
  ValueApplier() noexcept = default;
  ~ValueApplier() { Destroy(); }
  ValueApplier(const ValueApplier&) = delete;
  ValueApplier& operator=(const ValueApplier&) = delete;

  // maxEdits: remembered cells; maxBanks: Config.max_banks; maxCellsPerObject: banks
  // holding the same object (shared objects are copied into every bank, A1-16).
  bool Init(IAllocator& allocator, std::uint32_t maxEdits, std::uint32_t maxBanks) noexcept;
  void Destroy() noexcept;

  // --- audio thread ----------------------------------------------------------------
  ApplyOutcome SetValue(core::System& sys, const cmd::LiveSetValue& c) noexcept;
  ApplyOutcome ReplaceChunk(core::System& sys, const PendingChunk& chunk) noexcept;
  ApplyOutcome RevertAll(core::System& sys) noexcept;

  // Any thread (statistics).
  std::uint32_t Remembered() const noexcept { return count_.load(std::memory_order_relaxed); }
  std::uint32_t Capacity() const noexcept { return capacity_; }

 private:
  struct Edit {
    std::uint32_t bankSlot;
    std::uint32_t bankGeneration;
    std::uint32_t fourcc;
    std::uint32_t column;
    std::uint32_t row;
    std::uint32_t used;
    std::uint64_t original;
  };
  std::uint32_t Find(std::uint32_t bankSlot, std::uint32_t fourcc, std::uint32_t column, std::uint32_t row) const noexcept;
  bool Remember(std::uint32_t bankSlot, std::uint32_t fourcc, std::uint32_t column, std::uint32_t row,
                std::uint64_t original) noexcept;
  std::uint32_t Generation(std::uint32_t bankSlot) const noexcept;
  void LoadBanks(core::System& sys) noexcept;

  IAllocator* alloc_ = nullptr;
  Edit* edits_ = nullptr;          // open addressing, tableSize_ entries
  std::uint32_t tableSize_ = 0;    // power of two >= 2 * capacity_
  std::uint32_t capacity_ = 0;
  std::atomic<std::uint32_t> count_{0};
  core::LiveCell* cells_ = nullptr;        // [maxBanks] scratch
  core::LiveBankInfo* banks_ = nullptr;    // [maxBanks] scratch
  std::uint32_t* bankSlots_ = nullptr;     // [maxBanks] scratch
  std::uint32_t maxBanks_ = 0;
  std::uint32_t bankCount_ = 0;
};

}  // namespace snd::livelink

#endif  // SND_LIVELINK_VALUE_APPLIER_HPP
