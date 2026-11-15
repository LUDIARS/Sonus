// BankManager: loading path (IO -> job -> registration), failures, dependencies,
// unload, language switch, dev override layer, memory accounting.
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
using namespace snd::bank;

namespace {

struct Env {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  BankManager* m = nullptr;

  explicit Env(snd::mock::JobMode mode = snd::mock::JobMode::Immediate, std::uint32_t maxBanks = 8,
               bool dev = false, std::uint32_t chunk = 1u << 20)
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
  }
  ~Env() { BankManager::Destroy(m); }

  // Pumps until `h` leaves LOADING (or `n` rounds), popping registrations.
  void Settle(int n = 32) {
    snd::cmd::BankRegister reg{};
    for (int i = 0; i < n; ++i) {
      m->Update();
      jobs.RunAll();
      m->Update();
      while (m->PopRegistration(&reg)) m->MarkRegistered(reg.bank);
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

}  // namespace

SND_TEST(BankManager, LoadsThroughIoJobAndRegistration) {
  Env env(snd::mock::JobMode::Manual);
  SND_REQUIRE(env.m != nullptr);
  env.fs.AddFile("Common.sndb", fixture::RichBank().Build());
  snd_result r = SND_ERR_IO;
  const HandleRef h = env.m->RequestLoad("Common.sndb", &r);
  SND_CHECK_EQ(r, SND_OK);
  SND_CHECK(!h.IsNone());
  SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_LOADING);
  // Same path -> same handle.
  SND_CHECK_EQ(env.m->RequestLoad("Common.sndb").Value(), h.Value());
  env.m->PumpIo();  // reads complete, job submitted
  SND_CHECK_EQ(env.jobs.Pending(snd::JobLane::Background), 1u);
  snd::cmd::BankRegister reg{};
  env.m->Pump();
  SND_CHECK(!env.m->PopRegistration(&reg));  // job not run yet
  SND_CHECK(env.jobs.RunNext(snd::JobLane::Background));
  env.m->Pump();
  SND_REQUIRE(env.m->PopRegistration(&reg));
  SND_CHECK_EQ(reg.bank.Value(), h.Value());
  SND_CHECK_EQ(reg.bankId, HashId("Common"));
  SND_CHECK_EQ(reg.languageId, 0u);
  const PreparedBank* pb = AsPrepared(reg.prepared);
  SND_REQUIRE(pb != nullptr);
  SND_CHECK_EQ(pb->view.Rows(ch::EVNT), 3u);
  SND_CHECK_EQ(pb->dspNeed.instanceBytes, 384u);
  SND_CHECK(!pb->view.writable());
  SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_LOADING);  // until the audio side applies it
  // The audio thread stores LOADED itself.
  env.m->SlotStates()[h.slot].state.store(SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(env.m->FindBank(HashId("Common")).Value(), h.Value());
  SND_CHECK_EQ(env.m->RequestLoad("Common.sndb").Value(), h.Value());
  // Unload: check, begin, release.
  SND_CHECK_EQ(env.m->CheckUnload(h, SND_UNLOAD_IF_UNUSED, true), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(env.m->CheckUnload(h, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  SND_CHECK_EQ(env.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, true), SND_OK);
  SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_UNLOADING);
  env.m->AbortUnload(h);
  SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(env.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(env.m->Release(h), SND_OK);
  SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_UNLOADED);
  SND_CHECK_EQ(env.m->Release(h), SND_ERR_INVALID_HANDLE);  // stale
  SND_CHECK_EQ(env.fs.OpenFiles(), 0u);
}

SND_TEST(BankManager, IoLatencyAndChunkedReads) {
  Env env(snd::mock::JobMode::Immediate, 4, false, 1000);  // 1000-byte reads
  env.fs.AddFile("A.sndb", fixture::RichBank().Build());
  env.fs.SetLatency("A.sndb", 3);
  const HandleRef h = env.m->RequestLoad("A.sndb");
  for (int i = 0; i < 3; ++i) {
    env.m->Update();
    SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_LOADING);
  }
  env.Settle(200);
  SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_LOADED);
  SND_CHECK(env.fs.ReadCalls() > 3u);
  // Held file: stays LOADING; unloading it cancels after the read completes.
  env.fs.AddFile("B.sndb", SimpleBank("B").Build());
  env.fs.Hold("B.sndb");
  const HandleRef b = env.m->RequestLoad("B.sndb");
  env.Settle(4);
  SND_CHECK_EQ(env.m->State(b), SND_BANK_STATE_LOADING);
  SND_CHECK_EQ(env.m->BeginUnload(b, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  env.Settle(2);
  SND_CHECK_EQ(env.m->State(b), SND_BANK_STATE_UNLOADING);  // read still in flight
  env.fs.Release("B.sndb");
  env.Settle(2);
  SND_CHECK_EQ(env.m->State(b), SND_BANK_STATE_UNLOADED);
  SND_CHECK(env.m->FindBank(HashId("B")).IsNone());
}

SND_TEST(BankManager, FailuresAreReportedAndDoNotLeak) {
  Env env;
  const std::size_t base = env.alloc.Live();
  // Missing file.
  HandleRef h = env.m->RequestLoad("missing.sndb");
  env.Settle(2);
  SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(env.m->Result(h), SND_ERR_IO);
  // Read error.
  env.fs.AddFile("io.sndb", fixture::RichBank().Build());
  env.fs.FailReads("io.sndb", 200, 300);
  h = env.m->RequestLoad("io.sndb");
  env.Settle(2);
  SND_CHECK_EQ(env.m->Result(h), SND_ERR_IO);
  SND_CHECK(std::strstr(env.m->FailureReason(h), "io.sndb") != nullptr);
  // Retry in place after the fault is gone: same handle, loads.
  env.fs.ClearFailures();
  SND_CHECK_EQ(env.m->RequestLoad("io.sndb").Value(), h.Value());
  env.Settle(2);
  SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_LOADED);
  // Corrupt bank: reason from validation.
  std::vector<std::uint8_t> bad = fixture::RichBank().Build();
  bad[bad.size() - 1] ^= 1;
  env.fs.AddFile("bad.sndb", bad);
  HandleRef c = env.m->RequestLoad("bad.sndb");
  env.Settle(2);
  SND_CHECK_EQ(env.m->Result(c), SND_ERR_CORRUPT);
  SND_CHECK(std::strstr(env.m->FailureReason(c), "checksum") != nullptr);
  // Platform mismatch.
  fixture::Bank w = SimpleBank("Win");
  w.platform = bf::kPlatformWindows;
  env.fs.AddFile("win.sndb", w.Build());
  HandleRef wh = env.m->RequestLoad("win.sndb");
  env.Settle(2);
  SND_CHECK_EQ(env.m->Result(wh), SND_ERR_VERSION_MISMATCH);
  // Empty file.
  env.fs.AddFile("empty.sndb", std::vector<std::uint8_t>{});
  HandleRef e = env.m->RequestLoad("empty.sndb");
  env.Settle(2);
  SND_CHECK_EQ(env.m->Result(e), SND_ERR_CORRUPT);
  // Failed slots are freed by unload; only the loaded bank stays allocated.
  for (HandleRef f : {c, wh, e, env.m->RequestLoad("missing.sndb")}) {
    SND_CHECK_EQ(env.m->BeginUnload(f, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  }
  SND_CHECK_EQ(env.m->BeginUnload(h, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  SND_CHECK_EQ(env.m->Release(h), SND_OK);
  env.Settle(1);  // the retried "missing.sndb" was cancelled while opening: freed by Pump
  SND_CHECK_EQ(env.alloc.Live(), base);
  SND_CHECK_EQ(env.alloc.BadFrees(), 0u);
  SND_CHECK_EQ(env.fs.OpenFiles(), 0u);
}

SND_TEST(BankManager, AllocationFailuresNeverLeak) {
  const std::vector<std::uint8_t> bytes = fixture::RichBank().Build();
  // Fail each allocation of a full load (dev overlay on) in turn.
  for (long n = 0; n < 12; ++n) {
    Env env(snd::mock::JobMode::Immediate, 4, true);
    SND_REQUIRE(env.m != nullptr);
    env.fs.AddFile("A.sndb", bytes);
    const std::size_t base = env.alloc.Live();
    env.alloc.FailAt(n);
    const HandleRef h = env.m->RequestLoad("A.sndb");
    env.Settle(2);
    env.alloc.FailAt(-1);
    const snd_bank_state st = env.m->State(h);
    if (h.IsNone()) {
      SND_CHECK_EQ(env.alloc.Live(), base);
      continue;
    }
    SND_CHECK(st == SND_BANK_STATE_LOADED || st == SND_BANK_STATE_FAILED);
    if (st == SND_BANK_STATE_FAILED) SND_CHECK_EQ(env.m->Result(h), SND_ERR_OUT_OF_BUDGET);
    SND_CHECK_EQ(env.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
    if (st == SND_BANK_STATE_LOADED) SND_CHECK_EQ(env.m->Release(h), SND_OK);
    SND_CHECK_EQ(env.alloc.Live(), base);
  }
  // Create itself under allocation failure.
  for (long n = 0; n < 6; ++n) {
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
    if (r == SND_OK) BankManager::Destroy(m);
    SND_CHECK_EQ(a.Live(), 0u);
  }
}

SND_TEST(BankManager, Dependencies) {
  Env env(snd::mock::JobMode::Manual);
  fixture::Bank base = SimpleBank("Base", 0xAAAA);
  fixture::Bank level = SimpleBank("Level", 0xBBBB);
  level.deps.push_back(bf::Dependency{HashId("Base"), 0, 0xAAAA});
  fixture::Bank stale = SimpleBank("Stale", 0xCCCC);
  stale.deps.push_back(bf::Dependency{HashId("Base"), 0, 0x1234});
  env.fs.AddFile("Base.sndb", base.Build());
  env.fs.AddFile("Level.sndb", level.Build());
  env.fs.AddFile("Stale.sndb", stale.Build());

  // Dependency not loaded: not registered, fails once nothing else is loading.
  HandleRef l = env.m->RequestLoad("Level.sndb");
  env.Settle(2);
  SND_CHECK_EQ(env.m->State(l), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(env.m->Result(l), SND_ERR_BANK_DEPENDENCY);

  // Both requested together; the dependant is validated first and waits.
  env.fs.SetLatency("Base.sndb", 5);
  l = env.m->RequestLoad("Level.sndb");
  const HandleRef b = env.m->RequestLoad("Base.sndb");
  snd::cmd::BankRegister reg{};
  std::vector<std::uint64_t> order;
  for (int i = 0; i < 20; ++i) {
    env.m->Update();
    env.jobs.RunAll();
    env.m->Update();
    while (env.m->PopRegistration(&reg)) {
      order.push_back(reg.bank.Value());
      env.m->MarkRegistered(reg.bank);
    }
  }
  SND_REQUIRE_EQ(order.size(), 2u);
  SND_CHECK_EQ(order[0], b.Value());  // dependency registered first
  SND_CHECK_EQ(order[1], l.Value());
  const PreparedBank* lp = env.m->Prepared(l);
  SND_REQUIRE(lp != nullptr);
  SND_CHECK_EQ(lp->dependencyCount, 1u);
  SND_CHECK_EQ(lp->dependencies[0].Value(), b.Value());
  SND_CHECK_EQ(env.m->DependentCount(b), 1u);

  // Built against another version of Base.
  const HandleRef s = env.m->RequestLoad("Stale.sndb");
  env.Settle(2);
  SND_CHECK_EQ(env.m->Result(s), SND_ERR_BANK_DEPENDENCY);
  SND_CHECK(std::strstr(env.m->FailureReason(s), "built as") != nullptr);

  // Base cannot be unloaded while Level is loaded (both modes).
  SND_CHECK_EQ(env.m->CheckUnload(b, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(env.m->BeginUnload(b, SND_UNLOAD_IF_UNUSED, false), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(env.m->BeginUnload(l, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  SND_CHECK_EQ(env.m->Release(l), SND_OK);
  SND_CHECK_EQ(env.m->DependentCount(b), 0u);
  SND_CHECK_EQ(env.m->BeginUnload(b, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  SND_CHECK_EQ(env.m->Release(b), SND_OK);
}

SND_TEST(BankManager, DependencyCycleFails) {
  Env env;
  fixture::Bank a = SimpleBank("CycA", 1);
  fixture::Bank b = SimpleBank("CycB", 2);
  a.deps.push_back(bf::Dependency{HashId("CycB"), 0, 2});
  b.deps.push_back(bf::Dependency{HashId("CycA"), 0, 1});
  env.fs.AddFile("a.sndb", a.Build());
  env.fs.AddFile("b.sndb", b.Build());
  const HandleRef ha = env.m->RequestLoad("a.sndb");
  const HandleRef hb = env.m->RequestLoad("b.sndb");
  env.Settle(4);
  SND_CHECK_EQ(env.m->Result(ha), SND_ERR_BANK_DEPENDENCY);
  SND_CHECK_EQ(env.m->Result(hb), SND_ERR_BANK_DEPENDENCY);
}

SND_TEST(BankManager, SlotsAndCancellation) {
  Env env(snd::mock::JobMode::Manual, 2);
  env.fs.AddFile("a.sndb", SimpleBank("a").Build());
  env.fs.AddFile("b.sndb", SimpleBank("b").Build());
  env.fs.AddFile("c.sndb", SimpleBank("c").Build());
  const HandleRef a = env.m->RequestLoad("a.sndb");
  const HandleRef b = env.m->RequestLoad("b.sndb");
  snd_result r = SND_OK;
  SND_CHECK(env.m->RequestLoad("c.sndb", &r).IsNone());
  SND_CHECK_EQ(r, SND_ERR_LIMIT_REACHED);
  // Cancel while the validation job is queued: the job is skipped, memory freed.
  env.m->Update();
  SND_CHECK_EQ(env.jobs.Pending(), 2u);
  const std::size_t live = env.alloc.Live();
  SND_CHECK_EQ(env.m->BeginUnload(a, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  env.jobs.RunAll();
  SND_CHECK_EQ(env.jobs.SkippedCount(), 1u);
  env.m->Update();
  SND_CHECK_EQ(env.m->State(a), SND_BANK_STATE_UNLOADED);
  SND_CHECK(env.alloc.Live() < live);
  // Cancel while queued for registration (never handed to core).
  SND_CHECK_EQ(env.m->State(b), SND_BANK_STATE_LOADING);
  SND_CHECK_EQ(env.m->BeginUnload(b, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  snd::cmd::BankRegister reg{};
  SND_CHECK(!env.m->PopRegistration(&reg));
  // Slots are free again; a reload gets a new generation.
  const HandleRef a2 = env.m->RequestLoad("a.sndb");
  SND_CHECK(a2.Value() != a.Value());
  SND_CHECK_EQ(env.m->State(a), SND_BANK_STATE_UNLOADED);
}

SND_TEST(BankManager, DspPoolHookRejects) {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  struct Pool {
    std::uint64_t free = 300;
  } pool;
  BankManagerDesc d;
  d.allocator = &alloc;
  d.fileSystem = &fs;
  d.jobs = &jobs;
  d.dsp.user = &pool;
  d.dsp.reserve = [](void* u, const bf::DspPoolNeed& n) {
    Pool* p = static_cast<Pool*>(u);
    if (n.instanceBytes > p->free) return false;
    p->free -= n.instanceBytes;
    return true;
  };
  d.dsp.release = [](void* u, const bf::DspPoolNeed& n) { static_cast<Pool*>(u)->free += n.instanceBytes; };
  BankManager* m = nullptr;
  SND_REQUIRE_EQ(BankManager::Create(d, &m), SND_OK);
  fs.AddFile("rich.sndb", fixture::RichBank().Build());  // needs 384
  HandleRef h = m->RequestLoad("rich.sndb");
  m->Update();
  m->Update();
  SND_CHECK_EQ(m->Result(h), SND_ERR_OUT_OF_BUDGET);
  pool.free = 1000;
  h = m->RequestLoad("rich.sndb");
  m->Update();
  m->Update();
  snd::cmd::BankRegister reg{};
  SND_REQUIRE(m->PopRegistration(&reg));
  SND_CHECK_EQ(pool.free, 1000u - 384u);
  SND_CHECK_EQ(m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(m->Release(h), SND_OK);
  SND_CHECK_EQ(pool.free, 1000u);
  BankManager::Destroy(m);
  SND_CHECK_EQ(alloc.Live(), 0u);
}

SND_TEST(BankManager, LanguageSwitch) {
  Env env;
  for (const char* lang : {"ja", "en"}) {
    fixture::Bank v = SimpleBank("Voice");
    v.flags = bf::kFileLanguageBank;
    v.languageId = HashId(lang);
    env.fs.AddFile(std::string("vo/Voice.") + lang + ".sndb", v.Build());
  }
  const HandleRef ja = env.m->RequestLoad("vo/Voice.ja.sndb");
  env.Settle(2);
  SND_REQUIRE_EQ(env.m->State(ja), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(env.m->Prepared(ja)->languageId, HashId("ja"));
  LanguageSwitch plan[4];
  snd_result r = SND_OK;
  // "en" is unknown until declared.
  SND_CHECK_EQ(env.m->PlanLanguageSwitch(HashId("en"), plan, 4, &r), 0u);
  SND_CHECK_EQ(r, SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(env.m->DeclareLanguage("en"), SND_OK);
  const std::uint32_t n = env.m->SetLanguage(HashId("en"), plan, 4, &r);
  SND_CHECK_EQ(r, SND_OK);
  SND_REQUIRE_EQ(n, 1u);
  SND_CHECK_EQ(plan[0].oldBank.Value(), ja.Value());
  SND_CHECK(std::strcmp(plan[0].newPath, "vo/Voice.en.sndb") == 0);
  SND_CHECK_EQ(env.m->CurrentLanguage(), HashId("en"));
  env.Settle(2);
  const HandleRef en = env.m->FindBank(HashId("Voice"), HashId("en"));
  SND_CHECK(!en.IsNone());
  SND_CHECK_EQ(env.m->State(en), SND_BANK_STATE_LOADED);
  // "ja" was learned from the loaded path: switching back plans the reverse.
  SND_CHECK_EQ(env.m->PlanLanguageSwitch(HashId("ja"), plan, 4, &r), 1u);  // the "en" bank
  SND_CHECK_EQ(plan[0].oldBank.Value(), en.Value());
  SND_CHECK(std::strcmp(plan[0].newPath, "vo/Voice.ja.sndb") == 0);
}

SND_TEST(BankManager, DevOverlayLayer) {
  Env env(snd::mock::JobMode::Immediate, 4, true);
  env.fs.AddFile("dev.sndb", fixture::RichBank().Build());  // RichBank is a dev build
  const HandleRef h = env.m->RequestLoad("dev.sndb");
  env.Settle(2);
  const PreparedBank* pb = env.m->Prepared(h);
  SND_REQUIRE(pb != nullptr);
  SND_REQUIRE(pb->view.writable());
  SND_CHECK(pb->overlay.base != nullptr);
  SND_CHECK_EQ(pb->overlay.spareBytes, 4096u);
  SND_CHECK_EQ(pb->view.Table(ch::BUSS)->capacity, 3u + 16u);
  SND_CHECK_EQ(pb->view.Table(ch::EVNT)->capacity, 3u + 64u);
  SND_CHECK_EQ(pb->view.Table(ch::CLIP)->capacity, 4u);
  // Values copied, absent columns materialised with defaults, writable.
  std::uint32_t* prio = pb->view.MutableColumn<col::EVNT_priority>();
  SND_REQUIRE(prio != nullptr);
  SND_CHECK_EQ(prio[0], 200u);
  float* retrig = pb->view.MutableColumn<col::EVNT_retriggerMs>();
  SND_REQUIRE(retrig != nullptr);
  SND_CHECK_EQ(retrig[1], 0.0f);
  SND_CHECK_EQ(pb->view.Column<col::EVNT_category>()[66], bf::kNone);  // reserve rows default
  prio[0] = 17;
  SND_CHECK_EQ(pb->view.Column<col::EVNT_priority>()[0], 17u);
  WritableColumn wc;
  SND_CHECK_EQ(FindWritableColumn(*pb, ch::BUSS, bf::col::BUSS_volumeDb, &wc), SND_OK);
  SND_CHECK_EQ(wc.capacity, 19u);
  SND_CHECK_EQ(wc.elemBytes, 4u);
  SND_CHECK(reinterpret_cast<std::uintptr_t>(wc.data) % 64 == 0);
  SND_CHECK_EQ(FindWritableColumn(*pb, ch::BUSS, 99, &wc), SND_ERR_NOT_FOUND);
  // The bank image itself is untouched.
  const ByteSpan evnt = pb->view.Chunk(ch::EVNT);
  SND_CHECK(evnt.data < pb->overlay.base || evnt.data >= pb->overlay.base + pb->overlay.bytes);
  // A release (non-dev) bank in a dev manager stays read-only.
  fixture::Bank rel = SimpleBank("Rel");
  env.fs.AddFile("rel.sndb", rel.Build());
  const HandleRef r = env.m->RequestLoad("rel.sndb");
  env.Settle(2);
  SND_CHECK(!env.m->Prepared(r)->view.writable());
  SND_CHECK_EQ(FindWritableColumn(*env.m->Prepared(r), ch::BUSS, 0, &wc), SND_ERR_UNSUPPORTED);
}

// Decision b: the same bank (bankId + language) from a second path fails; the first stays.
SND_TEST(BankManager, SameBankFromAnotherPathFails) {
  Env env;
  const std::vector<std::uint8_t> bytes = SimpleBank("Dup").Build();
  env.fs.AddFile("a/Dup.sndb", bytes);
  env.fs.AddFile("b/Dup.sndb", bytes);
  const HandleRef a = env.m->RequestLoad("a/Dup.sndb");
  env.Settle(2);
  const HandleRef b = env.m->RequestLoad("b/Dup.sndb");
  env.Settle(2);
  SND_CHECK_EQ(env.m->State(a), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(env.m->State(b), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(env.m->Result(b), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(std::strstr(env.m->FailureReason(b), "already loaded") != nullptr);
}

// Decision c: a retry of a failed slot resets `notified` so on_bank_loaded is delivered again.
SND_TEST(BankManager, RetryResetsNotified) {
  Env env;
  const HandleRef h = env.m->RequestLoad("late.sndb");
  env.Settle(2);
  SND_REQUIRE_EQ(env.m->State(h), SND_BANK_STATE_FAILED);
  env.m->SlotStates()[h.slot].notified.store(1);  // core delivered on_bank_loaded(FAILED)
  env.fs.AddFile("late.sndb", SimpleBank("Late").Build());
  SND_CHECK_EQ(env.m->RequestLoad("late.sndb").Value(), h.Value());
  SND_CHECK_EQ(env.m->SlotStates()[h.slot].notified.load(), 0u);
  env.Settle(2);
  SND_CHECK_EQ(env.m->State(h), SND_BANK_STATE_LOADED);
}

// The second load of the same bank waits for the first one's outcome: when the
// first fails for a missing dependency, so does the second, with that reason.
SND_TEST(BankManager, SameBankTwiceWithMissingDependencyGivesTheRealReason) {
  Env env;
  fixture::Bank b = SimpleBank("Dep2");
  b.deps.push_back(bf::Dependency{HashId("Nowhere"), 0, 1});
  const std::vector<std::uint8_t> bytes = b.Build();
  env.fs.AddFile("a/Dep2.sndb", bytes);
  env.fs.AddFile("b/Dep2.sndb", bytes);
  const HandleRef a = env.m->RequestLoad("a/Dep2.sndb");
  const HandleRef c = env.m->RequestLoad("b/Dep2.sndb");
  env.Settle(3);
  SND_CHECK_EQ(env.m->Result(a), SND_ERR_BANK_DEPENDENCY);
  SND_CHECK_EQ(env.m->Result(c), SND_ERR_BANK_DEPENDENCY);
  SND_CHECK(std::strstr(env.m->FailureReason(c), "is not loaded") != nullptr);
}
