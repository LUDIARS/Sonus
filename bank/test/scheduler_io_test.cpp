// BankManager over the io module's IoScheduler (stage 1.5, A1-11): the
// default BankIoMode::Scheduler. Deterministic: the scheduler is driven with
// RunOnce() (no IO thread), jobs with the deterministic mock, the memory FS
// injects latency, holds and failures. Covers the load path, failures,
// cancellation at every IO step, IoScheduler::Shutdown while loading, and the
// order of Shutdown / Destroy (no leak, no access to freed memory under ASan).
#include <cstring>
#include <string>
#include <vector>

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

fixture::Bank Named(const std::string& name, std::uint64_t hash = 1, const char* dep = nullptr) {
  fixture::Bank b = fixture::RichBank();
  b.bankId = HashId(name);
  b.contentHash = hash;
  if (dep != nullptr) b.deps.push_back(bf::Dependency{HashId(dep), 0, 1});
  return b;
}

struct Env {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  snd::io::StreamPool pool;
  snd::io::IoScheduler io;
  BankManager* m = nullptr;
  std::uint32_t reserved = 0;
  explicit Env(std::uint32_t maxBanks = 8, std::uint32_t piece = 4096, std::uint32_t fileQueue = 32) {
    SND_CHECK(pool.Init(alloc, 64u << 10, 16u << 10, 4));
    snd::io::IoSchedulerConfig c;
    c.maxStreams = 4;
    c.filePieceBytes = piece;
    c.fileQueueCapacity = fileQueue;
    c.bytesPerWindow = 0;  // unlimited: the test controls progress with the FS mock
    SND_CHECK(io.Init(alloc, fs, pool, c));
    BankManagerDesc d;
    d.allocator = &alloc;
    d.scheduler = &io;  // ioMode: Scheduler (the default)
    d.jobs = &jobs;
    d.maxBanks = maxBanks;
    if (BankManager::Create(d, &m) != SND_OK) m = nullptr;
  }
  ~Env() {
    io.Shutdown();
    BankManager::Destroy(m);
    io.Destroy();
    pool.Destroy();
  }
  // One round: IO step, manager, jobs, registrations.
  void Step() {
    io.RunOnce();
    m->Update();
    jobs.RunAll();
    m->Update();
    snd::cmd::BankRegister reg{};
    while (m->PopRegistration(&reg)) m->MarkRegistered(reg.bank);
  }
  void Settle(int n = 64) {
    for (int i = 0; i < n; ++i) Step();
  }
};

}  // namespace

