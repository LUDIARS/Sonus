// Independent concurrency test: BankManager (Scheduler mode) + io::IoScheduler
// (IO thread) + jobs::WorkerPool (background lane) + memory FS with latency and
// read failures. Run under TSan and repeated (ctest --repeat until-fail:20).
//
// Checked while it runs (core thread):
//  - every registration's PreparedBank is the file's bytes (no torn / stale
//    image after cancel + reuse of slots and FileRequests);
//  - never two registered banks with the same bankId + language at once
//    (D-50, including two paths of the same bank loaded concurrently);
//  - BankSlotState generations stay below 2^31 and are never 0 (D-46).
// At the end: every slot UNLOADED / FAILED, documented teardown, no leak.
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
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
  std::string path;
  std::string bank;
};
const FileDef kFiles[] = {{"a.sndb", "A"},   {"b.sndb", "B"},         {"c.sndb", "C"},
                          {"d.sndb", "D"},   {"dup/one.sndb", "Dup"}, {"dup/two.sndb", "Dup"},
                          {"e.sndb", "E"},   {"big.sndb", "Big"}};
constexpr std::uint32_t kFileCount = sizeof(kFiles) / sizeof(kFiles[0]);

std::vector<std::uint8_t> MakeImage(const std::string& bank, std::uint32_t extra) {
  fixture::Bank b = fixture::RichBank();
  b.bankId = HashId(bank);
  b.contentHash = 0x5151;
  std::vector<std::uint8_t> pad(extra);
  for (std::uint32_t k = 0; k < extra; ++k) pad[k] = static_cast<std::uint8_t>(k * 7u + b.bankId);
  b.Blob(SND_FOURCC('X', 'P', 'A', 'D'), pad);  // unknown chunk: ignored by the parser, but read and checksummed
  return b.Build();
}

struct Rig {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::jobs::WorkerPool workers;
  snd::io::StreamPool pool;
  snd::io::IoScheduler io;
  BankManager* m = nullptr;
  std::map<std::uint32_t, std::vector<std::uint8_t>> imageOf;  // bankId -> bytes (Dup paths are identical)
  bool ok = false;
  Rig() {
    fs.SetDefaultLatency(1);
    for (std::uint32_t i = 0; i < kFileCount; ++i) {
      const std::uint32_t id = HashId(kFiles[i].bank);
      if (imageOf.find(id) == imageOf.end()) imageOf[id] = MakeImage(kFiles[i].bank, kFiles[i].bank == "Big" ? 40000 : 900 * i);
      fs.AddFile(kFiles[i].path, imageOf[id]);
      fs.SetLatency(kFiles[i].path, i % 4);
    }
    snd::jobs::WorkerPoolConfig wc;
    wc.workers = 3;
    snd::io::IoSchedulerConfig ic;
    ic.maxStreams = 2;
    ic.filePieceBytes = 600;
    ic.fileQueueCapacity = 2;  // Submit is refused often: retry path under contention
    ic.maxInFlight = 3;
    ic.pollIntervalNs = 50000;
    ic.bytesPerWindow = 0;
    BankManagerDesc d;
    d.allocator = &alloc;
    d.scheduler = &io;
    d.jobs = &workers;
    d.maxBanks = 5;  // fewer slots than files: LIMIT_REACHED and slot reuse
    ok = workers.Init(alloc, wc) && pool.Init(alloc, 32u << 10, 16u << 10, 2) && io.Init(alloc, fs, pool, ic) &&
         io.Start() && BankManager::Create(d, &m) == SND_OK;
  }
};

struct CoreStats {
  std::atomic<std::uint32_t> registered{0};
  std::atomic<std::uint32_t> badImage{0};
  std::atomic<std::uint32_t> duplicateRegistered{0};
  std::atomic<std::uint32_t> badGeneration{0};
};

void CoreLoop(Rig& rig, std::atomic<bool>& stop, CoreStats& st) {
  BankManager* m = rig.m;
  std::map<std::uint32_t, std::uint64_t> live;  // slot -> (bankId << 32 | languageId) of registered banks
  snd::cmd::BankRegister reg{};
  while (!stop.load(std::memory_order_acquire)) {
    m->Update();
    while (m->PopRegistration(&reg)) {
      const PreparedBank* pb = AsPrepared(reg.prepared);
      if (pb == nullptr) {
        st.badImage.fetch_add(1);
        continue;
      }
      const auto it = rig.imageOf.find(pb->bankId);
      if (it == rig.imageOf.end() || pb->view.fileBytes() != it->second.size() ||
          std::memcmp(pb->view.file(), it->second.data(), it->second.size()) != 0) {
        st.badImage.fetch_add(1);
      }
      const std::uint64_t key = (std::uint64_t{pb->bankId} << 32) | pb->languageId;
      for (const auto& kv : live) {
        if (kv.second == key) st.duplicateRegistered.fetch_add(1);
      }
      live[reg.bank.slot] = key;
      m->MarkRegistered(reg.bank);
      st.registered.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
      const std::uint32_t state = m->SlotStates()[s].state.load();
      const std::uint32_t gen = m->SlotStates()[s].generation.load();
      if (gen == 0u || gen >= 0x80000000u) st.badGeneration.fetch_add(1);
      if (state == SND_BANK_STATE_UNLOADING && live.count(s) != 0) {
        if (m->Release(HandleRef{s, gen}) == SND_OK) live.erase(s);
      }
    }
    std::this_thread::yield();
  }
}

