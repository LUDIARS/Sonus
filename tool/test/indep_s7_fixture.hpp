// Fixture of the independent tests of group M (tool/test/indep_s7_model_properties.md).
// Written by the test owner from the specs and the public headers, not from the
// implementation or the implementer's fixtures.
//
// A small but complete project held in memory: a bank, a bus tree with a send and an
// effect, a category, a voice limit group, parameters, a snapshot, a ducking rule, assets
// (resident, looping, streams with and without preload), a sound event with tracks,
// clips, actions and curves, a 3D event, a music event with segments / playlists / states /
// rules / a stinger, a material table and a patch. WAV files are generated here.
#ifndef SND_TOOL_TEST_INDEP_S7_FIXTURE_HPP
#define SND_TOOL_TEST_INDEP_S7_FIXTURE_HPP

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "snd/buildtool/json.hpp"
#include "snd/tool/document.hpp"

namespace indep_s7 {

namespace json = ::snd::buildtool::json;
namespace fs = std::filesystem;

// --- GUIDs of the fixture objects ------------------------------------------------------------
inline constexpr const char* kBank = "00000000-0000-4000-8000-000000000001";
inline constexpr const char* kMaster = "00000000-0000-4000-8000-000000000002";
inline constexpr const char* kSfx = "00000000-0000-4000-8000-000000000003";
inline constexpr const char* kMusicBus = "00000000-0000-4000-8000-000000000004";
inline constexpr const char* kReverb = "00000000-0000-4000-8000-000000000005";
inline constexpr const char* kCatUi = "00000000-0000-4000-8000-000000000006";
inline constexpr const char* kCatSfx = "00000000-0000-4000-8000-000000000007";
inline constexpr const char* kGroup = "00000000-0000-4000-8000-000000000008";
inline constexpr const char* kSpeed = "00000000-0000-4000-8000-000000000009";
inline constexpr const char* kSurface = "00000000-0000-4000-8000-00000000000a";
inline constexpr const char* kSnap = "00000000-0000-4000-8000-00000000000b";
inline constexpr const char* kDuck = "00000000-0000-4000-8000-00000000000c";
inline constexpr const char* kTone = "00000000-0000-4000-8000-00000000000d";
inline constexpr const char* kLoopAsset = "00000000-0000-4000-8000-00000000000e";
inline constexpr const char* kStream = "00000000-0000-4000-8000-00000000000f";
inline constexpr const char* kStreamNoPre = "00000000-0000-4000-8000-000000000010";
inline constexpr const char* kMusicAsset = "00000000-0000-4000-8000-000000000011";
inline constexpr const char* kShot = "00000000-0000-4000-8000-000000000012";
inline constexpr const char* kHit = "00000000-0000-4000-8000-000000000013";
inline constexpr const char* kEngine = "00000000-0000-4000-8000-000000000014";
inline constexpr const char* kSting = "00000000-0000-4000-8000-000000000015";
inline constexpr const char* kBgm = "00000000-0000-4000-8000-000000000016";
inline constexpr const char* kSegA = "00000000-0000-4000-8000-000000000017";
inline constexpr const char* kSegB = "00000000-0000-4000-8000-000000000018";
inline constexpr const char* kSegBridge = "00000000-0000-4000-8000-000000000019";
inline constexpr const char* kPlExplore = "00000000-0000-4000-8000-00000000001a";
inline constexpr const char* kPlBattle = "00000000-0000-4000-8000-00000000001b";
inline constexpr const char* kPhys = "00000000-0000-4000-8000-00000000001c";
inline constexpr const char* kPatch = "00000000-0000-4000-8000-00000000001d";
inline constexpr const char* kSynthEvent = "00000000-0000-4000-8000-00000000001e";
inline constexpr const char* kProject = "00000000-0000-4000-8000-00000000001f";

// --- WAV -------------------------------------------------------------------------------------
inline void Put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
inline void Put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
  b.push_back(static_cast<std::uint8_t>(v));
  b.push_back(static_cast<std::uint8_t>(v >> 8));
}
inline void PutTag(std::vector<std::uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }

