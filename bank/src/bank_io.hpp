// The single place where the bank module calls IFileSystem (design.md 10.3).
// The io module's scheduler (priorities, bandwidth) replaces this in the next
// stage; BankManager only sees OpenBankFile / StepRead / CloseBankFile.
#ifndef SND_BANK_SRC_BANK_IO_HPP
#define SND_BANK_SRC_BANK_IO_HPP

#include <cstdint>

#include "snd/ext/file_system.hpp"
#include "snd/types.h"

namespace snd::bank::detail {

struct FileRead {
  IFileSystem* fs = nullptr;
  FileId file = kInvalidFile;
  std::uint8_t* dst = nullptr;
  std::uint64_t size = 0;
  std::uint64_t done = 0;
  std::uint32_t chunkBytes = 1u << 20;
  std::uint32_t inflightBytes = 0;
  bool inflight = false;
  IoToken token{};
  std::uint64_t tokenBase = 0;
  std::uint32_t seq = 0;
};

enum class ReadStep { Pending, Done, Failed };

// Opens `path` and returns its size. SND_ERR_IO if it cannot be opened / sized.
snd_result OpenBankFile(IFileSystem& fs, const char* path, FileId* file, std::uint64_t* size) noexcept;
// Polls the read in flight and issues the next one. With `stopIssuing`, only
// waits for the read in flight (cancellation: dst must stay valid until then).
ReadStep StepRead(FileRead& r, bool stopIssuing, char* reason, std::uint32_t reasonBytes) noexcept;
void CloseBankFile(IFileSystem& fs, FileId file) noexcept;

}  // namespace snd::bank::detail

#endif  // SND_BANK_SRC_BANK_IO_HPP
