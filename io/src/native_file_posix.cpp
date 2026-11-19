// POSIX file access of DefaultFileSystem (native_file.hpp): Linux, macOS, iOS
// and other POSIX systems.
//
// Containment (decision b of D-24): the resolved file must lie inside the root
// (symlinks to files or directories outside are refused, links staying inside
// are followed).
// 1. Linux openat2(rootFd, path, RESOLVE_BENEATH): the kernel refuses any
//    resolution that leaves the root (EXDEV). It also refuses ABSOLUTE
//    symlinks even when they point inside the root, so on EXDEV, and when
//    openat2 is unavailable (ENOSYS, EPERM under seccomp, ...), we fall back to
// 2. open the canonical root joined with the path, then check the canonical
//    path of the OPENED descriptor (/proc/self/fd/N, no race with a concurrent
//    rename of the link) or, without /proc (macOS, iOS), realpath() of the
//    path, and require that this path names the opened file (same device and
//    inode, so a link swapped after the open is caught).
// The root is resolved once at Open (realpath): a relative root is relative to
// the working directory of that moment, later chdir() calls do not move it.
#include "native_file.hpp"

#if defined(__unix__) || defined(__APPLE__)

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "snd/base/allocator.hpp"

#if defined(__linux__)
#include <sys/syscall.h>
#if defined(SYS_openat2) && __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#define SND_IO_OPENAT2 1
#endif
#endif
#ifndef SND_IO_OPENAT2
#define SND_IO_OPENAT2 0
#endif

namespace snd::io::detail {

namespace {

constexpr std::size_t kMaxRelBytes = 2048;

struct PosixRoot {
  int rootFd = -1;               // the root directory (openat2 base)
  char rootReal[PATH_MAX] = {};  // canonical root ("" if it could not be resolved)
  std::size_t rootRealLen = 0;
  char full[PATH_MAX + kMaxRelBytes + 2u] = {};  // IO thread scratch: canonical root + '/' + rel

  // True if the canonical path `real` is the root or below it.
  bool InsideRoot(const char* real) const noexcept {
    if (rootRealLen == 0u) return false;
    if (rootRealLen == 1u) return real[0] == '/';  // root is "/"
    return std::strncmp(real, rootReal, rootRealLen) == 0 && (real[rootRealLen] == '/' || real[rootRealLen] == '\0');
  }

  int OpenBeneath(const char* rel, int flags) noexcept {
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
#endif
    if (rootRealLen == 0u) return -1;
    const std::size_t relLen = std::strlen(rel);
    if (rootRealLen + 1u + relLen + 1u > sizeof(full)) return -1;
    std::memcpy(full, rootReal, rootRealLen);
    std::size_t at = rootRealLen;
    if (rootRealLen != 1u) full[at++] = '/';  // root "/" already ends with the separator
    std::memcpy(full + at, rel, relLen + 1u);
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
    // The canonical path must be inside the root AND name the very file we
    // opened (same device and inode). This closes the realpath() race (a link
    // swapped between open() and realpath() resolves to a different inode) and
    // double-checks /proc. A hard link inside the root to an outside file is a
    // file of the root (it has a name there) and is accepted.
    struct stat opened, named;
    if (!resolved || !InsideRoot(real) || ::fstat(fd, &opened) != 0 || ::stat(real, &named) != 0 ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) {
      ::close(fd);
      return -1;
    }
    return fd;
  }
};

}  // namespace

bool NativeRoot::Open(IAllocator& allocator, const char* root) noexcept {
  Close();
  auto* r = base::New<PosixRoot>(allocator, MemTag::Misc);
  if (r == nullptr) return false;
  int dflags = O_RDONLY;
#if defined(O_DIRECTORY)
  dflags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
  dflags |= O_CLOEXEC;
#endif
  r->rootFd = ::open(root, dflags);  // -1: a missing root makes every Open fail
  if (r->rootFd >= 0 && ::realpath(root, r->rootReal) != nullptr) {
    r->rootRealLen = std::strlen(r->rootReal);
  } else {
    if (r->rootFd >= 0) ::close(r->rootFd);
    r->rootFd = -1;
    r->rootReal[0] = '\0';
    r->rootRealLen = 0;
  }
  impl_ = r;
  alloc_ = &allocator;
  return true;
}

void NativeRoot::Close() noexcept {
  if (impl_ == nullptr) return;
  auto* r = static_cast<PosixRoot*>(impl_);
  if (r->rootFd >= 0) ::close(r->rootFd);
  base::Delete(*alloc_, r, MemTag::Misc);
  impl_ = nullptr;
  alloc_ = nullptr;
}

bool NativeRoot::Available() const noexcept {
  return impl_ != nullptr && static_cast<const PosixRoot*>(impl_)->rootRealLen != 0u;
}

NativeFile NativeRoot::OpenBeneath(const char* rel, std::uint64_t* size) noexcept {
  if (impl_ == nullptr || rel == nullptr) return kNoNativeFile;
  auto* r = static_cast<PosixRoot*>(impl_);
  // Only regular files are opened (decision). O_NONBLOCK: opening a FIFO
  // without a writer (or some devices) would otherwise block the IO thread,
  // and the fallback opens BEFORE it can tell whether the target lies outside
  // the root. O_NOCTTY: never acquire a controlling terminal. The fstat below
  // then refuses FIFOs, devices, sockets and directories. (O_NONBLOCK has no
  // effect on reads of regular files.)
  int flags = O_RDONLY | O_NONBLOCK | O_NOCTTY;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
  const int fd = r->OpenBeneath(rel, flags);
  if (fd < 0) return kNoNativeFile;
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return kNoNativeFile;
  }
  if (size != nullptr) *size = static_cast<std::uint64_t>(st.st_size);
  return static_cast<NativeFile>(fd);
}

bool ReadAt(NativeFile file, std::uint64_t offset, void* dst, std::uint32_t bytes, std::uint32_t* got) noexcept {
  const int fd = static_cast<int>(file);
  auto* out = static_cast<std::uint8_t*>(dst);
  std::uint32_t n = 0;
  bool ok = true;
  while (n < bytes) {
    const ssize_t r = ::pread(fd, out + n, bytes - n, static_cast<off_t>(offset + n));
    if (r < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    if (r == 0) break;  // end of file
    n += static_cast<std::uint32_t>(r);
  }
  *got = n;
  return ok;
}

void CloseFile(NativeFile file) noexcept {
  if (file != kNoNativeFile) ::close(static_cast<int>(file));
}

}  // namespace snd::io::detail

#endif  // POSIX
