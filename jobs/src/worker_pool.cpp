#include "snd/jobs/worker_pool.hpp"

#include <cstring>

#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"

namespace snd::jobs {

namespace {
thread_local const WorkerPool* tPool = nullptr;
thread_local std::uint32_t tWorker = 0;

constexpr int kLanes = static_cast<int>(JobLane::Count);
}  // namespace

WorkerPool::~WorkerPool() { Shutdown(true); }

bool WorkerPool::Init(IAllocator& allocator, const WorkerPoolConfig& config) noexcept {
  Shutdown(true);
  if (config.workers == 0u) return false;
  alloc_ = &allocator;
  config_ = config;
  pending_.value.store(0, std::memory_order_relaxed);
  sleepers_.value.store(0, std::memory_order_relaxed);
  stopping_.store(false, std::memory_order_relaxed);
  cancelPending_.store(false, std::memory_order_relaxed);
  shutDown_.store(false, std::memory_order_relaxed);
  workers_ = base::AllocateArray<Worker>(allocator, config.workers, MemTag::Misc);
  if (workers_ == nullptr) return false;
  for (std::uint32_t w = 0; w < config.workers; ++w) {
    workers_[w].pool = this;
    workers_[w].index = w;
    for (Queue& q : workers_[w].lanes) {
      q.capacity = config.initialQueueCapacity == 0u ? 16u : config.initialQueueCapacity;
      q.heap = base::AllocateArray<Entry>(allocator, q.capacity, MemTag::Misc);
      if (q.heap == nullptr) {
        q.capacity = 0;
        Shutdown(false);
        return false;
      }
    }
  }
  // Workers read workerCount_ (stealing), so it is final before any starts;
  // all queues exist already. Joining a thread that never started is a no-op.
  workerCount_ = config.workers;
  for (std::uint32_t w = 0; w < config.workers; ++w) {
    platform::ThreadDesc td;
    td.name = config.threadName;
    td.priority = platform::ThreadPriority::Normal;
    if (!workers_[w].thread.Start(td, &WorkerPool::WorkerMain, &workers_[w])) {
      Shutdown(false);
      return false;
    }
  }
  return true;
}

void WorkerPool::Shutdown(bool runPending) noexcept {
  if (workers_ == nullptr) return;
  cancelPending_.store(!runPending, std::memory_order_release);
  stopping_.store(true, std::memory_order_seq_cst);
  wake_.Post(workerCount_ + 1u);
  for (std::uint32_t w = 0; w < workerCount_; ++w) workers_[w].thread.Join();
  // Workers exit only with empty queues; anything left (no worker started) is
  // completed here so done counters still drop.
  for (std::uint32_t w = 0; w < config_.workers; ++w) {
    for (Queue& q : workers_[w].lanes) {
      Entry e;
      while (Pop(q, e)) Execute(e);
      base::FreeArray(*alloc_, q.heap, q.capacity, MemTag::Misc);
      q.heap = nullptr;
      q.capacity = 0;
    }
  }
  base::FreeArray(*alloc_, workers_, config_.workers, MemTag::Misc);
  workers_ = nullptr;
  workerCount_ = 0;
  // Drain the semaphore so a re-Init starts clean.
  while (wake_.TryWait()) {
  }
  shutDown_.store(true, std::memory_order_release);
}

int WorkerPool::CurrentWorkerIndex() const noexcept { return tPool == this ? static_cast<int>(tWorker) : -1; }

// --- queues -------------------------------------------------------------------
namespace {
// a runs before b
inline bool Before(const JobDesc& a, std::uint64_t sa, const JobDesc& b, std::uint64_t sb) noexcept {
  return a.priority != b.priority ? a.priority > b.priority : sa < sb;
}
}  // namespace

bool WorkerPool::Push(Queue& q, const JobDesc& d) noexcept {
  base::LockGuard lock(q.mutex);
  if (q.size == q.capacity) {
    const std::uint32_t cap = q.capacity < 16u ? 16u : q.capacity * 2u;
    Entry* grown = base::AllocateArray<Entry>(*alloc_, cap, MemTag::Misc);
    if (grown == nullptr) return false;
    if (q.size != 0u) std::memcpy(static_cast<void*>(grown), q.heap, sizeof(Entry) * q.size);
    base::FreeArray(*alloc_, q.heap, q.capacity, MemTag::Misc);
    q.heap = grown;
    q.capacity = cap;
  }
  Entry e{d, seq_.fetch_add(1u, std::memory_order_relaxed)};
  std::uint32_t i = q.size++;
  while (i > 0u) {
    const std::uint32_t parent = (i - 1u) / 2u;
    if (!Before(e.desc, e.seq, q.heap[parent].desc, q.heap[parent].seq)) break;
    q.heap[i] = q.heap[parent];
    i = parent;
  }
  q.heap[i] = e;
  return true;
}

bool WorkerPool::Pop(Queue& q, Entry& out) noexcept {
  base::LockGuard lock(q.mutex);
  if (q.size == 0u) return false;
  out = q.heap[0];
  const Entry last = q.heap[--q.size];
  std::uint32_t i = 0;
  for (;;) {
    std::uint32_t c = 2u * i + 1u;
    if (c >= q.size) break;
    if (c + 1u < q.size && Before(q.heap[c + 1u].desc, q.heap[c + 1u].seq, q.heap[c].desc, q.heap[c].seq)) ++c;
    if (!Before(q.heap[c].desc, q.heap[c].seq, last.desc, last.seq)) break;
    q.heap[i] = q.heap[c];
    i = c;
  }
  if (q.size != 0u) q.heap[i] = last;
  // Counted as running before it stops being pending, so WaitIdle never sees a gap.
  running_.fetch_add(1u, std::memory_order_seq_cst);
  pending_.value.fetch_sub(1u, std::memory_order_seq_cst);
  return true;
}

bool WorkerPool::FindJob(std::uint32_t self, Entry& out) noexcept {
  const bool backgroundFirst = workerCount_ >= 2u && self == workerCount_ - 1u;
  for (int pass = 0; pass < kLanes; ++pass) {
    const int lane = backgroundFirst ? (kLanes - 1 - pass) : pass;
    if (Pop(workers_[self].lanes[lane], out)) return true;
    for (std::uint32_t k = 1; k < workerCount_; ++k) {
      const std::uint32_t victim = (self + k) % workerCount_;
      if (Pop(workers_[victim].lanes[lane], out)) {
        stolen_.fetch_add(1u, std::memory_order_relaxed);
        return true;
      }
    }
  }
  return false;
}

void WorkerPool::Execute(const Entry& e) noexcept {
  const bool cancel = cancelPending_.load(std::memory_order_acquire) || IsJobCancelled(e.desc);
  if (cancel) {
    MarkJobDone(e.desc);
    skipped_.fetch_add(1u, std::memory_order_relaxed);
  } else {
    if (e.desc.fn != nullptr) e.desc.fn(e.desc.data);
    MarkJobDone(e.desc);
    executed_.fetch_add(1u, std::memory_order_relaxed);
  }
  running_.fetch_sub(1u, std::memory_order_seq_cst);
}

void WorkerPool::RunInline(const JobDesc& d) noexcept {
  const bool cancelled = IsJobCancelled(d);
  RunJob(d);
  (cancelled ? skipped_ : executed_).fetch_add(1u, std::memory_order_relaxed);
  inline_.fetch_add(1u, std::memory_order_relaxed);
}

// --- IJobSystem ------------------------------------------------------------------
void WorkerPool::Submit(const JobDesc* jobs, std::uint32_t count, JobLane lane) {
  if (jobs == nullptr || count == 0u) return;
  int l = static_cast<int>(lane);
  if (l < 0 || l >= kLanes) l = static_cast<int>(JobLane::Background);
  submitted_.fetch_add(count, std::memory_order_relaxed);
  const bool fromWorker = tPool == this;
  // After Shutdown (or while stopping, from a thread that is not a worker)
  // there may be nobody left to run the job: run it here.
  if (workers_ == nullptr || (!fromWorker && stopping_.load(std::memory_order_acquire))) {
    for (std::uint32_t i = 0; i < count; ++i) RunInline(jobs[i]);
    return;
  }
  std::uint32_t pushed = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t target =
        fromWorker ? tWorker : roundRobin_.fetch_add(1u, std::memory_order_relaxed) % workerCount_;
    if (Push(workers_[target].lanes[l], jobs[i])) {
      ++pushed;
    } else {
      RunInline(jobs[i]);  // out of memory: last resort, documented
    }
  }
  if (pushed == 0u) return;
  pending_.value.fetch_add(pushed, std::memory_order_seq_cst);
  const std::int32_t sleeping = sleepers_.value.load(std::memory_order_seq_cst);
  if (sleeping > 0) wake_.Post(static_cast<std::uint32_t>(sleeping) < pushed ? static_cast<std::uint32_t>(sleeping) : pushed);
}

