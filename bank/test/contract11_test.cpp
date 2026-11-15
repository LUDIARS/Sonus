// Contract 1.1 in the bank manager (D-46, D-50): the language name rules of
// snd_declare_language, the generations issued for bank slots.
#include <string>

#include "snd/bank/bank_manager.hpp"
#include "snd/id.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"
#include "test_support.hpp"

using namespace snd::bank;

namespace {
struct Env {
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  BankManager* m = nullptr;
  Env() {
    BankManagerDesc d;
    d.allocator = &alloc;
    d.fileSystem = &fs;
    d.ioMode = BankIoMode::DirectFileSystem;
    d.jobs = &jobs;
    if (BankManager::Create(d, &m) != SND_OK) m = nullptr;
  }
  ~Env() { BankManager::Destroy(m); }
};
}  // namespace

SND_TEST(BankContract11, DeclareLanguageNameRules) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  SND_CHECK_EQ(e.m->DeclareLanguage("ja"), SND_OK);
  SND_CHECK_EQ(e.m->DeclareLanguage("ja"), SND_OK);  // again: fine
  SND_CHECK_EQ(e.m->DeclareLanguage("pt-BR"), SND_OK);
  SND_CHECK_EQ(e.m->DeclareLanguage(nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(e.m->DeclareLanguage(""), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(e.m->DeclareLanguage("en.us"), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(e.m->DeclareLanguage("en/us"), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(e.m->DeclareLanguage("en\\us"), SND_ERR_INVALID_ARGUMENT);
  const std::string longest(SND_MAX_LANGUAGE_NAME_BYTES - 1u, 'x');
  SND_CHECK_EQ(e.m->DeclareLanguage(longest.c_str()), SND_OK);
  const std::string tooLong(SND_MAX_LANGUAGE_NAME_BYTES, 'y');
  SND_CHECK_EQ(e.m->DeclareLanguage(tooLong.c_str()), SND_ERR_INVALID_ARGUMENT);
  // At most SND_MAX_LANGUAGES names (3 declared above).
  snd_result last = SND_OK;
  for (std::uint32_t i = 0; i < SND_MAX_LANGUAGES; ++i) last = e.m->DeclareLanguage(("l" + std::to_string(i)).c_str());
  SND_CHECK_EQ(last, SND_ERR_LIMIT_REACHED);
}

SND_TEST(BankContract11, SlotGenerationsStayBelow2Pow31) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  for (std::uint32_t s = 0; s < e.m->MaxBanks(); ++s) {
    SND_CHECK(e.m->SlotStates()[s].generation.load() < 0x80000000u);
    SND_CHECK(e.m->SlotStates()[s].generation.load() != 0u);
  }
  // Many load / cancel cycles on one slot: generations advance, never 0 or >= 2^31.
  std::uint32_t prev = 0;
  for (int i = 0; i < 200; ++i) {
    const snd::HandleRef h = e.m->RequestLoad("missing.sndb");
    SND_REQUIRE(!h.IsNone());
    SND_CHECK(h.generation != 0u && h.generation < 0x80000000u);
    SND_CHECK(h.generation != prev);
    prev = h.generation;
    SND_CHECK_EQ(e.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
    e.m->Update();
  }
}

// D-50 (acceptance decision G): a name whose id equals another declared name's.
SND_TEST(BankContract11, DeclareLanguageHashCollisionFails) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  static_assert(snd::HashId("glbvs") == snd::HashId("yacxa"));  // FNV-1a 32 collision
  SND_CHECK_EQ(e.m->DeclareLanguage("glbvs"), SND_OK);
  SND_CHECK_EQ(e.m->DeclareLanguage("yacxa"), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(e.m->DeclareLanguage("glbvs"), SND_OK);  // the same name again: fine
}