// At least `ops` operations, and (bounded by 15 s) until `minRegistrations`
// registrations happened so that a slow machine still runs the whole path.
void GameLoop(BankManager* m, std::uint64_t seed, int ops, const std::atomic<std::uint32_t>* registered,
              std::uint32_t minRegistrations) {
  testsupport::Rng rng(seed);
  std::vector<HandleRef> handles;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < ops || (registered->load() < minRegistrations &&
                              std::chrono::steady_clock::now() - start < std::chrono::seconds(15));
       ++i) {
    const std::uint32_t pick = rng.Below(kFileCount);
    const std::uint32_t what = rng.Below(10);
    if (what < 6) {
      snd_result r = SND_OK;
      const HandleRef h = m->RequestLoad(kFiles[pick].path.c_str(), &r);
      if (!h.IsNone()) handles.push_back(h);
    } else if (what < 9 && !handles.empty()) {
      const HandleRef h = handles[rng.Below(static_cast<std::uint32_t>(handles.size()))];
      (void)m->BeginUnload(h, rng.Below(2) ? SND_UNLOAD_STOP_AND_UNLOAD : SND_UNLOAD_IF_UNUSED, false);
    } else if (!handles.empty()) {
      const HandleRef h = handles.back();
      (void)m->State(h);
      (void)m->Result(h);
      (void)m->FailureReason(h);
      (void)m->FindBank(HashId("Dup"));
    }
    if (i % 4 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

}  // namespace

SND_TEST(IndepBankIo11Threads, ContentAndDuplicateInvariantsUnderContention) {
  Rig rig;
  SND_REQUIRE(rig.ok);
  std::atomic<bool> stop{false};
  std::atomic<bool> chaosStop{false};
  CoreStats st;
  std::thread core(CoreLoop, std::ref(rig), std::ref(stop), std::ref(st));
  std::thread chaos([&] {
    testsupport::Rng rng(0xFA11ull);
    while (!chaosStop.load()) {
      if (rng.Below(3) == 0) rig.fs.FailNthRead(rng.Below(8));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    rig.fs.ClearFailures();
  });
  std::thread g1(GameLoop, rig.m, 0x1111ull, 700, &st.registered, 60u);
  std::thread g2(GameLoop, rig.m, 0x2222ull, 700, &st.registered, 60u);
  std::thread g3(GameLoop, rig.m, 0x3333ull, 700, &st.registered, 60u);
  g1.join();
  g2.join();
  g3.join();
  chaosStop.store(true);
  chaos.join();
  // Faults are gone: both Dup paths requested at once, exactly one of them loads.
  const HandleRef one = rig.m->RequestLoad("dup/one.sndb");
  const HandleRef two = rig.m->RequestLoad("dup/two.sndb");
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  auto settled = [&](HandleRef h) {
    const snd_bank_state s = rig.m->State(h);
    return h.IsNone() || s == SND_BANK_STATE_LOADED || s == SND_BANK_STATE_FAILED || s == SND_BANK_STATE_UNLOADED;
  };
  while ((!settled(one) || !settled(two)) && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const int loadedDup = (rig.m->State(one) == SND_BANK_STATE_LOADED) + (rig.m->State(two) == SND_BANK_STATE_LOADED);
  SND_CHECK_MSG(loadedDup <= 1, "both paths of the same bank are LOADED");
  // Unload everything.
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
  SND_CHECK_EQ(st.badImage.load(), 0u);
  SND_CHECK_EQ(st.duplicateRegistered.load(), 0u);
  SND_CHECK_EQ(st.badGeneration.load(), 0u);
  SND_CHECK(st.registered.load() >= 60u);
  for (std::uint32_t s = 0; s < rig.m->MaxBanks(); ++s) {
    const std::uint32_t state = rig.m->SlotStates()[s].state.load();
    SND_CHECK_MSG(state == SND_BANK_STATE_UNLOADED, "slot %u state %u", s, state);
  }
  snd::test::Note("%u registrations, %llu file requests, %llu reads", st.registered.load(),
                  static_cast<unsigned long long>(rig.io.Stats().filesCompleted),
                  static_cast<unsigned long long>(rig.io.Stats().readsCompleted));
  // Documented teardown: IoScheduler::Shutdown, drain jobs, Destroy, io.Destroy.
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

// Shutdown of the scheduler while game threads load and a core thread pumps,
// then Destroy right after the jobs drained: no hang, no LOADING left, no leak.
SND_TEST(IndepBankIo11Threads, ShutdownThenDestroyWhileGameThreadsLoad) {
  for (int round = 0; round < 3; ++round) {
    Rig rig;
    SND_REQUIRE(rig.ok);
    std::atomic<bool> stop{false};
    CoreStats st;
    std::thread core(CoreLoop, std::ref(rig), std::ref(stop), std::ref(st));
    std::thread g1(GameLoop, rig.m, 0xA0ull + round, 300, &st.registered, 0u);
    std::thread g2(GameLoop, rig.m, 0xB0ull + round, 300, &st.registered, 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(3 + 4 * round));
    rig.io.Shutdown();
    g1.join();
    g2.join();
    rig.workers.WaitIdle();
    stop.store(true, std::memory_order_release);
    core.join();
    rig.m->Update();
    for (std::uint32_t s = 0; s < rig.m->MaxBanks(); ++s) {
      const std::uint32_t state = rig.m->SlotStates()[s].state.load();
      SND_CHECK_MSG(state != SND_BANK_STATE_LOADING, "round %d slot %u LOADING after shutdown", round, s);
    }
    SND_CHECK_EQ(st.badImage.load(), 0u);
    SND_CHECK_EQ(st.duplicateRegistered.load(), 0u);
    BankManager::Destroy(rig.m);
    rig.m = nullptr;
    rig.io.Destroy();
    rig.pool.Destroy();
    rig.workers.Shutdown();
    SND_CHECK_EQ(rig.alloc.Live(), 0u);
  }
}
