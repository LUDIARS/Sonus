// Independent tests: BankManager over io::IoScheduler (stage 1.5, A1-11) and
// the bank part of contract 1.1 (D-46, D-50). Deterministic (RunOnce driver,
// job mock) except where the IO thread is started on purpose.
//  - IoScheduler::Shutdown at each stage: a bank whose read is done (validating
//    / validated / queued) is not affected and still ends LOADED; a bank that
//    has not submitted yet, or whose submit is waiting for a full file queue,
//    ends FAILED (SND_ERR_IO), never stuck LOADING.
//  - Destroy with the IO thread running and a read that cannot finish yet:
//    Destroy waits for it (no free under the read), nothing leaks afterwards.
//  - Duplicate rules (D-50): language banks of different languages are not
//    duplicates; the same language from another path is; the same path while
//    loading reads the file once; a failed duplicate is retried in place.
//  - Language names (D-50) through BankManager::DeclareLanguage.
//  - Loaded image == the file (byte for byte) when read in many pieces.
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "indep_bank_io11_env.hpp"

using snd::HandleRef;
using snd::HashId;
using namespace snd::bank;
using indep11::Env;
using indep11::Named;

namespace {
// Steps without running jobs until a validation job is queued.
bool StepUntilValidationQueued(Env& e, int limit = 64) {
  for (int i = 0; i < limit; ++i) {
    e.io.RunOnce();
    e.m->Update();
    if (e.jobs.Pending() > 0) return true;
  }
  return false;
}
}  // namespace

SND_TEST(IndepBankIo11, ShutdownAfterTheReadDoesNotAffectValidatingBanks) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  const std::vector<std::uint8_t> img = Named("V").Build();
  e.fs.AddFile("v.sndb", img);
  const HandleRef h = e.m->RequestLoad("v.sndb");
  SND_REQUIRE(StepUntilValidationQueued(e));
  SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_LOADING);
  e.io.Shutdown();  // the read is done, only validation is left
  SND_CHECK(!e.io.Accepting());
  e.jobs.RunAll();
  e.m->Update();
  snd::cmd::BankRegister reg{};
  SND_REQUIRE(e.m->PopRegistration(&reg));
  e.m->MarkRegistered(reg.bank);
  SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_LOADED);
  const PreparedBank* pb = e.m->Prepared(h);
  SND_REQUIRE(pb != nullptr);
  SND_CHECK(std::memcmp(pb->view.file(), img.data(), img.size()) == 0);
  // Unload after the shutdown works (no IO needed).
  SND_CHECK_EQ(e.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(e.m->Release(h), SND_OK);
  SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_UNLOADED);
}

