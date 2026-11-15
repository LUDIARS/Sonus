// Re-check of the BankManager fixes (independent tester): cancellation at
// every step combined with every failure kind, the registration queue under
// random operation sequences, the duplicate-bank check, and the language
// guess from the path vs the validated header.
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

struct DspState {
  bool refuse = false;
  std::uint64_t reserved = 0;
};
bool Reserve(void* u, const bf::DspPoolNeed& n) {
  auto* d = static_cast<DspState*>(u);
  if (d->refuse) return false;
  d->reserved += n.instanceBytes;
  return true;
}
void ReleaseDsp(void* u, const bf::DspPoolNeed& n) { static_cast<DspState*>(u)->reserved -= n.instanceBytes; }

struct Env {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  DspState dsp;
  BankManager* m = nullptr;
  std::size_t baseline = 0;
  explicit Env(JobMode mode = JobMode::Immediate, std::uint32_t maxBanks = 8, std::uint32_t chunk = 1u << 20)
      : jobs(mode) {
    BankManagerDesc d;
    d.allocator = &alloc;
    d.fileSystem = &fs;
    d.ioMode = snd::bank::BankIoMode::DirectFileSystem;  // stage 1.5: the default is the IO scheduler
    d.jobs = &jobs;
    d.maxBanks = maxBanks;
    d.ioChunkBytes = chunk;
    d.dsp.user = &dsp;
    d.dsp.reserve = &Reserve;
    d.dsp.release = &ReleaseDsp;
    if (BankManager::Create(d, &m) != SND_OK) m = nullptr;
    baseline = alloc.Live();
  }
  ~Env() { BankManager::Destroy(m); }
  void Settle(int n = 64) {
    snd::cmd::BankRegister reg{};
    for (int i = 0; i < n; ++i) {
      m->Update();
      jobs.RunAll();
      m->Update();
      while (m->PopRegistration(&reg)) m->MarkRegistered(reg.bank);
    }
  }
  void UnloadAll() {
    for (int round = 0; round < 8; ++round) {
      for (std::uint32_t s = 0; s < m->MaxBanks(); ++s) {
        const HandleRef h{s, m->SlotStates()[s].generation.load()};
        if (m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false) == SND_OK &&
            m->State(h) == SND_BANK_STATE_UNLOADING) {
          (void)m->Release(h);
        }
      }
      Settle(4);
    }
  }
};

fixture::Bank Simple(const char* name, std::uint64_t hash = 1) {
  fixture::Bank b;
  b.bankId = HashId(name);
  b.contentHash = hash;
  b.Table(ch::BUSS, 1, {fixture::U32(bf::col::BUSS_id, {HashId("Master")}), fixture::U32(bf::col::BUSS_parent, {bf::kNone})});
  return b;
}

}  // namespace

// Every step x every failure kind: a cancelled load always ends UNLOADED
// (never FAILED / LOADED afterwards), releases the DSP reservation, leaks nothing.
SND_TEST(BankManagerFix, CancelAtEveryStepWithEveryFailure) {
  enum Kind { kOk, kOpenFails, kReadFails, kCorrupt, kMissingDep, kDspRefused, kCount };
  const char* names[] = {"ok", "open fails", "read fails", "corrupt", "missing dependency", "dsp refused"};
  fixture::Bank rich = fixture::RichBank();
  for (int kind = 0; kind < kCount; ++kind) {
    for (int cancelAt = 0; cancelAt < 48; ++cancelAt) {
      Env e(JobMode::Manual, 3, 512);
      fixture::Bank b = rich;
      if (kind == kMissingDep) b.deps.push_back(bf::Dependency{HashId("Nope"), 0, 1});
      std::vector<std::uint8_t> img = b.Build();
      if (kind == kCorrupt) img[img.size() / 2] ^= 1;
      e.fs.AddFile("r.sndb", img);
      e.fs.SetLatency("r.sndb", 1);
      if (kind == kOpenFails) e.fs.FailOpen("r.sndb");
      if (kind == kReadFails) e.fs.FailReads("r.sndb", 1024, 2048);
      e.dsp.refuse = kind == kDspRefused;
      const HandleRef h = e.m->RequestLoad("r.sndb");
      snd::cmd::BankRegister reg{};
      bool cancelled = false;
      bool badAfterCancel = false;
      for (int step = 0; step < 400; ++step) {
        switch (step % 4) {
          case 0: e.m->PumpIo(); break;
          case 1: e.jobs.RunNext(); break;
          case 2: e.m->Pump(); break;
          default:
            if (e.m->PopRegistration(&reg)) e.m->MarkRegistered(reg.bank);
            break;
        }
        const auto st = static_cast<snd_bank_state>(e.m->SlotStates()[h.slot].state.load());
        const std::uint32_t gen = e.m->SlotStates()[h.slot].generation.load();
        if (cancelled && gen == h.generation && (st == SND_BANK_STATE_FAILED || st == SND_BANK_STATE_LOADED)) {
          badAfterCancel = true;
        }
        if (step == cancelAt) {
          const snd_bank_state before = e.m->State(h);
          (void)e.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false);
          cancelled = before == SND_BANK_STATE_LOADING;  // LOADED / FAILED: ordinary unload
          if (e.m->State(h) == SND_BANK_STATE_UNLOADING && before == SND_BANK_STATE_LOADED) (void)e.m->Release(h);
        }
      }
      e.Settle(8);
      if (e.m->State(h) == SND_BANK_STATE_UNLOADING) (void)e.m->Release(h);
      SND_CHECK_MSG(!badAfterCancel, "%s, cancel at %d: FAILED/LOADED after the cancel", names[kind], cancelAt);
      SND_CHECK_MSG(e.m->State(h) == SND_BANK_STATE_UNLOADED || !cancelled, "%s, cancel at %d: state %u", names[kind],
                    cancelAt, e.m->State(h));
      e.UnloadAll();
      SND_CHECK_MSG(e.alloc.Live() == e.baseline, "%s, cancel at %d: %zu live (baseline %zu)", names[kind], cancelAt,
                    e.alloc.Live(), e.baseline);
      SND_CHECK_EQ(e.alloc.BadFrees(), 0u);
      SND_CHECK_MSG(e.dsp.reserved == 0, "%s, cancel at %d: DSP reservation leaked", names[kind], cancelAt);
      SND_CHECK_EQ(e.fs.OpenFiles(), 0u);
    }
  }
}

