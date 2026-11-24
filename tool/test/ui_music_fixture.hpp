// The music project of the music editor model's tests (music_editor_test.cpp) for the screen tests:
// assets a (8 s, stream, preload 0), b (8 s, memory), br (2 s bridge); segments SegA (a, 4 bars 120
// BPM 4/4, markers Drop / Out), SegB (b, 2 bars 60 BPM 3/4), Bridge (br, 1 bar); playlists PA, PB;
// music event Music (Explore -> PA, Battle -> PB) with three rules (the first covers the others)
// and a stinger.
#ifndef SND_TOOL_TEST_UI_MUSIC_FIXTURE_HPP
#define SND_TOOL_TEST_UI_MUSIC_FIXTURE_HPP

#include <map>
#include <string>

#include "doc_fixture.hpp"

namespace edtest {

inline constexpr std::uint32_t kMusicRate = 48000;

inline std::map<std::string, std::string> MusicFiles() {
  using doctest::G;
  std::map<std::string, std::string> f = doctest::BaseFiles();
  f["assets/a.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(80) + "\", \"name\": \"a\", \"file\": \"audio/a.wav\", "
                       "\"loadForm\": \"stream\", \"preloadMs\": 0}";
  f["assets/b.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(81) + "\", \"name\": \"b\", \"file\": \"audio/b.wav\"}";
  f["assets/br.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(82) + "\", \"name\": \"br\", \"file\": \"audio/br.wav\", "
                        "\"loadForm\": \"stream\"}";
  f["segments/SegA.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(83) + "\", \"name\": \"SegA\", \"asset\": \"a\", \"bars\": 4, "
                            "\"tempo\": [{\"tick\": 0, \"bpm\": 120, \"beatsPerBar\": 4, \"beatUnit\": 4}], "
                            "\"markers\": [{\"name\": \"Drop\", \"frame\": 30000}, {\"name\": \"Out\", \"frame\": 90000, \"kind\": \"exit\"}]}";
  f["segments/SegB.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(84) + "\", \"name\": \"SegB\", \"asset\": \"b\", \"bars\": 2, "
                            "\"tempo\": [{\"tick\": 0, \"bpm\": 60, \"beatsPerBar\": 3, \"beatUnit\": 4}], "
                            "\"markers\": [{\"name\": \"Hit\", \"frame\": 24000}]}";
  f["segments/Bridge.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(85) + "\", \"name\": \"Bridge\", \"asset\": \"br\", \"bars\": 1}";
  f["playlists/PA.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(86) + "\", \"name\": \"PA\", \"entries\": [{\"segment\": \"SegA\"}]}";
  f["playlists/PB.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(87) + "\", \"name\": \"PB\", \"entries\": [{\"segment\": \"SegB\"}]}";
  f["events/Music.json"] =
      "{\"type\": \"event\", \"guid\": \"" + G(88) + "\", \"name\": \"Music\", \"bank\": \"Main\", \"music\": {"
      "\"initialState\": \"Explore\", \"states\": [{\"name\": \"Explore\", \"playlist\": \"PA\"}, {\"name\": \"Battle\", \"playlist\": \"PB\"}],"
      "\"transitions\": [{\"from\": \"*\", \"to\": \"*\", \"timing\": \"nextBar\"},"
      "                  {\"from\": \"Explore\", \"to\": \"Battle\", \"timing\": \"nextBeat\", \"fadeOutBeats\": 0.5, \"bridge\": \"Bridge\"},"
      "                  {\"from\": \"*\", \"to\": \"Battle\", \"timing\": \"segmentEnd\"}],"
      "\"stingers\": [{\"name\": \"Hit\", \"event\": \"Shot\"}]}}";
  return f;
}

inline void AddMusicWaves(snd::tool::ProjectDocument& doc) {
  doc.SetBinaryFile("audio/a.wav", doctest::ToneWav(8 * kMusicRate, kMusicRate, 2));
  doc.SetBinaryFile("audio/b.wav", doctest::ToneWav(8 * kMusicRate, kMusicRate, 2));
  doc.SetBinaryFile("audio/br.wav", doctest::ToneWav(2 * kMusicRate, kMusicRate, 2));
}

}  // namespace edtest

#endif  // SND_TOOL_TEST_UI_MUSIC_FIXTURE_HPP
