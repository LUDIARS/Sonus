// Independent concurrency re-check of c65fd9a (decisions C, D, E) with real
// threads: IoScheduler IO thread, jobs::WorkerPool, memory FS faults, three game
// threads (RequestLoad of several paths of the same banks, BeginUnload in both
// modes), a core thread (register, release, sometimes AbortUnload instead of
// Release, as when IF_UNUSED finds references). TSan, repeated 20 times.
//
// Invariants while it runs (core thread):
//  - never two registered (LOADED / UNLOADING) slots of one bank (C, D);
//  - a dependant is never registered while its dependency is not registered (10.3);
//  - an UNLOADING slot whose Release is refused with BANK_IN_USE (a dependant
//    registered after the unload was accepted: the gap E closes) never occurs.
// At the end: after releasing every UNLOADING slot nothing stays LOADING
// (no permanent wait), everything unloads, no leak.
#include <atomic>
#include <chrono>
#include <map>
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

struct FileDef {
  const char* path;
  const char* bank;
  const char* dep;
};
const FileDef kFiles[] = {
    {"base.sndb", "Base", nullptr},     {"mods/base.sndb", "Base", nullptr}, {"child.sndb", "Child", "Base"},
    {"dlc/child.sndb", "Child", "Base"}, {"solo.sndb", "Solo", nullptr},     {"solo2.sndb", "Solo", nullptr},
    {"other.sndb", "Other", nullptr},
};
constexpr std::uint32_t kFileCount = sizeof(kFiles) / sizeof(kFiles[0]);

struct Rig {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::jobs::WorkerPool workers;
  snd::io::StreamPool pool;
  snd::io::IoScheduler io;
  BankManager* m = nullptr;
  bool ok = false;
  Rig() {
    for (std::uint32_t i = 0; i < kFileCount; ++i) {
      fixture::Bank b = fixture::RichBank();
      b.bankId = HashId(kFiles[i].bank);
      b.contentHash = 0x77;
      if (kFiles[i].dep != nullptr) b.deps.push_back(bf::Dependency{HashId(kFiles[i].dep), 0, 0x77});
      fs.AddFile(kFiles[i].path, b.Build());
      fs.SetLatency(kFiles[i].path, i % 3);
    }
    snd::jobs::WorkerPoolConfig wc;
    wc.workers = 3;
    snd::io::IoSchedulerConfig ic;
    ic.maxStreams = 2;
    ic.filePieceBytes = 900;
    ic.fileQueueCapacity = 3;
    ic.pollIntervalNs = 50000;
    ic.bytesPerWindow = 0;
    BankManagerDesc d;
    d.allocator = &alloc;
    d.scheduler = &io;
    d.jobs = &workers;
    d.maxBanks = 6;
    ok = workers.Init(alloc, wc) && pool.Init(alloc, 32u << 10, 16u << 10, 2) && io.Init(alloc, fs, pool, ic) &&
         io.Start() && BankManager::Create(d, &m) == SND_OK;
  }
};

struct Stats {
  std::atomic<std::uint32_t> registered{0};
  std::atomic<std::uint32_t> duplicate{0};
  std::atomic<std::uint32_t> depMissing{0};
  std::atomic<std::uint32_t> refusedRelease{0};
  std::atomic<std::uint32_t> aborts{0};
};

void CoreLoop(BankManager* m, std::atomic<bool>& stop, Stats& st) {
  std::map<std::uint32_t, std::uint32_t> live;  // slot -> bankId (registered by this thread)
  testsupport::Rng rng(0xC0DEull);
  snd::cmd::BankRegister reg{};
  while (!stop.load(std::memory_order_acquire)) {
    m->Update();
    while (m->PopRegistration(&reg)) {
      const PreparedBank* pb = AsPrepared(reg.prepared);
      for (const auto& kv : live) {
        if (kv.first != reg.bank.slot && kv.second == reg.bankId) st.duplicate.fetch_add(1);
      }
      if (pb != nullptr) {
        for (std::uint32_t d = 0; d < pb->dependencyCount; ++d) {
          if (live.count(pb->dependencies[d].slot) == 0) st.depMissing.fetch_add(1);
        }
      }
      live[reg.bank.slot] = reg.bankId;
      m->MarkRegistered(reg.bank);
      st.registered.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
      const std::uint32_t state = m->SlotStates()[s].state.load();
      const std::uint32_t gen = m->SlotStates()[s].generation.load();
      if (state != SND_BANK_STATE_UNLOADING || live.count(s) == 0) continue;
      const HandleRef h{s, gen};
      if (rng.Below(8) == 0) {  // the audio side refused (IF_UNUSED found references)
        m->AbortUnload(h);
        st.aborts.fetch_add(1);
        continue;
      }
      const snd_result r = m->Release(h);
      if (r == SND_OK) {
        live.erase(s);
      } else if (r == SND_ERR_BANK_IN_USE) {
        st.refusedRelease.fetch_add(1);
      }
    }
    std::this_thread::yield();
  }
}

