// Helpers for the independent (adversarial) io tests.
//
// ScriptFs is an IFileSystem written for these tests (not the shared mock): it
// checks that the scheduler uses the file system as the contract says
// (snd/ext/file_system.hpp, D-24) and lets a test misbehave on purpose:
//   - contract checks: one calling thread, Close only without reads in flight,
//     unique in-flight tokens, non-null dst, no two in-flight reads writing to
//     overlapping memory, Poll of a token that was never issued or already retired;
//   - injection: per-read latency (in polls), held reads, read errors, rejected
//     ReadAsync, UNKNOWN_TOKEN instead of a result, over-reported byte counts,
//     failing Open;
//   - an issue log (clock time, bytes) of every accepted read, to check budgets.
// All members are synchronised with a mutex (the IO thread may run while the
// test thread injects faults). Never call it inside an RT scope.
#ifndef SND_IO_ADVERSARIAL_UTIL_HPP
#define SND_IO_ADVERSARIAL_UTIL_HPP

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "io_test_util.hpp"
#include "snd/ext/file_system.hpp"

namespace snd::iotest {

class ScriptFs final : public IFileSystem {
 public:
  struct Issue {
    std::uint64_t ns;
    std::uint32_t bytes;
    std::uint64_t offset;
    std::string path;
  };

  explicit ScriptFs(ManualClock* clock = nullptr) : clock_(clock) {}

  // --- content ----------------------------------------------------------------
  void AddFile(const std::string& path, std::vector<std::uint8_t> data) {
    std::lock_guard<std::mutex> l(m_);
    files_[path] = std::move(data);
  }

  // --- injection ----------------------------------------------------------------
  void SetLatency(std::uint32_t polls) { std::lock_guard<std::mutex> l(m_); latency_ = polls; }
  void SetRandomLatency(std::uint32_t maxPolls, std::uint32_t seed) {
    std::lock_guard<std::mutex> l(m_);
    randomLatency_ = maxPolls;
    rng_ = seed | 1u;
  }
  void Hold(bool hold) { std::lock_guard<std::mutex> l(m_); hold_ = hold; }
  void FailOpen(const std::string& path) { std::lock_guard<std::mutex> l(m_); failOpen_.insert(path); }
  void FailNextReads(std::uint32_t n) { std::lock_guard<std::mutex> l(m_); failNext_ = n; }
  void SetFailEvery(std::uint32_t n) { std::lock_guard<std::mutex> l(m_); failEvery_ = n; }
  void RejectNextReads(std::uint32_t n) { std::lock_guard<std::mutex> l(m_); rejectNext_ = n; }
  void SetRejectAll(bool r) { std::lock_guard<std::mutex> l(m_); rejectAll_ = r; }
  void SetMaxInFlight(std::uint32_t n) { std::lock_guard<std::mutex> l(m_); maxInFlight_ = n; }
  // Completed reads report `extra` more bytes than they copied (a broken FS).
  void SetOverReport(std::uint32_t extra) { std::lock_guard<std::mutex> l(m_); overReport_ = extra; }
  // The next n completions answer UNKNOWN_TOKEN instead of Done (the read is dropped).
  void UnknownNextCompletions(std::uint32_t n) { std::lock_guard<std::mutex> l(m_); unknownNext_ = n; }

  // --- observation ----------------------------------------------------------------
  std::vector<std::string> Violations() const { std::lock_guard<std::mutex> l(m_); return violations_; }
  std::vector<Issue> Issues() const { std::lock_guard<std::mutex> l(m_); return issues_; }
  void ClearIssues() { std::lock_guard<std::mutex> l(m_); issues_.clear(); }
  std::uint32_t InFlight() const { std::lock_guard<std::mutex> l(m_); return static_cast<std::uint32_t>(reads_.size()); }
  std::uint32_t MaxInFlightSeen() const { std::lock_guard<std::mutex> l(m_); return maxSeen_; }
  std::uint32_t OpenFiles() const { std::lock_guard<std::mutex> l(m_); return static_cast<std::uint32_t>(open_.size()); }
  std::uint32_t OpenCalls() const { std::lock_guard<std::mutex> l(m_); return openCalls_; }
  std::uint32_t ReadCalls() const { std::lock_guard<std::mutex> l(m_); return readCalls_; }
  std::uint32_t RejectedReads() const { std::lock_guard<std::mutex> l(m_); return rejected_; }
  std::uint32_t UnknownPolls() const { std::lock_guard<std::mutex> l(m_); return unknownPolls_; }
  void ForgetThread() { std::lock_guard<std::mutex> l(m_); threadSet_ = false; }

