// Default IFileSystem: OS files under a root directory (design.md 6.5, D-24).
//
// Asynchrony: ReadAsync only queues the read; a small pool of reader threads
// (DefaultFileSystemConfig::readerThreads, default 1) performs it with a
// positional read (POSIX pread) and publishes the result with an atomic store.
// Poll() is a non-blocking look at that result. An optional completion
// callback (SetCompletionNotify) is called on the reader thread after each
// read so the IO thread can be woken instead of polling.
//
// Contract (snd/ext/file_system.hpp, CONTRACTS.md D-24):
//   - every IFileSystem member is called from ONE thread (the IO thread) and
//     notes SND_RT_NOTE(FileIo) first;
//   - Poll reports Done / Error exactly once, then the token is retired
//     (UnknownToken afterwards);
//   - ReadAsync returns false (rejected, retry later) for an invalid id, a
//     null dst, a token already in flight, or when maxInFlight reads are queued
//     or running;
//   - Close with reads in flight is a caller error; the descriptor is then
//     closed when the last of those reads is retired by Poll (never under a
//     running read).
// Paths: UTF-8, '/'-separated, relative to the root. Rejected (Open returns
// kInvalidFile): empty paths, absolute paths ('/...'), backslashes, NUL-free
// paths longer than the internal limit, and any path whose ".." components
// would leave the root. "." and empty components ("a//b") are ignored.
// Symbolic links: a path whose RESOLVED file lies outside the root is refused
// (links to files or directories outside, absolute or relative); links that
// stay inside the root are followed. Linux uses openat2(RESOLVE_BENEATH) and,
// where that is unavailable or refuses an absolute in-root link, checks the
// canonical path of the opened descriptor (/proc/self/fd) or realpath(), and
// requires that this path names the opened file (same device and inode, so a
// link swapped after the open is caught). A root that does not exist at Init
// makes every Open fail.
// Only regular files are opened: FIFOs, devices, sockets and directories are
// refused. Files are opened with O_NONBLOCK | O_NOCTTY, so Open never blocks
// on a special file, inside the root or behind a link (it opens, sees with
// fstat that it is not a regular file, and closes).
//
// Platforms: POSIX (Linux, macOS, ...) is implemented. Other platforms build a
// stub whose Open always fails.
//
// Init / Shutdown / SetCompletionNotify are non-RT owner-thread calls, made
// while the IO thread is not using the file system.
#ifndef SND_IO_DEFAULT_FILE_SYSTEM_HPP
#define SND_IO_DEFAULT_FILE_SYSTEM_HPP

#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/ext/file_system.hpp"
#include "snd/ext/types.hpp"

namespace snd::io {

struct DefaultFileSystemConfig {
  const char* root = ".";            // copied at Init; no trailing '/' needed
  std::uint32_t maxInFlight = 16;    // reads queued or running at once
  std::uint32_t maxOpenFiles = 64;   // open descriptors at once (<= 4095)
  std::uint32_t readerThreads = 1;   // threads performing reads (>= 1)
  const char* threadName = "snd-fsread";
};

using CompletionNotifyFn = void (*)(void* user) noexcept;

class DefaultFileSystem final : public IFileSystem {
 public:
  DefaultFileSystem() noexcept = default;
  ~DefaultFileSystem() override;
  DefaultFileSystem(const DefaultFileSystem&) = delete;
  DefaultFileSystem& operator=(const DefaultFileSystem&) = delete;

  // Non-RT. Allocates (MemTag::Misc) and starts the reader threads. false on
  // failure (bad config, root too long, allocation or thread failure).
  bool Init(IAllocator& allocator, const DefaultFileSystemConfig& config) noexcept;
  // Non-RT. Finishes the reads already queued (results are discarded), stops
  // the readers, closes every descriptor and frees memory. Idempotent.
  void Shutdown() noexcept;
  // Non-RT, before reads start. fn(user) runs on a reader thread after every
  // completed read (e.g. IoScheduler::WakeThunk). nullptr disables it. A read
  // is not reported Done/Error by Poll until its notify call has returned, so
  // once the caller has retired all its reads, `user` may be destroyed.
  void SetCompletionNotify(CompletionNotifyFn fn, void* user) noexcept;

  // Pure. True if `path` is acceptable (see header comment).
  static bool ValidatePath(const char* path) noexcept;

  // IFileSystem (IO thread only).
  FileId Open(const char* path) override;
  std::uint64_t Size(FileId file) override;
  bool ReadAsync(FileId file, std::uint64_t offset, void* dst, std::uint32_t bytes, IoToken token) override;
  IoStatus Poll(IoToken token) override;
  void Close(FileId file) override;

  // Status (any thread, approximate).
  std::uint32_t OpenFileCount() const noexcept;
  std::uint32_t InFlight() const noexcept;

  struct Impl;  // private (io/src)

 private:
  Impl* impl_ = nullptr;
  IAllocator* alloc_ = nullptr;
};

}  // namespace snd::io

#endif  // SND_IO_DEFAULT_FILE_SYSTEM_HPP
