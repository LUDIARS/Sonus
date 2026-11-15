#include "snd/bank/prepared.hpp"

#include <cstring>
#include <new>

#include "snd/base/atomic.hpp"
#include "view_access.hpp"

namespace snd::bank {
namespace {

std::uint32_t ExtraRows(std::uint32_t fourcc, const DevReserve& r) noexcept {
  if (fourcc == bf::chunk::BUSS) return r.buses;
  if (fourcc == bf::chunk::EFFX) return r.effects;
  if (fourcc == bf::chunk::EVNT) return r.events;
  return 0;
}

void FillDefault(std::uint8_t* dst, std::uint64_t rows, const bf::ColumnInfo& info) noexcept {
  switch (info.type) {
    case bf::ColumnType::U32: {
      const std::uint32_t v = static_cast<std::uint32_t>(info.defaultValue);
      for (std::uint64_t i = 0; i < rows; ++i) std::memcpy(dst + i * 4, &v, 4);
      break;
    }
    case bf::ColumnType::F32: {
      const float v = static_cast<float>(info.defaultValue);
      for (std::uint64_t i = 0; i < rows; ++i) std::memcpy(dst + i * 4, &v, 4);
      break;
    }
    case bf::ColumnType::U64: {
      const std::uint64_t v = static_cast<std::uint64_t>(info.defaultValue);
      for (std::uint64_t i = 0; i < rows; ++i) std::memcpy(dst + i * 8, &v, 8);
      break;
    }
    case bf::ColumnType::I64: {
      const std::int64_t v = static_cast<std::int64_t>(info.defaultValue);
      for (std::uint64_t i = 0; i < rows; ++i) std::memcpy(dst + i * 8, &v, 8);
      break;
    }
  }
}

snd_result BuildOverlay(PreparedBank* pb, const DevReserve& reserve, IAllocator& allocator) noexcept {
  TableView* tables = ViewAccess::Tables(pb->view);
  const ChunkRef* chunks = ViewAccess::Chunks(pb->view);
  std::uint64_t total = 0;
  std::uint32_t capacity[kKnownChunkCount] = {};
  bool include[kKnownChunkCount] = {};
  for (std::uint32_t i = 0; i < kKnownChunkCount; ++i) {
    if (bf::kChunks[i].kind != bf::ChunkKind::Table) continue;
    const std::uint32_t extra = ExtraRows(bf::kChunks[i].fourcc, reserve);
    if (chunks[i].data == nullptr && extra == 0) continue;
    const std::uint64_t cap = std::uint64_t{tables[i].rows} + extra;
    if (cap > 0xFFFFFFFFull) return SND_ERR_OUT_OF_BUDGET;
    include[i] = true;
    capacity[i] = static_cast<std::uint32_t>(cap);
    for (const bf::ColumnInfo& c : bf::kColumns) {
      if (c.chunk == bf::kChunks[i].fourcc) total += base::AlignUp64(cap * bf::ColumnTypeBytes(c.type), 64);
    }
  }
  const std::uint64_t spare = base::AlignUp64(reserve.bytes, 64);
  const std::uint64_t all = total + spare;
  if (all == 0) return SND_OK;
  if (all > static_cast<std::uint64_t>(SIZE_MAX)) return SND_ERR_OUT_OF_BUDGET;
  auto* base = static_cast<std::uint8_t*>(allocator.Allocate(static_cast<std::size_t>(all), 64, MemTag::Bank));
  if (base == nullptr) return SND_ERR_OUT_OF_BUDGET;
  std::uint64_t at = 0;
  for (std::uint32_t i = 0; i < kKnownChunkCount; ++i) {
    if (!include[i]) continue;
    TableView& t = tables[i];
    for (const bf::ColumnInfo& c : bf::kColumns) {
      if (c.chunk != bf::kChunks[i].fourcc) continue;
      const std::uint32_t eb = bf::ColumnTypeBytes(c.type);
      std::uint8_t* dst = base + at;
      std::uint64_t filled = 0;
      if (t.columns[c.columnId] != nullptr) {
        std::memcpy(dst, t.columns[c.columnId], std::uint64_t{t.rows} * eb);
        filled = t.rows;
      }
      FillDefault(dst + filled * eb, capacity[i] - filled, c);
      const std::uint64_t used = std::uint64_t{capacity[i]} * eb;
      std::memset(dst + used, 0, base::AlignUp64(used, 64) - used);
      t.columns[c.columnId] = dst;
      at += base::AlignUp64(used, 64);
    }
    t.capacity = capacity[i];
  }
  std::memset(base + total, 0, spare);
  pb->overlay.base = base;
  pb->overlay.bytes = total;
  pb->overlay.spare = spare > 0 ? base + total : nullptr;
  pb->overlay.spareBytes = spare;
  pb->overlay.reserveBuses = reserve.buses;
  pb->overlay.reserveEffects = reserve.effects;
  pb->overlay.reserveEvents = reserve.events;
  ViewAccess::Writable(pb->view) = true;
  return SND_OK;
}

}  // namespace

snd_result PrepareBank(const void* image, std::uint64_t bytes, const PrepareOptions& options,
                       IAllocator& allocator, PreparedBank** out, ParseError* error) noexcept {
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  *out = nullptr;
  void* mem = allocator.Allocate(sizeof(PreparedBank), 64, MemTag::Bank);
  if (mem == nullptr) {
    if (error != nullptr) {
      *error = ParseError{};
      error->code = SND_ERR_OUT_OF_BUDGET;
      std::strncpy(error->message, "allocation of the prepared bank failed", sizeof(error->message) - 1);
    }
    return SND_ERR_OUT_OF_BUDGET;
  }
  auto* pb = new (mem) PreparedBank();
  snd_result r = ParseBank(image, bytes, options.parse, &pb->view, error);
  if (r == SND_OK) {
    pb->bankId = pb->view.bankId();
    pb->languageId = pb->view.languageId();
    pb->dspNeed = pb->view.DspNeed();
    pb->builtDspNeed = pb->dspNeed;
    pb->dependencyCount = pb->view.dependencyCount();
    if (pb->dependencyCount > 0) {
      void* deps = allocator.Allocate(sizeof(HandleRef) * pb->dependencyCount, 16, MemTag::Bank);
      if (deps == nullptr) {
        r = SND_ERR_OUT_OF_BUDGET;
      } else {
        pb->dependencies = static_cast<HandleRef*>(deps);
        for (std::uint32_t i = 0; i < pb->dependencyCount; ++i) pb->dependencies[i] = HandleRef{0, 0};
      }
    }
  }
  if (r == SND_OK && options.devOverlay && pb->view.devBuild()) {
    r = BuildOverlay(pb, options.devReserve, allocator);
  }
  if (r != SND_OK) {
    if (error != nullptr && error->code == SND_OK) {
      error->code = r;
      std::strncpy(error->message, "out of memory while preparing the bank", sizeof(error->message) - 1);
    }
    DestroyPrepared(pb, allocator);
    return r;
  }
  *out = pb;
  return SND_OK;
}

void DestroyPrepared(PreparedBank* bank, IAllocator& allocator) noexcept {
  if (bank == nullptr) return;
  if (bank->overlay.base != nullptr) allocator.Free(bank->overlay.base, MemTag::Bank);
  if (bank->dependencies != nullptr) allocator.Free(bank->dependencies, MemTag::Bank);
  bank->magic = 0;
  bank->~PreparedBank();
  allocator.Free(bank, MemTag::Bank);
}

snd_result FindWritableColumn(const PreparedBank& bank, std::uint32_t fourcc, std::uint16_t columnId,
                              WritableColumn* out) noexcept {
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  *out = WritableColumn{};
  if (!bank.view.writable()) return SND_ERR_UNSUPPORTED;
  const bf::ColumnInfo* info = FindColumnInfo(fourcc, columnId);
  const TableView* t = bank.view.Table(fourcc);
  if (info == nullptr || t == nullptr || t->columns[columnId] == nullptr) return SND_ERR_NOT_FOUND;
  out->data = t->columns[columnId];
  out->rows = t->rows;
  out->capacity = t->capacity;
  out->elemBytes = bf::ColumnTypeBytes(info->type);
  return SND_OK;
}

}  // namespace snd::bank
