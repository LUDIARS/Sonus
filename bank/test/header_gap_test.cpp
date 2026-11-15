// The header bytes [52, 128) are covered by no checksum (bank-format.md §3:
// headerChecksum covers [0, 52), checksum covers [headerBytes, fileBytes)).
// These tests corrupt single fields of that range WITHOUT re-signing anything
// (a plain storage / transfer corruption) and check whether ParseBank notices.
//
// Tests named SpecDefect_* fail today: the corruption is accepted and changes
// what the bank means (dropped dependencies, another language, dropped chunks).
// The fix needs the format (headerChecksum over [0, 128) except itself, or a
// second checksum) - this is a defect of the frozen format, see the report.
#include <cstring>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/bank/validate.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"
#include "spec_walk.hpp"

namespace bf = snd::bank_format;
namespace ch = bf::chunk;
using fixture::Bank;
using fixture::Image;
using namespace snd::bank;

namespace {

Bank LanguageBankWithDeps() {
  Bank b = fixture::RichBank();
  b.deps.push_back(bf::Dependency{snd::HashId("Common"), 0, 0x1111});
  b.deps.push_back(bf::Dependency{snd::HashId("Music"), 0, 0x2222});
  b.bankId = snd::HashId("Voice");
  b.flags |= bf::kFileLanguageBank;
  b.languageId = snd::HashId("ja");
  return b;
}

template <class T>
void PutAt(std::vector<std::uint8_t>& f, std::size_t off, T v) {
  std::memcpy(f.data() + off, &v, sizeof(T));
}

snd_result ParseCopy(const std::vector<std::uint8_t>& f, BankView* v, Image** keep) {
  *keep = new Image(f);
  ParseError e;
  return ParseBank((*keep)->data(), (*keep)->size(), ParseOptions{}, v, &e);
}

}  // namespace

SND_TEST(BankHeaderGap, BaselineParses) {
  const std::vector<std::uint8_t> f = LanguageBankWithDeps().Build();
  Image* img = nullptr;
  BankView v;
  SND_REQUIRE_EQ(ParseCopy(f, &v, &img), SND_OK);
  SND_CHECK_EQ(v.dependencyCount(), 2u);
  SND_CHECK_EQ(v.languageId(), snd::HashId("ja"));
  delete img;
}

// dependencyCount 2 -> 0 (offset 56): the bank loads with no dependency check
// and DependentCount of its dependencies stays 0 (they can be unloaded under it).
SND_TEST(BankHeaderGap, SpecDefect_DependencyCountCorruptionIsNotDetected) {
  std::vector<std::uint8_t> f = LanguageBankWithDeps().Build();
  PutAt<std::uint32_t>(f, 56, 0);
  Image* img = nullptr;
  BankView v;
  const snd_result r = ParseCopy(f, &v, &img);
  SND_CHECK_MSG(r != SND_OK, "accepted: dependencies silently dropped (dependencyCount now %u)", v.dependencyCount());
  delete img;
}

// languageId (offset 80): the Japanese voice bank is accepted as another language.
SND_TEST(BankHeaderGap, SpecDefect_LanguageIdCorruptionIsNotDetected) {
  std::vector<std::uint8_t> f = LanguageBankWithDeps().Build();
  PutAt<std::uint32_t>(f, 80, snd::HashId("en"));
  Image* img = nullptr;
  BankView v;
  const snd_result r = ParseCopy(f, &v, &img);
  SND_CHECK_MSG(r != SND_OK, "accepted as language %08x ('en') instead of 'ja'", v.languageId());
  delete img;
}

// chunkCount (offset 60) lowered: trailing chunks vanish (here: everything
// after the first table), the bank is accepted with fewer tables.
SND_TEST(BankHeaderGap, SpecDefect_ChunkCountCorruptionIsNotDetected) {
  std::vector<std::uint8_t> f = LanguageBankWithDeps().Build();
  bf::FileHeader h;
  std::memcpy(&h, f.data(), sizeof h);
  for (std::uint32_t keep : {0u, 1u, h.chunkCount - 1}) {
    std::vector<std::uint8_t> g = f;
    PutAt<std::uint32_t>(g, 60, keep);
    Image* img = nullptr;
    BankView v;
    const snd_result r = ParseCopy(g, &v, &img);
    SND_CHECK_MSG(r != SND_OK, "chunkCount %u -> %u accepted (EVNT rows now %u)", h.chunkCount, keep,
                  v.Rows(ch::EVNT));
    delete img;
  }
}

// Reserved header bytes (84..127, "0" per §3) are neither checksummed nor
// checked: any value is accepted. Spec gap: §8 does not list them.
SND_TEST(BankHeaderGap, ReservedBytesAreNotChecked) {
  std::vector<std::uint8_t> f = LanguageBankWithDeps().Build();
  for (std::size_t i = 84; i < 128; ++i) f[i] = 0xA5;
  Image* img = nullptr;
  BankView v;
  const snd_result r = ParseCopy(f, &v, &img);
  snd::test::Note("non-zero reserved header bytes: %s", r == SND_OK ? "accepted" : "rejected");
  delete img;
}

// Every single-byte corruption of [52, 128): never crashes, and when accepted
// the invariants still hold (it may mean something else, but it is safe).
SND_TEST(BankHeaderGap, EveryByteOfTheUnprotectedRangeIsSafe) {
  const std::vector<std::uint8_t> base = LanguageBankWithDeps().Build();
  std::uint32_t accepted = 0;
  std::uint32_t meaningChanged = 0;
  for (std::size_t pos = 52; pos < 128; ++pos) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<std::uint8_t> f = base;
      f[pos] ^= static_cast<std::uint8_t>(1u << bit);
      Image img(f);
      BankView v;
      ParseError e;
      if (ParseBank(img.data(), img.size(), ParseOptions{}, &v, &e) != SND_OK) continue;
      ++accepted;
      const std::string bad = specwalk::CheckInvariants(v);
      SND_CHECK_MSG(bad.empty(), "byte %zu bit %d: %s", pos, bit, bad.c_str());
      if (bad.empty()) (void)specwalk::DeepWalk(v);
      if (pos < 84) ++meaningChanged;  // counts / offsets / language
    }
  }
  snd::test::Note("single-bit flips in [52,128): %u accepted, %u of them in meaningful fields (56..83)", accepted,
                  meaningChanged);
}