// 16-bit PCM WAV, `channels` interleaved, a quiet ramp. smplStart / smplEnd (end exclusive
// in frames; the smpl chunk stores the inclusive end) add a smpl loop when smplEnd > 0.
inline std::vector<std::uint8_t> MakeWav(std::uint32_t rate, std::uint32_t frames, std::uint32_t channels = 1,
                                         std::uint32_t smplStart = 0, std::uint32_t smplEnd = 0) {
  std::vector<std::uint8_t> b;
  const std::uint32_t dataBytes = frames * channels * 2u;
  const bool smpl = smplEnd > 0;
  const std::uint32_t smplBytes = smpl ? (36u + 24u) : 0u;
  PutTag(b, "RIFF");
  Put32(b, 4u + (8u + 16u) + (8u + dataBytes) + (smpl ? 8u + smplBytes : 0u));
  PutTag(b, "WAVE");
  PutTag(b, "fmt ");
  Put32(b, 16);
  Put16(b, 1);
  Put16(b, static_cast<std::uint16_t>(channels));
  Put32(b, rate);
  Put32(b, rate * channels * 2u);
  Put16(b, static_cast<std::uint16_t>(channels * 2u));
  Put16(b, 16);
  PutTag(b, "data");
  Put32(b, dataBytes);
  for (std::uint32_t i = 0; i < frames * channels; ++i) Put16(b, static_cast<std::uint16_t>((i % 64u) * 16u));
  if (smpl) {
    PutTag(b, "smpl");
    Put32(b, smplBytes);
    for (int i = 0; i < 7; ++i) Put32(b, 0);
    Put32(b, 1);  // loops
    Put32(b, 0);  // sampler data
    Put32(b, 0);  // cue id
    Put32(b, 0);  // type forward
    Put32(b, smplStart);
    Put32(b, smplEnd - 1u);  // inclusive
    Put32(b, 0);
    Put32(b, 0);
  }
  return b;
}