void GameLoop(BankManager* m, std::uint64_t seed, int ops, const std::atomic<std::uint32_t>* registered,
              std::uint32_t minReg) {
  testsupport::Rng rng(seed);
  std::vector<HandleRef> hs;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < ops || (registered->load() < minReg &&
                              std::chrono::steady_clock::now() - start < std::chrono::seconds(15));
       ++i) {
    if (rng.Below(5) < 3) {
      const HandleRef h = m->RequestLoad(kFiles[rng.Below(kFileCount)].path);
      if (!h.IsNone()) hs.push_back(h);
    } else if (!hs.empty()) {
      (void)m->BeginUnload(hs[rng.Below(static_cast<std::uint32_t>(hs.size()))],
                           rng.Below(2) ? SND_UNLOAD_STOP_AND_UNLOAD : SND_UNLOAD_IF_UNUSED, false);
    }
    if (i % 4 == 0) std::this_thread::sleep_for(std::chrono::microseconds(150));
  }
}

}  // namespace

SND_TEST(IndepBankOrder11Threads, OrderWaitAndUnloadInvariants) {
  Rig rig;
  SND_REQUIRE(rig.ok);
  std::atomic<bool> stop{false}, chaosStop{false};
  Stats st;
  std::thread core(CoreLoop, rig.m, std::ref(stop), std::ref(st));
  std::thread chaos([&] {
    testsupport::Rng rng(0xBADull);
    while (!chaosStop.load()) {
      if (rng.Below(4) == 0) rig.fs.FailNthRead(rng.Below(10));
      rig.fs.FailOpen("solo2.sndb", rng.Below(3) == 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    rig.fs.ClearFailures();
  });
  std::thread g1(GameLoop, rig.m, 0x51ull, 600, &st.registered, 60u);
  std::thread g2(GameLoop, rig.m, 0x52ull, 600, &st.registered, 60u);
  std::thread g3(GameLoop, rig.m, 0x53ull, 600, &st.registered, 60u);
  g1.join();
  g2.join();
  g3.join();
  chaosStop.store(true);
  chaos.join();
  // Unload everything (dependants first by retrying), the core thread releases.
  for (int round = 0; round < 4000; ++round) {
    bool any = false;
    for (std::uint32_t s = 0; s < rig.m->MaxBanks(); ++s) {
      const std::uint32_t state = rig.m->SlotStates()[s].state.load();
      const std::uint32_t gen = rig.m->SlotStates()[s].generation.load();
      if (state == SND_BANK_STATE_UNLOADED) continue;
      any = true;
      (void)rig.m->BeginUnload(HandleRef{s, gen}, SND_UNLOAD_STOP_AND_UNLOAD, false);
    }
    if (!any) break;
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }
  stop.store(true, std::memory_order_release);
  core.join();
  // Release whatever the core left UNLOADING (aborted rounds) and settle: nothing may wait forever.
  for (int round = 0; round < 64; ++round) {
    for (std::uint32_t s = 0; s < rig.m->MaxBanks(); ++s) {
      const HandleRef h{s, rig.m->SlotStates()[s].generation.load()};
      const snd_bank_state state = rig.m->State(h);
      if (state == SND_BANK_STATE_UNLOADING) (void)rig.m->Release(h);
      if (state != SND_BANK_STATE_UNLOADED) (void)rig.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false);
    }
    rig.m->Update();
    snd::cmd::BankRegister reg{};
    while (rig.m->PopRegistration(&reg)) rig.m->MarkRegistered(reg.bank);
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  for (std::uint32_t s = 0; s < rig.m->MaxBanks(); ++s) {
    const std::uint32_t state = rig.m->SlotStates()[s].state.load();
    SND_CHECK_MSG(state != SND_BANK_STATE_LOADING, "slot %u stuck LOADING", s);
  }
  SND_CHECK_EQ(st.duplicate.load(), 0u);
  SND_CHECK_EQ(st.depMissing.load(), 0u);
  SND_CHECK_EQ(st.refusedRelease.load(), 0u);
  SND_CHECK(st.registered.load() >= 60u);
  snd::test::Note("%u registrations, %u aborted unloads", st.registered.load(), st.aborts.load());
  rig.io.Shutdown();
  rig.workers.WaitIdle();
  rig.m->Update();
  BankManager::Destroy(rig.m);
  rig.m = nullptr;
  rig.io.Destroy();
  rig.pool.Destroy();
  rig.workers.Shutdown();
  SND_CHECK_EQ(rig.alloc.Live(), 0u);
  SND_CHECK_EQ(rig.alloc.BadFrees(), 0u);
}
