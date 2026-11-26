// In-memory projects for the document / editor model tests (no game, no Tela).
//
// Base project "Doc" (bank "Main"):
//   buses     Master (master), Sfx (parent Master, effect snd.fx.lowpass [1000], send -> Reverb),
//             Reverb (parent Master)
//   category  SE; parameter Speed (0..10, default 1, rampRate 5)
//   asset     tone: mono 48 kHz WAV of kToneFrames frames (in memory), loop 0..kToneFrames
//   event     Shot (Sfx, SE, one track with one clip of "tone", 3D, maxDistance 50,
//             volume distance curve, a Speed pitch curve)
//   snapshot  Quiet (bank Main: Sfx -6 dB)
// Every file carries an unknown member and "notes" at two levels (they must survive).
#ifndef SND_TOOL_TEST_DOC_FIXTURE_HPP
#define SND_TOOL_TEST_DOC_FIXTURE_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

#include "snd/buildtool/wav.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/document.hpp"

// SND_CHECK_MSG that also leaves the test (the framework has no SND_REQUIRE_MSG).
#define SND_REQUIRE_MSG(cond, ...)                                                    \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      ::snd::test::ReportFailureF(__FILE__, __LINE__, #cond, __VA_ARGS__);            \
      return;                                                                         \
    }                                                                                 \
  } while (0)

