// The preview engine's file system: files held in memory (the banks the tool built, the rebuilt
// banks of a hot reload, stream files). Implements snd::IFileSystem for the IO scheduler (its IO
// thread) and the bank loader. A read completes at once (ReadAsync copies; Poll reports Done
// once). Files can be added or replaced at any time from the owner thread: an open file keeps the
// bytes it was opened with (shared), so a hot reload's new file never changes a read in flight.
#ifndef SND_TOOL_PREVIEW_MEMORY_FS_HPP
#define SND_TOOL_PREVIEW_MEMORY_FS_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "snd/ext/file_system.hpp"

namespace snd::tool::preview {

class MemoryFs final : public IFileSystem {
 public:
  void Put(const std::string& path, std::vector<std::uint8_t> bytes);
  bool Has(const std::string& path) const;
  void Remove(const std::string& path);

  FileId Open(const char* path) override;
  std::uint64_t Size(FileId file) override;
  bool ReadAsync(FileId file, std::uint64_t offset, void* dst, std::uint32_t bytes, IoToken token) override;
  IoStatus Poll(IoToken token) override;
  void Close(FileId file) override;

 private:
  using Bytes = std::shared_ptr<const std::vector<std::uint8_t>>;
  mutable std::mutex mutex_;
  std::map<std::string, Bytes> files_;
  std::map<FileId, Bytes> open_;
  std::map<std::uint64_t, IoStatus> done_;  // IoToken::value
  FileId next_ = 1;
};

}  // namespace snd::tool::preview

#endif  // SND_TOOL_PREVIEW_MEMORY_FS_HPP
