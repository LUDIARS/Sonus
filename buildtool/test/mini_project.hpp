// Tiny project builder for the independent buildtool tests: writes JSON
// objects with generated GUIDs and WAV inputs into a fresh temp directory.
#ifndef SND_BUILDTOOL_TEST_MINI_PROJECT_HPP
#define SND_BUILDTOOL_TEST_MINI_PROJECT_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

#include "snd/buildtool/build.hpp"
#include "snd/buildtool/wav.hpp"

namespace mini {

namespace fs = std::filesystem;
namespace bt = snd::buildtool;

inline void WriteBytes(const fs::path& p, const std::vector<std::uint8_t>& b) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
inline std::vector<std::uint8_t> ReadBytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
inline std::string ReadText(const fs::path& p) {
  const std::vector<std::uint8_t> b = ReadBytes(p);
  return std::string(b.begin(), b.end());
}

inline bt::PcmData Tone(std::uint32_t channels, std::uint32_t rate, std::uint64_t frames, double hz = 440) {
  bt::PcmData p;
  p.channels = channels;
  p.sampleRate = rate;
  p.frames = frames;
  p.samples.resize(static_cast<std::size_t>(frames * channels));
  for (std::uint64_t f = 0; f < frames; ++f) {
    for (std::uint32_t c = 0; c < channels; ++c) {
      p.samples[static_cast<std::size_t>(f * channels + c)] =
          static_cast<float>(0.5 * std::sin(6.283185307179586 * hz * (c + 1) * static_cast<double>(f) / rate));
    }
  }
  return p;
}

class Project {
 public:
  explicit Project(const std::string& name) {
    std::error_code ec;
    root_ = fs::temp_directory_path(ec) / ("sonus-it-" + name + "-" + std::to_string(::getpid()));
    fs::remove_all(root_, ec);
    fs::create_directories(dir(), ec);
  }
  ~Project() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }
  fs::path root() const { return root_; }
  fs::path dir() const { return root_ / "project"; }
  fs::path out() const { return root_ / "out"; }

  static std::string Guid(std::uint32_t n) {
    char b[48];
    std::snprintf(b, sizeof b, "7e570000-0000-4000-8000-%012x", n);
    return b;
  }
  // Writes `{"type": type, "guid": <next>, "name": name, <body>}` into rel.
  std::string Add(const std::string& rel, const std::string& type, const std::string& name,
                  const std::string& body = "") {
    const std::string g = Guid(++guid_);
    std::string text = "{\n  \"type\": \"" + type + "\",\n  \"guid\": \"" + g + "\"";
    if (!name.empty()) text += ",\n  \"name\": \"" + name + "\"";
    if (!body.empty()) text += ",\n  " + body;
    text += "\n}\n";
    Raw(rel, text);
    return g;
  }
  void Raw(const std::string& rel, const std::string& text) {
    WriteBytes(dir() / rel, std::vector<std::uint8_t>(text.begin(), text.end()));
  }
  void Wav(const std::string& rel, const bt::PcmData& p, bt::WavSampleFormat f = bt::WavSampleFormat::Int16) {
    WriteBytes(dir() / rel, bt::WriteWav(p, f));
  }
  // Bank "Main", master bus "Master", one asset "a" (mono 48 kHz), one event "E".
  void Minimal() {
    Add("banks/Main.json", "bank", "Main");
    Add("buses/Master.json", "bus", "Master", "\"master\": true");
    Wav("audio/a.wav", Tone(1, 48000, 480));
    Add("assets/a.json", "asset", "a", "\"file\": \"audio/a.wav\"");
    Add("events/E.json", "event", "E", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"a\"}]}]");
  }

  bt::BuildOptions Options() const {
    bt::BuildOptions o;
    o.projectDir = dir().string();
    o.outDir = out().string();
    return o;
  }
  bool Build(bt::Diagnostics* d, bt::BuildResult* r = nullptr) const { return Build(Options(), d, r); }
  bool Build(const bt::BuildOptions& o, bt::Diagnostics* d, bt::BuildResult* r = nullptr) const {
    bt::BuildResult local;
    return bt::Build(o, *d, r != nullptr ? r : &local);
  }

 private:
  fs::path root_;
  std::uint32_t guid_ = 0;
};

inline std::string Dump(const bt::Diagnostics& d) {
  std::string s;
  for (const bt::Diagnostic& x : d.items()) s += bt::Diagnostics::Format(x) + "\n";
  return s;
}

inline bool HasDiag(const bt::Diagnostics& d, bt::Severity sev, const std::string& needle) {
  for (const bt::Diagnostic& x : d.items()) {
    if (x.severity == sev && bt::Diagnostics::Format(x).find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace mini

#endif  // SND_BUILDTOOL_TEST_MINI_PROJECT_HPP
