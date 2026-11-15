// Prepared bank: what cmd::BankRegister::prepared points to (design.md 10.3,
// CONTRACTS.md §6 "BankRegister::prepared が指す構造は bank が定義").
//
// Produced on a normal worker (background lane) by PrepareBank after the bank
// image was read and validated. The audio thread (core) reads it when it
// applies BankRegister at a block boundary and swaps its reference tables to
// `view`. Ownership stays with the bank module: the memory is returned only
// after the bank was unregistered and the core's epoch says nobody references
// it any more (BankManager::Release).
//
// Layout guarantees (read-only for everybody but the bank module):
//   - `view` is a validated BankView (bank-format.md §8): every row reference,
//     range and blob offset was checked, so the audio thread can index without
//     further checks (row < view.Rows(table)).
//   - Release builds: tables point into the bank image (read-only intent).
//   - Dev override layer (11.2; BankManagerDesc::devOverlay and a bank built
//     with kFileDevBuild): every known table column is copied into one
//     writable region (`overlay`), absent optional columns are materialised
//     with their default, and BUSS / EFFX / EVNT get `devReserve` extra rows of
//     capacity. view.writable() is true and BankView::MutableColumn works.
//     Writes happen only at block boundaries through commands (livelink stage).
#ifndef SND_BANK_PREPARED_HPP
#define SND_BANK_PREPARED_HPP

#include <cstdint>

#include "snd/bank/bank_view.hpp"
#include "snd/bank/validate.hpp"
#include "snd/ext/allocator.hpp"
#include "snd/internal/common.hpp"

namespace snd::bank {

// Config.devReserve (11.2). `bytes` is a spare region per bank for structural
// edits / extra DSP state; the table copies themselves are sized from the bank.
struct DevReserve {
  std::uint32_t buses = 16;
  std::uint32_t effects = 32;
  std::uint32_t events = 64;
  std::uint64_t bytes = 1ull << 20;
};

struct DevOverlay {
  std::uint8_t* base = nullptr;   // writable region holding the table copies (MemTag::Bank)
  std::uint64_t bytes = 0;        // size of the table copies
  std::uint8_t* spare = nullptr;  // devReserve.bytes of zeroed spare memory (64-aligned)
  std::uint64_t spareBytes = 0;
  std::uint32_t reserveBuses = 0;   // extra rows of capacity in BUSS
  std::uint32_t reserveEffects = 0; // ... EFFX
  std::uint32_t reserveEvents = 0;  // ... EVNT
  std::uint32_t reserved = 0;
};

struct PreparedBank {
  static constexpr std::uint32_t kMagic = Fourcc('S', 'N', 'B', 'P');
  static constexpr std::uint32_t kVersion = 1;

  std::uint32_t magic = kMagic;
  std::uint32_t version = kVersion;
  HandleRef handle{};           // bank slot handle (as in cmd::BankRegister::bank)
  Id bankId = 0;
  std::uint32_t languageId = 0;  // 0 = not a language bank
  BankView view;                 // validated tables and blobs
  bank_format::DspPoolNeed dspNeed{};
  DevOverlay overlay;
  // Resolved dependency slots, in the bank's dependency order (filled by
  // BankManager when it decides to register; dependencyCount entries).
  std::uint32_t dependencyCount = 0;
  std::uint32_t reserved = 0;
  HandleRef* dependencies = nullptr;
};

// Checks the tag of a BankRegister::prepared pointer (nullptr if it is not a PreparedBank).
inline const PreparedBank* AsPrepared(const void* p) noexcept {
  const PreparedBank* b = static_cast<const PreparedBank*>(p);
  return (b != nullptr && b->magic == PreparedBank::kMagic && b->version == PreparedBank::kVersion) ? b
                                                                                                    : nullptr;
}

struct PrepareOptions {
  ParseOptions parse;
  bool devOverlay = false;  // build the writable override layer for kFileDevBuild banks
  DevReserve devReserve;
};

// Validates `image` (64-aligned, `bytes` long; must outlive the result) and
// builds a PreparedBank from `allocator` (MemTag::Bank). Normal workers only.
// On failure nothing stays allocated, *out is nullptr and `error` says why.
snd_result PrepareBank(const void* image, std::uint64_t bytes, const PrepareOptions& options,
                       IAllocator& allocator, PreparedBank** out, ParseError* error) noexcept;
// Frees the PreparedBank and its overlay (not the image). Null is ignored.
void DestroyPrepared(PreparedBank* bank, IAllocator& allocator) noexcept;

// Location of one writable column of the override layer (11.2).
struct WritableColumn {
  void* data = nullptr;
  std::uint32_t rows = 0;      // rows currently in use
  std::uint32_t capacity = 0;  // rows + dev reserve
  std::uint32_t elemBytes = 0;
};
// SND_ERR_UNSUPPORTED without an overlay, SND_ERR_NOT_FOUND for an unknown
// (chunk, column) or a table the bank does not carry.
snd_result FindWritableColumn(const PreparedBank& bank, std::uint32_t fourcc, std::uint16_t columnId,
                              WritableColumn* out) noexcept;

}  // namespace snd::bank

#endif  // SND_BANK_PREPARED_HPP
