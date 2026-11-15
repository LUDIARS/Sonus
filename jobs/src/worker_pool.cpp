#include "snd/jobs/worker_pool.hpp"

#include <cstring>

#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"

namespace snd::jobs {

namespace {
thread_local const WorkerPool* tPool = nullptr;
thread_local std::uint32_t tWorker = 0;

constexpr int kPrefetch = static_cast<int>(JobLane::Prefetch);
constexpr int kBackground = static_cast<int>(JobLane::Background);

// a runs before b: higher priority, then earlier submission (global sequence).
inline bool Before(std::uint32_t pa, std::uint64_t sa, std::uint32_t pb, std::uint64_t sb) noexcept {
  return pa != pb ? pa > pb : sa < sb;
}
}  // namespace

WorkerPool::~WorkerPool() { Shutdown(true); }

bool WorkerPool::Init(IAllocator& allocator, const WorkerPoolConfig& config) noexcept {
  Shutdown(true);
  if (config.workers == 0u) return false;
  alloc_ = &allocator;
  config_ = config;
  if (config_.prefetchBurst == 0u) config_.prefetchBurst = 1u;
  pending_.value.store(0, std::memory_order_relaxed);
  for (auto& lp : lanePending_) lp.store(0, std::memory_order_relaxed);
  sleepers_.value.store(0, std::memory_order_relaxed);
  cancelPending_.store(false, std::memory_order_relaxed);
  workers_ = base::AllocateArray<Worker>(allocator, config.workers, MemTag::Misc);
  if (workers_ == nullptr) return false;
  // Workers read workerCount_ (stealing), so it is final before any starts.
  workerCount_ = config.workers;
  for (std::uint32_t w = 0; w < config.workers; ++w) {
    workers_[w].pool = this;
    workers_[w].index = w;
    for (Queue& q : workers_[w].lanes) {
      q.capacity = config.initialQueueCapacity == 0u ? 16u : config.initialQueueCapacity;
      q.heap = base::AllocateArray<Entry>(allocator, q.capacity, MemTag::Misc);
      if (q.heap == nullptr) {
        q.capacity = 0;
        stopping_.store(false, std::memory_order_seq_cst);  // let Shutdown run its normal path
        Shutdown(false);
        return false;
      }
    }
  }
  // Open for Submit before the workers start (a worker never reads stopping_
  // before its first job anyway). Pairs with the acquire in Submit.
  stopping_.store(false, std::memory_order_seq_cst);
  for (std::uint32_t w = 0; w < config.workers; ++w) {
    platform::ThreadDesc td;
    td.name = config.threadName;
    td.priority = platform::ThreadPriority::Normal;
    if (!workers_[w].thread.Start(td, &WorkerPool::WorkerMain, &workers_[w])) {
      Shutdown(false);  // joining threads that never started is a no-op
      return false;
    }
  }
  return true;
}

void WorkerPool::Shutdown(bool runPending) noexcept {
  if (workers_ == nullptr) return;
  cancelPending_.store(!runPending, std::memory_order_release);
  // Handshake with Submit (Dekker): Submit increments submitsInFlight_ and
  // then reads stopping_; we set stopping_ and then read submitsInFlight_
  // (all seq_cst). Either the Submit sees stopping_ (and runs its jobs
  // inline without touching the queues), or we see it in flight and wait
  // until it has finished pushing. After the wait no thread other than our
  // workers can reach the queues.
  stopping_.store(true, std::memory_order_seq_cst);
  while (submitsInFlight_.value.load(std::memory_order_seq_cst) != 0u) platform::YieldThread();
  wake_.Post(workerCount_ + 1u);
  for (std::uint32_t w = 0; w < workerCount_; ++w) workers_[w].thread.Join();
  // Workers exit only with empty queues; anything left (a worker that never
  // started, or jobs pushed by the last running job) is completed here.
  for (std::uint32_t w = 0; w < config_.workers; ++w) {
    for (int lane = 0; lane < kLanes; ++lane) {
      Entry e;
      while (Pop(workers_[w].lanes[lane], lane, e)) Execute(e);
    }
  }
  for (std::uint32_t w = 0; w < config_.workers; ++w) {
    for (Queue& q : workers_[w].lanes) {
      base::FreeArray(*alloc_, q.heap, q.capacity, MemTag::Misc);
      q.heap = nullptr;
      q.capacity = 0;
    }
  }
  base::FreeArray(*alloc_, workers_, config_.workers, MemTag::Misc);
  workers_ = nullptr;
  workerCount_ = 0;
  while (wake_.TryWait()) {  // drain so a re-Init starts clean
  }
}

int WorkerPool::CurrentWorkerIndex() const noexcept { return tPool == this ? static_cast<int>(tWorker) : -1; }

// --- queues -------------------------------------------------------------------
bool WorkerPool::Push(Queue& q, const JobDesc& d) noexcept {
  base::LockGuard lock(q.mutex);
  if (q.size == q.capacity) {
    if (q.capacity > 0x7FFFFFFFu) return false;
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
    if (!Before(e.desc.priority, e.seq, q.heap[parent].desc.priority, q.heap[parent].seq)) break;
    q.heap[i] = q.heap[parent];
    i = parent;
  }
  q.heap[i] = e;
  return true;
}

bool WorkerPool::Peek(Queue& q, Entry& out) noexcept {
  base::LockGuard lock(q.mutex);
  if (q.size == 0u) return false;
  out = q.heap[0];
  return true;
}

bool WorkerPool::Pop(Queue& q, int lane, Entry& out) noexcept {
  base::LockGuard lock(q.mutex);
  if (q.size == 0u) return false;
  out = q.heap[0];
  const Entry last = q.heap[--q.size];
  std::uint32_t i = 0;
  for (;;) {
    std::uint32_t c = 2u * i + 1u;
    if (c >= q.size) break;
    if (c + 1u < q.size &&
        Before(q.heap[c + 1u].desc.priority, q.heap[c + 1u].seq, q.heap[c].desc.priority, q.heap[c].seq)) {
      ++c;
    }
    if (!Before(q.heap[c].desc.priority, q.heap[c].seq, last.desc.priority, last.seq)) break;
    q.heap[i] = q.heap[c];
    i = c;
  }
  if (q.size != 0u) q.heap[i] = last;
  // Counted as running before it stops being pending, so WaitIdle never sees a gap.
  running_.fetch_add(1u, std::memory_order_seq_cst);
  lanePending_[lane].fetch_sub(1, std::memory_order_relaxed);
  pending_.value.fetch_sub(1, std::memory_order_seq_cst);
  return true;
}

// Takes the best head of `lane` over all workers' queues.
bool WorkerPool::TakeBest(std::uint32_t self, int lane, Entry& out) noexcept {
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::uint32_t best = workerCount_;
    Entry head{}, bestHead{};
    for (std::uint32_t k = 0; k < workerCount_; ++k) {
      const std::uint32_t w = (self + k) % workerCount_;  // own queue first on ties
      if (!Peek(workers_[w].lanes[lane], head)) continue;
      if (best == workerCount_ || Before(head.desc.priority, head.seq, bestHead.desc.priority, bestHead.seq)) {
        best = w;
        bestHead = head;
      }
    }
    if (best == workerCount_) return false;  // lane empty
    // The head may have changed since the peek (another worker took it or a
    // better job arrived); popping the current head is still the best of that
    // queue, which is the accepted momentary inversion.
    if (Pop(workers_[best].lanes[lane], lane, out)) {
      if (best != self) stolen_.fetch_add(1u, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

bool WorkerPool::FindJob(std::uint32_t self, std::uint32_t& prefetchStreak, Entry& out) noexcept {
  const bool backgroundTurn = prefetchStreak >= config_.prefetchBurst &&
                              lanePending_[kBackground].load(std::memory_order_relaxed) > 0;
  const int first = backgroundTurn ? kBackground : kPrefetch;
  const int second = backgroundTurn ? kPrefetch : kBackground;
  if (TakeBest(self, first, out)) {
    prefetchStreak = first == kPrefetch ? prefetchStreak + 1u : 0u;
    return true;
  }
  if (TakeBest(self, second, out)) {
    prefetchStreak = second == kPrefetch ? prefetchStreak + 1u : 0u;
    return true;
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
  if (l < 0 || l >= kLanes) l = kBackground;
  submitted_.fetch_add(count, std::memory_order_relaxed);
  const bool fromWorker = tPool == this;  // a worker of this pool: alive until its job returns
  submitsInFlight_.value.fetch_add(1u, std::memory_order_seq_cst);
  if (!fromWorker && stopping_.load(std::memory_order_seq_cst)) {
    // Not initialised, stopping or stopped: nobody may be left to run the jobs.
    submitsInFlight_.value.fetch_sub(1u, std::memory_order_seq_cst);
    for (std::uint32_t i = 0; i < count; ++i) RunInline(jobs[i]);
    return;
  }
  // Shutdown cannot free the queues until submitsInFlight_ drops (see Shutdown).
  std::uint32_t pushed = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t target =
        fromWorker ? tWorker : roundRobin_.fetch_add(1u, std::memory_order_relaxed) % workerCount_;
    if (Push(workers_[target].lanes[l], jobs[i])) {
      ++pushed;
      lanePending_[l].fetch_add(1, std::memory_order_relaxed);
    } else {
      RunInline(jobs[i]);  // out of memory: last resort, documented
    }
  }
  if (pushed != 0u) {
    pending_.value.fetch_add(pushed, std::memory_order_seq_cst);
    const std::int32_t sleeping = sleepers_.value.load(std::memory_order_seq_cst);
    if (sleeping > 0) {
      wake_.Post(static_cast<std::uint32_t>(sleeping) < pushed ? static_cast<std::uint32_t>(sleeping) : pushed);
    }
  }
  submitsInFlight_.value.fetch_sub(1u, std::memory_order_seq_cst);
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
  std::uint32_t prefetchStreak = 0;
  for (;;) {
    Entry e;
    if (FindJob(w.index, prefetchStreak, e)) {
      Execute(e);
      continue;
    }
    if (stopping_.load(std::memory_order_seq_cst) && pending_.value.load(std::memory_order_seq_cst) <= 0) break;
    sleepers_.value.fetch_add(1, std::memory_order_seq_cst);
    if (pending_.value.load(std::memory_order_seq_cst) <= 0 && !stopping_.load(std::memory_order_seq_cst)) {
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
    if (pending_.value.load(std::memory_order_seq_cst) <= 0 && running_.load(std::memory_order_seq_cst) == 0u) {
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
