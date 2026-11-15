// BankManager over IoScheduler with real threads (stage 1.5, A1-11; run under
// TSan and repeated): jobs::WorkerPool (background lane) validates, the io
// module's IO thread reads, the memory FS injects latency and failures. Game
// threads request / unload, a core thread pumps, registers and releases.
// Also IoScheduler::Shutdown() racing with loads, and the teardown order
// io.Shutdown -> jobs drained -> BankManager::Destroy -> io.Destroy.
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/bank/bank_manager.hpp"
#include "snd/id.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"
#include "test_support.hpp"

namespace bf = snd::bank_format;
using snd::HandleRef;
using snd::HashId;
using namespace snd::bank;

namespace {

fixture::Bank Named(const std::string& name, const char* dep = nullptr) {
  fixture::Bank b = fixture::RichBank();
  b.bankId = HashId(name);
  b.contentHash = 7;
  if (dep != nullptr) b.deps.push_back(bf::Dependency{HashId(dep), 0, 7});
  return b;
}

const char* const kNames[] = {"Base", "A", "B", "C", "D"};

struct Rig {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::jobs::WorkerPool workers;
  snd::io::StreamPool pool;
  snd::io::IoScheduler io;
  BankManager* m = nullptr;
  bool ok = false;
  Rig() {
    fs.SetDefaultLatency(2);
    for (const char* n : kNames) {
      const bool dep = std::string(n) == "A" || std::string(n) == "B";  // A and B depend on Base
      fs.AddFile(std::string(n) + ".sndb", Named(n, dep ? "Base" : nullptr).Build());
    }
    snd::jobs::WorkerPoolConfig wc;
    wc.workers = 3;
    snd::io::IoSchedulerConfig ic;
    ic.maxStreams = 2;
    ic.filePieceBytes = 700;     // several pieces per bank
    ic.fileQueueCapacity = 3;    // Submit sometimes refuses: retry path
    ic.pollIntervalNs = 100000;  // 0.1 ms
    BankManagerDesc d;
    d.allocator = &alloc;
    d.scheduler = &io;
    d.jobs = &workers;
    d.maxBanks = 6;
    ok = workers.Init(alloc, wc) && pool.Init(alloc, 32u << 10, 16u << 10, 2) && io.Init(alloc, fs, pool, ic) &&
         io.Start() && BankManager::Create(d, &m) == SND_OK;
  }
  // The documented teardown order.
  void Teardown() {
    io.Shutdown();
    workers.WaitIdle();
    if (m != nullptr) {
      m->Update();  // banks whose reads were cancelled by the shutdown end FAILED
      BankManager::Destroy(m);
      m = nullptr;
    }
    io.Destroy();
    pool.Destroy();
    workers.Shutdown();
  }
};

// Core thread: pump, register, release unloading banks (epoch passes at once).
void CoreLoop(BankManager* m, std::atomic<bool>& stop, std::atomic<std::uint32_t>& registered) {
  snd::cmd::BankRegister reg{};
  while (!stop.load(std::memory_order_acquire)) {
    m->Update();  // Scheduler mode: PumpIo on any non-RT thread
    while (m->PopRegistration(&reg)) {
      m->MarkRegistered(reg.bank);
      registered.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
      const std::uint32_t st = m->SlotStates()[s].state.load();
      const std::uint32_t gen = m->SlotStates()[s].generation.load();
      if (st == SND_BANK_STATE_UNLOADING) (void)m->Release(HandleRef{s, gen});
    }
    std::this_thread::yield();
  }
}

// At least `ops` operations and (bounded by 20 s) until `minRegistrations`
// banks were registered, so a loaded machine still exercises the whole path.
void GameLoop(BankManager* m, std::uint64_t seed, int ops, const std::atomic<std::uint32_t>* registered,
              std::uint32_t minRegistrations) {
  testsupport::Rng rng(seed);
  std::vector<HandleRef> handles;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < ops || (registered->load() < minRegistrations &&
                              std::chrono::steady_clock::now() - start < std::chrono::seconds(20));
       ++i) {
    const char* n = kNames[rng.Below(5)];
    if (rng.Below(3) != 0) {
      const HandleRef h = m->RequestLoad((std::string(n) + ".sndb").c_str());
      if (!h.IsNone()) handles.push_back(h);
    } else if (!handles.empty()) {
      const HandleRef h = handles[rng.Below(static_cast<std::uint32_t>(handles.size()))];
      (void)m->BeginUnload(h, rng.Below(2) ? SND_UNLOAD_STOP_AND_UNLOAD : SND_UNLOAD_IF_UNUSED, false);
    }
    (void)m->State(handles.empty() ? HandleRef{0, 0} : handles.back());
    if (i % 8 == 0) std::this_thread::sleep_for(std::chrono::microseconds(150));
  }
}

