// Override layer, audio-thread side: SetValue / ReplaceChunk / RevertAll.
#include "snd/livelink/value_applier.hpp"

#include <cstring>

#include "snd/core/core.hpp"
#include "snd/format/bank_check.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/livelink/wire.hpp"

namespace snd::livelink {

namespace bf = ::snd::bank_format;

namespace {
std::uint32_t Hash(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept {
  std::uint64_t h = 0x9E3779B97F4A7C15ull;
  for (std::uint32_t v : {a, b, c, d}) {
    h ^= v;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
  }
  return static_cast<std::uint32_t>(h);
}
}  // namespace

bool ValueApplier::Init(IAllocator& allocator, std::uint32_t maxEdits, std::uint32_t maxBanks) noexcept {
  Destroy();
  alloc_ = &allocator;
  capacity_ = maxEdits < 1u ? 1u : maxEdits;
  tableSize_ = 4;
  while (tableSize_ < 2u * capacity_) tableSize_ <<= 1;
  maxBanks_ = maxBanks < 1u ? 1u : maxBanks;
  edits_ = static_cast<Edit*>(allocator.Allocate(sizeof(Edit) * tableSize_, 64, MemTag::LiveLink));
  spare_ = static_cast<Edit*>(allocator.Allocate(sizeof(Edit) * tableSize_, 64, MemTag::LiveLink));
  cells_ = static_cast<core::LiveCell*>(allocator.Allocate(sizeof(core::LiveCell) * maxBanks_, 64, MemTag::LiveLink));
  banks_ = static_cast<core::LiveBankInfo*>(
      allocator.Allocate(sizeof(core::LiveBankInfo) * maxBanks_, 64, MemTag::LiveLink));
  bankSlots_ = static_cast<std::uint32_t*>(allocator.Allocate(sizeof(std::uint32_t) * maxBanks_, 64, MemTag::LiveLink));
  if (edits_ == nullptr || spare_ == nullptr || cells_ == nullptr || banks_ == nullptr || bankSlots_ == nullptr) {
    Destroy();
    return false;
  }
  std::memset(static_cast<void*>(edits_), 0, sizeof(Edit) * tableSize_);
  count_.store(0, std::memory_order_relaxed);
  return true;
}

void ValueApplier::Destroy() noexcept {
  if (alloc_ != nullptr) {
    alloc_->Free(edits_, MemTag::LiveLink);
    alloc_->Free(spare_, MemTag::LiveLink);
    alloc_->Free(cells_, MemTag::LiveLink);
    alloc_->Free(banks_, MemTag::LiveLink);
    alloc_->Free(bankSlots_, MemTag::LiveLink);
  }
  edits_ = nullptr;
  spare_ = nullptr;
  cells_ = nullptr;
  banks_ = nullptr;
  bankSlots_ = nullptr;
  capacity_ = tableSize_ = maxBanks_ = 0;
}

std::uint32_t ValueApplier::Find(std::uint32_t bankSlot, std::uint32_t fourcc, std::uint32_t column,
                                 std::uint32_t row) const noexcept {
  const std::uint32_t mask = tableSize_ - 1u;
  for (std::uint32_t i = Hash(bankSlot, fourcc, column, row) & mask, n = 0; n < tableSize_; i = (i + 1u) & mask, ++n) {
    const Edit& e = edits_[i];
    if (e.used == 0u) return kInvalidSlot;
    if (e.bankSlot == bankSlot && e.fourcc == fourcc && e.column == column && e.row == row) {
      return e.bankGeneration == Generation(bankSlot) ? i : kInvalidSlot;
    }
  }
  return kInvalidSlot;
}

bool ValueApplier::Remember(std::uint32_t bankSlot, std::uint32_t fourcc, std::uint32_t column, std::uint32_t row,
                            std::uint64_t original) noexcept {
  if (Find(bankSlot, fourcc, column, row) != kInvalidSlot) return true;  // first write wins
  const std::uint32_t mask = tableSize_ - 1u;
  const std::uint32_t gen = Generation(bankSlot);
  for (std::uint32_t i = Hash(bankSlot, fourcc, column, row) & mask;; i = (i + 1u) & mask) {
    Edit& e = edits_[i];
    if (e.used != 0u) {
      if (e.bankSlot == bankSlot && e.fourcc == fourcc && e.column == column && e.row == row) {
        // A record of an earlier registration of this bank slot: the new registration's
        // value is the original from now on.
        e.bankGeneration = gen;
        e.original = original;
        return true;
      }
      continue;
    }
    if (count_.load(std::memory_order_relaxed) >= capacity_) return false;
    e = Edit{bankSlot, gen, fourcc, column, row, 1u, original};
    count_.store(count_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    return true;
  }
}

void ValueApplier::LoadBanks(core::System& sys) noexcept {
  const std::uint32_t n = sys.LiveBanks(banks_, maxBanks_);
  bankCount_ = n < maxBanks_ ? n : maxBanks_;
  std::uint64_t key = 0xCBF29CE484222325ull;
  for (std::uint32_t i = 0; i < bankCount_; ++i) {
    key = (key ^ banks_[i].handle.Value()) * 0x100000001B3ull;
  }
  if (key != bankKey_) {
    bankKey_ = key;
    Purge();
  }
}

void ValueApplier::Purge() noexcept {
  std::memset(static_cast<void*>(spare_), 0, sizeof(Edit) * tableSize_);
  const std::uint32_t mask = tableSize_ - 1u;
  std::uint32_t kept = 0;
  for (std::uint32_t k = 0; k < tableSize_; ++k) {
    const Edit& e = edits_[k];
    if (e.used == 0u || e.bankGeneration == 0u || Generation(e.bankSlot) != e.bankGeneration) continue;
    for (std::uint32_t i = Hash(e.bankSlot, e.fourcc, e.column, e.row) & mask;; i = (i + 1u) & mask) {
      if (spare_[i].used == 0u) {
        spare_[i] = e;
        break;
      }
    }
    ++kept;
  }
  Edit* t = edits_;
  edits_ = spare_;
  spare_ = t;
  count_.store(kept, std::memory_order_relaxed);
}

std::uint32_t ValueApplier::Generation(std::uint32_t bankSlot) const noexcept {
  for (std::uint32_t i = 0; i < bankCount_; ++i) {
    if (banks_[i].handle.slot == bankSlot) return banks_[i].handle.generation;
  }
  return 0;
}

ApplyOutcome ValueApplier::SetValue(core::System& sys, const cmd::LiveSetValue& c) noexcept {
  if (c.field == kFieldRevertAll) return RevertAll(sys);
  if (c.field == kFieldSession) return ApplyOutcome{};  // the live link's session marker (runtime)
  std::uint32_t fourcc = 0, column = 0;
  if (edits_ == nullptr || !DecodeField(c.field, &fourcc, &column)) return ApplyOutcome{SND_ERR_INVALID_ARGUMENT, 0};
  LoadBanks(sys);
  std::uint32_t n = sys.LiveFindCells(c.objectId, fourcc, c.index, cells_, maxBanks_);
  if (n > maxBanks_) n = maxBanks_;
  if (n == 0u) return ApplyOutcome{SND_ERR_NOT_FOUND, 0};
  const std::uint64_t bits = c.value;
  std::uint32_t newKeys = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    std::uint64_t prev = 0;
    const snd_result r = sys.LiveWriteCell(cells_[i], fourcc, column, bits, &prev, false);
    if (r != SND_OK) return ApplyOutcome{r, 0};
    if (prev != bits && Find(cells_[i].bankSlot, fourcc, column, cells_[i].row) == kInvalidSlot) ++newKeys;
  }
  if (count_.load(std::memory_order_relaxed) + newKeys > capacity_) return ApplyOutcome{SND_ERR_LIMIT_REACHED, 0};
  ApplyOutcome out;
  for (std::uint32_t i = 0; i < n; ++i) {
    std::uint64_t prev = 0;
    if (sys.LiveWriteCell(cells_[i], fourcc, column, bits, &prev, true) != SND_OK) continue;  // validated above
    if (prev != bits) {
      Remember(cells_[i].bankSlot, fourcc, column, cells_[i].row, prev);
      ++out.cells;
    }
  }
  return out;
}

ApplyOutcome ValueApplier::ReplaceChunk(core::System& sys, const PendingChunk& chunk) noexcept {
  if (edits_ == nullptr || chunk.data == nullptr) return ApplyOutcome{SND_ERR_INVALID_ARGUMENT, 0};
  LoadBanks(sys);
  std::uint32_t nb = sys.LiveFindBanks(chunk.bankId, bankSlots_, maxBanks_);
  if (nb > maxBanks_) nb = maxBanks_;
  if (nb == 0u) return ApplyOutcome{SND_ERR_NOT_FOUND, 0};
  bf::TableChunkHeader th;
  std::memcpy(&th, chunk.data, sizeof(th));
  // Visits every (known column, row) of the replacement with its raw bits.
  auto forCells = [&](auto&& fn) -> snd_result {
    for (std::uint32_t k = 0; k < th.columnCount; ++k) {
      bf::ColumnEntry ce;
      std::memcpy(&ce, chunk.data + sizeof(th) + std::size_t{k} * sizeof(ce), sizeof(ce));
      if (bf::check::FindColumnInfo(chunk.fourcc, ce.columnId) == nullptr) continue;
      for (std::uint32_t row = 0; row < th.rowCount; ++row) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, chunk.data + ce.offset + std::uint64_t{row} * ce.elemBytes, ce.elemBytes);
        const snd_result r = fn(ce.columnId, row, bits);
        if (r != SND_OK) return r;
      }
    }
    return SND_OK;
  };
  std::uint32_t newKeys = 0;
  for (std::uint32_t b = 0; b < nb; ++b) {
    const std::uint32_t slot = bankSlots_[b];
    const std::uint32_t rows = sys.LiveTableRows(slot, chunk.fourcc);
    if (rows == 0u && th.rowCount != 0u) return ApplyOutcome{SND_ERR_UNSUPPORTED, 0};  // no override layer
    if (rows != th.rowCount) return ApplyOutcome{SND_ERR_UNSUPPORTED, 0};  // layout change: rebuild + hot reload
    const snd_result r = forCells([&](std::uint32_t column, std::uint32_t row, std::uint64_t bits) {
      std::uint64_t prev = 0;
      const snd_result w = sys.LiveWriteCell(core::LiveCell{slot, row}, chunk.fourcc, column, bits, &prev, false);
      if (w == SND_OK && prev != bits && Find(slot, chunk.fourcc, column, row) == kInvalidSlot) ++newKeys;
      return w;
    });
    if (r != SND_OK) return ApplyOutcome{r, 0};
  }
  if (count_.load(std::memory_order_relaxed) + newKeys > capacity_) return ApplyOutcome{SND_ERR_LIMIT_REACHED, 0};
  ApplyOutcome out;
  for (std::uint32_t b = 0; b < nb; ++b) {
    const std::uint32_t slot = bankSlots_[b];
    forCells([&](std::uint32_t column, std::uint32_t row, std::uint64_t bits) {
      std::uint64_t prev = 0;
      if (sys.LiveWriteCell(core::LiveCell{slot, row}, chunk.fourcc, column, bits, &prev, true) == SND_OK &&
          prev != bits) {
        Remember(slot, chunk.fourcc, column, row, prev);
        ++out.cells;
      }
      return SND_OK;
    });
  }
  return out;
}

ApplyOutcome ValueApplier::RevertAll(core::System& sys) noexcept {
  ApplyOutcome out;
  if (edits_ == nullptr) return out;
  LoadBanks(sys);
  for (std::uint32_t i = 0; i < tableSize_; ++i) {
    Edit& e = edits_[i];
    if (e.used == 0u) continue;
    if (e.bankGeneration != 0u && Generation(e.bankSlot) == e.bankGeneration &&
        sys.LiveWriteCell(core::LiveCell{e.bankSlot, e.row}, e.fourcc, e.column, e.original, nullptr, true) == SND_OK) {
      ++out.cells;
    }
    e = Edit{};
  }
  count_.store(0, std::memory_order_relaxed);
  return out;
}

}  // namespace snd::livelink
