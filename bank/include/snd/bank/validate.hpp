// Bank parsing and validation (spec/interface/bank-format.md §8).
//
// ParseBank checks a complete bank image and fills a BankView. It never reads
// outside [data, data + bytes), never allocates and never aborts on any input;
// every failure returns a code and a human-readable reason.
//
// Checks, in order (§8):
//  1. size >= 128, headerChecksum, magic, formatMajor, endianness, alignLog2,
//     headerBytes, platform (ANY_ or the runtime's), language flag/id.
//  2. fileBytes == size, checksum of [headerBytes, fileBytes).
//  3. dependency table and chunk table in range and not overlapping the header;
//     dependencies non-zero, not self, not duplicated; every chunk in range,
//     64-aligned, not overlapping another chunk or the header/tables; at most
//     one chunk per fourcc; known chunks have the table flag of their kind.
//  4. table chunks: header and column directory in range, no duplicate column
//     id, elemBytes of known columns match the type, every column array in
//     range and 64-aligned (from the chunk start), required columns present.
//  5. every row reference (other than kNone) is below the target table's row
//     count, every first/count range lies inside the target table, every blob
//     offset + length lies inside its chunk, every string offset points to a
//     NUL-terminated string inside its pool, kind-dependent references
//     (CLIP.kind, SNPV.targetKind, DUCK.kind) have a known kind, buses are in
//     graph order (parent and send targets after the bus), PHYS ranges, DSPN size.
//  6. failures are SND_ERR_CORRUPT; version / platform are SND_ERR_VERSION_MISMATCH.
//
// Unknown chunks and unknown columns are ignored (after range checks); a
// known chunk with an unknown ChunkEntry.version is ignored when it is
// optional (STRS, NAME, GUID, PTCH) and rejected otherwise (§7).
#ifndef SND_BANK_VALIDATE_HPP
#define SND_BANK_VALIDATE_HPP

#include <cstdint>

#include "snd/bank/bank_view.hpp"
#include "snd/types.h"

namespace snd::bank {

// Platform tag of the running build (bank-format.md §3).
std::uint32_t RuntimePlatform() noexcept;

struct ParseOptions {
  std::uint32_t platform = RuntimePlatform();  // banks must be ANY_ or this
};

struct ParseError {
  snd_result code = SND_OK;
  std::uint32_t chunk = 0;           // fourcc of the offending chunk, 0 = header
  std::uint32_t column = 0xFFFFFFFFu;  // column id, or 0xFFFFFFFF
  std::uint32_t row = 0xFFFFFFFFu;     // row, or 0xFFFFFFFF
  char message[192] = {};            // reason, NUL-terminated
};

// `data` must be 64-byte aligned (SND_ERR_INVALID_ARGUMENT otherwise) and stay
// valid while `out` is used. On failure `out` is reset to an empty view.
// Thread: any. Never allocates.
snd_result ParseBank(const void* data, std::uint64_t bytes, const ParseOptions& options,
                     BankView* out, ParseError* error) noexcept;

}  // namespace snd::bank

#endif  // SND_BANK_VALIDATE_HPP
