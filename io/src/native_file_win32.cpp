// Win32 file access of DefaultFileSystem (native_file.hpp, CONTRACTS.md D-103).
//
// Paths are UTF-8 at the interface and UTF-16 for Win32 (MultiByteToWideChar,
// invalid UTF-8 is refused). The root is made absolute once at Open
// (GetFullPathNameW: a relative root is relative to the working directory of
// that moment), opened as a directory handle and resolved to its final path
// (GetFinalPathNameByHandleW, links and junctions followed, "\\?\" form).
//
// OpenBeneath:
//   - the relative path is normalised lexically ("." and empty components
//     dropped, ".." removes the previous component; ValidatePath already
//     guaranteed it never climbs above the root) and joined to the root's
//     final path with '\\'. The "\\?\" form passes the name to the file system
//     unchanged: no ".." processing, no DOS device names (CON, NUL, ...), no
//     MAX_PATH limit, no stripping of trailing dots or spaces;
//   - CreateFileW(GENERIC_READ, share read / write / delete, OPEN_EXISTING).
//     Directories do not open without FILE_FLAG_BACKUP_SEMANTICS; the handle
//     must be a disk file (GetFileType) that is not a directory;
//   - containment: the final path of the OPENED handle (symbolic links and
//     junctions resolved by the OS, so no race with a link swapped later) must
//     be the root's final path followed by '\\' (compared without case, as
//     NTFS names are by default). Links leaving the root are refused, links
//     staying inside are followed.
// ReadAt: ReadFile with an OVERLAPPED offset on a synchronous handle (a
// positional read; concurrent reads on one handle are serialised by the OS).
#include "native_file.hpp"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <cwchar>

#include "snd/base/allocator.hpp"

namespace snd::io::detail {

namespace {

// UTF-16 units: root (<= 2048 UTF-8 bytes) + "\\?\UNC\" + '\\' + relative path
// (<= 2048 bytes). A final path longer than this lies outside the root anyway.
constexpr DWORD kMaxWide = 4352;

struct WinRoot {
  HANDLE dir = INVALID_HANDLE_VALUE;
  wchar_t finalPath[kMaxWide] = {};  // root's final path, no trailing '\\'
  DWORD finalLen = 0;            // 0: unavailable
  // IO thread scratch.
  wchar_t rel[kMaxWide] = {};
  wchar_t full[kMaxWide] = {};
  wchar_t opened[kMaxWide] = {};
};

bool Utf8ToWide(const char* in, wchar_t* out, DWORD outChars) noexcept {
  out[0] = L'\0';
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, out, static_cast<int>(outChars));
  if (n <= 0) {
    out[0] = L'\0';
    return false;
  }
  return true;
}

// Final path of `h` into out (no trailing '\\' except for a drive root). 0 on failure / too long.
DWORD FinalPath(HANDLE h, wchar_t* out, DWORD outChars) noexcept {
  const DWORD n = GetFinalPathNameByHandleW(h, out, outChars, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0u || n >= outChars) return 0u;
  return n;
}

bool SameText(const wchar_t* a, const wchar_t* b, DWORD n) noexcept {
  return CompareStringOrdinal(a, static_cast<int>(n), b, static_cast<int>(n), TRUE) == CSTR_EQUAL;
}

// Lexical normalisation of a ValidatePath-approved UTF-16 relative path into
// '\\'-separated components. false if it ends up empty or above the root.
bool NormaliseRel(const wchar_t* in, wchar_t* out, DWORD outChars) noexcept {
  DWORD len = 0;
  DWORD i = 0;
  for (;;) {
    const DWORD start = i;
    while (in[i] != L'\0' && in[i] != L'/') ++i;
    const DWORD n = i - start;
    if (n == 2u && in[start] == L'.' && in[start + 1u] == L'.') {
      if (len == 0u) return false;
      while (len > 0u && out[len - 1u] != L'\\') --len;  // drop the last component
      if (len > 0u) --len;                                // and its separator
    } else if (n != 0u && !(n == 1u && in[start] == L'.')) {
      if (len + (len != 0u ? 1u : 0u) + n + 1u > outChars) return false;
      if (len != 0u) out[len++] = L'\\';
      std::memcpy(out + len, in + start, sizeof(wchar_t) * n);
      len += n;
    }
    if (in[i] == L'\0') break;
    ++i;
  }
  out[len] = L'\0';
  return len != 0u;
}

}  // namespace

