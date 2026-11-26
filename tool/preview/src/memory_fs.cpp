// The preview engine's in-memory file system.
#include "memory_fs.hpp"

#include <cstring>

namespace snd::tool::preview {

void MemoryFs::Put(const std::string& path, std::vector<std::uint8_t> bytes) {
  std::lock_guard<std::mutex> g(mutex_);
  files_[path] = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
}

bool MemoryFs::Has(const std::string& path) const {
  std::lock_guard<std::mutex> g(mutex_);
  return files_.count(path) != 0u;
}

void MemoryFs::Remove(const std::string& path) {
  std::lock_guard<std::mutex> g(mutex_);
  files_.erase(path);
}

FileId MemoryFs::Open(const char* path) {
  if (path == nullptr) return kInvalidFile;
  std::lock_guard<std::mutex> g(mutex_);
  auto it = files_.find(path);
  if (it == files_.end()) return kInvalidFile;
  const FileId id = next_++;
  if (next_ == kInvalidFile) next_ = 1;
  open_[id] = it->second;
  return id;
}

std::uint64_t MemoryFs::Size(FileId file) {
  std::lock_guard<std::mutex> g(mutex_);
  auto it = open_.find(file);
  return it == open_.end() ? UINT64_MAX : it->second->size();
}

bool MemoryFs::ReadAsync(FileId file, std::uint64_t offset, void* dst, std::uint32_t bytes, IoToken token) {
  std::lock_guard<std::mutex> g(mutex_);
  auto it = open_.find(file);
  if (it == open_.end() || dst == nullptr) return false;
  IoStatus st{};
  const std::vector<std::uint8_t>& data = *it->second;
  if (offset > data.size()) {
    st.state = SND_IO_ERROR;
  } else {
    const std::uint64_t left = data.size() - offset;
    const std::uint32_t n = left < bytes ? static_cast<std::uint32_t>(left) : bytes;
    if (n != 0u) std::memcpy(dst, data.data() + offset, n);
    st.state = SND_IO_DONE;
    st.bytes = n;
  }
  done_[token.value] = st;
  return true;
}

IoStatus MemoryFs::Poll(IoToken token) {
  std::lock_guard<std::mutex> g(mutex_);
  auto it = done_.find(token.value);
  if (it == done_.end()) return IoStatus{SND_IO_UNKNOWN_TOKEN, 0};
  const IoStatus st = it->second;
  done_.erase(it);
  return st;
}

void MemoryFs::Close(FileId file) {
  std::lock_guard<std::mutex> g(mutex_);
  open_.erase(file);
}

}  // namespace snd::tool::preview
