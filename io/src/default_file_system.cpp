#include "snd/io/default_file_system.hpp"

#include <atomic>
#include <cstring>

#include "snd/base/mpmc.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/semaphore.hpp"
#include "snd/platform/thread.hpp"

#if defined(__unix__) || defined(__APPLE__)
#define SND_IO_POSIX 1
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#if defined(__linux__)
#include <sys/syscall.h>
#if defined(SYS_openat2) && __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#define SND_IO_OPENAT2 1
#endif
#endif
#else
#define SND_IO_POSIX 0
#endif
#ifndef SND_IO_OPENAT2
#define SND_IO_OPENAT2 0
#endif

namespace snd::io {

namespace {

constexpr std::uint32_t kMaxRootBytes = 2048;
constexpr std::uint32_t kMaxRelBytes = 2048;
constexpr std::uint32_t kIndexBits = 12;
constexpr std::uint32_t kIndexMask = (1u << kIndexBits) - 1u;
constexpr std::uint32_t kGenMask = 0xFFFFFFFFu >> kIndexBits;

enum : std::uint32_t { kSlotQueued = 1, kSlotDone = 2, kSlotError = 3 };

struct FileEntry {
  int fd = -1;
  std::uint32_t gen = 0;
  std::uint32_t inflight = 0;  // IO thread
  std::uint64_t size = 0;
  bool used = false;
  bool closePending = false;
};

struct ReadSlot {
  // Written by the IO thread before the slot is queued (published by the queue).
  std::uint64_t token = 0;
  std::uint64_t offset = 0;
  void* dst = nullptr;
  std::uint32_t bytes = 0;
  std::uint32_t fileIndex = 0;
  int fd = -1;
  bool active = false;  // IO thread
  // Written by the reader thread.
  std::uint32_t result = 0;
  std::atomic<std::uint32_t> state{0};
  std::atomic<std::uint32_t> notifying{0};  // reader is inside the completion notify
};

}  // namespace

struct DefaultFileSystem::Impl {
  char root[kMaxRootBytes] = {};
  std::uint32_t rootLen = 0;
#if SND_IO_POSIX
  int rootFd = -1;                 // the root directory (openat2 base)
  char rootReal[PATH_MAX] = {};    // canonical root ("" if it could not be resolved)
  std::size_t rootRealLen = 0;

  // True if the canonical path `real` is the root or below it.
  bool InsideRoot(const char* real) const noexcept {
    if (rootRealLen == 0u) return false;
    if (rootRealLen == 1u) return real[0] == '/';  // root is "/"
    return std::strncmp(real, rootReal, rootRealLen) == 0 && (real[rootRealLen] == '/' || real[rootRealLen] == '\0');
  }
  // Decision b: the resolved file must lie inside the root (symlinks to files
  // or directories outside are refused, links staying inside are fine).
  // 1. Linux openat2(rootFd, path, RESOLVE_BENEATH): the kernel refuses any
  //    resolution that leaves the root (EXDEV). It also refuses ABSOLUTE
  //    symlinks even when they point inside the root, so on EXDEV, and when
  //    openat2 is unavailable (ENOSYS, EPERM under seccomp, ...), we fall back to
  // 2. open the joined path, then check the canonical path of the OPENED
  //    descriptor (/proc/self/fd/N, no race with a concurrent rename of the
  //    link) or, without /proc, realpath() of the path.
  int OpenBeneath(const char* rel, const char* full, int flags) const noexcept {
#if SND_IO_OPENAT2
    if (rootFd >= 0) {
      open_how how;
      std::memset(&how, 0, sizeof(how));
      how.flags = static_cast<std::uint64_t>(flags);
      how.resolve = RESOLVE_BENEATH;
      long r = -1;
      do {
        r = ::syscall(SYS_openat2, rootFd, rel, &how, sizeof(how));
      } while (r < 0 && errno == EINTR);
      if (r >= 0) return static_cast<int>(r);
      if (errno != EXDEV && errno != ENOSYS && errno != EPERM && errno != E2BIG && errno != EINVAL) return -1;
    }
#else
    (void)rel;
#endif
    int fd = -1;
    do {
      fd = ::open(full, flags);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return -1;
    char real[PATH_MAX];
    bool resolved = false;
#if defined(__linux__)
    char proc[64];
    std::snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    const ssize_t n = ::readlink(proc, real, sizeof(real) - 1u);
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(real) - 1u) {
      real[n] = '\0';
      resolved = true;
    }
#endif
    if (!resolved) resolved = ::realpath(full, real) != nullptr;
    if (!resolved || !InsideRoot(real)) {
      ::close(fd);
      return -1;
    }
    return fd;
  }
#endif
  DefaultFileSystemConfig cfg{};
  FileEntry* files = nullptr;
  ReadSlot* slots = nullptr;
  base::MpmcRing<std::uint32_t> queue;
  platform::Semaphore work{0};
  platform::Thread* threads = nullptr;
  std::uint32_t threadCount = 0;
  std::atomic<std::uint32_t> stop{0};
  std::atomic<CompletionNotifyFn> notify{nullptr};
  std::atomic<void*> notifyUser{nullptr};
  std::atomic<std::uint32_t> openCount{0};
  std::atomic<std::uint32_t> inflight{0};

