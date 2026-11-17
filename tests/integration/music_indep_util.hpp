// Helpers of the independent music tests (music_indep_test.cpp, defect_music_*.cpp).
// Expected values are computed from design 8 / the contract, not from the
// implementation: 1 quarter note = 960 ticks, bpm counts quarter notes,
// a beat = the signature's beat unit, block = 256 frames at 48 kHz.
#ifndef SND_TESTS_INTEGRATION_MUSIC_INDEP_UTIL_HPP
#define SND_TESTS_INTEGRATION_MUSIC_INDEP_UTIL_HPP

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "music_fixture.hpp"

namespace mi {

using namespace mt;

inline std::vector<std::uint8_t> BuildBank(MusicProject& p) {
  std::string diag;
  std::vector<std::uint8_t> bank = p.Build(&diag);
  if (bank.empty()) std::fprintf(stderr, "%s\n", diag.c_str());
  return bank;
}

inline const char* kOneState = "\"initialState\": \"S\", \"states\": [{\"name\": \"S\", \"playlist\": \"PL\"}]";

// Samples per quarter note at `bpm` (48 kHz).
inline double Quarter(double bpm) { return 60.0 * kRate / bpm; }

inline std::uint64_t Round(double c) { return static_cast<std::uint64_t>(std::llround(c)); }

// All beat records of transport `t`.
inline std::vector<Beat> BeatsOf(const Rig& r, snd_transport_handle t) {
  std::vector<Beat> v;
  for (const Beat& b : r.ev.beats) {
    if (b.transport == t.value) v.push_back(b);
  }
  return v;
}

inline std::vector<StartLog> StartsOf(const Rig& r, const char* asset) {
  std::vector<StartLog> v;
  for (const StartLog& s : r.kind.Starts()) {
    if (s.asset == HashId(asset)) v.push_back(s);
  }
  return v;
}

inline std::string Rule(const std::string& from, const std::string& to, const std::string& more) {
  return "{\"from\": \"" + from + "\", \"to\": \"" + to + "\"" + (more.empty() ? "" : ", " + more) + "}";
}

}  // namespace mi

#endif  // SND_TESTS_INTEGRATION_MUSIC_INDEP_UTIL_HPP
