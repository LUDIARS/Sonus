// Independent concurrency re-check of a228470 with real threads (IoScheduler IO
// thread, jobs::WorkerPool, memory FS latency / faults, game threads, a core
// thread that releases - or sometimes aborts - unloads). TSan, repeated.
//
// Phase 1 (chaos): game threads load / unload / reload at once (same and other
//   paths of A, dependants B -> A, C -> B, D -> {A, C}); the core thread checks
//   while running: no bank registered twice, no dependant registered while a
//   dependency is not registered, no refused Release.
// Phase 2 (decided outcome): many rounds of "A loaded -> UnloadBank(A) ->
//   immediately reload A (random path) and request B, C, D" with the core only
//   releasing (no aborts, no faults): every one of them must end LOADED - in
//   particular no SND_ERR_BANK_DEPENDENCY while the dependency is coming.
// End: nothing stuck LOADING after releasing, documented teardown, no leak.
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
  const char* dep1;
  const char* dep2;
};
const FileDef kFiles[] = {
    {"a.sndb", "A", nullptr, nullptr},   {"mods/a.sndb", "A", nullptr, nullptr}, {"b.sndb", "B", "A", nullptr},
    {"c.sndb", "C", "B", nullptr},       {"d.sndb", "D", "A", "C"},            {"e.sndb", "E", nullptr, nullptr},
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
      b.contentHash = 0x5A;
      for (const char* d : {kFiles[i].dep1, kFiles[i].dep2}) {
        if (d != nullptr) b.deps.push_back(bf::Dependency{HashId(d), 0, 0x5A});
      }
      fs.AddFile(kFiles[i].path, b.Build());
      fs.SetLatency(kFiles[i].path, i % 3);
    }
    // Phase 2 sentinel: an earlier request whose read is held while the test
    // thread issues a round's requests, so the manager never sees "nothing is
    // loading" between two of them (it cannot know requests not made yet).
    fixture::Bank sentinel = fixture::RichBank();
    sentinel.bankId = HashId("Sentinel");
    fs.AddFile("sentinel.sndb", sentinel.Build());
    snd::jobs::WorkerPoolConfig wc;
    wc.workers = 3;
    snd::io::IoSchedulerConfig ic;
    ic.maxStreams = 2;
    ic.filePieceBytes = 1200;
    ic.fileQueueCapacity = 3;
    ic.pollIntervalNs = 50000;
    ic.bytesPerWindow = 0;
    BankManagerDesc d;
    d.allocator = &alloc;
    d.scheduler = &io;
    d.jobs = &workers;
    d.maxBanks = 10;
    ok = workers.Init(alloc, wc) && pool.Init(alloc, 32u << 10, 16u << 10, 2) && io.Init(alloc, fs, pool, ic) &&
         io.Start() && BankManager::Create(d, &m) == SND_OK;
  }
};

struct Stats {
  std::atomic<std::uint32_t> registered{0}, duplicate{0}, depMissing{0}, refusedRelease{0}, aborts{0};
};

void CoreLoop(BankManager* m, std::atomic<bool>& stop, std::atomic<bool>& allowAbort, Stats& st) {
  std::map<std::uint32_t, std::uint32_t> live;
  // Core's epoch: a slot is released only some time after it became UNLOADING,
  // so reloads and their dependants really wait for it (the a228470 window).
  std::map<std::uint32_t, std::chrono::steady_clock::time_point> since;
  testsupport::Rng rng(0xFEEDull);
  snd::cmd::BankRegister reg{};
  while (!stop.load(std::memory_order_acquire)) {
    m->Update();
    while (m->PopRegistration(&reg)) {
      for (const auto& kv : live) {
        if (kv.first != reg.bank.slot && kv.second == reg.bankId) st.duplicate.fetch_add(1);
      }
      if (const PreparedBank* pb = AsPrepared(reg.prepared)) {
        for (std::uint32_t d = 0; d < pb->dependencyCount; ++d) {
          if (live.count(pb->dependencies[d].slot) == 0) st.depMissing.fetch_add(1);
        }
      }
      live[reg.bank.slot] = reg.bankId;
      m->MarkRegistered(reg.bank);
      st.registered.fetch_add(1);
    }
    for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
      const std::uint32_t state = m->SlotStates()[s].state.load();
      const HandleRef h{s, m->SlotStates()[s].generation.load()};
      if (state != SND_BANK_STATE_UNLOADING || live.count(s) == 0) {
        since.erase(s);
        continue;
      }
      const auto now = std::chrono::steady_clock::now();
      if (since.emplace(s, now).second || now - since[s] < std::chrono::milliseconds(30)) continue;
      since.erase(s);
      if (allowAbort.load() && rng.Below(10) == 0) {
        m->AbortUnload(h);
        st.aborts.fetch_add(1);
        continue;
      }
      const snd_result r = m->Release(h);
      if (r == SND_OK) live.erase(s);
      if (r == SND_ERR_BANK_IN_USE) st.refusedRelease.fetch_add(1);
    }
    std::this_thread::yield();
  }
}