// --- the project -------------------------------------------------------------------------------
inline std::map<std::string, std::string> BaseFiles() {
  std::map<std::string, std::string> f;
  f["project.json"] = std::string(R"({"type": "project", "guid": ")") + kProject + R"(", "name": "Game", "formatVersion": 1})";
  f["banks/Main.json"] = std::string(R"({"type": "bank", "guid": ")") + kBank + R"(", "name": "Main"})";
  f["buses/Master.json"] = std::string(R"({"type": "bus", "guid": ")") + kMaster + R"(", "name": "Master", "master": true})";
  f["buses/SFX.json"] = std::string(R"({"type": "bus", "guid": ")") + kSfx + R"(", "name": "SFX", "parent": ")" + kMaster +
                        R"(", "volumeDb": -3, "sends": [{"bus": ")" + kReverb +
                        R"(", "levelDb": -12, "preFader": false}], "effects": [{"type": "snd.fx.lowpass", "params": [8000]}]})";
  f["buses/Music.json"] = std::string(R"({"type": "bus", "guid": ")") + kMusicBus + R"(", "name": "Music", "parent": ")" +
                          kMaster + R"(", "volumeDb": -6})";
  f["buses/Reverb.json"] = std::string(R"({"type": "bus", "guid": ")") + kReverb + R"(", "name": "Reverb", "parent": ")" +
                           kMaster + R"("})";
  f["categories/UI.json"] = std::string(R"({"type": "category", "guid": ")") + kCatUi +
                            R"(", "name": "UI", "ignorePause": true, "volumeDb": -2})";
  f["categories/SFX.json"] = std::string(R"({"type": "category", "guid": ")") + kCatSfx + R"(", "name": "CatSFX", "volumeDb": -2.5})";
  f["voiceLimitGroups/Steps.json"] = std::string(R"({"type": "voiceLimitGroup", "guid": ")") + kGroup +
                                     R"(", "name": "Steps", "maxVoices": 8, "steal": "oldest"})";
  f["parameters/Speed.json"] = std::string(R"({"type": "parameter", "guid": ")") + kSpeed +
                               R"(", "name": "Speed", "kind": "continuous", "scope": ["global", "emitter"], "min": 0, "max": 10, "default": 2, "rampRate": 5})";
  f["parameters/Surface.json"] = std::string(R"({"type": "parameter", "guid": ")") + kSurface +
                                 R"(", "name": "Surface", "kind": "switch", "min": 0, "max": 4})";
  f["snapshots/Quiet.json"] = std::string(R"({"type": "snapshot", "guid": ")") + kSnap + R"(", "name": "Quiet", "bank": ")" +
                              kBank + R"(", "values": [{"kind": "busVolume", "bus": ")" + kSfx + R"(", "value": -6}]})";
  f["duckings/Duck.json"] = std::string(R"({"type": "ducking", "guid": ")") + kDuck + R"(", "name": "Duck", "bank": ")" + kBank +
                            R"(", "kind": "sidechain", "source": ")" + kMusicBus + R"(", "target": ")" + kSfx +
                            R"(", "amountDb": -6, "attackMs": 20})";
  f["assets/Tone.json"] = std::string(R"({"type": "asset", "guid": ")") + kTone + R"(", "name": "Tone", "file": "wav/tone.wav"})";
  f["assets/Loop.json"] = std::string(R"({"type": "asset", "guid": ")") + kLoopAsset +
                          R"(", "name": "Loop", "file": "wav/tone.wav", "loop": {"start": 12000, "end": 36000}})";
  f["assets/Stream.json"] = std::string(R"({"type": "asset", "guid": ")") + kStream +
                            R"(", "name": "Stream", "file": "wav/music.wav", "loadForm": "stream", "preloadMs": 100})";
  f["assets/StreamNoPre.json"] = std::string(R"({"type": "asset", "guid": ")") + kStreamNoPre +
                                 R"(", "name": "StreamNoPre", "file": "wav/music.wav", "loadForm": "stream", "preloadMs": 0})";
  f["assets/MusicMem.json"] = std::string(R"({"type": "asset", "guid": ")") + kMusicAsset +
                              R"(", "name": "MusicMem", "file": "wav/music.wav"})";
  f["events/SE/Shot.json"] =
      std::string(R"({"type": "event", "guid": ")") + kShot + R"(", "name": "SE/Shot", "bank": ")" + kBank + R"(", "bus": ")" +
      kSfx + R"(", "category": ")" + kCatSfx + R"(", "limitGroup": ")" + kGroup +
      R"(", "priority": 100, "sequence": "random", "maxInstances": 4, "retriggerMs": 50, "fadeOutMs": 30,
 "tracks": [{"clips": [{"asset": ")" +
      kTone + R"(", "startMs": 0, "weight": 2, "volumeDb": -1, "random": {"volumeDb": 2, "pitchCents": 50}},
                        {"asset": ")" +
      kLoopAsset + R"(", "startMs": 100, "loopCount": 3, "pitchCents": 0}],
             "actions": [{"timeMs": 10, "action": "setParameter", "target": ")" +
      kSpeed + R"(", "value": 3}]}],
 "curves": [{"parameter": ")" +
      kSpeed + R"(", "target": "volume", "points": [[0, 0], [10, -12]]}]})";
  f["events/SE/Hit.json"] = std::string(R"({"type": "event", "guid": ")") + kHit + R"(", "name": "SE/Hit", "bank": ")" + kBank +
                            R"(", "tracks": [{"clips": [{"asset": ")" + kTone + R"("}]}]})";
  f["events/SE/Engine.json"] =
      std::string(R"({"type": "event", "guid": ")") + kEngine + R"(", "name": "SE/Engine", "bank": ")" + kBank + R"(", "bus": ")" +
      kSfx + R"(", "is3D": true, "maxDistance": 50, "cone": {"innerDeg": 90, "outerDeg": 180, "outerDb": -12},
 "tracks": [{"clips": [{"asset": ")" +
      kTone + R"(", "loopCount": 0}]}]})";
  f["events/SE/Sting.json"] = std::string(R"({"type": "event", "guid": ")") + kSting + R"(", "name": "SE/Sting", "bank": ")" +
                              kBank + R"(", "stinger": true, "tracks": [{"clips": [{"asset": ")" + kTone + R"("}]}]})";
  f["segments/Seg/A.json"] = std::string(R"({"type": "segment", "guid": ")") + kSegA +
                             R"(", "name": "Seg/A", "asset": ")" + kStream +
                             R"(", "bars": 4, "tempo": [{"tick": 0, "bpm": 120, "beatsPerBar": 4, "beatUnit": 4}],
 "markers": [{"name": "M1", "frame": 16000, "kind": "user"}]})";
  f["segments/Seg/B.json"] = std::string(R"({"type": "segment", "guid": ")") + kSegB + R"(", "name": "Seg/B", "asset": ")" +
                             kStreamNoPre + R"(", "bars": 4, "tempo": [{"tick": 0, "bpm": 120}]})";
  f["segments/Seg/Bridge.json"] = std::string(R"({"type": "segment", "guid": ")") + kSegBridge +
                                  R"(", "name": "Seg/Bridge", "asset": ")" + kMusicAsset + R"(", "bars": 1})";
  f["playlists/PL/Explore.json"] = std::string(R"({"type": "playlist", "guid": ")") + kPlExplore +
                                   R"(", "name": "PL/Explore", "entries": [{"segment": ")" + kSegA + R"("}]})";
  f["playlists/PL/Battle.json"] = std::string(R"({"type": "playlist", "guid": ")") + kPlBattle +
                                  R"(", "name": "PL/Battle", "entries": [{"segment": ")" + kSegB + R"("}]})";
  f["events/Music/BGM.json"] =
      std::string(R"({"type": "event", "guid": ")") + kBgm + R"(", "name": "Music/BGM", "bank": ")" + kBank + R"(", "bus": ")" +
      kMusicBus + R"(",
 "music": {"initialState": "Explore",
           "states": [{"name": "Explore", "playlist": ")" +
      kPlExplore + R"("}, {"name": "Battle", "playlist": ")" + kPlBattle + R"("}],
           "transitions": [{"from": "Explore", "to": "Battle", "timing": "nextBeat", "fadeOutBeats": 0.5, "bridge": ")" +
      kSegBridge + R"(", "late": "nextGrid"},
                           {"from": "*", "to": "*", "timing": "nextBar", "fadeOutBeats": 1, "fadeInBeats": 1}],
           "stingers": [{"name": "Sting/Hit", "event": ")" +
      kSting + R"(", "timing": "nextBeat"}]}})";
  f["materials/Phys.json"] =
      std::string(R"({"type": "materialTable", "guid": ")") + kPhys + R"(", "name": "Phys/Default", "bank": ")" + kBank +
      R"(", "materials": ["metal", "wood", "glass"],
 "single": {"metal": {"impact": ")" +
      kShot + R"("}, "wood": {"impact": ")" + kHit + R"("}},
 "pairs": [{"a": "glass", "b": "metal", "event": ")" +
      kSting + R"("}],
 "footsteps": {"wood": ")" +
      kHit + R"("},
 "defaults": {"impact": ")" +
      kEngine + R"("}})";
  f["patches/Patch/Laser.json"] = std::string(R"({"type": "patch", "guid": ")") + kPatch + R"(", "name": "Patch/Laser",
 "nodes": [{"id": "osc", "kind": "oscillator", "params": {"waveform": "saw", "frequencyHz": 220}, "position": [100, 0]},
           {"id": "env", "kind": "adsr", "params": {"attackMs": 2, "releaseMs": 300}},
           {"id": "vca", "kind": "amp"},
           {"id": "out", "kind": "output"}],
 "connections": [{"from": "osc", "to": "vca", "input": "in"}, {"from": "env", "to": "vca", "input": "gain"},
                 {"from": "vca", "to": "out"}],
 "output": "out"})";
  f["events/SE/Laser.json"] = std::string(R"({"type": "event", "guid": ")") + kSynthEvent +
                              R"(", "name": "SE/Laser", "bank": ")" + kBank +
                              R"(", "tracks": [{"clips": [{"synth": {"kind": "patch", "patch": ")" + kPatch + R"("}}]}]})";
  return f;
}