// Random operation sequences against small slot counts. Debug builds of the
// manager abort on a broken queue invariant; here we check: every popped
// registration is a LOADING bank with a PreparedBank, no bank is popped twice
// per load, and after unloading everything no memory is left.
SND_TEST(BankManagerFix, RandomOperationSequences) {
  const char* paths[] = {"a.sndb", "b.sndb", "c.sndb", "d.sndb", "dup.sndb"};
  for (std::uint64_t seed = 1; seed <= 300; ++seed) {
    testsupport::Rng rng(seed * 0x9E3779B97F4A7C15ull);
    Env e(JobMode::Manual, 1 + rng.Below(3), 256);
    e.fs.AddFile("a.sndb", Simple("A").Build());
    e.fs.AddFile("b.sndb", Simple("B").Build());
    e.fs.AddFile("c.sndb", Simple("C").Build());
    e.fs.AddFile("d.sndb", Simple("D").Build());
    e.fs.AddFile("dup.sndb", Simple("A").Build());  // same bankId as a.sndb
    e.fs.SetDefaultLatency(rng.Below(3));
    std::vector<HandleRef> hs;
    std::vector<std::uint64_t> popped;
    for (int op = 0; op < 200; ++op) {
      switch (rng.Below(8)) {
        case 0:
        case 1: {
          const HandleRef h = e.m->RequestLoad(paths[rng.Below(5)]);
          if (!h.IsNone()) hs.push_back(h);
          break;
        }
        case 2:
          if (!hs.empty()) {
            const HandleRef h = hs[rng.Below(static_cast<std::uint32_t>(hs.size()))];
            (void)e.m->BeginUnload(h, rng.Below(2) ? SND_UNLOAD_STOP_AND_UNLOAD : SND_UNLOAD_IF_UNUSED, false);
          }
          break;
        case 3: e.m->PumpIo(); break;
        case 4: e.jobs.RunNext(); break;
        case 5: e.m->Pump(); break;
        case 6: {
          snd::cmd::BankRegister reg{};
          if (e.m->PopRegistration(&reg)) {
            SND_CHECK(AsPrepared(reg.prepared) != nullptr);
            SND_CHECK_EQ(e.m->State(reg.bank), SND_BANK_STATE_LOADING);
            for (std::uint64_t p : popped) SND_CHECK_MSG(p != reg.bank.Value(), "seed %llu: popped twice",
                                                         static_cast<unsigned long long>(seed));
            popped.push_back(reg.bank.Value());
            // D-50: never two registered copies of one bank (a.sndb and dup.sndb are both "A").
            for (std::uint32_t s = 0; s < e.m->MaxBanks(); ++s) {
              const HandleRef o{s, e.m->SlotStates()[s].generation.load()};
              if (o.slot == reg.bank.slot) continue;
              const PreparedBank* other = e.m->Prepared(o);
              const snd_bank_state ost = e.m->State(o);
              if (other != nullptr && (ost == SND_BANK_STATE_LOADED || ost == SND_BANK_STATE_UNLOADING)) {
                SND_CHECK_MSG(other->bankId != reg.bankId, "seed %llu: bank %08x registered twice",
                              static_cast<unsigned long long>(seed), reg.bankId);
              }
            }
            e.m->MarkRegistered(reg.bank);
          }
          break;
        }
        default:
          for (std::uint32_t s = 0; s < e.m->MaxBanks(); ++s) {
            const HandleRef h{s, e.m->SlotStates()[s].generation.load()};
            if (e.m->State(h) == SND_BANK_STATE_UNLOADING) (void)e.m->Release(h);
          }
          break;
      }
    }
    // Settle: every bank still LOADING must finish (no lost registration).
    // Decision D: a load of the same bank as an UNLOADING slot waits for that
    // slot's Release (core's part after its epoch), so release them first; after
    // that nothing may be left waiting.
    e.Settle(16);
    for (std::uint32_t s = 0; s < e.m->MaxBanks(); ++s) {
      const HandleRef h{s, e.m->SlotStates()[s].generation.load()};
      if (e.m->State(h) == SND_BANK_STATE_UNLOADING) (void)e.m->Release(h);
    }
    e.Settle(64);
    for (std::uint32_t s = 0; s < e.m->MaxBanks(); ++s) {
      const std::uint32_t st = e.m->SlotStates()[s].state.load();
      SND_CHECK_MSG(st != SND_BANK_STATE_LOADING, "seed %llu: slot %u stuck LOADING",
                    static_cast<unsigned long long>(seed), s);
    }
    e.UnloadAll();
    SND_CHECK_MSG(e.alloc.Live() == e.baseline, "seed %llu: %zu live", static_cast<unsigned long long>(seed),
                  e.alloc.Live());
  }
}