bool NativeRoot::Open(IAllocator& allocator, const char* root) noexcept {
  Close();
  auto* r = base::New<WinRoot>(allocator, MemTag::Misc);
  if (r == nullptr) return false;
  impl_ = r;
  alloc_ = &allocator;
  wchar_t* given = r->rel;  // scratch, not used yet
  if (!Utf8ToWide(root, given, kMaxWide)) return true;  // unavailable
  for (DWORD i = 0; given[i] != L'\0'; ++i) {
    if (given[i] == L'/') given[i] = L'\\';
  }
  const DWORD n = GetFullPathNameW(given, kMaxWide, r->full, nullptr);
  if (n == 0u || n >= kMaxWide) return true;
  r->dir = CreateFileW(r->full, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (r->dir == INVALID_HANDLE_VALUE) return true;
  BY_HANDLE_FILE_INFORMATION info;
  if (GetFileInformationByHandle(r->dir, &info) == 0 || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
    CloseHandle(r->dir);
    r->dir = INVALID_HANDLE_VALUE;
    return true;
  }
  DWORD len = FinalPath(r->dir, r->finalPath, kMaxWide);
  while (len > 0u && r->finalPath[len - 1u] == L'\\') r->finalPath[--len] = L'\0';  // "\\?\C:\" -> "\\?\C:"
  r->finalLen = len;
  return true;
}

void NativeRoot::Close() noexcept {
  if (impl_ == nullptr) return;
  auto* r = static_cast<WinRoot*>(impl_);
  if (r->dir != INVALID_HANDLE_VALUE) CloseHandle(r->dir);
  base::Delete(*alloc_, r, MemTag::Misc);
  impl_ = nullptr;
  alloc_ = nullptr;
}

bool NativeRoot::Available() const noexcept {
  return impl_ != nullptr && static_cast<const WinRoot*>(impl_)->finalLen != 0u;
}

NativeFile NativeRoot::OpenBeneath(const char* rel, std::uint64_t* size) noexcept {
  if (impl_ == nullptr || rel == nullptr) return kNoNativeFile;
  auto* r = static_cast<WinRoot*>(impl_);
  if (r->finalLen == 0u) return kNoNativeFile;
  if (!Utf8ToWide(rel, r->opened, kMaxWide)) return kNoNativeFile;
  if (!NormaliseRel(r->opened, r->rel, kMaxWide)) return kNoNativeFile;
  const DWORD relLen = static_cast<DWORD>(wcslen(r->rel));
  if (r->finalLen + 1u + relLen + 1u > kMaxWide) return kNoNativeFile;
  std::memcpy(r->full, r->finalPath, sizeof(wchar_t) * r->finalLen);
  r->full[r->finalLen] = L'\\';
  std::memcpy(r->full + r->finalLen + 1u, r->rel, sizeof(wchar_t) * (relLen + 1u));
  HANDLE h = CreateFileW(r->full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return kNoNativeFile;
  BY_HANDLE_FILE_INFORMATION info;
  LARGE_INTEGER bytes;
  bool ok = GetFileType(h) == FILE_TYPE_DISK && GetFileInformationByHandle(h, &info) != 0 &&
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u && GetFileSizeEx(h, &bytes) != 0 &&
            bytes.QuadPart >= 0;
  if (ok) {
    const DWORD n = FinalPath(h, r->opened, kMaxWide);
    ok = n > r->finalLen + 1u && r->opened[r->finalLen] == L'\\' && SameText(r->opened, r->finalPath, r->finalLen);
  }
  if (!ok) {
    CloseHandle(h);
    return kNoNativeFile;
  }
  if (size != nullptr) *size = static_cast<std::uint64_t>(bytes.QuadPart);
  return reinterpret_cast<NativeFile>(h);
}

bool ReadAt(NativeFile file, std::uint64_t offset, void* dst, std::uint32_t bytes, std::uint32_t* got) noexcept {
  HANDLE h = reinterpret_cast<HANDLE>(file);
  auto* out = static_cast<std::uint8_t*>(dst);
  std::uint32_t n = 0;
  bool ok = true;
  while (n < bytes) {
    OVERLAPPED ov;
    std::memset(&ov, 0, sizeof(ov));
    const std::uint64_t at = offset + n;
    ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>(at >> 32);
    DWORD r = 0;
    if (ReadFile(h, out + n, static_cast<DWORD>(bytes - n), &r, &ov) == 0) {
      if (GetLastError() != ERROR_HANDLE_EOF) ok = false;
      break;
    }
    if (r == 0u) break;  // end of file
    n += static_cast<std::uint32_t>(r);
  }
  *got = n;
  return ok;
}

void CloseFile(NativeFile file) noexcept {
  if (file != kNoNativeFile) CloseHandle(reinterpret_cast<HANDLE>(file));
}

}  // namespace snd::io::detail

#endif  // _WIN32
