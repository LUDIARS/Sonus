// File reading / writing without exceptions (the library is built with
// -fno-exceptions: iostreams and the throwing std::filesystem overloads would
// terminate the process on errors such as "is a directory").
#ifndef SND_BUILDTOOL_SRC_DETAIL_FILE_IO_HPP
#define SND_BUILDTOOL_SRC_DETAIL_FILE_IO_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace snd::buildtool::detail {

// fopen for a std::filesystem::path. On Windows the native path is wide (and may hold
// characters the ANSI code page cannot), so it goes through _wfopen.
inline std::FILE* OpenFile(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
  wchar_t wmode[8] = {};
  for (std::size_t i = 0; i + 1 < sizeof(wmode) / sizeof(wmode[0]) && mode[i] != '\0'; ++i) {
    wmode[i] = static_cast<wchar_t>(mode[i]);
  }
  return ::_wfopen(p.c_str(), wmode);
#else
  return std::fopen(p.c_str(), mode);
#endif
}

// Whole regular file. false (and `why`) for missing files, directories and read errors.
inline bool ReadFileBytes(const std::filesystem::path& p, std::vector<std::uint8_t>* out, std::string* why = nullptr) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec)) {
    if (why != nullptr) *why = std::filesystem::exists(p, ec) ? "not a regular file" : "no such file";
    return false;
  }
  std::FILE* f = OpenFile(p, "rb");
  if (f == nullptr) {
    if (why != nullptr) *why = "cannot open";
    return false;
  }
  out->clear();
  std::uint8_t buf[65536];
  bool ok = true;
  for (;;) {
    const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
    out->insert(out->end(), buf, buf + n);
    if (n < sizeof(buf)) {
      ok = std::ferror(f) == 0;
      break;
    }
  }
  std::fclose(f);
  if (!ok && why != nullptr) *why = "read error";
  return ok;
}

// Writes (truncating) a file. false on any error.
inline bool WriteFileBytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
  std::FILE* f = OpenFile(p, "wb");
  if (f == nullptr) return false;
  const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

}  // namespace snd::buildtool::detail

#endif  // SND_BUILDTOOL_SRC_DETAIL_FILE_IO_HPP
