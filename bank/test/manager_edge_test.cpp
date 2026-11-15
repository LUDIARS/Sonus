// BankManager edge cases (independent tester): cancellation at every point of
// the loading path, allocation failure at every n, the registration queue,
// dependencies, stale handles, languages and slot-state transitions.
//
// Tests whose name starts with "Defect_" fail on the current implementation
// on purpose: they document a defect found by this review (see the comment
// above each one for the minimal reproduction).
#include <cstring>
#include <string>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/bank/bank_manager.hpp"
#include "snd/id.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"
#include "test_support.hpp"

namespace bf = snd::bank_format;
namespace ch = bf::chunk;
using snd::HandleRef;
using snd::HashId;
using snd::mock::JobMode;
using namespace snd::bank;

namespace {

struct Env {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  BankManager* m = nullptr;
  std::size_t baseline = 0;

  explicit Env(JobMode mode = JobMode::Immediate, std::uint32_t maxBanks = 8, bool dev = false,
               std::uint32_t chunk = 1u << 20)
      : jobs(mode) {
    BankManagerDesc d;
    d.allocator = &alloc;
    d.fileSystem = &fs;
    d.jobs = &jobs;
    d.maxBanks = maxBanks;
    d.ioChunkBytes = chunk;
    d.prepare.devOverlay = dev;
    d.prepare.devReserve.bytes = 4096;
    if (BankManager::Create(d, &m) != SND_OK) m = nullptr;
    baseline = alloc.Live();
  }
  ~Env() { BankManager::Destroy(m); }

  void Settle(int n = 16) {
    snd::cmd::BankRegister reg{};
    for (int i = 0; i < n; ++i) {
      m->Update();
      jobs.RunAll();
      m->Update();
      while (m->PopRegistration(&reg)) m->MarkRegistered(reg.bank);
    }
  }
  // Pumps without popping registrations.
  void SettleNoPop(int n = 16) {
    for (int i = 0; i < n; ++i) {
      m->Update();
      jobs.RunAll();
      m->Update();
    }
  }
};

fixture::Bank SimpleBank(const char* name, std::uint64_t hash = 1) {
  fixture::Bank b;
  b.bankId = HashId(name);
  b.contentHash = hash;
  b.Table(ch::BUSS, 1, {fixture::U32(bf::col::BUSS_id, {HashId("Master")}), fixture::U32(bf::col::BUSS_parent, {bf::kNone})});
  return b;
}

void AddBank(Env& e, const std::string& path, const fixture::Bank& b) { e.fs.AddFile(path, b.Build()); }

fixture::Bank DependentBank(const char* name, const char* dep, std::uint64_t depHash, std::uint64_t hash = 2) {
  fixture::Bank b = SimpleBank(name, hash);
  b.deps.push_back(bf::Dependency{HashId(dep), 0, depHash});
  return b;
}

snd_bank_state StateOfSlot(const Env& e, HandleRef h) {
  return static_cast<snd_bank_state>(e.m->SlotStates()[h.slot].state.load());
}

}  // namespace

// ---------------------------------------------------------------------------
// Defect: cancelling a load whose read is in flight, when that read then
// fails, leaves the slot FAILED with the cancel mark set. The slot is never
// freed: RequestLoad skips cancelled slots, CheckUnload/BeginUnload answer
// SND_ERR_INVALID_HANDLE for it, Pump does not free FAILED slots. The slot
// (and its path allocation) leaks until Destroy, and the slot state goes
// UNLOADING -> FAILED (core would deliver on_bank_loaded(FAILED) for a bank
// the game already unloaded).
// Repro: maxBanks 1; FailReads + Hold(path); RequestLoad; PumpIo (read
// issued, pending); BeginUnload; Release(path); Update -> slot stuck.
// Code: bank_manager.cpp PumpIoLocked, `if (st == ReadStep::Failed) { Fail(...) }`
// ignores s.cancel (the Done branch checks it, the Failed branch does not).
// ---------------------------------------------------------------------------
SND_TEST(BankManagerEdge, Defect_CancelledLoadWhoseReadFailsLeaksTheSlot) {
  Env e(JobMode::Immediate, 1);
  SND_REQUIRE(e.m != nullptr);
  AddBank(e, "a.sndb", SimpleBank("A"));
  AddBank(e, "b.sndb", SimpleBank("B"));
  e.fs.FailReads("a.sndb");
  e.fs.Hold("a.sndb");
  const HandleRef h = e.m->RequestLoad("a.sndb");
  e.m->PumpIo();  // open + first read issued, pending (held)
  SND_REQUIRE_EQ(e.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(StateOfSlot(e, h), SND_BANK_STATE_UNLOADING);
  e.fs.Release("a.sndb");
  e.Settle(4);
  // The cancelled load must end UNLOADED, never FAILED.
  SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_UNLOADED);
  SND_CHECK_MSG(StateOfSlot(e, h) != SND_BANK_STATE_FAILED, "cancelled load reported FAILED");
  // And the only slot must be reusable.
  snd_result r = SND_OK;
  const HandleRef h2 = e.m->RequestLoad("b.sndb", &r);
  SND_CHECK_EQ(r, SND_OK);
  SND_CHECK(!h2.IsNone());
  e.Settle();
  if (!h2.IsNone()) SND_CHECK_EQ(e.m->State(h2), SND_BANK_STATE_LOADED);
}

