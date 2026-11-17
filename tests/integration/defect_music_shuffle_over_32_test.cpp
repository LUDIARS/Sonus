// Defect (independent music tests): a shuffle playlist with more than 32
// entries never plays the entries after the 32nd.
//
// Expected (8.3, project-format 2.x playlist / bank MPLE): a shuffle pass plays
// every entry once; neither the project format nor the bank format limits the
// entry count.
// Seen: the shuffle permutation is capped at kMaxShuffle = 32 entries; entry 33
// is silently never chosen (pass length 32).
// Reproduce: 33 one-bar segments at 960 BPM (bar = 12000 samples), mode shuffle,
// loopCount 1: after 33 bars every asset must have started once.
#include "music_indep_util.hpp"

using namespace mi;

SND_TEST(MusicIndepDefect, ShuffleWithThirtyThreeEntriesPlaysEveryEntry) {
  MusicProject p("defect-shuffle");
  std::string entries;
  constexpr int kN = 33;
  for (int i = 0; i < kN; ++i) {
    const std::string n = "a" + std::to_string(i);
    p.Asset(n, Material(12000, 0.1f));
    p.Segment("S" + n, n, 1, "\"tempo\": [{\"tick\": 0, \"bpm\": 960}]");
    entries += (i == 0 ? "" : ", ") + std::string("{\"segment\": \"S") + n + "\"}";
  }
  p.Playlist("PL", entries, "\"mode\": \"shuffle\", \"loopCount\": 1");
  p.MusicEvent("Music", kOneState);
  const std::vector<std::uint8_t> bank = BuildBank(p);
  SND_REQUIRE(!bank.empty());
  Rig r;
  SND_REQUIRE(r.Load(bank));
  const std::uint64_t s0 = r.Clock();
  r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + (kN + 2) * 12000);
  int missing = 0;
  for (int i = 0; i < kN; ++i) {
    const std::string n = "a" + std::to_string(i);
    const std::size_t k = StartsOf(r, n.c_str()).size();
    if (k != 1u) {
      ++missing;
      std::fprintf(stderr, "%s started %zu times\n", n.c_str(), k);
    }
  }
  SND_CHECK_EQ(missing, 0);
}
