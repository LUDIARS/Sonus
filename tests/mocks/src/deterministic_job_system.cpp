#include "snd/mocks/deterministic_job_system.hpp"

namespace snd::mock {

DeterministicJobSystem::DeterministicJobSystem(JobMode mode) : mode_(mode) {}
DeterministicJobSystem::~DeterministicJobSystem() = default;

void DeterministicJobSystem::Submit(const JobDesc* jobs, std::uint32_t count, JobLane lane) {
  const int l = static_cast<int>(lane);
  if (jobs == nullptr || l < 0 || l >= static_cast<int>(JobLane::Count)) return;
  bool immediate;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::uint32_t i = 0; i < count; ++i) {
      queues_[l].push_back(Entry{jobs[i], lane, nextOrder_++});
      ++submitted_;
    }
    immediate = mode_ == JobMode::Immediate;
  }
  if (immediate) DrainImmediate();
}

void DeterministicJobSystem::SetMode(JobMode mode) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = mode;
  }
  if (mode == JobMode::Immediate) DrainImmediate();
}

JobMode DeterministicJobSystem::Mode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mode_;
}

std::size_t DeterministicJobSystem::Pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t n = 0;
  for (const auto& q : queues_) n += q.size();
  return n;
}

std::size_t DeterministicJobSystem::Pending(JobLane lane) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queues_[static_cast<int>(lane)].size();
}

// lane < 0: oldest of all lanes.
bool DeterministicJobSystem::PopLocked(int lane, Entry* out) {
  int best = -1;
  for (int l = 0; l < static_cast<int>(JobLane::Count); ++l) {
    if (lane >= 0 && l != lane) continue;
    if (queues_[l].empty()) continue;
    if (best < 0 || queues_[l].front().order < queues_[best].front().order) best = l;
  }
  if (best < 0) return false;
  *out = queues_[best].front();
  queues_[best].pop_front();
  return true;
}

void DeterministicJobSystem::Execute(const Entry& e) {
  const bool cancelled = IsJobCancelled(e.desc);
  RunJob(e.desc);  // skips fn when cancelled, always signals completion
  std::lock_guard<std::mutex> lock(mutex_);
  if (cancelled) {
    ++skipped_;
  } else {
    ++executed_;
  }
}

bool DeterministicJobSystem::RunNext() {
  Entry e{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!PopLocked(-1, &e)) return false;
  }
  Execute(e);
  return true;
}

bool DeterministicJobSystem::RunNext(JobLane lane) {
  Entry e{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!PopLocked(static_cast<int>(lane), &e)) return false;
  }
  Execute(e);
  return true;
}

std::size_t DeterministicJobSystem::RunAll() {
  std::size_t n = 0;
  while (RunNext()) ++n;
  return n;
}

void DeterministicJobSystem::DrainImmediate() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (draining_) return;  // a job submitted from inside a job: the outer drain runs it
    draining_ = true;
  }
  for (;;) {
    Entry e{};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!PopLocked(-1, &e)) {
        draining_ = false;
        return;
      }
    }
    Execute(e);
  }
}

std::uint64_t DeterministicJobSystem::SubmittedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return submitted_;
}
std::uint64_t DeterministicJobSystem::ExecutedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return executed_;
}
std::uint64_t DeterministicJobSystem::SkippedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return skipped_;
}

}  // namespace snd::mock
