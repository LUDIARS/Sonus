// In-memory file system with latency and failure injection (design.md 12.2-6, 14).
//
// Implements snd::IFileSystem. Reads complete in Poll(): a read issued with
// latency L returns Pending for the first L polls of its token and completes
// (copying the data) on poll L+1. Held paths stay Pending until Release().
// Every IFileSystem entry point notes FileIo for RT-safety detection.
//
// Thread safety: all members are internally synchronised (the IO thread polls
// while the test thread injects faults).
#ifndef SND_MOCKS_MEMORY_FILE_SYSTEM_HPP
#define SND_MOCKS_MEMORY_FILE_SYSTEM_HPP

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "snd/ext/file_system.hpp"

namespace snd::mock {

class MemoryFileSystem final : public IFileSystem {
 public:
  MemoryFileSystem();
  ~MemoryFileSystem() override;

  // --- content -----------------------------------------------------------------
  void AddFile(const std::string& path, std::vector<std::uint8_t> data);
  void AddFile(const std::string& path, const void* data, std::size_t bytes);
  void RemoveFile(const std::string& path);  // open ids keep their data
  bool HasFile(const std::string& path) const;

  // --- delay injection -----------------------------------------------------------
  void SetDefaultLatency(std::uint32_t polls);                    // default 0
  void SetLatency(const std::string& path, std::uint32_t polls);  // per path
  void Hold(const std::string& path);     // reads stay Pending ...
  void Release(const std::string& path);  // ... until released
  void SetMaxInFlight(std::uint32_t n);   // ReadAsync rejects beyond n (0 = unlimited)

  // --- failure injection ---------------------------------------------------------
  void FailOpen(const std::string& path, bool fail = true);
  // Reads of `path` overlapping [begin, end) complete with Error.
  void FailReads(const std::string& path, std::uint64_t begin = 0, std::uint64_t end = UINT64_MAX);
  // The n-th ReadAsync from now (0 = next) completes with Error.
  void FailNthRead(std::uint32_t n);
  void ClearFailures();

  // --- statistics ------------------------------------------------------------------
  std::uint32_t OpenCalls() const;
  std::uint32_t ReadCalls() const;
  std::uint64_t BytesRead() const;
  std::uint32_t InFlight() const;
  std::uint32_t OpenFiles() const;

  // --- IFileSystem (IO thread) -------------------------------------------------------
  FileId Open(const char* path) override;
  std::uint64_t Size(FileId file) override;
  bool ReadAsync(FileId file, std::uint64_t offset, void* dst, std::uint32_t bytes,
                 IoToken token) override;
  IoStatus Poll(IoToken token) override;
  void Close(FileId file) override;

 private:
  struct FileData {
    std::vector<std::uint8_t> bytes;
  };
  struct OpenFile {
    std::string path;
    std::vector<std::uint8_t> bytes;  // snapshot at Open
  };
  struct Read {
    FileId file;
    std::uint64_t offset;
    void* dst;
    std::uint32_t bytes;
    std::uint32_t pollsLeft;
    bool fail;
    std::string path;
  };
  struct FailRange {
    std::string path;
    std::uint64_t begin, end;
  };

  mutable std::mutex mutex_;
  std::map<std::string, FileData> files_;
  std::map<FileId, OpenFile> open_;
  std::map<std::uint64_t, Read> reads_;
  std::map<std::string, std::uint32_t> latency_;
  std::map<std::string, bool> held_;
  std::map<std::string, bool> failOpen_;
  std::vector<FailRange> failRanges_;
  std::uint32_t defaultLatency_ = 0;
  std::uint32_t maxInFlight_ = 0;
  std::int64_t failNth_ = -1;
  FileId nextId_ = 1;
  std::uint32_t openCalls_ = 0;
  std::uint32_t readCalls_ = 0;
  std::uint64_t bytesRead_ = 0;
};

}  // namespace snd::mock

#endif  // SND_MOCKS_MEMORY_FILE_SYSTEM_HPP