// tone.wav: 48000 Hz mono, 48000 frames (1000 ms). music.wav: 8000 Hz stereo, 64000 frames (8 s = 4 bars at 120 BPM 4/4).
inline void AddBinaries(snd::tool::ProjectDocument& doc) {
  doc.SetBinaryFile("wav/tone.wav", MakeWav(48000, 48000));
  doc.SetBinaryFile("wav/music.wav", MakeWav(8000, 64000, 2));
}

inline std::uint64_t g_rng = 0x9E3779B97F4A7C15ull;
inline std::uint64_t NextRandom() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

// A document loaded from BaseFiles (no directory), with a deterministic GUID generator.
inline bool LoadBase(snd::tool::ProjectDocument& doc, const std::map<std::string, std::string>& files = BaseFiles()) {
  auto state = std::make_shared<std::uint64_t>(0x1234567u);
  doc.SetRandom([state]() {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
  });
  const bool ok = doc.LoadFiles(files);
  AddBinaries(doc);
  return ok;
}

// --- comparing documents -------------------------------------------------------------------------
// guid -> (file, serialized JSON).
inline std::map<std::string, std::string> Image(const snd::tool::ProjectDocument& doc) {
  std::map<std::string, std::string> m;
  for (const auto& o : doc.objects()) m[o.guid] = o.file + "\n" + json::Write(o.doc);
  return m;
}

