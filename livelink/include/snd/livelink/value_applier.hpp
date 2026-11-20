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
// 1.1: RevertCell (LiveSetValue with valueType kValueTypeRevertCell) restores one cell
// (every bank holding it) and forgets it; ReadCell reads a cell without writing (the
// current value, the remembered original, whether it is overridden).
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

// What LiveReplaceChunk::prepared points to: a validated table chunk. The slots and
// their data live in LiveRuntime (which outlives core), so the audio thread always
// writes into live memory, even when the Server was destroyed meanwhile.
//   Free -(live link thread)-> Queued -(audio thread)-> Done -(live link thread)-> Free
//   Queued -(Server::Destroy)-> Cancelled -(audio thread: not applied)-> Free
//   Done   -(Server::Destroy)-> Free
struct PendingChunk {
  enum State : std::uint32_t { kFree = 0, kQueued = 1, kDone = 2, kCancelled = 3 };
  std::atomic<std::uint32_t> state{kFree};
  std::int32_t result = 0;        // snd_result, written before state = kDone
  std::uint32_t cells = 0;        // cells changed
  std::uint32_t seq = 0;          // live link bookkeeping (frame seq)
  Id bankId = 0;
  std::uint32_t fourcc = 0;
  std::uint32_t rows = 0;
  std::uint32_t reserved = 0;
  std::uint8_t* data = nullptr;  // TableChunkHeader + ColumnEntry[] + columns (capacity: RuntimeDesc::chunkBytes)
  std::uint64_t bytes = 0;
  // 1.1: fourcc == kStructEditTag: `data` holds StructEdit + args, and this is what it
  // reserved of Config.dev_reserve (given back if it is not applied).
  std::uint32_t costBuses = 0;
  std::uint32_t costEffects = 0;
  std::uint64_t costBytes = 0;
};

// 1.1: the answer to a ReadCell (audio thread -> live link thread).
struct CellReading {
  std::uint32_t seq = 0;      // the ReadCell frame's seq (LiveSetValue::value)
  std::uint32_t session = 0;
  std::int32_t result = 0;    // snd_result
  std::uint32_t cells = 0;    // banks holding the cell
  std::uint32_t flags = 0;    // livelink::CellFlags
  std::uint32_t reserved = 0;
  std::uint64_t current = 0;
  std::uint64_t original = 0;
};

// 1.1: one remembered edit, as the recording's starting state lists it.
struct EditView {
  std::uint32_t bankSlot;
  std::uint32_t fourcc;
  std::uint32_t column;
  std::uint32_t row;
  std::uint64_t original;
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
  // session: the live link session of the command (D-83); 0 = none. A remembered cell
  // carries the newest session that wrote it.
  ApplyOutcome SetValue(core::System& sys, const cmd::LiveSetValue& c, std::uint32_t session = 0) noexcept;
  ApplyOutcome ReplaceChunk(core::System& sys, const PendingChunk& chunk, std::uint32_t session = 0) noexcept;
  // Restores the remembered cells whose newest writer is `session` or older (0: every
  // cell) and forgets them; cells a newer session wrote stay (D-87: an earlier Server's
  // disconnect revert never undoes a newer Server's edits).
  ApplyOutcome RevertAll(core::System& sys, std::uint32_t session = 0) noexcept;
  // 1.1: puts one cell (c.objectId, c.field, c.index; every bank holding it) back to the
  // remembered original and forgets it. cells = cells restored (0: nothing was overridden).
  // SND_ERR_NOT_FOUND: no such cell.
  ApplyOutcome RevertCell(core::System& sys, const cmd::LiveSetValue& c) noexcept;
  // 1.1: reads one cell (no write). *out gets result, cells, flags, current, original.
  void ReadCell(core::System& sys, const cmd::LiveSetValue& c, CellReading* out) noexcept;
  // 1.1 (replay of a recording's starting state): writes one cell of the registered banks
  // with this bank id and remembers its original. SND_ERR_NOT_FOUND if no bank / cell.
  snd_result WriteCell(core::System& sys, Id bankId, std::uint32_t fourcc, std::uint32_t column, std::uint32_t row,
                       std::uint64_t bits, std::uint32_t session = 0) noexcept;
  // 1.1 (audio thread): the remembered edits of the current registrations (up to capacity;
  // returns how many exist).
  std::uint32_t CopyEdits(core::System& sys, EditView* out, std::uint32_t capacity) noexcept;

  // 1.1 (audio thread): reads one cell's raw bits without writing (false: no such cell).
  static bool ReadBits(core::System& sys, const core::LiveCell& cell, std::uint32_t fourcc, std::uint32_t column,
                       std::uint64_t* bits) noexcept;

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
    std::uint32_t session;  // newest session that wrote the cell
    std::uint32_t reserved;
  };
  // Entry of the cell of the bank's CURRENT registration (kInvalidSlot if none).
  std::uint32_t Find(std::uint32_t bankSlot, std::uint32_t fourcc, std::uint32_t column, std::uint32_t row) const noexcept;
  // changed: the write changed the cell (a new record is made only then); the session of
  // an existing record is raised either way.
  bool Remember(std::uint32_t bankSlot, std::uint32_t fourcc, std::uint32_t column, std::uint32_t row,
                std::uint64_t original, std::uint32_t session, bool changed) noexcept;
  void Rebuild(bool byGeneration) noexcept;
  std::uint32_t Generation(std::uint32_t bankSlot) const noexcept;
  // Loads the registered banks; when they changed, drops the entries of registrations
  // that are gone (their banks were unloaded: nothing to restore, room freed).
  void LoadBanks(core::System& sys) noexcept;
  void Purge() noexcept;

  IAllocator* alloc_ = nullptr;
  Edit* edits_ = nullptr;          // open addressing, tableSize_ entries
  Edit* spare_ = nullptr;          // rebuild target of Purge (same size)
  std::uint64_t bankKey_ = 0;      // hash of the registered (slot, generation) set at the last purge
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
