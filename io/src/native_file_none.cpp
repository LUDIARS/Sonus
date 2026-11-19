// File access of DefaultFileSystem on systems that are neither POSIX nor Windows
// (native_file.hpp): the root is never available and every open fails.
#include "native_file.hpp"

#if !defined(__unix__) && !defined(__APPLE__) && !defined(_WIN32)

namespace snd::io::detail {

bool NativeRoot::Open(IAllocator& allocator, const char*) noexcept {
  Close();
  alloc_ = &allocator;
  return true;
}
void NativeRoot::Close() noexcept {
  impl_ = nullptr;
  alloc_ = nullptr;
}
bool NativeRoot::Available() const noexcept { return false; }
NativeFile NativeRoot::OpenBeneath(const char*, std::uint64_t*) noexcept { return kNoNativeFile; }
bool ReadAt(NativeFile, std::uint64_t, void*, std::uint32_t, std::uint32_t* got) noexcept {
  *got = 0;
  return false;
}
void CloseFile(NativeFile) noexcept {}

}  // namespace snd::io::detail

#endif
