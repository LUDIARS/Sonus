// File reading / writing without exceptions (the library is built with
// -fno-exceptions: iostreams and the throwing std::filesystem overloads would
// terminate the process on errors such as "is a directory").
#ifndef SND_BUILDTOOL_SRC_DETAIL_FILE_IO_HPP
#define SND_BUILDTOOL_SRC_DETAIL_FILE_IO_HPP

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace snd::buildtool::detail {

// Whole regular file. false (and `why`) for missing files, directories and read errors.
inline bool ReadFileBytes(const std::filesystem::path& p, std::vector<std::uint8_t>* out, std::string* why = nullptr) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec)) {
    if (why != nullptr) *why = std::filesystem::exists(p, ec) ? "not a regular file" : "no such file";
    return false;
  }
  std::FILE* f = std::fopen(p.c_str(), "rb");
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
  std::FILE* f = std::fopen(p.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

}  // namespace snd::buildtool::detail

#endif  // SND_BUILDTOOL_SRC_DETAIL_FILE_IO_HPP
