// Regression (acceptance check of c65fd9a): reloading a bank while its old copy
// is UNLOADING, together with a bank that depends on it. The dependant must
// wait with the reloaded copy (D-50), not fail with a false
// SND_ERR_BANK_DEPENDENCY; really missing dependencies and cycles still fail;
// after AbortUnload the dependant resolves against the old copy. Both IO paths
// (IoScheduler and DirectFileSystem); nothing may stay LOADING for good.
#include <cstring>
#include <memory>
#include <string>

#include "fixture_writer.hpp"
#include "snd/bank/bank_manager.hpp"
#include "snd/id.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"
#include "test_support.hpp"

namespace bf = snd::bank_format;
using snd::HandleRef;
using snd::HashId;
using namespace snd::bank;

namespace {

fixture::Bank Bank(const char* name, std::uint64_t hash, const char* dep = nullptr, std::uint64_t depHash = 0) {
  fixture::Bank b = fixture::RichBank();
  b.bankId = HashId(name);
  b.contentHash = hash;
  if (dep != nullptr) b.deps.push_back(bf::Dependency{HashId(dep), 0, depHash});
  return b;
}

struct Env {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs{snd::mock::JobMode::Manual};
  snd::io::StreamPool pool;
  snd::io::IoScheduler io;
  BankManager* m = nullptr;
  BankIoMode mode;
  explicit Env(BankIoMode ioMode) : mode(ioMode) {
    BankManagerDesc d;
    d.allocator = &alloc;
    d.jobs = &jobs;
    d.maxBanks = 8;
    d.ioMode = ioMode;
    if (ioMode == BankIoMode::Scheduler) {
      SND_CHECK(pool.Init(alloc, 64u << 10, 16u << 10, 4));
      snd::io::IoSchedulerConfig c;
      c.maxStreams = 4;
      c.filePieceBytes = 1024;
      c.bytesPerWindow = 0;
      SND_CHECK(io.Init(alloc, fs, pool, c));
      d.scheduler = &io;
    } else {
      d.fileSystem = &fs;
    }
    if (BankManager::Create(d, &m) != SND_OK) m = nullptr;
  }
  ~Env() {
    io.Shutdown();
    jobs.RunAll();
    if (m != nullptr) {
      m->Update();
      BankManager::Destroy(m);
    }
    io.Destroy();
    pool.Destroy();
  }
  void Settle(int n = 128) {
    snd::cmd::BankRegister reg{};
    for (int i = 0; i < n; ++i) {
      if (mode == BankIoMode::Scheduler) io.RunOnce();
      m->Update();
      jobs.RunAll();
      m->Update();
      while (m->PopRegistration(&reg)) m->MarkRegistered(reg.bank);
    }
  }
  // No slot may stay LOADING once nothing waits for a Release any more.
  void ExpectNothingLoading(const char* what) {
    for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
      SND_CHECK_MSG(m->SlotStates()[s].state.load() != SND_BANK_STATE_LOADING, "%s: slot %u still LOADING", what, s);
    }
  }
};

const BankIoMode kModes[] = {BankIoMode::Scheduler, BankIoMode::DirectFileSystem};
const char* ModeName(BankIoMode m) { return m == BankIoMode::Scheduler ? "scheduler" : "direct"; }

}  // namespace