SND_TEST(IndepBankIo11, ShutdownWithSubmitsWaitingForAFullQueueFailsThemAll) {
  Env e(8, 1);  // one FileRequest at a time: the others wait in "Retry"
  SND_REQUIRE(e.m != nullptr);
  std::vector<HandleRef> hs;
  for (int i = 0; i < 5; ++i) {
    const std::string p = "q" + std::to_string(i) + ".sndb";
    e.fs.AddFile(p, Named("Q" + std::to_string(i)).Build());
    e.fs.SetLatency(p, 3);
    hs.push_back(e.m->RequestLoad(p.c_str()));
  }
  e.m->PumpIo();  // q0 submitted, q1..q4 refused (queue full) -> retried later
  e.io.Shutdown();
  for (int i = 0; i < 4; ++i) e.Step();
  for (std::size_t i = 0; i < hs.size(); ++i) {
    SND_CHECK_MSG(e.m->State(hs[i]) == SND_BANK_STATE_FAILED, "bank %zu: state %u", i, e.m->State(hs[i]));
    SND_CHECK_EQ(e.m->Result(hs[i]), SND_ERR_IO);
  }
  // Failed slots are freed by BeginUnload; the allocator is back to the manager's own memory.
  for (const HandleRef& h : hs) SND_CHECK_EQ(e.m->BeginUnload(h, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  for (const HandleRef& h : hs) SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_UNLOADED);
}

SND_TEST(IndepBankIo11, LanguageBanksOfDifferentLanguagesAreNotDuplicates) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  const std::uint32_t ja = HashId("ja"), en = HashId("en");
  e.fs.AddFile("Voice.ja.sndb", Named("Voice", ja).Build());
  e.fs.AddFile("Voice.en.sndb", Named("Voice", en).Build());
  e.fs.AddFile("dlc/Voice.ja.sndb", Named("Voice", ja).Build());
  SND_CHECK_EQ(e.m->DeclareLanguage("ja"), SND_OK);
  SND_CHECK_EQ(e.m->DeclareLanguage("en"), SND_OK);
  const HandleRef a = e.m->RequestLoad("Voice.ja.sndb");
  const HandleRef b = e.m->RequestLoad("Voice.en.sndb");
  e.Settle();
  SND_CHECK_EQ(e.m->State(a), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_LOADED);
  SND_CHECK(e.m->FindBank(HashId("Voice"), ja).slot == a.slot);
  SND_CHECK(e.m->FindBank(HashId("Voice"), en).slot == b.slot);
  const HandleRef c = e.m->RequestLoad("dlc/Voice.ja.sndb");
  e.Settle();
  SND_CHECK_EQ(e.m->State(c), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(c), SND_ERR_BANK_ALREADY_LOADED);
  SND_CHECK_EQ(e.m->State(a), SND_BANK_STATE_LOADED);  // the first one stays
  // A failed duplicate is retried in place once the first one is gone.
  SND_CHECK_EQ(e.m->BeginUnload(a, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(e.m->Release(a), SND_OK);
  const HandleRef c2 = e.m->RequestLoad("dlc/Voice.ja.sndb");
  SND_CHECK(c2.slot == c.slot && c2.generation == c.generation);
  e.Settle();
  SND_CHECK_EQ(e.m->State(c), SND_BANK_STATE_LOADED);
}

SND_TEST(IndepBankIo11, SamePathWhileLoadingReadsTheFileOnce) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  const std::vector<std::uint8_t> img = Named("Once").Build();
  e.fs.AddFile("once.sndb", img);
  e.fs.SetLatency("once.sndb", 4);
  const HandleRef a = e.m->RequestLoad("once.sndb");
  e.Step();
  snd_result r = SND_ERR_IO;
  const HandleRef b = e.m->RequestLoad("once.sndb", &r);
  SND_CHECK_EQ(r, SND_OK);
  SND_CHECK(a.slot == b.slot && a.generation == b.generation);
  e.Settle(256);
  const HandleRef c = e.m->RequestLoad("once.sndb", &r);  // loaded: same handle again
  SND_CHECK_EQ(r, SND_OK);
  SND_CHECK(a.slot == c.slot && a.generation == c.generation);
  SND_CHECK_EQ(e.m->State(a), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.fs.BytesRead(), static_cast<std::uint64_t>(img.size()));
  SND_CHECK(e.fs.OpenCalls() >= 1u && e.fs.OpenCalls() <= 2u);  // size query + read, not per handle
}

SND_TEST(IndepBankIo11, ImageIsTheFileByteForByteInSmallPieces) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  // Several banks of different sizes; pieces of 1024 bytes, FS latency per path.
  std::vector<std::vector<std::uint8_t>> imgs;
  std::vector<HandleRef> hs;
  for (int i = 0; i < 6; ++i) {
    fixture::Bank b = Named("Img" + std::to_string(i));
    std::vector<std::uint8_t> pad(static_cast<std::size_t>(i) * 3000 + 17);
    for (std::size_t k = 0; k < pad.size(); ++k) pad[k] = static_cast<std::uint8_t>(k * 31 + i);
    b.Blob(SND_FOURCC('X', 'T', 'R', static_cast<char>('0' + i)), pad);  // unknown chunk: ignored, but read
    imgs.push_back(b.Build());
    const std::string p = "img" + std::to_string(i) + ".sndb";
    e.fs.AddFile(p, imgs.back());
    e.fs.SetLatency(p, static_cast<std::uint32_t>(i % 3));
    hs.push_back(e.m->RequestLoad(p.c_str()));
  }
  e.Settle(256);
  for (std::size_t i = 0; i < hs.size(); ++i) {
    SND_REQUIRE_MSG(e.m->State(hs[i]) == SND_BANK_STATE_LOADED, "bank %zu: %u %s", i, e.m->State(hs[i]),
                    e.m->FailureReason(hs[i]));
    const PreparedBank* pb = e.m->Prepared(hs[i]);
    SND_REQUIRE(pb != nullptr);
    SND_CHECK_EQ(pb->view.fileBytes(), static_cast<std::uint64_t>(imgs[i].size()));
    SND_CHECK(std::memcmp(pb->view.file(), imgs[i].data(), imgs[i].size()) == 0);
  }
}