void UnloadEverything(BankManager* m) {
  for (int round = 0; round < 4000; ++round) {
    bool any = false;
    for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
      const std::uint32_t st = m->SlotStates()[s].state.load();
      const std::uint32_t gen = m->SlotStates()[s].generation.load();
      if (st == SND_BANK_STATE_UNLOADED) continue;
      any = true;
      (void)m->BeginUnload(HandleRef{s, gen}, SND_UNLOAD_STOP_AND_UNLOAD, false);
    }
    if (!any) return;
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }
}

}  // namespace

SND_TEST(BankSchedulerThreads, LoadUnloadWithWorkersAndIoThread) {
  Rig rig;
  SND_REQUIRE(rig.ok);
  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> registered{0};
  std::atomic<bool> chaosStop{false};
  std::thread core(CoreLoop, rig.m, std::ref(stop), std::ref(registered));
  // Failure injection while everything runs: some reads fail, a bank sometimes cannot be opened.
  std::thread chaos([&] {
    testsupport::Rng rng(0xC4A05ull);
    while (!chaosStop.load()) {
      if (rng.Below(4) == 0) rig.fs.FailNthRead(rng.Below(6));
      rig.fs.FailOpen("C.sndb", rng.Below(3) == 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    rig.fs.ClearFailures();
  });
  std::thread g1(GameLoop, rig.m, 0x5EEDull, 600, &registered, 40u);
  std::thread g2(GameLoop, rig.m, 0xBEEFull, 600, &registered, 40u);
  g1.join();
  g2.join();
  chaosStop.store(true);
  chaos.join();
  // Loads still succeed after the faults stopped.
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  HandleRef base{};
  while (std::chrono::steady_clock::now() < until) {
    base = rig.m->RequestLoad("Base.sndb");
    if (rig.m->State(base) == SND_BANK_STATE_LOADED) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  SND_CHECK_EQ(rig.m->State(base), SND_BANK_STATE_LOADED);
  UnloadEverything(rig.m);
  stop.store(true, std::memory_order_release);
  core.join();
  for (std::uint32_t s = 0; s < rig.m->MaxBanks(); ++s) {
    const std::uint32_t st = rig.m->SlotStates()[s].state.load();
    SND_CHECK_MSG(st == SND_BANK_STATE_UNLOADED || st == SND_BANK_STATE_FAILED, "slot %u state %u", s, st);
  }
  SND_CHECK(registered.load() >= 40u);
  snd::test::Note("%u registrations, %llu file requests", registered.load(),
                  static_cast<unsigned long long>(rig.io.Stats().filesCompleted));
  rig.Teardown();
  SND_CHECK_EQ(rig.alloc.Live(), 0u);
  SND_CHECK_EQ(rig.alloc.BadFrees(), 0u);
}

// IoScheduler::Shutdown while game threads keep loading: every bank ends in a
// final state (no LOADING left once the jobs drained), nothing hangs or leaks.
SND_TEST(BankSchedulerThreads, SchedulerShutdownRacesWithLoads) {
  Rig rig;
  SND_REQUIRE(rig.ok);
  rig.fs.SetDefaultLatency(4);
  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> registered{0};
  std::thread core(CoreLoop, rig.m, std::ref(stop), std::ref(registered));
  std::thread g1(GameLoop, rig.m, 0x1234ull, 400, &registered, 0u);
  std::thread g2(GameLoop, rig.m, 0x4321ull, 400, &registered, 0u);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  rig.io.Shutdown();  // while loads are in flight
  g1.join();
  g2.join();
  rig.workers.WaitIdle();
  stop.store(true, std::memory_order_release);
  core.join();
  rig.m->Update();
  rig.workers.WaitIdle();
  rig.m->Update();
  for (std::uint32_t s = 0; s < rig.m->MaxBanks(); ++s) {
    const std::uint32_t st = rig.m->SlotStates()[s].state.load();
    SND_CHECK_MSG(st != SND_BANK_STATE_LOADING, "slot %u still LOADING after the scheduler shut down", s);
  }
  // A new load fails at once (the scheduler accepts nothing).
  const HandleRef late = rig.m->RequestLoad("late.sndb");
  rig.m->Update();
  SND_CHECK_EQ(rig.m->State(late), SND_BANK_STATE_FAILED);
  rig.Teardown();
  SND_CHECK_EQ(rig.alloc.Live(), 0u);
}