  // --- IFileSystem ----------------------------------------------------------------------
  FileId Open(const char* path) override {
    std::lock_guard<std::mutex> l(m_);
    CheckThread("Open");
    ++openCalls_;
    if (path == nullptr) return kInvalidFile;
    auto it = files_.find(path);
    if (it == files_.end() || failOpen_.count(path) != 0u) return kInvalidFile;
    const FileId id = nextId_++;
    open_[id] = OpenFile{path, it->second, 0};
    return id;
  }
  std::uint64_t Size(FileId file) override {
    std::lock_guard<std::mutex> l(m_);
    CheckThread("Size");
    auto it = open_.find(file);
    return it == open_.end() ? UINT64_MAX : it->second.data.size();
  }
  bool ReadAsync(FileId file, std::uint64_t offset, void* dst, std::uint32_t bytes, IoToken token) override {
    std::lock_guard<std::mutex> l(m_);
    CheckThread("ReadAsync");
    if (dst == nullptr) violations_.push_back("ReadAsync with dst == nullptr");
    if (bytes == 0u) violations_.push_back("ReadAsync of 0 bytes");
    auto it = open_.find(file);
    if (it == open_.end()) {
      violations_.push_back("ReadAsync on a file that is not open");
      return false;
    }
    if (rejectAll_ || rejectNext_ > 0u || (maxInFlight_ != 0u && reads_.size() >= maxInFlight_)) {
      if (rejectNext_ > 0u) --rejectNext_;
      ++rejected_;
      return false;
    }
    if (reads_.count(token.value) != 0u) {
      violations_.push_back("ReadAsync with a token already in flight");
      return false;
    }
    const auto* b = static_cast<const std::uint8_t*>(dst);
    for (const auto& kv : reads_) {
      const auto* o = static_cast<const std::uint8_t*>(kv.second.dst);
      if (b < o + kv.second.bytes && o < b + bytes) violations_.push_back("two in-flight reads overlap in memory");
    }
    ++readCalls_;
    Read r{};
    r.file = file;
    r.offset = offset;
    r.dst = dst;
    r.bytes = bytes;
    r.polls = latency_;
    if (randomLatency_ != 0u) {
      rng_ = rng_ * 1664525u + 1013904223u;
      r.polls = (rng_ >> 16) % (randomLatency_ + 1u);
    }
    if (failNext_ > 0u) {
      --failNext_;
      r.fail = true;
    }
    if (failEvery_ != 0u && (readCalls_ % failEvery_) == 0u) r.fail = true;
    reads_[token.value] = r;
    ++it->second.inflight;
    if (reads_.size() > maxSeen_) maxSeen_ = static_cast<std::uint32_t>(reads_.size());
    issues_.push_back(Issue{clock_ != nullptr ? clock_->now.load() : 0u, bytes, offset, it->second.path});
    return true;
  }
  IoStatus Poll(IoToken token) override {
    std::lock_guard<std::mutex> l(m_);
    CheckThread("Poll");
    auto it = reads_.find(token.value);
    if (it == reads_.end()) {
      ++unknownPolls_;
      return IoStatus{SND_IO_UNKNOWN_TOKEN, 0};
    }
    Read& r = it->second;
    if (hold_) return IoStatus{SND_IO_PENDING, 0};
    if (r.polls > 0u) {
      --r.polls;
      return IoStatus{SND_IO_PENDING, 0};
    }
    IoStatus st{SND_IO_ERROR, 0};
    auto of = open_.find(r.file);
    if (of != open_.end()) {
      if (of->second.inflight > 0u) --of->second.inflight;
    } else {
      violations_.push_back("read completed on a closed file");
    }
    if (!r.fail && of != open_.end()) {
      const std::vector<std::uint8_t>& d = of->second.data;
      std::uint32_t n = 0;
      if (r.offset < d.size()) {
        const std::uint64_t avail = d.size() - r.offset;
        n = static_cast<std::uint32_t>(avail < r.bytes ? avail : r.bytes);
        std::memcpy(r.dst, d.data() + r.offset, n);
      }
      st = IoStatus{SND_IO_DONE, n + overReport_};
    }
    reads_.erase(it);
    if (unknownNext_ > 0u) {
      --unknownNext_;
      return IoStatus{SND_IO_UNKNOWN_TOKEN, 0};
    }
    return st;
  }
  void Close(FileId file) override {
    std::lock_guard<std::mutex> l(m_);
    CheckThread("Close");
    auto it = open_.find(file);
    if (it == open_.end()) {
      violations_.push_back("Close of a file that is not open");
      return;
    }
    if (it->second.inflight != 0u) violations_.push_back("Close with reads in flight (" + it->second.path + ")");
    open_.erase(it);
  }