  FileEntry* Lookup(FileId id) noexcept {
    const std::uint32_t index = (id & kIndexMask);
    if (index == 0u || index > cfg.maxOpenFiles) return nullptr;
    FileEntry& e = files[index - 1u];
    if (!e.used || e.closePending || e.gen != (id >> kIndexBits)) return nullptr;
    return &e;
  }

  void CloseEntry(FileEntry& e) noexcept {
#if SND_IO_POSIX
    if (e.fd >= 0) ::close(e.fd);
#endif
    e.fd = -1;
    e.used = false;
    e.closePending = false;
    openCount.fetch_sub(1u, std::memory_order_relaxed);
  }

  void Perform(ReadSlot& s) noexcept {
    std::uint32_t state = kSlotError;
    std::uint32_t got = 0;
#if SND_IO_POSIX
    auto* dst = static_cast<std::uint8_t*>(s.dst);
    state = kSlotDone;
    while (got < s.bytes) {
      const ssize_t r = ::pread(s.fd, dst + got, s.bytes - got, static_cast<off_t>(s.offset + got));
      if (r < 0) {
        if (errno == EINTR) continue;
        state = kSlotError;
        break;
      }
      if (r == 0) break;  // end of file
      got += static_cast<std::uint32_t>(r);
    }
#endif
    s.result = got;
    // The slot is not retired by Poll while `notifying` is set, so once the IO
    // thread has retired every read no reader is still inside the notify (its
    // target, e.g. the scheduler, may then be destroyed).
    const CompletionNotifyFn fn = notify.load(std::memory_order_acquire);
    if (fn != nullptr) s.notifying.store(1u, std::memory_order_relaxed);
    s.state.store(state, std::memory_order_release);
    if (fn != nullptr) {
      fn(notifyUser.load(std::memory_order_acquire));
      s.notifying.store(0u, std::memory_order_release);
    }
  }