// ---------------------------------------------------------------------------
// Defect: the registration queue is a ring of maxBanks entries, but a bank
// cancelled while READY (queued, not yet popped) leaves its entry in the ring.
// Further Decide() calls push past capacity and overwrite still-valid entries:
// a READY bank is then never handed to PopRegistration and stays LOADING forever.
// Repro: maxBanks 2; load X (slot0, queued), load Y (slot1, queued), unload Y
// (still queued), load W (slot1, queued -> overwrites X's entry). Pop drains
// W only; X is never registered.
// Code: bank_manager.cpp Decide(): `queue[(queueHead + queueCount) % maxBanks]`
// with queueCount growing beyond maxBanks; BeginUnload(Ready) frees the slot
// without removing its queue entry.
// ---------------------------------------------------------------------------
SND_TEST(BankManagerEdge, Defect_RegistrationLostAfterCancellingAQueuedBank) {
  Env e(JobMode::Immediate, 2);
  SND_REQUIRE(e.m != nullptr);
  AddBank(e, "x.sndb", SimpleBank("X"));
  AddBank(e, "y.sndb", SimpleBank("Y"));
  AddBank(e, "w.sndb", SimpleBank("W"));
  const HandleRef x = e.m->RequestLoad("x.sndb");
  const HandleRef y = e.m->RequestLoad("y.sndb");
  e.SettleNoPop(2);  // both READY, nothing popped yet
  SND_REQUIRE_EQ(e.m->BeginUnload(y, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  const HandleRef w = e.m->RequestLoad("w.sndb");
  SND_REQUIRE(!w.IsNone());
  e.Settle(4);
  SND_CHECK_EQ(e.m->State(w), SND_BANK_STATE_LOADED);
  SND_CHECK_MSG(e.m->State(x) == SND_BANK_STATE_LOADED, "bank X was never handed to PopRegistration (state %u)",
                static_cast<unsigned>(e.m->State(x)));
}

// Same defect, longer churn: load / cancel the same path repeatedly while
// another bank waits in the queue. maxBanks 1 is enough to overflow.
SND_TEST(BankManagerEdge, Defect_QueueChurnWithOneSlot) {
  Env e(JobMode::Immediate, 3);
  SND_REQUIRE(e.m != nullptr);
  AddBank(e, "keep.sndb", SimpleBank("Keep"));
  AddBank(e, "churn.sndb", SimpleBank("Churn"));
  const HandleRef keep = e.m->RequestLoad("keep.sndb");
  e.SettleNoPop(1);
  for (int i = 0; i < 5; ++i) {
    const HandleRef c = e.m->RequestLoad("churn.sndb");
    e.SettleNoPop(1);
    SND_CHECK_EQ(e.m->BeginUnload(c, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  }
  e.Settle(4);
  SND_CHECK_MSG(e.m->State(keep) == SND_BANK_STATE_LOADED, "queued bank lost after churn (state %u)",
                static_cast<unsigned>(e.m->State(keep)));
}

// ---------------------------------------------------------------------------
// Cancellation at every point of the pipeline (manual jobs, IO latency, small
// chunks): no leak, no double free, slot reusable, never FAILED / LOADED.
// ---------------------------------------------------------------------------
SND_TEST(BankManagerEdge, CancelAtEveryStep) {
  const std::vector<std::uint8_t> image = fixture::RichBank().Build();
  for (int devOverlay = 0; devOverlay < 2; ++devOverlay) {
    for (int cancelAt = 0; cancelAt < 40; ++cancelAt) {
      Env e(JobMode::Manual, 2, devOverlay != 0, 256);
      SND_REQUIRE(e.m != nullptr);
      e.fs.AddFile("r.sndb", image);
      e.fs.SetLatency("r.sndb", 2);
      const HandleRef h = e.m->RequestLoad("r.sndb");
      int step = 0;
      bool cancelled = false;
      snd::cmd::BankRegister reg{};
      for (int round = 0; round < 200 && !cancelled; ++round) {
        // One micro step per iteration: PumpIo, run one job, Pump, pop, mark.
        switch (round % 4) {
          case 0: e.m->PumpIo(); break;
          case 1: e.jobs.RunNext(); break;
          case 2: e.m->Pump(); break;
          default:
            if (e.m->PopRegistration(&reg)) e.m->MarkRegistered(reg.bank);
            break;
        }
        if (step++ == cancelAt) {
          const snd_bank_state before = e.m->State(h);
          const snd_result r = e.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false);
          SND_CHECK_MSG(r == SND_OK, "cancelAt %d state %u: BeginUnload %d", cancelAt, before, r);
          cancelled = true;
          if (before == SND_BANK_STATE_LOADED || e.m->State(h) == SND_BANK_STATE_UNLOADING) {
            // Registered banks need Release (core's epoch).
            for (int k = 0; k < 8; ++k) {
              e.m->Update();
              e.jobs.RunAll();
            }
            if (e.m->State(h) == SND_BANK_STATE_UNLOADING) (void)e.m->Release(h);
          }
        }
      }
      for (int k = 0; k < 400; ++k) {
        e.m->Update();
        e.jobs.RunAll();
        e.m->Update();
      }
      if (e.m->State(h) == SND_BANK_STATE_UNLOADING) (void)e.m->Release(h);
      SND_CHECK_MSG(e.m->State(h) == SND_BANK_STATE_UNLOADED, "cancelAt %d: state %u", cancelAt,
                    static_cast<unsigned>(e.m->State(h)));
      SND_CHECK_MSG(e.alloc.Live() == e.baseline, "cancelAt %d: %zu live allocations (baseline %zu)", cancelAt,
                    e.alloc.Live(), e.baseline);
      SND_CHECK_EQ(e.alloc.BadFrees(), 0u);
      SND_CHECK_EQ(e.fs.OpenFiles(), 0u);
      // Slot is reusable: load again to completion.
      const HandleRef again = e.m->RequestLoad("r.sndb");
      SND_CHECK(!again.IsNone());
      e.Settle(400);
      SND_CHECK_EQ(e.m->State(again), SND_BANK_STATE_LOADED);
    }
  }
}

// Re-request at every step after a cancel: the cancelled slot must not be
// handed back, and both slots end consistently.
SND_TEST(BankManagerEdge, ReRequestAfterCancelAtEveryStep) {
  const std::vector<std::uint8_t> image = SimpleBank("A").Build();
  for (int cancelAt = 0; cancelAt < 12; ++cancelAt) {
    Env e(JobMode::Manual, 4, false, 64);
    e.fs.AddFile("a.sndb", image);
    e.fs.SetLatency("a.sndb", 1);
    const HandleRef h = e.m->RequestLoad("a.sndb");
    for (int i = 0; i < cancelAt; ++i) {
      if (i % 2 == 0) e.m->Update(); else e.jobs.RunNext();
    }
    (void)e.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false);
    const HandleRef h2 = e.m->RequestLoad("a.sndb");
    SND_CHECK(!h2.IsNone());
    SND_CHECK_MSG(h2.Value() != h.Value(), "cancelAt %d: cancelled handle handed back", cancelAt);
    e.Settle();
    if (e.m->State(h) == SND_BANK_STATE_UNLOADING) (void)e.m->Release(h);
    SND_CHECK_EQ(e.m->State(h2), SND_BANK_STATE_LOADED);
    SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_UNLOADED);
    SND_CHECK_EQ(e.m->BeginUnload(h2, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
    SND_CHECK_EQ(e.m->Release(h2), SND_OK);
    SND_CHECK_EQ(e.alloc.Live(), e.baseline);
  }
}

// ---------------------------------------------------------------------------
// Allocation failure injected at every n (n = 0, 1, 2, ...) for a load with a
// dependency and the dev overlay: either loads or fails cleanly, never leaks.
// ---------------------------------------------------------------------------
SND_TEST(BankManagerEdge, AllocationFailureAtEveryN) {
  fixture::Bank dep = SimpleBank("Dep", 77);
  fixture::Bank rich = fixture::RichBank();
  rich.deps.push_back(bf::Dependency{HashId("Dep"), 0, 77});
  bool sawSuccess = false;
  for (long n = 0; n < 200; ++n) {
    Env e(JobMode::Immediate, 4, true);
    SND_REQUIRE(e.m != nullptr);
    AddBank(e, "dep.sndb", dep);
    e.fs.AddFile("rich.sndb", rich.Build());
    const HandleRef d = e.m->RequestLoad("dep.sndb");
    const HandleRef r = e.m->RequestLoad("rich.sndb");
    e.alloc.FailAt(n);
    e.Settle();
    e.alloc.FailAt(-1);
    const snd_bank_state sr = e.m->State(r);
    SND_CHECK_MSG(sr == SND_BANK_STATE_LOADED || sr == SND_BANK_STATE_FAILED, "n=%ld: state %u", n, sr);
    if (sr == SND_BANK_STATE_FAILED) {
      // Retry must succeed once memory is back.
      e.m->RequestLoad("rich.sndb");
      e.m->RequestLoad("dep.sndb");
      e.Settle();
      SND_CHECK_MSG(e.m->State(r) == SND_BANK_STATE_LOADED, "n=%ld: retry state %u (%s)", n, e.m->State(r),
                    e.m->FailureReason(r));
    } else {
      sawSuccess = true;
    }
    for (HandleRef h : {r, d}) {
      if (e.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false) == SND_OK && e.m->State(h) == SND_BANK_STATE_UNLOADING) {
        SND_CHECK_EQ(e.m->Release(h), SND_OK);
      }
    }
    SND_CHECK_MSG(e.alloc.Live() == e.baseline, "n=%ld: %zu live (baseline %zu)", n, e.alloc.Live(), e.baseline);
    SND_CHECK_EQ(e.alloc.BadFrees(), 0u);
    if (sawSuccess && sr == SND_BANK_STATE_LOADED && n > 20) break;
  }
  SND_CHECK(sawSuccess);
}

// Create() with an allocation failure at each of its allocations: no leak.
SND_TEST(BankManagerEdge, CreateAllocationFailures) {
  for (long n = 0; n < 8; ++n) {
    testsupport::CountingAllocator a;
    snd::mock::MemoryFileSystem fs;
    snd::mock::DeterministicJobSystem jobs;
    BankManagerDesc d;
    d.allocator = &a;
    d.fileSystem = &fs;
    d.jobs = &jobs;
    a.FailAt(n);
    BankManager* m = nullptr;
    const snd_result r = BankManager::Create(d, &m);
    a.FailAt(-1);
    if (r == SND_OK) {
      BankManager::Destroy(m);
    } else {
      SND_CHECK(m == nullptr);
    }
    SND_CHECK_EQ(a.Live(), 0u);
    SND_CHECK_EQ(a.BadFrees(), 0u);
  }
}

// ---------------------------------------------------------------------------
// Dependencies
// ---------------------------------------------------------------------------
SND_TEST(BankManagerEdge, DependencyRequestedAfterTheDependentInTheSameFrame) {
  Env e;
  AddBank(e, "base.sndb", SimpleBank("Base", 5));
  AddBank(e, "lvl.sndb", DependentBank("Lvl", "Base", 5));
  const HandleRef l = e.m->RequestLoad("lvl.sndb");
  const HandleRef b = e.m->RequestLoad("base.sndb");
  e.Settle();
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(l), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->DependentCount(b), 1u);
}

SND_TEST(BankManagerEdge, DependencyStillLoadingThroughSlowIo) {
  Env e(JobMode::Manual, 4, false, 64);
  AddBank(e, "base.sndb", SimpleBank("Base", 5));
  AddBank(e, "lvl.sndb", DependentBank("Lvl", "Base", 5));
  e.fs.SetLatency("base.sndb", 6);
  const HandleRef b = e.m->RequestLoad("base.sndb");
  const HandleRef l = e.m->RequestLoad("lvl.sndb");
  e.Settle(40);
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_LOADED);
  SND_CHECK_MSG(e.m->State(l) == SND_BANK_STATE_LOADED, "dependent: %u (%s)", e.m->State(l), e.m->FailureReason(l));
}