namespace doctest {

namespace fs = std::filesystem;
namespace bt = snd::buildtool;

inline constexpr std::uint32_t kToneFrames = 4800;

inline std::string G(std::uint32_t n) {
  char b[48];
  std::snprintf(b, sizeof b, "d0c00000-0000-4000-8000-%012x", n);
  return b;
}

inline std::vector<std::uint8_t> ToneWav(std::uint32_t frames = kToneFrames, std::uint32_t rate = 48000,
                                         std::uint32_t channels = 1, float value = 0.25f) {
  bt::PcmData p;
  p.channels = channels;
  p.sampleRate = rate;
  p.frames = frames;
  p.samples.assign(std::size_t{frames} * channels, value);
  return bt::WriteWav(p, bt::WavSampleFormat::Float32);
}

inline std::map<std::string, std::string> BaseFiles() {
  std::map<std::string, std::string> f;
  f["project.json"] = "{\"type\": \"project\", \"guid\": \"" + G(100) + "\", \"name\": \"Doc\", \"formatVersion\": 1}\n";
  f["banks/Main.json"] = "{\"type\": \"bank\", \"guid\": \"" + G(1) + "\", \"name\": \"Main\"}\n";
  f["buses/Master.json"] =
      "{\n  \"type\": \"bus\",\n  \"guid\": \"" + G(2) + "\",\n  \"name\": \"Master\",\n  \"master\": true,\n"
      "  \"x-editor\": {\"color\": \"#ff0000\"},\n  \"notes\": {\"who\": \"mixer\"}\n}\n";
  f["buses/Sfx.json"] =
      "{\"type\": \"bus\", \"guid\": \"" + G(3) + "\", \"name\": \"Sfx\", \"parent\": \"Master\", \"volumeDb\": -1.5,\n"
      " \"sends\": [{\"bus\": \"" + G(4) + "\", \"levelDb\": -12, \"x-send\": 1}],\n"
      " \"effects\": [{\"type\": \"snd.fx.lowpass\", \"params\": [1000], \"notes\": \"keep\"}]}\n";
  f["buses/Reverb.json"] = "{\"type\": \"bus\", \"guid\": \"" + G(4) + "\", \"name\": \"Reverb\", \"parent\": \"" + G(2) + "\"}\n";
  f["categories/SE.json"] = "{\"type\": \"category\", \"guid\": \"" + G(5) + "\", \"name\": \"SE\"}\n";
  f["parameters/Speed.json"] = "{\"type\": \"parameter\", \"guid\": \"" + G(6) +
                               "\", \"name\": \"Speed\", \"min\": 0, \"max\": 10, \"default\": 1, \"rampRate\": 5}\n";
  f["assets/tone.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(7) + "\", \"name\": \"tone\", \"file\": \"audio/tone.wav\", "
                          "\"codec-hint\": \"keep me\"}\n";
  f["events/Shot.json"] =
      "{\"type\": \"event\", \"guid\": \"" + G(8) + "\", \"name\": \"Shot\", \"bank\": \"Main\", \"bus\": \"" + G(3) +
      "\", \"category\": \"SE\", \"is3D\": true, \"maxDistance\": 50,\n"
      " \"distanceCurves\": {\"volume\": {\"points\": [[1, 0], [50, -40]]}},\n"
      " \"curves\": [{\"parameter\": \"Speed\", \"target\": \"pitch\", \"points\": [[0, 0], [10, 1200]]}],\n"
      " \"tracks\": [{\"clips\": [{\"asset\": \"" + G(7) + "\", \"volumeDb\": -3, \"x-clip\": [1, 2], \"notes\": \"c\"}]}],\n"
      " \"x-top\": 7}\n";
  f["snapshots/Quiet.json"] = "{\"type\": \"snapshot\", \"guid\": \"" + G(9) + "\", \"name\": \"Quiet\", \"bank\": \"Main\", "
                              "\"values\": [{\"kind\": \"busVolume\", \"bus\": \"Sfx\", \"value\": -6}]}\n";
  return f;
}

// A deterministic GUID source for tests.
struct Counter {
  std::uint64_t n = 0;
  std::uint64_t operator()() { return 0x1111111111111111ull * (++n); }
};

inline void LoadBase(snd::tool::ProjectDocument& doc, const std::map<std::string, std::string>& files = BaseFiles(),
                     const std::string& dir = std::string()) {
  doc.LoadFiles(files, dir);
  doc.SetBinaryFile("audio/tone.wav", ToneWav());
  doc.SetRandom(Counter{});
}

// A scratch directory (removed at the end).
class TempDir {
 public:
  explicit TempDir(const std::string& name) {
    static int counter = 0;
    std::error_code ec;
    root_ = fs::temp_directory_path(ec) /
            ("sonus-doc-" + name + "-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
    fs::remove_all(root_, ec);
    fs::create_directories(root_, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  fs::path path() const { return root_; }
  void Write(const std::string& rel, const std::string& text) const {
    std::error_code ec;
    fs::create_directories((root_ / rel).parent_path(), ec);
    std::FILE* f = std::fopen((root_ / rel).string().c_str(), "wb");
    if (f != nullptr) {
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    }
  }
  void WriteBytes(const std::string& rel, const std::vector<std::uint8_t>& b) const {
    std::error_code ec;
    fs::create_directories((root_ / rel).parent_path(), ec);
    std::FILE* f = std::fopen((root_ / rel).string().c_str(), "wb");
    if (f != nullptr) {
      std::fwrite(b.data(), 1, b.size(), f);
      std::fclose(f);
    }
  }
  std::string Read(const std::string& rel) const {
    std::string s;
    std::FILE* f = std::fopen((root_ / rel).string().c_str(), "rb");
    if (f == nullptr) return s;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
    std::fclose(f);
    return s;
  }
  bool Exists(const std::string& rel) const {
    std::error_code ec;
    return fs::exists(root_ / rel, ec);
  }
  // Every file with its modification time (to see that nothing was rewritten).
  std::map<std::string, fs::file_time_type> Stamps() const {
    std::map<std::string, fs::file_time_type> m;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root_, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec)) m[fs::relative(it->path(), root_, ec).generic_string()] = it->last_write_time(ec);
    }
    return m;
  }

 private:
  fs::path root_;
};

inline void WriteAll(const TempDir& d, const std::map<std::string, std::string>& files) {
  for (const auto& [f, t] : files) d.Write(f, t);
  d.WriteBytes("audio/tone.wav", ToneWav());
}

inline bool HasIssue(const std::vector<snd::tool::DocIssue>& issues, const std::string& needle, bool error = true) {
  for (const snd::tool::DocIssue& i : issues) {
    if (i.error == error && i.message.find(needle) != std::string::npos) return true;
  }
  return false;
}

inline std::string Dump(const std::vector<snd::tool::DocIssue>& issues) {
  std::string s;
  for (const snd::tool::DocIssue& i : issues) s += i.text + "\n";
  return s;
}

}  // namespace doctest

#endif  // SND_TOOL_TEST_DOC_FIXTURE_HPP