// Destroy while the IO thread still holds a read of a loading bank: Destroy
// cancels it and waits until the scheduler is done with the image and path.
SND_TEST(IndepBankIo11, DestroyWaitsForAReadInFlightWithTheIoThread) {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  snd::io::StreamPool pool;
  snd::io::IoScheduler io;
  SND_REQUIRE(pool.Init(alloc, 64u << 10, 16u << 10, 2));
  snd::io::IoSchedulerConfig c;
  c.maxStreams = 2;
  c.filePieceBytes = 512;
  c.bytesPerWindow = 0;
  c.pollIntervalNs = 100000;
  SND_REQUIRE(io.Init(alloc, fs, pool, c));
  SND_REQUIRE(io.Start());
  const std::vector<std::uint8_t> img = Named("Held").Build();
  fs.AddFile("held.sndb", img);
  BankManagerDesc d;
  d.allocator = &alloc;
  d.scheduler = &io;
  d.jobs = &jobs;
  BankManager* m = nullptr;
  SND_REQUIRE_EQ(BankManager::Create(d, &m), SND_OK);
  const std::size_t base = alloc.Live();
  const HandleRef h = m->RequestLoad("held.sndb");
  // Let the size query finish and the read start, then hold it in the FS.
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (fs.ReadCalls() == 0u && std::chrono::steady_clock::now() < until) {
    m->PumpIo();
    if (fs.OpenCalls() >= 1u) fs.Hold("held.sndb");
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  fs.Hold("held.sndb");
  SND_REQUIRE(fs.ReadCalls() > 0u);
  SND_CHECK_EQ(m->State(h), SND_BANK_STATE_LOADING);
  std::atomic<bool> released{false};
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    released.store(true);
    fs.Release("held.sndb");
  });
  const auto t0 = std::chrono::steady_clock::now();
  BankManager::Destroy(m);  // must not return (and free the image) before the read completed
  const auto waited = std::chrono::steady_clock::now() - t0;
  const bool releasedBefore = released.load();
  releaser.join();
  SND_CHECK_MSG(releasedBefore || fs.InFlight() == 0u, "Destroy returned while a read into the image was in flight");
  snd::test::Note("Destroy waited %lld us", static_cast<long long>(
                                                std::chrono::duration_cast<std::chrono::microseconds>(waited).count()));
  io.Shutdown();
  io.Destroy();
  pool.Destroy();
  SND_CHECK(alloc.Live() <= base);  // the manager's memory and the slot went back
  SND_CHECK_EQ(alloc.Live(), 0u);
  SND_CHECK_EQ(alloc.BadFrees(), 0u);
}

SND_TEST(IndepBankIo11, DeclareLanguageNameRulesAndLimit) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  const std::string max31(31, 'x');
  const std::string len32(32, 'y');
  SND_CHECK_EQ(e.m->DeclareLanguage(max31.c_str()), SND_OK);
  SND_CHECK_EQ(e.m->DeclareLanguage(len32.c_str()), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(e.m->DeclareLanguage(nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(e.m->DeclareLanguage(""), SND_ERR_INVALID_ARGUMENT);
  for (const char* bad : {"a/b", "a.b", "a\\b", "/", ".", "\\"}) {
    SND_CHECK_MSG(e.m->DeclareLanguage(bad) == SND_ERR_INVALID_ARGUMENT, "'%s' accepted", bad);
  }
  SND_CHECK_EQ(e.m->DeclareLanguage(max31.c_str()), SND_OK);  // again: OK, not counted twice
  std::uint32_t declared = 1;
  for (int i = 0; declared < SND_MAX_LANGUAGES; ++i, ++declared) {
    SND_REQUIRE_EQ(e.m->DeclareLanguage(("l" + std::to_string(i)).c_str()), SND_OK);
  }
  SND_CHECK_EQ(e.m->DeclareLanguage("one-too-many"), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(e.m->DeclareLanguage("l0"), SND_OK);  // already declared: still fine at the limit
  // Unknown language id: NOT_FOUND, current language unchanged.
  LanguageSwitch plan[4];
  snd_result r = SND_OK;
  const snd::Id before = e.m->CurrentLanguage();
  SND_CHECK_EQ(e.m->SetLanguage(HashId("zz"), plan, 4, &r), 0u);
  SND_CHECK_EQ(r, SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(e.m->CurrentLanguage(), before);
}

// Language switch builds "<base>.<name>.sndb" from the loaded bank's path (D-50).
SND_TEST(IndepBankIo11, LanguageSwitchPathsUseTheDeclaredName) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  const std::uint32_t ja = HashId("ja-JP");
  e.fs.AddFile("vo/Story.ja-JP.sndb", Named("Story", ja).Build());
  e.fs.AddFile("vo/Story.en-US.sndb", Named("Story", HashId("en-US")).Build());
  SND_CHECK_EQ(e.m->DeclareLanguage("ja-JP"), SND_OK);
  SND_CHECK_EQ(e.m->DeclareLanguage("en-US"), SND_OK);
  const HandleRef old = e.m->RequestLoad("vo/Story.ja-JP.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(old), SND_BANK_STATE_LOADED);
  LanguageSwitch plan[4];
  snd_result r = SND_ERR_IO;
  const std::uint32_t n = e.m->SetLanguage(HashId("en-US"), plan, 4, &r);
  SND_CHECK_EQ(r, SND_OK);
  SND_REQUIRE_EQ(n, 1u);
  SND_CHECK(std::strcmp(plan[0].newPath, "vo/Story.en-US.sndb") == 0);
  SND_CHECK(plan[0].oldBank.slot == old.slot);
  SND_CHECK_EQ(e.m->CurrentLanguage(), HashId("en-US"));
  e.Settle();
  const HandleRef nw = e.m->FindBank(HashId("Story"), HashId("en-US"));
  SND_CHECK(!nw.IsNone());
  SND_CHECK_EQ(e.m->State(nw), SND_BANK_STATE_LOADED);
}
