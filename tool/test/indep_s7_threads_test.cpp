// Independent re-check of D-178d: a build's const reads from several threads at once (run under
// TSan: -DSND_SANITIZE=thread, ctest --repeat until-fail:20). Group P builds on one thread and
// reads the banks on others.
#include <atomic>
#include <thread>
#include <vector>

#include "indep_s7_fixture.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/doc_build.hpp"

using namespace indep_s7;
using namespace snd::tool;
namespace bf = snd::bank_format;

namespace {
std::uint64_t ReadAll(const BankImage& img) {
  const ParsedBank* v = img.view();
  if (v == nullptr) return 0;
  std::uint64_t sum = v->Rows(bf::chunk::BUSS) + v->Rows(bf::chunk::EVNT);
  const std::uint32_t sfx = v->FindId(bf::chunk::BUSS, snd::HashId("SFX"));
  std::uint64_t bits = 0;
  if (sfx != bf::kNone && v->Cell(bf::chunk::BUSS, bf::col::BUSS_volumeDb, sfx, &bits)) sum += bits;
  std::uint64_t n = 0;
  if (v->Chunk(bf::chunk::PHYS, &n) != nullptr) sum += n;
  return sum;
}
}  // namespace

SND_TEST(IndepS7Threads, D178d_BuiltBanksReadConcurrently) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE(b.ok && !b.banks.empty());
  const std::uint64_t expect = ReadAll(b.banks[0]);
  SND_REQUIRE(expect != 0u);
  std::atomic<std::uint32_t> wrong{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t) {
    ts.emplace_back([&b, &wrong, expect, t] {
      for (int i = 0; i < 200; ++i) {
        if (ReadAll(b.banks[0]) != expect) wrong.fetch_add(1);
        // Copies made on other threads share the parse.
        if ((i + t) % 7 == 0) {
          const BankImage copy = b.banks[0];
          if (ReadAll(copy) != expect) wrong.fetch_add(1);
          const DocBuild whole = b;
          if (ReadAll(whole.banks[0]) != expect) wrong.fetch_add(1);
        }
      }
    });
  }
  for (auto& t : ts) t.join();
  SND_CHECK_EQ(wrong.load(), 0u);
}

SND_TEST(IndepS7Threads, D178d_HandFilledImageParsedOnceUnderALock) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE(b.ok);
  const std::uint64_t expect = ReadAll(b.banks[0]);
  for (int round = 0; round < 20; ++round) {
    BankImage img;
    img.bytes = b.banks[0].bytes;  // filled by hand: parsed on the first view()
    std::atomic<std::uint32_t> wrong{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; ++t) {
      ts.emplace_back([&] {
        while (!go.load()) std::this_thread::yield();
        if (ReadAll(img) != expect) wrong.fetch_add(1);
      });
    }
    go.store(true);
    for (auto& t : ts) t.join();
    SND_CHECK_EQ(wrong.load(), 0u);
  }
}
