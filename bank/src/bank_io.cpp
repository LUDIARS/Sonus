#include "bank_io.hpp"

#include <cstdio>

namespace snd::bank::detail {

snd_result OpenBankFile(IFileSystem& fs, const char* path, FileId* file, std::uint64_t* size) noexcept {
  *file = fs.Open(path);
  if (*file == kInvalidFile) return SND_ERR_IO;
  *size = fs.Size(*file);
  if (*size == UINT64_MAX) {
    fs.Close(*file);
    *file = kInvalidFile;
    return SND_ERR_IO;
  }
  return SND_OK;
}

ReadStep StepRead(FileRead& r, bool stopIssuing, char* reason, std::uint32_t reasonBytes) noexcept {
  for (;;) {
    if (r.inflight) {
      const IoStatus st = r.fs->Poll(r.token);
      const IoState s = StateOf(st);
      if (s == IoState::Pending) return ReadStep::Pending;
      r.inflight = false;
      if (s != IoState::Done) {
        std::snprintf(reason, reasonBytes, "read of %u bytes at %llu failed", r.inflightBytes,
                      static_cast<unsigned long long>(r.done));
        return ReadStep::Failed;
      }
      if (st.bytes != r.inflightBytes) {
        std::snprintf(reason, reasonBytes, "short read: %u of %u bytes at %llu", st.bytes, r.inflightBytes,
                      static_cast<unsigned long long>(r.done));
        return ReadStep::Failed;
      }
      r.done += st.bytes;
    }
    if (r.done >= r.size || stopIssuing) return ReadStep::Done;
    const std::uint64_t left = r.size - r.done;
    const std::uint32_t n = left < r.chunkBytes ? static_cast<std::uint32_t>(left) : r.chunkBytes;
    r.token.value = r.tokenBase + r.seq;
    if (!r.fs->ReadAsync(r.file, r.done, r.dst + r.done, n, r.token)) return ReadStep::Pending;  // retry later
    ++r.seq;
    r.inflight = true;
    r.inflightBytes = n;
  }
}

void CloseBankFile(IFileSystem& fs, FileId file) noexcept {
  if (file != kInvalidFile) fs.Close(file);
}

namespace {
SubmitResult Submit(io::IoScheduler& io, SchedulerRead& r, SchedulerRead::Phase phase) noexcept {
  r.req.ioClass = io::IoClass::Bank;
  r.req.onComplete = nullptr;  // polled by PumpIo under the manager's mutex
  r.req.user = nullptr;
  r.cancelAsked = false;
  if (!io.Submit(r.req)) {
    r.phase = SchedulerRead::Phase::Idle;
    // Submit refuses a full file queue (retry later) or a scheduler that no
    // longer accepts work (Shutdown): only the latter is final.
    return io.Accepting() ? SubmitResult::Retry : SubmitResult::ShutDown;
  }
  r.phase = phase;
  return SubmitResult::Submitted;
}
}  // namespace

SubmitResult SubmitSizeQuery(io::IoScheduler& io, SchedulerRead& r, const char* path) noexcept {
  r.req.path = path;
  r.req.offset = 0;
  r.req.bytes = 0;
  r.req.dst = nullptr;
  return Submit(io, r, SchedulerRead::Phase::Sizing);
}

SubmitResult SubmitWholeRead(io::IoScheduler& io, SchedulerRead& r, const char* path, std::uint8_t* dst,
                             std::uint64_t bytes) noexcept {
  r.req.path = path;
  r.req.offset = 0;
  r.req.bytes = bytes;
  r.req.dst = dst;
  return Submit(io, r, SchedulerRead::Phase::Reading);
}

void CancelRead(io::IoScheduler& io, SchedulerRead& r) noexcept {
  if (!r.InFlight() || r.cancelAsked) return;
  r.cancelAsked = true;
  io.Cancel(r.req);
}

}  // namespace snd::bank::detail
