// OS file access of DefaultFileSystem (default_file_system.hpp, CONTRACTS.md D-24, D-103).
//
// DefaultFileSystem keeps the platform-independent parts (paths, the id table,
// the reader threads, Poll); this layer opens a regular file beneath the root,
// reads at an offset and closes. One implementation per OS family, chosen at
// build time:
//   native_file_posix.cpp  Linux, macOS, iOS and other POSIX systems
//   native_file_win32.cpp  Windows (Win32, UTF-16 paths)
//   native_file_none.cpp   anything else: the root never opens, every Open fails
//
// Threads: Open / Close of the root on the owner thread (DefaultFileSystem's
// Init / Shutdown); OpenBeneath on the IO thread only (it may use scratch
// memory of the root); ReadAt on the reader threads (several at once, also on
// one file); CloseFile on the IO thread.
#ifndef SND_IO_SRC_NATIVE_FILE_HPP
#define SND_IO_SRC_NATIVE_FILE_HPP

#include <cstdint>

#include "snd/ext/allocator.hpp"

namespace snd::io::detail {

// A native file: a POSIX descriptor or a Win32 HANDLE, widened to intptr_t.
using NativeFile = std::intptr_t;
inline constexpr NativeFile kNoNativeFile = -1;  // also INVALID_HANDLE_VALUE

class NativeRoot {
 public:
  NativeRoot() noexcept = default;
  ~NativeRoot() { Close(); }
  NativeRoot(const NativeRoot&) = delete;
  NativeRoot& operator=(const NativeRoot&) = delete;

  // Owner thread. `root` is a UTF-8 directory path, absolute or relative to the
  // working directory now (it is resolved here once). Returns false only on an
  // allocation failure; a root that does not exist or is not a directory is
  // remembered as unavailable (every OpenBeneath fails, Available() is false).
  bool Open(IAllocator& allocator, const char* root) noexcept;
  void Close() noexcept;
  bool Available() const noexcept;

  // IO thread. `rel` passed DefaultFileSystem::ValidatePath (relative, '/'
  // separated, no '\\' or ':', ".." never above the root). Opens it read-only if
  // the file it resolves to (after links) lies inside the root and is a regular
  // file; kNoNativeFile otherwise. *size = its size in bytes.
  NativeFile OpenBeneath(const char* rel, std::uint64_t* size) noexcept;

 private:
  void* impl_ = nullptr;
  IAllocator* alloc_ = nullptr;
};

// Reader threads. Reads up to `bytes` at `offset` (fewer only at the end of the file).
// false on a read error; *got = bytes read before it.
bool ReadAt(NativeFile file, std::uint64_t offset, void* dst, std::uint32_t bytes, std::uint32_t* got) noexcept;
void CloseFile(NativeFile file) noexcept;

}  // namespace snd::io::detail

#endif  // SND_IO_SRC_NATIVE_FILE_HPP