// Same bank (bankId + language) from two paths: the second fails, the first
// stays; after the first is unloaded the second can load. A cancelled first
// load does not block the second. Language variants are not duplicates.
SND_TEST(BankManagerFix, DuplicateBankCheck) {
  Env e;
  const std::vector<std::uint8_t> img = Simple("Dup").Build();
  e.fs.AddFile("dup.sndb", img);
  e.fs.AddFile("copy/dup.sndb", img);
  const HandleRef a = e.m->RequestLoad("dup.sndb");
  const HandleRef b = e.m->RequestLoad("copy/dup.sndb");
  e.Settle();
  SND_CHECK_EQ(e.m->State(a), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(b), SND_ERR_BANK_ALREADY_LOADED);  // contract 1.1
  SND_CHECK_EQ(e.m->BeginUnload(a, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  // Contract 1.1 decision D (D-50): while the first is UNLOADING, a new load of the
  // same bank (here the failed copy, retried in place) is read and validated but
  // WAITS for the old slot's Release: it neither fails nor registers meanwhile.
  const HandleRef b2 = e.m->RequestLoad("copy/dup.sndb");
  SND_CHECK(b2.slot == b.slot && b2.generation == b.generation);
  e.Settle();
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_LOADING);
  SND_CHECK(e.m->Result(b) != SND_ERR_BANK_ALREADY_LOADED);
  SND_CHECK_EQ(e.m->State(a), SND_BANK_STATE_UNLOADING);
  SND_CHECK(e.m->FindBank(HashId("Dup")).slot == a.slot);  // only the old copy is registered
  SND_CHECK_EQ(e.m->Release(a), SND_OK);
  e.Settle();
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_LOADED);
  // The same PATH while its old slot is UNLOADING: also waits, then loads.
  SND_CHECK_EQ(e.m->BeginUnload(b, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  const HandleRef b3 = e.m->RequestLoad("copy/dup.sndb");
  SND_CHECK(!b3.IsNone() && b3.slot != b.slot);
  e.Settle();
  SND_CHECK_EQ(e.m->State(b3), SND_BANK_STATE_LOADING);
  SND_CHECK_EQ(e.m->Release(b), SND_OK);
  e.Settle();
  SND_CHECK_EQ(e.m->State(b3), SND_BANK_STATE_LOADED);
  // Cancelled first load does not block.
  Env f(JobMode::Manual);
  f.fs.AddFile("dup.sndb", img);
  f.fs.AddFile("copy/dup.sndb", img);
  const HandleRef c1 = f.m->RequestLoad("dup.sndb");
  f.m->PumpIo();
  f.jobs.RunAll();
  f.m->Pump();  // c1 READY
  SND_CHECK_EQ(f.m->BeginUnload(c1, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  const HandleRef c2 = f.m->RequestLoad("copy/dup.sndb");
  f.Settle();
  SND_CHECK_EQ(f.m->State(c2), SND_BANK_STATE_LOADED);
  // Two language variants of one bank are two banks.
  Env g;
  fixture::Bank ja = Simple("Voice");
  ja.flags |= bf::kFileLanguageBank;
  ja.languageId = HashId("ja");
  fixture::Bank en = ja;
  en.languageId = HashId("en");
  g.fs.AddFile("voice.ja.sndb", ja.Build());
  g.fs.AddFile("voice.en.sndb", en.Build());
  const HandleRef j = g.m->RequestLoad("voice.ja.sndb");
  const HandleRef n = g.m->RequestLoad("voice.en.sndb");
  g.Settle();
  SND_CHECK_EQ(g.m->State(j), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(g.m->State(n), SND_BANK_STATE_LOADED);
}

// A duplicate that waits for a dependency which then fails: the earlier
// copy is chosen first; report whether the later copy is rejected because of
// a copy that itself never loads.
SND_TEST(BankManagerFix, DuplicateWhileTheFirstWaitsForADependency) {
  Env e;
  fixture::Bank b = Simple("Dup");
  b.deps.push_back(bf::Dependency{HashId("Base"), 0, 5});
  const std::vector<std::uint8_t> img = b.Build();
  e.fs.AddFile("dup.sndb", img);
  e.fs.AddFile("copy/dup.sndb", img);
  const HandleRef a = e.m->RequestLoad("dup.sndb");
  const HandleRef c = e.m->RequestLoad("copy/dup.sndb");
  e.Settle();  // Base never requested: both wait, then fail
  snd::test::Note("dependency never loaded: first %u (%d), copy %u (%d: %s)", e.m->State(a), e.m->Result(a),
                  e.m->State(c), e.m->Result(c), e.m->FailureReason(c));
  SND_CHECK(e.m->State(a) == SND_BANK_STATE_FAILED);
  SND_CHECK(e.m->State(c) == SND_BANK_STATE_FAILED);
}

// The path says "en", the header says "ja". While loading it is planned as
// 'en'; after validation the header wins: FindBank sees it as ja, and the
// switch plans never name a path built from the wrong language.
SND_TEST(BankManagerFix, LanguageGuessVersusHeader) {
  Env e(JobMode::Manual);
  fixture::Bank ja = Simple("Voice");
  ja.flags |= bf::kFileLanguageBank;
  ja.languageId = HashId("ja");
  e.fs.AddFile("voice.en.sndb", ja.Build());  // misnamed
  SND_REQUIRE_EQ(e.m->DeclareLanguage("ja"), SND_OK);
  SND_REQUIRE_EQ(e.m->DeclareLanguage("en"), SND_OK);
  e.fs.Hold("voice.en.sndb");
  const HandleRef h = e.m->RequestLoad("voice.en.sndb");
  e.Settle(2);
  LanguageSwitch plan[4];
  snd_result r = SND_OK;
  std::uint32_t n = e.m->PlanLanguageSwitch(HashId("ja"), plan, 4, &r);
  SND_CHECK_EQ(n, 1u);  // guessed 'en' from the path
  if (n == 1) SND_CHECK(std::strcmp(plan[0].newPath, "voice.ja.sndb") == 0);
  e.fs.Release("voice.en.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(h), SND_BANK_STATE_LOADED);
  SND_CHECK(!e.m->FindBank(HashId("Voice"), HashId("ja")).IsNone());
  SND_CHECK(e.m->FindBank(HashId("Voice"), HashId("en")).IsNone());
  n = e.m->PlanLanguageSwitch(HashId("ja"), plan, 4, &r);
  SND_CHECK_EQ(n, 0u);  // it IS ja
  n = e.m->PlanLanguageSwitch(HashId("en"), plan, 4, &r);
  snd::test::Note("misnamed ja bank, plan to 'en': %u entries (path does not encode its language)", n);
  // A path whose language is not declared is not guessed.
  Env f(JobMode::Manual);
  f.fs.AddFile("voice.fr.sndb", Simple("Voice").Build());
  f.fs.Hold("voice.fr.sndb");
  SND_REQUIRE_EQ(f.m->DeclareLanguage("ja"), SND_OK);
  f.m->RequestLoad("voice.fr.sndb");
  f.Settle(2);
  SND_CHECK_EQ(f.m->PlanLanguageSwitch(HashId("ja"), plan, 4, &r), 0u);
  f.fs.Release("voice.fr.sndb");
  f.Settle();
}