// Fresh directory under the test's working directory.
inline std::string TempDir(const char* tag) {
  static unsigned counter = 0;
  const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
  std::string d = "indep_s7_tmp/" + std::string(tag) + "_" + std::to_string(static_cast<unsigned long long>(t)) + "_" +
                  std::to_string(++counter);
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

inline bool WriteFile(const std::string& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  std::fclose(f);
  return ok;
}
inline bool WriteBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  return WriteFile(path, std::string(bytes.begin(), bytes.end()));
}
inline std::string ReadFileText(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return std::string("<missing>");
  std::string s;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

// Writes BaseFiles and the WAV files into `dir`.
inline void WriteBaseProject(const std::string& dir, const std::map<std::string, std::string>& files = BaseFiles()) {
  for (const auto& [p, t] : files) WriteFile(dir + "/" + p, t);
  WriteBytes(dir + "/wav/tone.wav", MakeWav(48000, 48000));
  WriteBytes(dir + "/wav/music.wav", MakeWav(8000, 64000, 2));
}

// Every file under dir (relative path -> bytes).
inline std::map<std::string, std::string> DirContents(const std::string& dir) {
  std::map<std::string, std::string> m;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      std::string rel = fs::relative(it->path(), dir, ec).generic_string();
      m[rel] = ReadFileText(it->path().string());
    }
  }
  return m;
}
inline std::map<std::string, fs::file_time_type> DirTimes(const std::string& dir) {
  std::map<std::string, fs::file_time_type> m;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec)) m[fs::relative(it->path(), dir, ec).generic_string()] = fs::last_write_time(it->path(), ec);
  }
  return m;
}

}  // namespace indep_s7

#endif  // SND_TOOL_TEST_INDEP_S7_FIXTURE_HPP
