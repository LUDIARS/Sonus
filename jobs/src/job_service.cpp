#include "snd/jobs/job_service.hpp"

#include "snd/platform/clock.hpp"

namespace snd::jobs {

// --- JobService ------------------------------------------------------------------
bool JobService::Init(IAllocator& allocator, const snd_job_system* external, std::uint32_t normalWorkers) noexcept {
  Shutdown();
  if (external != nullptr) {
    if (external->submit == nullptr) return false;
    external_.Reset(*external);  // copied by value (D-06)
    usesExternal_ = true;
    initialized_ = true;
    return true;
  }
  WorkerPoolConfig cfg;
  cfg.workers = normalWorkers == 0u ? 1u : normalWorkers;
  if (!pool_.Init(allocator, cfg)) return false;
  usesExternal_ = false;
  initialized_ = true;
  return true;
}

void JobService::Shutdown() noexcept {
  if (!initialized_) return;
  if (!usesExternal_) pool_.Shutdown(true);
  external_.Reset(snd_job_system{});
  usesExternal_ = false;
  initialized_ = false;
}

IJobSystem& JobService::Jobs() noexcept {
  if (usesExternal_) return external_;
  return pool_;
}

// --- JobPump -------------------------------------------------------------------------
bool JobPump::Init(IAllocator& allocator, std::uint32_t capacity, IJobSystem& target) noexcept {
  Destroy();
  if (!queue_.Init(allocator, capacity, MemTag::Misc)) return false;
  target_ = &target;
  return true;
}

void JobPump::Destroy() noexcept {
  StopThread();
  if (target_ != nullptr) Pump();
  queue_.Destroy();
  target_ = nullptr;
}

bool JobPump::Post(const JobDesc& job, JobLane lane) noexcept {
  if (!queue_.TryPush(PostedJob{job, static_cast<std::uint32_t>(lane), 0u})) {
    rejected_.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  posted_.fetch_add(1u, std::memory_order_relaxed);
  if (threadRunning_.load(std::memory_order_relaxed)) wake_.Post();  // pre-made semaphore: RT-allowed
  return true;
}

std::uint32_t JobPump::Pump(std::uint32_t max) noexcept {
  if (target_ == nullptr) return 0;
  bool expected = false;
  if (!pumping_.compare_exchange_strong(expected, true, std::memory_order_acquire)) return 0;
  constexpr std::uint32_t kBatch = 32;
  JobDesc batch[kBatch];
  std::uint32_t n = 0, total = 0;
  JobLane batchLane = JobLane::Prefetch;
  auto flush = [&] {
    if (n == 0u) return;
    // Counted before handing over: once submitted, a job may run (and its
    // done counter drop) at once, and Forwarded() must already include it.
    forwarded_.fetch_add(n, std::memory_order_release);
    target_->Submit(batch, n, batchLane);
    n = 0;
  };
  PostedJob p;
  while (total < max && queue_.TryPop(p)) {
    const JobLane lane = p.lane < static_cast<std::uint32_t>(JobLane::Count) ? static_cast<JobLane>(p.lane)
                                                                             : JobLane::Background;
    if (n != 0u && (lane != batchLane || n == kBatch)) flush();
    batchLane = lane;
    batch[n++] = p.desc;
    ++total;
  }
  flush();
  pumping_.store(false, std::memory_order_release);
  return total;
}

bool JobPump::StartThread(std::uint32_t pollMs) noexcept {
  if (target_ == nullptr || threadRunning_.load()) return false;
  pollMs_ = pollMs == 0u ? 1u : pollMs;
  threadRunning_.store(true, std::memory_order_release);
  platform::ThreadDesc td;
  td.name = "snd-job-pump";
  if (!thread_.Start(td, &JobPump::ThreadMain, this)) {
    threadRunning_.store(false);
    return false;
  }
  return true;
}

void JobPump::StopThread() noexcept {
  if (!thread_.Joinable()) return;
  threadRunning_.store(false, std::memory_order_release);
  wake_.Post();
  thread_.Join();
  while (wake_.TryWait()) {
  }
}

void JobPump::ThreadMain(void* self) noexcept {
  auto* p = static_cast<JobPump*>(self);
  while (p->threadRunning_.load(std::memory_order_acquire)) {
    p->wake_.WaitFor(static_cast<std::uint64_t>(p->pollMs_) * 1000000u);
    p->Pump();
  }
  p->Pump();
}

}  // namespace snd::jobs