  static void ReaderMain(void* p) noexcept {
    Impl& im = *static_cast<Impl*>(p);
    for (;;) {
      im.work.Wait();
      std::uint32_t index = 0;
      if (!im.queue.TryPop(index)) {
        if (im.stop.load(std::memory_order_acquire) != 0u) return;
        continue;
      }
      im.Perform(im.slots[index]);
    }
  }
};

DefaultFileSystem::~DefaultFileSystem() { Shutdown(); }

bool DefaultFileSystem::ValidatePath(const char* path) noexcept {
  if (path == nullptr || path[0] == '\0' || path[0] == '/') return false;
  std::int64_t depth = 0;
  std::size_t i = 0;
  for (;;) {
    const std::size_t start = i;
    while (path[i] != '\0' && path[i] != '/') {
      if (path[i] == '\\') return false;
      ++i;
    }
    const std::size_t len = i - start;
    if (len == 2u && path[start] == '.' && path[start + 1u] == '.') {
      if (--depth < 0) return false;
    } else if (len == 1u && path[start] == '.') {
      // current directory
    } else if (len != 0u) {
      ++depth;
    }
    if (path[i] == '\0') break;
    ++i;
    if (i >= kMaxRelBytes) return false;
  }
  return i < kMaxRelBytes;
}

bool DefaultFileSystem::Init(IAllocator& allocator, const DefaultFileSystemConfig& config) noexcept {
  Shutdown();
  if (config.maxInFlight == 0u || config.maxOpenFiles == 0u || config.maxOpenFiles > kIndexMask - 1u ||
      config.readerThreads == 0u) {
    return false;
  }
  const char* root = config.root != nullptr && config.root[0] != '\0' ? config.root : ".";
  const std::size_t rootLen = std::strlen(root);
  if (rootLen + 1u >= kMaxRootBytes) return false;

  alloc_ = &allocator;
  impl_ = base::New<Impl>(allocator, MemTag::Misc);
  if (impl_ == nullptr) {
    alloc_ = nullptr;
    return false;
  }
  Impl& im = *impl_;
  im.cfg = config;
  std::memcpy(im.root, root, rootLen + 1u);
  im.rootLen = static_cast<std::uint32_t>(rootLen);
  while (im.rootLen > 1u && im.root[im.rootLen - 1u] == '/') im.root[--im.rootLen] = '\0';
#if SND_IO_POSIX
  {
    int dflags = O_RDONLY;
#if defined(O_DIRECTORY)
    dflags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
    dflags |= O_CLOEXEC;
#endif
    im.rootFd = ::open(im.root, dflags);  // -1: a missing root makes every Open fail
    if (::realpath(im.root, im.rootReal) != nullptr) {
      im.rootRealLen = std::strlen(im.rootReal);
    } else {
      im.rootReal[0] = '\0';
      im.rootRealLen = 0;
    }
  }
#endif
  im.files = base::AllocateArray<FileEntry>(allocator, config.maxOpenFiles, MemTag::Misc);
  im.slots = base::AllocateArray<ReadSlot>(allocator, config.maxInFlight, MemTag::Misc);
  im.threads = base::AllocateArray<platform::Thread>(allocator, config.readerThreads, MemTag::Misc);
  bool ok = im.files != nullptr && im.slots != nullptr && im.threads != nullptr &&
            im.queue.Init(allocator, config.maxInFlight, MemTag::Misc);
  for (std::uint32_t i = 0; ok && i < config.readerThreads; ++i) {
    platform::ThreadDesc desc;
    desc.name = config.threadName != nullptr ? config.threadName : "snd-fsread";
    ok = im.threads[i].Start(desc, &Impl::ReaderMain, impl_);
    if (ok) im.threadCount = i + 1u;
  }
  if (!ok) {
    Shutdown();
    return false;
  }
  return true;
}

void DefaultFileSystem::Shutdown() noexcept {
  if (impl_ == nullptr) return;
  Impl& im = *impl_;
  im.stop.store(1u, std::memory_order_release);
  if (im.threadCount != 0u) im.work.Post(im.threadCount);
  for (std::uint32_t i = 0; i < im.threadCount; ++i) im.threads[i].Join();
  im.threadCount = 0;
  if (im.files != nullptr) {
    for (std::uint32_t i = 0; i < im.cfg.maxOpenFiles; ++i) {
      if (im.files[i].used) im.CloseEntry(im.files[i]);
    }
  }
#if SND_IO_POSIX
  if (im.rootFd >= 0) ::close(im.rootFd);
  im.rootFd = -1;
#endif
  IAllocator& a = *alloc_;
  base::FreeArray(a, im.files, im.cfg.maxOpenFiles, MemTag::Misc);
  base::FreeArray(a, im.slots, im.cfg.maxInFlight, MemTag::Misc);
  base::FreeArray(a, im.threads, im.cfg.readerThreads, MemTag::Misc);
  im.queue.Destroy();
  base::Delete(a, impl_, MemTag::Misc);
  impl_ = nullptr;
  alloc_ = nullptr;
}

void DefaultFileSystem::SetCompletionNotify(CompletionNotifyFn fn, void* user) noexcept {
  if (impl_ == nullptr) return;
  impl_->notifyUser.store(user, std::memory_order_release);
  impl_->notify.store(fn, std::memory_order_release);
}

FileId DefaultFileSystem::Open(const char* path) {
  SND_RT_NOTE(FileIo);
  if (impl_ == nullptr || !ValidatePath(path)) return kInvalidFile;
#if SND_IO_POSIX
  Impl& im = *impl_;
  std::uint32_t index = 0;
  while (index < im.cfg.maxOpenFiles && im.files[index].used) ++index;
  if (index == im.cfg.maxOpenFiles) return kInvalidFile;
  char full[kMaxRootBytes + kMaxRelBytes + 2u];
  const std::size_t rel = std::strlen(path);
  std::memcpy(full, im.root, im.rootLen);
  full[im.rootLen] = '/';
  std::memcpy(full + im.rootLen + 1u, path, rel + 1u);
  int flags = O_RDONLY;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
  const int fd = im.OpenBeneath(path, full, flags);
  if (fd < 0) return kInvalidFile;
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return kInvalidFile;
  }
  FileEntry& e = im.files[index];
  e.gen = (e.gen + 1u) & kGenMask;
  if (e.gen == 0u) e.gen = 1u;
  e.fd = fd;
  e.inflight = 0;
  e.size = static_cast<std::uint64_t>(st.st_size);
  e.used = true;
  e.closePending = false;
  im.openCount.fetch_add(1u, std::memory_order_relaxed);
  return (e.gen << kIndexBits) | (index + 1u);
#else
  return kInvalidFile;
#endif
}

