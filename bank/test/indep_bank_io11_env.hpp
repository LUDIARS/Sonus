// Shared rig of the independent bank <-> io tests (contract 1.1 / stage 1.5):
// BankManager in Scheduler mode over io::IoScheduler, the deterministic job
// mock and the memory FS. The scheduler is driven with RunOnce() unless the
// test starts the IO thread.
#ifndef SND_BANK_TEST_INDEP_BANK_IO11_ENV_HPP
#define SND_BANK_TEST_INDEP_BANK_IO11_ENV_HPP

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

#define SND_REQUIRE_MSG(cond, ...)      \
  do {                                  \
    if (!(cond)) {                      \
      SND_CHECK_MSG(cond, __VA_ARGS__); \
      return;                           \
    }                                   \
  } while (0)

namespace indep11 {

namespace bf = snd::bank_format;

inline fixture::Bank Named(const std::string& name, std::uint32_t languageId = 0) {
  fixture::Bank b = fixture::RichBank();
  b.bankId = snd::HashId(name);
  b.contentHash = 0x11;
  if (languageId != 0) {
    b.flags |= bf::kFileLanguageBank;
    b.languageId = languageId;
  }
  return b;
}

struct Env {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs{snd::mock::JobMode::Manual};  // validation runs in Step()
  snd::io::StreamPool pool;
  snd::io::IoScheduler io;
  snd::bank::BankManager* m = nullptr;
  bool threaded = false;
  explicit Env(std::uint32_t maxBanks = 8, std::uint32_t fileQueue = 32, bool ioThread = false) {
    SND_CHECK(pool.Init(alloc, 64u << 10, 16u << 10, 4));
    snd::io::IoSchedulerConfig c;
    c.maxStreams = 4;
    c.filePieceBytes = 1024;
    c.fileQueueCapacity = fileQueue;
    c.bytesPerWindow = 0;
    c.pollIntervalNs = 100000;
    SND_CHECK(io.Init(alloc, fs, pool, c));
    if (ioThread) {
      SND_CHECK(io.Start());
      threaded = true;
    }
    snd::bank::BankManagerDesc d;
    d.allocator = &alloc;
    d.scheduler = &io;
    d.jobs = &jobs;
    d.maxBanks = maxBanks;
    if (snd::bank::BankManager::Create(d, &m) != SND_OK) m = nullptr;
  }
  ~Env() {
    io.Shutdown();
    jobs.RunAll();
    if (m != nullptr) {
      m->Update();
      snd::bank::BankManager::Destroy(m);
    }
    io.Destroy();
    pool.Destroy();
  }
  void Step() {
    if (!threaded) io.RunOnce();
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

}  // namespace indep11

#endif  // SND_BANK_TEST_INDEP_BANK_IO11_ENV_HPP
