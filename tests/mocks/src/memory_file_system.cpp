#include "snd/mocks/memory_file_system.hpp"

#include <cstring>
#include <utility>

#include "snd/internal/rt_check.hpp"

namespace snd::mock {

MemoryFileSystem::MemoryFileSystem() = default;
MemoryFileSystem::~MemoryFileSystem() = default;

void MemoryFileSystem::AddFile(const std::string& path, std::vector<std::uint8_t> data) {
  std::lock_guard<std::mutex> lock(mutex_);
  files_[path].bytes = std::move(data);
}

void MemoryFileSystem::AddFile(const std::string& path, const void* data, std::size_t bytes) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  AddFile(path, std::vector<std::uint8_t>(p, p + bytes));
}

void MemoryFileSystem::RemoveFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  files_.erase(path);
}

bool MemoryFileSystem::HasFile(const std::string& path) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return files_.count(path) != 0;
}

void MemoryFileSystem::SetDefaultLatency(std::uint32_t polls) {
  std::lock_guard<std::mutex> lock(mutex_);
  defaultLatency_ = polls;
}
void MemoryFileSystem::SetLatency(const std::string& path, std::uint32_t polls) {
  std::lock_guard<std::mutex> lock(mutex_);
  latency_[path] = polls;
}
void MemoryFileSystem::Hold(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  held_[path] = true;
}
void MemoryFileSystem::Release(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  held_.erase(path);
}
void MemoryFileSystem::SetMaxInFlight(std::uint32_t n) {
  std::lock_guard<std::mutex> lock(mutex_);
  maxInFlight_ = n;
}
void MemoryFileSystem::FailOpen(const std::string& path, bool fail) {
  std::lock_guard<std::mutex> lock(mutex_);
  failOpen_[path] = fail;
}
void MemoryFileSystem::FailReads(const std::string& path, std::uint64_t begin, std::uint64_t end) {
  std::lock_guard<std::mutex> lock(mutex_);
  failRanges_.push_back(FailRange{path, begin, end});
}
void MemoryFileSystem::FailNthRead(std::uint32_t n) {
  std::lock_guard<std::mutex> lock(mutex_);
  failNth_ = static_cast<std::int64_t>(n);
}
void MemoryFileSystem::ClearFailures() {
  std::lock_guard<std::mutex> lock(mutex_);
  failOpen_.clear();
  failRanges_.clear();
  failNth_ = -1;
}

std::uint32_t MemoryFileSystem::OpenCalls() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return openCalls_;
}
std::uint32_t MemoryFileSystem::ReadCalls() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return readCalls_;
}
std::uint64_t MemoryFileSystem::BytesRead() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bytesRead_;
}
std::uint32_t MemoryFileSystem::InFlight() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<std::uint32_t>(reads_.size());
}
std::uint32_t MemoryFileSystem::OpenFiles() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<std::uint32_t>(open_.size());
}

FileId MemoryFileSystem::Open(const char* path) {
  SND_RT_NOTE(FileIo);
  std::lock_guard<std::mutex> lock(mutex_);
  ++openCalls_;
  if (path == nullptr) return kInvalidFile;
  auto f = files_.find(path);
  if (f == files_.end()) return kInvalidFile;
  auto fo = failOpen_.find(path);
  if (fo != failOpen_.end() && fo->second) return kInvalidFile;
  const FileId id = nextId_++;
  open_[id] = OpenFile{path, f->second.bytes};
  return id;
}

std::uint64_t MemoryFileSystem::Size(FileId file) {
  SND_RT_NOTE(FileIo);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = open_.find(file);
  return it == open_.end() ? UINT64_MAX : it->second.bytes.size();
}

bool MemoryFileSystem::ReadAsync(FileId file, std::uint64_t offset, void* dst, std::uint32_t bytes,
                                 IoToken token) {
  SND_RT_NOTE(FileIo);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = open_.find(file);
  if (it == open_.end() || dst == nullptr) return false;
  if (maxInFlight_ != 0 && reads_.size() >= maxInFlight_) return false;
  if (reads_.count(token.value) != 0) return false;  // token already in flight
  ++readCalls_;
  Read r{file, offset, dst, bytes, defaultLatency_, false, it->second.path};
  auto lat = latency_.find(r.path);
  if (lat != latency_.end()) r.pollsLeft = lat->second;
  for (const FailRange& fr : failRanges_) {
    if (fr.path == r.path && offset < fr.end && offset + bytes > fr.begin) r.fail = true;
  }
  if (failNth_ == 0) r.fail = true;
  if (failNth_ >= 0) --failNth_;
  reads_[token.value] = r;
  return true;
}

IoStatus MemoryFileSystem::Poll(IoToken token) {
  SND_RT_NOTE(FileIo);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = reads_.find(token.value);
  if (it == reads_.end()) return IoStatus{SND_IO_UNKNOWN_TOKEN, 0};
  Read& r = it->second;
  if (held_.count(r.path) != 0) return IoStatus{SND_IO_PENDING, 0};
  if (r.pollsLeft > 0) {
    --r.pollsLeft;
    return IoStatus{SND_IO_PENDING, 0};
  }
  IoStatus status{SND_IO_ERROR, 0};
  auto of = open_.find(r.file);
  if (!r.fail && of != open_.end()) {
    const std::vector<std::uint8_t>& data = of->second.bytes;
    std::uint32_t n = 0;
    if (r.offset < data.size()) {
      const std::uint64_t avail = data.size() - r.offset;
      n = static_cast<std::uint32_t>(avail < r.bytes ? avail : r.bytes);
      std::memcpy(r.dst, data.data() + r.offset, n);
    }
    bytesRead_ += n;
    status = IoStatus{SND_IO_DONE, n};
  }
  reads_.erase(it);
  return status;
}

void MemoryFileSystem::Close(FileId file) {
  SND_RT_NOTE(FileIo);
  std::lock_guard<std::mutex> lock(mutex_);
  open_.erase(file);
}

}  // namespace snd::mock