std::uint64_t DefaultFileSystem::Size(FileId file) {
  SND_RT_NOTE(FileIo);
  if (impl_ == nullptr) return UINT64_MAX;
  FileEntry* e = impl_->Lookup(file);
  return e == nullptr ? UINT64_MAX : e->size;
}

bool DefaultFileSystem::ReadAsync(FileId file, std::uint64_t offset, void* dst, std::uint32_t bytes,
                                  IoToken token) {
  SND_RT_NOTE(FileIo);
  if (impl_ == nullptr || dst == nullptr) return false;
  Impl& im = *impl_;
  FileEntry* e = im.Lookup(file);
  if (e == nullptr) return false;
  std::uint32_t freeSlot = im.cfg.maxInFlight;
  for (std::uint32_t i = 0; i < im.cfg.maxInFlight; ++i) {
    const ReadSlot& s = im.slots[i];
    if (s.active) {
      if (s.token == token.value) return false;  // token already in flight
    } else if (freeSlot == im.cfg.maxInFlight) {
      freeSlot = i;
    }
  }
  if (freeSlot == im.cfg.maxInFlight) return false;
  ReadSlot& s = im.slots[freeSlot];
  s.token = token.value;
  s.offset = offset;
  s.dst = dst;
  s.bytes = bytes;
  s.fileIndex = (file & kIndexMask) - 1u;
  s.fd = e->fd;
  s.result = 0;
  s.active = true;
  s.state.store(kSlotQueued, std::memory_order_relaxed);
  if (!im.queue.TryPush(freeSlot)) {  // cannot happen: queue capacity >= maxInFlight
    s.active = false;
    return false;
  }
  ++e->inflight;
  im.inflight.fetch_add(1u, std::memory_order_relaxed);
  im.work.Post();
  return true;
}

IoStatus DefaultFileSystem::Poll(IoToken token) {
  SND_RT_NOTE(FileIo);
  if (impl_ == nullptr) return IoStatus{SND_IO_UNKNOWN_TOKEN, 0};
  Impl& im = *impl_;
  for (std::uint32_t i = 0; i < im.cfg.maxInFlight; ++i) {
    ReadSlot& s = im.slots[i];
    if (!s.active || s.token != token.value) continue;
    const std::uint32_t state = s.state.load(std::memory_order_acquire);
    if (state == kSlotQueued) return IoStatus{SND_IO_PENDING, 0};
    // The notify is a few atomics (and at most one wake syscall): wait it out.
    while (s.notifying.load(std::memory_order_acquire) != 0u) platform::YieldThread();
    const IoStatus result = state == kSlotDone ? IoStatus{SND_IO_DONE, s.result} : IoStatus{SND_IO_ERROR, 0};
    s.active = false;
    im.inflight.fetch_sub(1u, std::memory_order_relaxed);
    FileEntry& e = im.files[s.fileIndex];
    if (e.inflight > 0u) --e.inflight;
    if (e.inflight == 0u && e.closePending) im.CloseEntry(e);
    return result;
  }
  return IoStatus{SND_IO_UNKNOWN_TOKEN, 0};
}

void DefaultFileSystem::Close(FileId file) {
  SND_RT_NOTE(FileIo);
  if (impl_ == nullptr) return;
  FileEntry* e = impl_->Lookup(file);
  if (e == nullptr) return;
  if (e->inflight != 0u) {
    e->closePending = true;  // contract violation by the caller: close after the last read retires
    return;
  }
  impl_->CloseEntry(*e);
}

std::uint32_t DefaultFileSystem::OpenFileCount() const noexcept {
  return impl_ == nullptr ? 0u : impl_->openCount.load(std::memory_order_relaxed);
}

std::uint32_t DefaultFileSystem::InFlight() const noexcept {
  return impl_ == nullptr ? 0u : impl_->inflight.load(std::memory_order_relaxed);
}

}  // namespace snd::io