void ChaosGame(BankManager* m, std::uint64_t seed, int ops) {
  testsupport::Rng rng(seed);
  std::vector<HandleRef> hs;
  for (int i = 0; i < ops; ++i) {
    const std::uint32_t what = rng.Below(10);
    if (what < 5) {
      const HandleRef h = m->RequestLoad(kFiles[rng.Below(kFileCount)].path);
      if (!h.IsNone()) hs.push_back(h);
    } else if (what < 8 && !hs.empty()) {
      (void)m->BeginUnload(hs[rng.Below(static_cast<std::uint32_t>(hs.size()))],
                           rng.Below(2) ? SND_UNLOAD_STOP_AND_UNLOAD : SND_UNLOAD_IF_UNUSED, false);
    } else {
      // Unload A then reload it and a dependant at once (the a228470 case).
      const HandleRef a = m->FindBank(HashId("A"));
      if (!a.IsNone()) (void)m->BeginUnload(a, SND_UNLOAD_STOP_AND_UNLOAD, false);
      hs.push_back(m->RequestLoad(rng.Below(2) ? "a.sndb" : "mods/a.sndb"));
      hs.push_back(m->RequestLoad(rng.Below(2) ? "b.sndb" : "d.sndb"));
    }
    if (i % 4 == 0) std::this_thread::sleep_for(std::chrono::microseconds(150));
  }
}

bool WaitFinal(BankManager* m, HandleRef h, std::chrono::seconds limit) {
  const auto until = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < until) {
    if (m->State(h) != SND_BANK_STATE_LOADING) return true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return false;
}

void UnloadEverything(BankManager* m) {
  for (int round = 0; round < 4000; ++round) {
    bool any = false;
    for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
      const std::uint32_t st = m->SlotStates()[s].state.load();
      if (st == SND_BANK_STATE_UNLOADED) continue;
      any = true;
      (void)m->BeginUnload(HandleRef{s, m->SlotStates()[s].generation.load()}, SND_UNLOAD_STOP_AND_UNLOAD, false);
    }
    if (!any) return;
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }
}

}  // namespace

SND_TEST(IndepReload11Threads, UnloadReloadWithDependantsUnderContention) {
  Rig rig;
  SND_REQUIRE(rig.ok);
  std::atomic<bool> stop{false}, allowAbort{true}, chaosStop{false};
  Stats st;
  std::thread core(CoreLoop, rig.m, std::ref(stop), std::ref(allowAbort), std::ref(st));
  // ---- Phase 1 ----
  std::thread chaos([&] {
    testsupport::Rng rng(0xD00Dull);
    while (!chaosStop.load()) {
      if (rng.Below(4) == 0) rig.fs.FailNthRead(rng.Below(10));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    rig.fs.ClearFailures();
  });
  std::thread g1(ChaosGame, rig.m, 0x61ull, 800);
  std::thread g2(ChaosGame, rig.m, 0x62ull, 800);
  std::thread g3(ChaosGame, rig.m, 0x63ull, 800);
  g1.join();
  g2.join();
  g3.join();
  chaosStop.store(true);
  chaos.join();
  allowAbort.store(false);
  UnloadEverything(rig.m);
  // ---- Phase 2: decided outcome ----
  testsupport::Rng rng(0x77ull);
  std::uint32_t rounds = 0, depFailures = 0, otherFailures = 0, stuck = 0;
  for (int round = 0; round < 40; ++round) {
    // All of this round's requests are issued while the sentinel's read is held
    // (property unchanged: once they are all known, a dependency that comes must
    // never make its dependant fail).
    auto holdSentinel = [&] {
      rig.fs.Hold("sentinel.sndb");
      (void)rig.m->RequestLoad("sentinel.sndb");
    };
    auto releaseSentinel = [&] { rig.fs.Release("sentinel.sndb"); };
    const HandleRef a = rig.m->RequestLoad("a.sndb");
    if (!WaitFinal(rig.m, a, std::chrono::seconds(10)) || rig.m->State(a) != SND_BANK_STATE_LOADED) {
      ++otherFailures;
      UnloadEverything(rig.m);
      continue;
    }
    holdSentinel();  // before the unload: the old A cannot be released earlier either
    SND_CHECK_EQ(rig.m->BeginUnload(a, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
    const HandleRef a2 = rig.m->RequestLoad(rng.Below(2) ? "a.sndb" : "mods/a.sndb");
    std::vector<HandleRef> deps;
    const char* order[3] = {"b.sndb", "c.sndb", "d.sndb"};
    const std::uint32_t rot = rng.Below(3);
    for (std::uint32_t k = 0; k < 3; ++k) deps.push_back(rig.m->RequestLoad(order[(k + rot) % 3]));
    releaseSentinel();
    for (const HandleRef& h : deps) {
      if (!WaitFinal(rig.m, h, std::chrono::seconds(10))) {
        ++stuck;
        continue;
      }
      if (rig.m->State(h) == SND_BANK_STATE_FAILED) {
        if (rig.m->Result(h) == SND_ERR_BANK_DEPENDENCY) ++depFailures;
        else ++otherFailures;
        snd::test::Note("round %d: %s failed: %s", round, "dependant", rig.m->FailureReason(h));
      }
    }
    if (!WaitFinal(rig.m, a2, std::chrono::seconds(10))) ++stuck;
    if (rig.m->State(a2) != SND_BANK_STATE_LOADED) ++otherFailures;
    ++rounds;
    UnloadEverything(rig.m);
  }
  SND_CHECK_EQ(depFailures, 0u);
  SND_CHECK_EQ(otherFailures, 0u);
  SND_CHECK_EQ(stuck, 0u);
  stop.store(true, std::memory_order_release);
  core.join();
  // ---- End ----
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
    SND_CHECK_MSG(rig.m->SlotStates()[s].state.load() != SND_BANK_STATE_LOADING, "slot %u stuck LOADING", s);
  }
  SND_CHECK_EQ(st.duplicate.load(), 0u);
  SND_CHECK_EQ(st.depMissing.load(), 0u);
  SND_CHECK_EQ(st.refusedRelease.load(), 0u);
  snd::test::Note("%u registrations, %u aborts, %u decided rounds", st.registered.load(), st.aborts.load(), rounds);
  SND_CHECK(rounds >= 30u);
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
