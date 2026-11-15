// IFileSystem (design.md 6.5, 12.3). FROZEN CONTRACT.
#ifndef SND_EXT_FILE_SYSTEM_HPP
#define SND_EXT_FILE_SYSTEM_HPP

#include <cstdint>

#include "snd/ext.h"
#include "snd/ext/types.hpp"

namespace snd {

// Called from the IO thread only (one caller thread; no internal locking needed
// for calls, but completion may be produced by other threads/the OS).
struct IFileSystem {
  virtual ~IFileSystem() = default;
  // kInvalidFile on failure. UTF-8, '/'-separated path relative to the FS root.
  virtual FileId Open(const char* path) = 0;
  // Size in bytes; UINT64_MAX for an invalid id.
  virtual std::uint64_t Size(FileId file) = 0;
  // Starts an asynchronous read. false = rejected (invalid id / too many in flight); retry later.
  // dst must stay valid until Poll reports Done or Error for the token.
  virtual bool ReadAsync(FileId file, std::uint64_t offset, void* dst, std::uint32_t bytes,
                         IoToken token) = 0;
  // Done/Error are reported exactly once, then the token is retired (UnknownToken afterwards).
  virtual IoStatus Poll(IoToken token) = 0;
  // No reads may be in flight on the file.
  virtual void Close(FileId file) = 0;
};

namespace bridge {
inline snd_file_id SND_CALL FsOpen(void* u, const char* p) {
  return static_cast<IFileSystem*>(u)->Open(p);
}
inline std::uint64_t SND_CALL FsSize(void* u, snd_file_id f) {
  return static_cast<IFileSystem*>(u)->Size(f);
}
inline std::int32_t SND_CALL FsRead(void* u, snd_file_id f, std::uint64_t o, void* d,
                                    std::uint32_t b, snd_io_token t) {
  return static_cast<IFileSystem*>(u)->ReadAsync(f, o, d, b, t) ? 1 : 0;
}
inline snd_io_status SND_CALL FsPoll(void* u, snd_io_token t) {
  return static_cast<IFileSystem*>(u)->Poll(t);
}
inline void SND_CALL FsClose(void* u, snd_file_id f) { static_cast<IFileSystem*>(u)->Close(f); }
}  // namespace bridge

inline snd_file_system ToC(IFileSystem& fs) noexcept {
  return snd_file_system{&fs, &bridge::FsOpen, &bridge::FsSize, &bridge::FsRead, &bridge::FsPoll,
                         &bridge::FsClose};
}

class CFileSystem final : public IFileSystem {
 public:
  explicit CFileSystem(const snd_file_system& c) noexcept : c_(c) {}
  FileId Open(const char* path) override { return c_.open(c_.user, path); }
  std::uint64_t Size(FileId file) override { return c_.size(c_.user, file); }
  bool ReadAsync(FileId file, std::uint64_t offset, void* dst, std::uint32_t bytes,
                 IoToken token) override {
    return c_.read_async(c_.user, file, offset, dst, bytes, token) != 0;
  }
  IoStatus Poll(IoToken token) override { return c_.poll(c_.user, token); }
  void Close(FileId file) override { c_.close(c_.user, file); }

 private:
  snd_file_system c_;
};

}  // namespace snd

#endif  // SND_EXT_FILE_SYSTEM_HPP