SND_TEST(BankScheduler, DefaultModeNeedsTheScheduler) {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  BankManagerDesc d;
  d.allocator = &alloc;
  d.fileSystem = &fs;  // a file system alone is not enough: the default is BankIoMode::Scheduler
  d.jobs = &jobs;
  BankManager* m = nullptr;
  SND_CHECK(d.ioMode == BankIoMode::Scheduler);
  SND_CHECK_EQ(BankManager::Create(d, &m), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(m == nullptr);
  SND_CHECK_EQ(alloc.Live(), 0u);
}

SND_TEST(BankScheduler, LoadsThroughTheSchedulerInPieces) {
  Env e(8, 1000);  // pieces of 1000 bytes
  SND_REQUIRE(e.m != nullptr);
  const std::vector<std::uint8_t> img = Named("Base").Build();
  e.fs.AddFile("Base.sndb", img);
  e.fs.AddFile("A.sndb", Named("A", 1, "Base").Build());
  e.fs.SetDefaultLatency(2);
  const HandleRef a = e.m->RequestLoad("A.sndb");
  const HandleRef base = e.m->RequestLoad("Base.sndb");
  e.Settle();
  SND_CHECK_EQ(e.m->State(base), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(a), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->DependentCount(base), 1u);
  const PreparedBank* pb = e.m->Prepared(base);
  SND_REQUIRE(pb != nullptr);
  SND_CHECK(std::memcmp(pb->view.file(), img.data(), img.size()) == 0);
  const snd::io::IoStats st = e.io.Stats();
  // Every byte through the scheduler: (size query + read) per bank, read in pieces.
  SND_CHECK_EQ(st.filesCompleted, 4u);
  SND_CHECK(st.readsCompleted >= (img.size() + 999) / 1000);
  SND_CHECK_EQ(st.bytesCompleted, e.fs.BytesRead());
}

SND_TEST(BankScheduler, OpenAndReadFailuresFailTheBank) {
  Env e;
  e.fs.AddFile("ok.sndb", Named("Ok").Build());
  e.fs.AddFile("badread.sndb", Named("BadRead").Build());
  e.fs.FailReads("badread.sndb", 200, 300);
  const HandleRef missing = e.m->RequestLoad("missing.sndb");
  const HandleRef badRead = e.m->RequestLoad("badread.sndb");
  const HandleRef ok = e.m->RequestLoad("ok.sndb");
  e.Settle();
  SND_CHECK_EQ(e.m->State(missing), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(missing), SND_ERR_IO);
  SND_CHECK(std::strstr(e.m->FailureReason(missing), "cannot open") != nullptr);
  SND_CHECK_EQ(e.m->State(badRead), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(badRead), SND_ERR_IO);
  SND_CHECK_EQ(e.m->State(ok), SND_BANK_STATE_LOADED);
  // Retry in place after the fault is gone.
  e.fs.ClearFailures();
  const HandleRef retried = e.m->RequestLoad("badread.sndb");
  SND_CHECK(retried.slot == badRead.slot && retried.generation == badRead.generation);
  e.Settle();
  SND_CHECK_EQ(e.m->State(badRead), SND_BANK_STATE_LOADED);
}

SND_TEST(BankScheduler, SameBankFromAnotherPathIsAlreadyLoaded) {
  Env e;
  const std::vector<std::uint8_t> img = Named("Dup").Build();
  e.fs.AddFile("dup.sndb", img);
  e.fs.AddFile("copy/dup.sndb", img);
  const HandleRef a = e.m->RequestLoad("dup.sndb");
  e.Settle();
  const HandleRef b = e.m->RequestLoad("copy/dup.sndb");
  e.Settle();
  SND_CHECK_EQ(e.m->State(a), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(b), SND_ERR_BANK_ALREADY_LOADED);
}

// Cancel (BeginUnload of a loading bank) at every step of the IO path, with the
// read held in the FS: the slot is freed only after the request finished, the
// bank ends UNLOADED, nothing leaks, and the image is never written after free
// (ASan run).
SND_TEST(BankScheduler, CancelAtEveryStepWhileTheReadIsHeld) {
  for (int cancelAt = 0; cancelAt < 24; ++cancelAt) {
    Env e(4, 512);
    SND_REQUIRE(e.m != nullptr);
    const std::size_t baseline = e.alloc.Live();
    e.fs.AddFile("b.sndb", Named("B").Build());
    e.fs.SetLatency("b.sndb", 1);
    const HandleRef h = e.m->RequestLoad("b.sndb");
    for (int i = 0; i < cancelAt; ++i) e.Step();
    e.fs.Hold("b.sndb");  // the read in flight (if any) cannot complete
    for (int i = 0; i < 3; ++i) e.Step();
    const bool wasLoaded = e.m->State(h) == SND_BANK_STATE_LOADED;
    const snd_result r = e.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false);
    SND_CHECK_EQ(r, SND_OK);
    if (wasLoaded) SND_CHECK_EQ(e.m->Release(h), SND_OK);  // core's part after the epoch
    for (int i = 0; i < 8; ++i) e.Step();
    e.fs.Release("b.sndb");
    e.Settle(16);
    SND_CHECK_MSG(e.m->State(h) == SND_BANK_STATE_UNLOADED, "cancelAt %d: state %u", cancelAt, e.m->State(h));
    SND_CHECK_MSG(e.alloc.Live() == baseline, "cancelAt %d: %zu live, baseline %zu", cancelAt, e.alloc.Live(),
                  baseline);
    // The slot is reusable.
    const HandleRef again = e.m->RequestLoad("b.sndb");
    e.Settle();
    SND_CHECK_EQ(e.m->State(again), SND_BANK_STATE_LOADED);
  }
}

// IoScheduler::Shutdown while banks are loading: their requests end Cancelled,
// the banks FAILED (SND_ERR_IO), loaded banks stay; later loads fail at once.
SND_TEST(BankScheduler, SchedulerShutdownWhileLoading) {
  Env e;
  e.fs.AddFile("loaded.sndb", Named("Loaded").Build());
  e.fs.AddFile("held.sndb", Named("Held").Build());
  e.fs.AddFile("queued.sndb", Named("Queued").Build());
  const HandleRef loaded = e.m->RequestLoad("loaded.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(loaded), SND_BANK_STATE_LOADED);
  e.fs.Hold("held.sndb");
  const HandleRef held = e.m->RequestLoad("held.sndb");
  for (int i = 0; i < 4; ++i) e.Step();  // size known, the read is in flight and held
  const HandleRef queued = e.m->RequestLoad("queued.sndb");
  e.m->PumpIo();  // its size query is submitted, not served
  e.fs.Release("held.sndb");  // Shutdown waits for reads in flight
  e.io.Shutdown();
  e.Settle(4);
  SND_CHECK_EQ(e.m->State(loaded), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(held), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(held), SND_ERR_IO);
  SND_CHECK(std::strstr(e.m->FailureReason(held), "shut down") != nullptr);
  SND_CHECK_EQ(e.m->State(queued), SND_BANK_STATE_FAILED);
  const HandleRef late = e.m->RequestLoad("loaded2.sndb");
  e.Settle(2);
  SND_CHECK_EQ(e.m->State(late), SND_BANK_STATE_FAILED);
  SND_CHECK(std::strstr(e.m->FailureReason(late), "shut down") != nullptr);
  SND_CHECK_EQ(e.m->BeginUnload(loaded, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(e.m->Release(loaded), SND_OK);
}

// A full file queue makes Submit refuse: the manager retries at later PumpIo()s.
SND_TEST(BankScheduler, FullFileQueueIsRetried) {
  Env e(8, 4096, 2);  // 2 FileRequests at once
  for (int i = 0; i < 6; ++i) e.fs.AddFile("b" + std::to_string(i) + ".sndb", Named("B" + std::to_string(i)).Build());
  std::vector<HandleRef> hs;
  for (int i = 0; i < 6; ++i) hs.push_back(e.m->RequestLoad(("b" + std::to_string(i) + ".sndb").c_str()));
  e.Settle(128);
  for (const HandleRef& h : hs) SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_LOADED);
}