SND_TEST(BankDepUnloading, DependantWaitsWithTheReloadedCopy) {
  for (BankIoMode mode : kModes) {
    Env e(mode);
    SND_REQUIRE(e.m != nullptr);
    e.fs.AddFile("base.sndb", Bank("Base", 0x11).Build());
    e.fs.AddFile("level.sndb", Bank("Level", 0x22, "Base", 0x11).Build());
    e.fs.AddFile("deeper.sndb", Bank("Deeper", 0x33, "Level", 0x22).Build());
    const HandleRef old = e.m->RequestLoad("base.sndb");
    e.Settle();
    SND_REQUIRE_EQ(e.m->State(old), SND_BANK_STATE_LOADED);
    SND_REQUIRE_EQ(e.m->BeginUnload(old, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
    const HandleRef again = e.m->RequestLoad("base.sndb");
    const HandleRef level = e.m->RequestLoad("level.sndb");
    const HandleRef deeper = e.m->RequestLoad("deeper.sndb");  // a chain through the waiting copy
    e.Settle(256);
    SND_CHECK_MSG(e.m->State(level) == SND_BANK_STATE_LOADING, "%s: level %u (%d: %s)", ModeName(mode),
                  e.m->State(level), e.m->Result(level), e.m->FailureReason(level));
    SND_CHECK_EQ(e.m->State(deeper), SND_BANK_STATE_LOADING);
    SND_CHECK_EQ(e.m->State(again), SND_BANK_STATE_LOADING);
    SND_CHECK_EQ(e.m->Release(old), SND_OK);
    e.Settle(256);
    SND_CHECK_EQ(e.m->State(again), SND_BANK_STATE_LOADED);
    SND_CHECK_MSG(e.m->State(level) == SND_BANK_STATE_LOADED, "%s: level %u (%d: %s)", ModeName(mode),
                  e.m->State(level), e.m->Result(level), e.m->FailureReason(level));
    SND_CHECK_EQ(e.m->State(deeper), SND_BANK_STATE_LOADED);
    e.ExpectNothingLoading(ModeName(mode));
  }
}

// AbortUnload: the reloaded copy becomes a duplicate (ALREADY_LOADED); the
// dependant resolves against the old copy: registered if the hash matches,
// SND_ERR_BANK_DEPENDENCY otherwise.
SND_TEST(BankDepUnloading, AbortUnloadResolvesAgainstTheOldCopy) {
  for (BankIoMode mode : kModes) {
    for (const bool matching : {true, false}) {
      Env e(mode);
      SND_REQUIRE(e.m != nullptr);
      e.fs.AddFile("base.sndb", Bank("Base", 0x11).Build());
      e.fs.AddFile("level.sndb", Bank("Level", 0x22, "Base", matching ? 0x11 : 0x99).Build());
      const HandleRef old = e.m->RequestLoad("base.sndb");
      e.Settle();
      SND_REQUIRE_EQ(e.m->BeginUnload(old, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
      const HandleRef again = e.m->RequestLoad("base.sndb");
      const HandleRef level = e.m->RequestLoad("level.sndb");
      e.Settle(256);
      SND_CHECK_EQ(e.m->State(level), SND_BANK_STATE_LOADING);
      e.m->AbortUnload(old);  // the audio side refused (IF_UNUSED found references)
      e.Settle(256);
      SND_CHECK_EQ(e.m->State(old), SND_BANK_STATE_LOADED);
      SND_CHECK_EQ(e.m->State(again), SND_BANK_STATE_FAILED);
      SND_CHECK_EQ(e.m->Result(again), SND_ERR_BANK_ALREADY_LOADED);
      if (matching) {
        SND_CHECK_MSG(e.m->State(level) == SND_BANK_STATE_LOADED, "%s: level %u (%s)", ModeName(mode),
                      e.m->State(level), e.m->FailureReason(level));
        SND_CHECK_EQ(e.m->DependentCount(old), 1u);
      } else {
        SND_CHECK_EQ(e.m->State(level), SND_BANK_STATE_FAILED);
        SND_CHECK_EQ(e.m->Result(level), SND_ERR_BANK_DEPENDENCY);
      }
      e.ExpectNothingLoading(ModeName(mode));
    }
  }
}

// Unchanged: a dependency that is really missing, a dependency that is only
// UNLOADING (no copy coming) and a cycle still fail.
SND_TEST(BankDepUnloading, MissingDependenciesAndCyclesStillFail) {
  for (BankIoMode mode : kModes) {
    Env e(mode);
    SND_REQUIRE(e.m != nullptr);
    e.fs.AddFile("base.sndb", Bank("Base", 0x11).Build());
    e.fs.AddFile("orphan.sndb", Bank("Orphan", 0x44, "Nowhere", 0x1).Build());
    e.fs.AddFile("level.sndb", Bank("Level", 0x22, "Base", 0x11).Build());
    fixture::Bank c = Bank("C", 0x55, "D", 0x66);
    fixture::Bank dd = Bank("D", 0x66, "C", 0x55);
    e.fs.AddFile("c.sndb", c.Build());
    e.fs.AddFile("d.sndb", dd.Build());
    const HandleRef old = e.m->RequestLoad("base.sndb");
    e.Settle();
    SND_REQUIRE_EQ(e.m->BeginUnload(old, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
    const HandleRef level = e.m->RequestLoad("level.sndb");  // Base is going away, no copy requested
    const HandleRef orphan = e.m->RequestLoad("orphan.sndb");
    const HandleRef hc = e.m->RequestLoad("c.sndb");
    const HandleRef hd = e.m->RequestLoad("d.sndb");
    e.Settle(256);
    for (const HandleRef h : {level, orphan, hc, hd}) {
      SND_CHECK_MSG(e.m->State(h) == SND_BANK_STATE_FAILED && e.m->Result(h) == SND_ERR_BANK_DEPENDENCY,
                    "%s: slot %u state %u result %d", ModeName(mode), h.slot, e.m->State(h), e.m->Result(h));
    }
    SND_CHECK_EQ(e.m->Release(old), SND_OK);
    e.Settle();
    e.ExpectNothingLoading(ModeName(mode));
  }
}