 private:
  struct OpenFile {
    std::string path;
    std::vector<std::uint8_t> data;
    std::uint32_t inflight;
  };
  struct Read {
    FileId file = 0;
    std::uint64_t offset = 0;
    void* dst = nullptr;
    std::uint32_t bytes = 0;
    std::uint32_t polls = 0;
    bool fail = false;
  };
  void CheckThread(const char* what) {
    const std::thread::id me = std::this_thread::get_id();
    if (!threadSet_) {
      threadSet_ = true;
      thread_ = me;
    } else if (thread_ != me) {
      violations_.push_back(std::string("IFileSystem called from a second thread: ") + what);
      thread_ = me;
    }
  }

  mutable std::mutex m_;
  ManualClock* clock_;
  std::map<std::string, std::vector<std::uint8_t>> files_;
  std::map<FileId, OpenFile> open_;
  std::map<std::uint64_t, Read> reads_;
  std::set<std::string> failOpen_;
  std::vector<std::string> violations_;
  std::vector<Issue> issues_;
  std::thread::id thread_{};
  bool threadSet_ = false;
  bool hold_ = false;
  bool rejectAll_ = false;
  std::uint32_t latency_ = 0, randomLatency_ = 0, rng_ = 1;
  std::uint32_t failNext_ = 0, failEvery_ = 0, rejectNext_ = 0, maxInFlight_ = 0;
  std::uint32_t overReport_ = 0, unknownNext_ = 0;
  std::uint32_t maxSeen_ = 0, openCalls_ = 0, readCalls_ = 0, rejected_ = 0, unknownPolls_ = 0;
  FileId nextId_ = 1;
};

// Scheduler + pool over a ScriptFs, driven manually with a manual clock.
struct ScriptRig {
  base::AccountingAllocator alloc{base::DefaultAllocator()};
  ManualClock clock;
  ScriptFs fs{&clock};
  io::StreamPool pool;
  io::IoScheduler io;
  io::IoSchedulerConfig cfg;
  std::uint32_t poolChunks;
  std::uint32_t chunkBytes;

  explicit ScriptRig(std::uint32_t chunks = 64, std::uint32_t chunk = kChunk) : poolChunks(chunks), chunkBytes(chunk) {
    cfg.maxStreams = 8;
    cfg.ringChunks = 8;
    cfg.maxInFlight = 4;
    cfg.bytesPerWindow = 0;
    cfg.clock = clock.Clock();
  }
  ~ScriptRig() {
    io.Destroy();
    pool.Destroy();
  }
  bool Init() noexcept {
    return pool.Init(alloc, static_cast<std::uint64_t>(poolChunks) * chunkBytes, chunkBytes, cfg.maxStreams) &&
           io.Init(alloc, fs, pool, cfg);
  }
  // RunOnce until idle (or `max` iterations). Returns the iterations used.
  std::uint32_t Run(std::uint32_t max = 1000) noexcept {
    for (std::uint32_t i = 0; i < max; ++i) {
      if (!io.RunOnce()) return i + 1u;
    }
    return max;
  }
  // Sum of the pool's per-owner books (owners + the unattributed bucket).
  std::uint32_t HeldSum() const noexcept {
    std::uint32_t s = pool.HeldBy(io::kNoOwner);
    for (std::uint32_t o = 0; o < pool.MaxOwners(); ++o) s += pool.HeldBy(o);
    return s;
  }
};

inline std::string JoinViolations(const std::vector<std::string>& v) {
  std::string s;
  for (std::size_t i = 0; i < v.size() && i < 5; ++i) {
    s += v[i];
    s += "; ";
  }
  return s;
}

}  // namespace snd::iotest

#endif  // SND_IO_ADVERSARIAL_UTIL_HPP
