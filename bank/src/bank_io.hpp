// Bank file IO (design.md 10.3), both paths of BankManagerDesc::ioMode:
//  - Scheduler (default, stage 1.5): io::FileRequest through the io module's
//    IoScheduler (SchedulerRead, SubmitSizeQuery / SubmitWholeRead);
//  - DirectFileSystem: the IFileSystem calls (OpenBankFile / StepRead /
//    CloseBankFile), the only place the bank module calls IFileSystem.
#ifndef SND_BANK_SRC_BANK_IO_HPP
#define SND_BANK_SRC_BANK_IO_HPP

#include <cstdint>

#include "snd/ext/file_system.hpp"
#include "snd/io/io_scheduler.hpp"
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

// --- Scheduler path -------------------------------------------------------------------
// One FileRequest per slot, reused: first a size query, then one whole-file read
// into the image (the scheduler splits it into filePieceBytes pieces).
struct SchedulerRead {
  enum class Phase : std::uint8_t { Idle = 0, Sizing = 1, Reading = 2 };
  io::FileRequest req;
  Phase phase = Phase::Idle;
  bool cancelAsked = false;
  // A submitted request that has not reached its final state: the scheduler may
  // still read the path and write dst.
  bool InFlight() const noexcept { return phase != Phase::Idle && !req.Finished(); }
  bool Finished() const noexcept { return phase != Phase::Idle && req.Finished(); }
};

enum class SubmitResult { Submitted, Retry, ShutDown };

// Size query of `path` (no read, no bandwidth). `path` must stay valid until Finished().
SubmitResult SubmitSizeQuery(io::IoScheduler& io, SchedulerRead& r, const char* path) noexcept;
// Reads [0, bytes) of `path` into dst (must stay valid until Finished()).
SubmitResult SubmitWholeRead(io::IoScheduler& io, SchedulerRead& r, const char* path, std::uint8_t* dst,
                             std::uint64_t bytes) noexcept;
// Asks once to cancel a request in flight (it still ends through the normal path).
void CancelRead(io::IoScheduler& io, SchedulerRead& r) noexcept;

}  // namespace snd::bank::detail

#endif  // SND_BANK_SRC_BANK_IO_HPP
