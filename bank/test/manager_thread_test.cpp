// BankManager under real threads (run under TSan): a small thread-pool
// IJobSystem (jobs run on worker threads, never inside Submit), an IO thread
// calling PumpIo, a "core" thread calling Pump / PopRegistration /
// MarkRegistered / Release, and a game thread requesting and unloading banks.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/bank/bank_manager.hpp"
#include "snd/id.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"
#include "test_support.hpp"

namespace bf = snd::bank_format;
namespace ch = bf::chunk;
using snd::HandleRef;
using snd::HashId;
using namespace snd::bank;

namespace {

class ThreadPool final : public snd::IJobSystem {
 public:
  explicit ThreadPool(int n) {
    for (int i = 0; i < n; ++i) threads_.emplace_back([this] { Loop(); });
  }
  ~ThreadPool() override {
    {
      std::lock_guard<std::mutex> l(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : threads_) t.join();
  }
  void Submit(const snd::JobDesc* jobs, std::uint32_t count, snd::JobLane) override {
    {
      std::lock_guard<std::mutex> l(m_);
      for (std::uint32_t i = 0; i < count; ++i) q_.push_back(jobs[i]);
    }
    cv_.notify_all();
  }
  void Drain() {
    std::unique_lock<std::mutex> l(m_);
    idle_.wait(l, [this] { return q_.empty() && running_ == 0; });
  }

 private:
  void Loop() {
    for (;;) {
      snd::JobDesc j;
      {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return stop_ || !q_.empty(); });
        if (q_.empty()) return;
        j = q_.front();
        q_.pop_front();
        ++running_;
      }
      std::this_thread::yield();
      snd::RunJob(j);
      {
        std::lock_guard<std::mutex> l(m_);
        --running_;
      }
      idle_.notify_all();
    }
  }
  std::mutex m_;
  std::condition_variable cv_;
  std::condition_variable idle_;
  std::deque<snd::JobDesc> q_;
  int running_ = 0;
  bool stop_ = false;
  std::vector<std::thread> threads_;
};

fixture::Bank Named(const std::string& name, std::uint64_t hash, const char* dep = nullptr, std::uint64_t depHash = 0) {
  fixture::Bank b = fixture::RichBank();
  b.bankId = HashId(name);
  b.contentHash = hash;
  if (dep != nullptr) b.deps.push_back(bf::Dependency{HashId(dep), 0, depHash});
  return b;
}

}  // namespace

SND_TEST(BankManagerThreads, ConcurrentLoadUnloadWithRealWorkers) {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  fs.SetDefaultLatency(1);
  std::size_t baseline = 0;
  {
    ThreadPool pool(3);
    BankManagerDesc d;
    d.allocator = &alloc;
    d.fileSystem = &fs;
    d.ioMode = snd::bank::BankIoMode::DirectFileSystem;  // stage 1.5: the default is the IO scheduler
    d.jobs = &pool;
    d.maxBanks = 6;
    d.ioChunkBytes = 512;
    d.prepare.devOverlay = true;
    d.prepare.devReserve.bytes = 1024;
    BankManager* m = nullptr;
    SND_REQUIRE_EQ(BankManager::Create(d, &m), SND_OK);
    baseline = alloc.Live();
    const char* names[] = {"Base", "A", "B", "C", "D"};
    for (const char* n : names) {
      const bool dep = std::strcmp(n, "Base") != 0 && n[0] != 'D';
      fs.AddFile(std::string(n) + ".sndb", Named(n, 100, dep ? "Base" : nullptr, 100).Build());
    }
    std::atomic<bool> stop{false};
    std::atomic<std::uint32_t> registered{0};
    std::thread io([&] {
      while (!stop.load()) {
        m->PumpIo();
        std::this_thread::yield();
      }
    });
    std::thread core([&] {
      snd::cmd::BankRegister reg{};
      while (!stop.load()) {
        m->Pump();
        while (m->PopRegistration(&reg)) {
          m->MarkRegistered(reg.bank);
          registered.fetch_add(1);
        }
        // Releases every bank the game asked to unload (epoch passed immediately).
        for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
          const std::uint32_t st = m->SlotStates()[s].state.load();
          const std::uint32_t gen = m->SlotStates()[s].generation.load();
          if (st == SND_BANK_STATE_UNLOADING) (void)m->Release(HandleRef{s, gen});
        }
        std::this_thread::yield();
      }
    });
    testsupport::Rng rng(0x7E57ull);
    std::vector<HandleRef> handles;
    const auto start = std::chrono::steady_clock::now();
    // At least 3000 operations and (bounded by 20 s) at least 20 registrations,
    // so a loaded machine still exercises the full pipeline.
    for (int i = 0; i < 3000 || (registered.load() < 20 &&
                                 std::chrono::steady_clock::now() - start < std::chrono::seconds(20));
         ++i) {
      const char* n = names[rng.Below(5)];
      if (rng.Below(3) != 0) {
        const HandleRef h = m->RequestLoad((std::string(n) + ".sndb").c_str());
        if (!h.IsNone()) handles.push_back(h);
      } else if (!handles.empty()) {
        const HandleRef h = handles[rng.Below(static_cast<std::uint32_t>(handles.size()))];
        (void)m->BeginUnload(h, rng.Below(2) ? SND_UNLOAD_STOP_AND_UNLOAD : SND_UNLOAD_IF_UNUSED, false);
      }
      (void)m->State(handles.empty() ? HandleRef{0, 0} : handles.back());
      (void)m->Prepared(handles.empty() ? HandleRef{0, 0} : handles.back());
      (void)m->FindBank(HashId("Base"));
      if (i % 16 == 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    // Unload everything (dependents first), wait for the pipeline to drain.
    for (int round = 0; round < 2000; ++round) {
      bool any = false;
      for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
        const std::uint32_t st = m->SlotStates()[s].state.load();
        const std::uint32_t gen = m->SlotStates()[s].generation.load();
        if (st == SND_BANK_STATE_UNLOADED) continue;
        any = true;
        (void)m->BeginUnload(HandleRef{s, gen}, SND_UNLOAD_STOP_AND_UNLOAD, false);
      }
      if (!any) break;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    stop.store(true);
    io.join();
    core.join();
    pool.Drain();
    m->Update();
    for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
      const std::uint32_t st = m->SlotStates()[s].state.load();
      SND_CHECK_MSG(st == SND_BANK_STATE_UNLOADED || st == SND_BANK_STATE_FAILED, "slot %u left in state %u", s, st);
    }
    SND_CHECK(registered.load() > 10);
    snd::test::Note("%u registrations", registered.load());
    snd::test::Note("live allocations after unloading everything: %zu (baseline %zu)", alloc.Live(), baseline);
    BankManager::Destroy(m);
  }
  SND_CHECK_EQ(alloc.Live(), 0u);
  SND_CHECK_EQ(alloc.BadFrees(), 0u);
}