void WorkerPool::WorkerMain(void* p) noexcept {
  auto* w = static_cast<Worker*>(p);
  w->pool->WorkerLoop(*w);
}

void WorkerPool::WorkerLoop(Worker& w) noexcept {
  tPool = this;
  tWorker = w.index;
  platform::FpControl saved{};
  if (config_.flushDenormals) saved = platform::SetFlushDenormals(true);
  for (;;) {
    Entry e;
    if (FindJob(w.index, e)) {
      Execute(e);
      continue;
    }
    if (stopping_.load(std::memory_order_seq_cst) &&
        static_cast<std::int64_t>(pending_.value.load(std::memory_order_seq_cst)) <= 0) {
      break;
    }
    sleepers_.value.fetch_add(1, std::memory_order_seq_cst);
    if (static_cast<std::int64_t>(pending_.value.load(std::memory_order_seq_cst)) <= 0 &&
        !stopping_.load(std::memory_order_seq_cst)) {
      wake_.Wait();
    }
    sleepers_.value.fetch_sub(1, std::memory_order_seq_cst);
  }
  if (config_.flushDenormals) platform::RestoreFpControl(saved);
  tPool = nullptr;
}

bool WorkerPool::WaitIdle(std::uint64_t timeoutNs) noexcept {
  const std::uint64_t start = platform::MonotonicNanoseconds();
  for (;;) {
    if (static_cast<std::int64_t>(pending_.value.load(std::memory_order_seq_cst)) <= 0 &&
        running_.load(std::memory_order_seq_cst) == 0u) {
      return true;
    }
    if (platform::MonotonicNanoseconds() - start >= timeoutNs) return false;
    platform::SleepFor(50000);
  }
}

WorkerPool::Stats WorkerPool::GetStats() const noexcept {
  Stats s;
  s.submitted = submitted_.load(std::memory_order_relaxed);
  s.executed = executed_.load(std::memory_order_relaxed);
  s.skipped = skipped_.load(std::memory_order_relaxed);
  s.stolen = stolen_.load(std::memory_order_relaxed);
  s.inline_ = inline_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace snd::jobs