SND_TEST(BankManagerEdge, MultipleDependentsAndUnloadOrder) {
  Env e;
  AddBank(e, "base.sndb", SimpleBank("Base", 5));
  AddBank(e, "a.sndb", DependentBank("A", "Base", 5));
  AddBank(e, "b.sndb", DependentBank("B", "Base", 5));
  const HandleRef base = e.m->RequestLoad("base.sndb");
  const HandleRef a = e.m->RequestLoad("a.sndb");
  const HandleRef b = e.m->RequestLoad("b.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(a), SND_BANK_STATE_LOADED);
  SND_REQUIRE_EQ(e.m->State(b), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->DependentCount(base), 2u);
  // Dependency first: refused in both modes, also by Release.
  SND_CHECK_EQ(e.m->CheckUnload(base, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(e.m->BeginUnload(base, SND_UNLOAD_IF_UNUSED, false), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(e.m->Release(base), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(e.m->State(base), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->BeginUnload(a, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(e.m->Release(a), SND_OK);
  SND_CHECK_EQ(e.m->DependentCount(base), 1u);
  SND_CHECK_EQ(e.m->BeginUnload(base, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(e.m->BeginUnload(b, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  // While the dependent is UNLOADING (not released yet) the dependency stays protected.
  SND_CHECK_EQ(e.m->BeginUnload(base, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(e.m->Release(b), SND_OK);
  SND_CHECK_EQ(e.m->BeginUnload(base, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(e.m->Release(base), SND_OK);
  SND_CHECK_EQ(e.alloc.Live(), e.baseline);
}

SND_TEST(BankManagerEdge, DependencyFailedMissingSelfAndCycle) {
  Env e;
  // Failed dependency (corrupt file).
  std::vector<std::uint8_t> bad = SimpleBank("Broken", 9).Build();
  bad[200] ^= 0x55;
  e.fs.AddFile("broken.sndb", bad);
  AddBank(e, "useBroken.sndb", DependentBank("UseBroken", "Broken", 9));
  const HandleRef ub = e.m->RequestLoad("useBroken.sndb");
  const HandleRef br = e.m->RequestLoad("broken.sndb");
  // Self dependency: validation rejects it.
  AddBank(e, "self.sndb", DependentBank("Self", "Self", 1));
  const HandleRef self = e.m->RequestLoad("self.sndb");
  // 3-cycle.
  AddBank(e, "c1.sndb", DependentBank("C1", "C2", 2));
  AddBank(e, "c2.sndb", DependentBank("C2", "C3", 2));
  AddBank(e, "c3.sndb", DependentBank("C3", "C1", 2));
  const HandleRef c1 = e.m->RequestLoad("c1.sndb");
  const HandleRef c2 = e.m->RequestLoad("c2.sndb");
  const HandleRef c3 = e.m->RequestLoad("c3.sndb");
  e.Settle();
  SND_CHECK_EQ(e.m->State(br), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->State(ub), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(ub), SND_ERR_BANK_DEPENDENCY);
  SND_CHECK_EQ(e.m->State(self), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(self), SND_ERR_CORRUPT);
  for (HandleRef h : {c1, c2, c3}) {
    SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_FAILED);
    SND_CHECK_EQ(e.m->Result(h), SND_ERR_BANK_DEPENDENCY);
  }
  // Nothing loaded: memory back to baseline + failed slots hold no images.
  for (HandleRef h : {ub, br, self, c1, c2, c3}) SND_CHECK_EQ(e.m->BeginUnload(h, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  SND_CHECK_EQ(e.alloc.Live(), e.baseline);
}

// A dependency that is being unloaded is not "loaded" (10.3: registration
// requires the dependency): the dependent must not register against it.
SND_TEST(BankManagerEdge, DependentDoesNotRegisterAgainstAnUnloadingDependency) {
  Env e;
  AddBank(e, "base.sndb", SimpleBank("Base", 5));
  AddBank(e, "lvl.sndb", DependentBank("Lvl", "Base", 5));
  const HandleRef base = e.m->RequestLoad("base.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->BeginUnload(base, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  const HandleRef l = e.m->RequestLoad("lvl.sndb");
  e.Settle();
  SND_CHECK(e.m->State(l) != SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->Release(base), SND_OK);
}

// ---------------------------------------------------------------------------
// Slots, handles
// ---------------------------------------------------------------------------
SND_TEST(BankManagerEdge, SamePathSlotsFullAndStaleHandles) {
  Env e(JobMode::Immediate, 2);
  AddBank(e, "a.sndb", SimpleBank("A"));
  AddBank(e, "b.sndb", SimpleBank("B"));
  AddBank(e, "c.sndb", SimpleBank("C"));
  snd_result r = SND_OK;
  const HandleRef a1 = e.m->RequestLoad("a.sndb", &r);
  const HandleRef a2 = e.m->RequestLoad("a.sndb", &r);
  SND_CHECK_EQ(a1.Value(), a2.Value());
  const HandleRef b = e.m->RequestLoad("b.sndb", &r);
  SND_CHECK_EQ(r, SND_OK);
  const HandleRef c = e.m->RequestLoad("c.sndb", &r);
  SND_CHECK_EQ(r, SND_ERR_LIMIT_REACHED);
  SND_CHECK(c.IsNone());
  e.Settle();
  SND_CHECK_EQ(e.m->RequestLoad("a.sndb", &r).Value(), a1.Value());  // loaded: existing handle
  SND_CHECK_EQ(e.m->BeginUnload(a1, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(e.m->Release(a1), SND_OK);
  const HandleRef c2 = e.m->RequestLoad("c.sndb", &r);
  SND_CHECK_EQ(r, SND_OK);
  SND_CHECK_EQ(c2.slot, a1.slot);
  SND_CHECK(c2.generation != a1.generation);
  e.Settle();
  // Stale handle: every entry point refuses it and nothing touches the new bank.
  SND_CHECK_EQ(e.m->State(a1), SND_BANK_STATE_UNLOADED);
  SND_CHECK_EQ(e.m->Result(a1), SND_ERR_INVALID_HANDLE);
  SND_CHECK(e.m->Prepared(a1) == nullptr);
  SND_CHECK_EQ(e.m->CheckUnload(a1, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(e.m->BeginUnload(a1, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(e.m->Release(a1), SND_ERR_INVALID_HANDLE);
  e.m->AbortUnload(a1);
  e.m->MarkRegistered(a1);
  SND_CHECK_EQ(e.m->State(c2), SND_BANK_STATE_LOADED);
  // Out-of-range and none handles.
  SND_CHECK_EQ(e.m->State(HandleRef{99, 1}), SND_BANK_STATE_UNLOADED);
  SND_CHECK_EQ(e.m->State(HandleRef{0, 0}), SND_BANK_STATE_UNLOADED);
  SND_CHECK_EQ(e.m->Release(HandleRef{0xFFFFFFFFu, 0xFFFFFFFFu}), SND_ERR_INVALID_HANDLE);
  (void)b;
  SND_CHECK(e.m->RequestLoad(nullptr, &r).IsNone());
  SND_CHECK_EQ(r, SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(e.m->RequestLoad("", &r).IsNone());
}

SND_TEST(BankManagerEdge, FailedThenRetrySameHandle) {
  Env e;
  e.fs.FailOpen("a.sndb");
  AddBank(e, "a.sndb", SimpleBank("A"));
  const HandleRef h = e.m->RequestLoad("a.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(h), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(h), SND_ERR_IO);
  SND_CHECK(e.m->FailureReason(h)[0] != '\0');
  e.m->SlotStates()[h.slot].notified.store(1);  // what core's Update does after on_bank_loaded(FAILED)
  e.fs.ClearFailures();
  const HandleRef h2 = e.m->RequestLoad("a.sndb");
  SND_CHECK_EQ(h2.Value(), h.Value());
  SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_LOADING);
  e.Settle();
  SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->Result(h), SND_OK);
  // The second result (LOADED) must be deliverable through the notification
  // block: "notified" is per transition (notify.hpp). Report what the
  // manager leaves there (spec gap: who resets it on an in-place retry).
  snd::test::Note("after in-place retry: notified=%u generation=%u (same generation as the failed load)",
                  e.m->SlotStates()[h.slot].notified.load(), e.m->SlotStates()[h.slot].generation.load());
}

SND_TEST(BankManagerEdge, UnloadModes) {
  Env e;
  AddBank(e, "a.sndb", SimpleBank("A"));
  const HandleRef h = e.m->RequestLoad("a.sndb");
  e.Settle();
  SND_CHECK_EQ(e.m->CheckUnload(h, SND_UNLOAD_IF_UNUSED, true), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(e.m->CheckUnload(h, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  SND_CHECK_EQ(e.m->CheckUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, true), SND_OK);
  SND_CHECK_EQ(e.m->CheckUnload(h, 7u, false), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(e.m->BeginUnload(h, SND_UNLOAD_IF_UNUSED, true), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->BeginUnload(h, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_UNLOADING);
  SND_CHECK_EQ(e.m->BeginUnload(h, SND_UNLOAD_IF_UNUSED, false), SND_OK);  // idempotent
  e.m->AbortUnload(h);
  SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, true), SND_OK);
  SND_CHECK_EQ(e.m->Release(h), SND_OK);
  SND_CHECK_EQ(e.m->Release(h), SND_ERR_INVALID_HANDLE);
}

// Slot states only follow LOADING -> (LOADED | FAILED) -> UNLOADING -> UNLOADED
// (a cancelled load: LOADING -> UNLOADING -> UNLOADED), and LOADED appears only
// after the registration was applied.
SND_TEST(BankManagerEdge, SlotStateTransitionsFollowTheContract) {
  Env e(JobMode::Manual, 2, false, 128);
  e.fs.AddFile("r.sndb", fixture::RichBank().Build());
  e.fs.SetLatency("r.sndb", 1);
  const HandleRef h = e.m->RequestLoad("r.sndb");
  std::vector<snd_bank_state> seen{StateOfSlot(e, h)};
  snd::cmd::BankRegister reg{};
  bool popped = false;
  for (int i = 0; i < 200; ++i) {
    e.m->PumpIo();
    e.jobs.RunNext();
    e.m->Pump();
    const snd_bank_state s = StateOfSlot(e, h);
    if (s != seen.back()) seen.push_back(s);
    SND_CHECK_MSG(s != SND_BANK_STATE_LOADED || popped, "LOADED before the registration was applied");
    if (!popped && e.m->PopRegistration(&reg)) {
      popped = true;
      SND_CHECK_EQ(StateOfSlot(e, h), SND_BANK_STATE_LOADING);
      SND_CHECK(AsPrepared(reg.prepared) != nullptr);
      SND_CHECK_EQ(reg.bankId, AsPrepared(reg.prepared)->bankId);
      e.m->MarkRegistered(reg.bank);
      seen.push_back(StateOfSlot(e, h));
    }
  }
  SND_CHECK_EQ(seen.size(), 2u);
  if (seen.size() == 2) {
    SND_CHECK_EQ(seen[0], SND_BANK_STATE_LOADING);
    SND_CHECK_EQ(seen[1], SND_BANK_STATE_LOADED);
  }
  SND_CHECK(!e.m->PopRegistration(&reg));  // exactly one registration
}

// Two different paths that hold the same bank (same bankId, e.g. a copy):
// both become LOADED and FindBank sees one of them. Reported as a spec gap
// (snd_load_bank_async only promises "same path -> same handle").
SND_TEST(BankManagerEdge, SameBankUnderTwoPaths) {
  Env e;
  const std::vector<std::uint8_t> img = SimpleBank("Dup").Build();
  e.fs.AddFile("dup.sndb", img);
  e.fs.AddFile("copy/dup.sndb", img);
  const HandleRef a = e.m->RequestLoad("dup.sndb");
  const HandleRef b = e.m->RequestLoad("copy/dup.sndb");
  e.Settle();
  snd::test::Note("same bankId under two paths: states %u / %u (two registrations of bank %08x)", e.m->State(a),
                  e.m->State(b), HashId("Dup"));
}

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------
namespace {
fixture::Bank VoiceBank(const char* lang) {
  fixture::Bank b = SimpleBank("Voice", HashId(lang));
  b.flags |= bf::kFileLanguageBank;
  b.languageId = HashId(lang);
  return b;
}
}  // namespace

SND_TEST(BankManagerEdge, LanguageSwitchUnknownAndDottedDirectories) {
  Env e;
  AddBank(e, "dir.v2/voice.ja.sndb", VoiceBank("ja"));
  AddBank(e, "dir.v2/voice.en.sndb", VoiceBank("en"));
  const HandleRef ja = e.m->RequestLoad("dir.v2/voice.ja.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(ja), SND_BANK_STATE_LOADED);
  LanguageSwitch plan[4];
  snd_result r = SND_OK;
  // Unknown language: NOT_FOUND, current language unchanged.
  const snd::Id before = e.m->CurrentLanguage();
  SND_CHECK_EQ(e.m->SetLanguage(HashId("xx"), plan, 4, &r), 0u);
  SND_CHECK_EQ(r, SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(e.m->CurrentLanguage(), before);
  SND_CHECK_EQ(e.m->DeclareLanguage("en"), SND_OK);
  const std::uint32_t n = e.m->SetLanguage(HashId("en"), plan, 4, &r);
  SND_CHECK_EQ(r, SND_OK);
  SND_REQUIRE_EQ(n, 1u);
  SND_CHECK(std::strcmp(plan[0].newPath, "dir.v2/voice.en.sndb") == 0);
  SND_CHECK_EQ(plan[0].oldBank.Value(), ja.Value());
  e.Settle();
  // The old language stays loaded until the caller unloads it (voices keep playing, 10.4).
  SND_CHECK_EQ(e.m->State(ja), SND_BANK_STATE_LOADED);
  SND_CHECK(!e.m->FindBank(HashId("Voice"), HashId("en")).IsNone());
  SND_CHECK(!e.m->FindBank(HashId("Voice"), HashId("ja")).IsNone());
  // Capacity 0 with a non-empty plan: LIMIT_REACHED.
  SND_CHECK_EQ(e.m->PlanLanguageSwitch(HashId("ja"), nullptr, 0, &r), 0u);
  SND_CHECK_EQ(r, SND_ERR_LIMIT_REACHED);
}

// ---------------------------------------------------------------------------
// Defect: switching again while the previous switch is still loading.
// bank_manager.hpp documents PlanLanguageSwitch as "one entry per
// loaded/loading language bank of another language", but loading banks are
// skipped (only Ready/Registered with a learnt baseLen are planned). After
// ja (loaded) -> en (loading) -> ja, the second plan is empty: nothing tells the
// caller to drop the en bank that is still loading, so it finishes loading and
// stays resident next to ja while CurrentLanguage() is ja.
// Code: bank_manager.cpp PlanLanguageSwitch: `if (s.stage != Stage::Ready &&
// s.stage != Stage::Registered) continue;` and baseLen is only learnt after
// validation (LearnLanguagePath).
// ---------------------------------------------------------------------------
SND_TEST(BankManagerEdge, Defect_SwitchBackWhileTheNewLanguageIsStillLoading) {
  Env e(JobMode::Manual);
  AddBank(e, "voice.ja.sndb", VoiceBank("ja"));
  AddBank(e, "voice.en.sndb", VoiceBank("en"));
  e.fs.Hold("voice.en.sndb");
  const HandleRef ja = e.m->RequestLoad("voice.ja.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(ja), SND_BANK_STATE_LOADED);
  SND_REQUIRE_EQ(e.m->DeclareLanguage("en"), SND_OK);
  LanguageSwitch plan[4];
  snd_result r = SND_OK;
  SND_REQUIRE_EQ(e.m->SetLanguage(HashId("en"), plan, 4, &r), 1u);
  e.Settle(2);  // en still loading (held IO)
  const HandleRef en = e.m->RequestLoad("voice.en.sndb");
  SND_REQUIRE_EQ(e.m->State(en), SND_BANK_STATE_LOADING);
  LanguageSwitch back[4];
  const std::uint32_t n = e.m->SetLanguage(HashId("ja"), back, 4, &r);
  SND_CHECK_EQ(e.m->CurrentLanguage(), HashId("ja"));
  bool plansEn = false;
  for (std::uint32_t i = 0; i < n; ++i) plansEn = plansEn || back[i].oldBank.Value() == en.Value();
  SND_CHECK_MSG(plansEn, "the loading 'en' bank is not in the switch-back plan (%u entries)", n);
  e.fs.Release("voice.en.sndb");
  e.Settle();
}

