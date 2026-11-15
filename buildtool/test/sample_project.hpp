// Test helper: copies buildtool/test/data/sample (JSON only) into a fresh
// temporary directory and generates its WAV / blob inputs, so tests can
// build, modify and rebuild without touching the source tree.
// Shared by buildtool/test and bank/test (round trip).
#ifndef SND_BUILDTOOL_TEST_SAMPLE_PROJECT_HPP
#define SND_BUILDTOOL_TEST_SAMPLE_PROJECT_HPP

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

#include "snd/buildtool/wav.hpp"

#ifndef SND_BUILDTOOL_TEST_DATA
#error "SND_BUILDTOOL_TEST_DATA must point at buildtool/test/data"
#endif

namespace sample {

namespace fs = std::filesystem;

inline void WriteFile(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
inline void WriteText(const fs::path& p, const std::string& text) {
  WriteFile(p, std::vector<std::uint8_t>(text.begin(), text.end()));
}
inline std::vector<std::uint8_t> ReadFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline snd::buildtool::PcmData Tone(std::uint32_t channels, std::uint32_t rate, std::uint64_t frames, double hz) {
  snd::buildtool::PcmData p;
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

// Fresh directory under the system temp dir (removed first; per process).
inline fs::path TempDir(const std::string& name) {
  std::error_code ec;
  const fs::path d = fs::temp_directory_path(ec) / ("sonus-test-" + name + "-" + std::to_string(::getpid()));
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

// Project copy + generated inputs. Returns the project directory.
inline fs::path MakeProject(const std::string& name) {
  const fs::path root = TempDir(name);
  const fs::path proj = root / "project";
  std::error_code ec;
  fs::copy(fs::path(SND_BUILDTOOL_TEST_DATA) / "sample", proj, fs::copy_options::recursive, ec);
  using snd::buildtool::WavSampleFormat;
  WriteFile(proj / "audio/boom.wav", snd::buildtool::WriteWav(Tone(1, 44100, 1000, 440), WavSampleFormat::Int16));
  WriteFile(proj / "audio/amb.wav", snd::buildtool::WriteWav(Tone(2, 48000, 6000, 220), WavSampleFormat::Int24));
  WriteFile(proj / "audio/seg.wav", snd::buildtool::WriteWav(Tone(2, 48000, 8000, 330), WavSampleFormat::Float32));
  WriteFile(proj / "audio/ja/hello.wav", snd::buildtool::WriteWav(Tone(1, 48000, 900, 500), WavSampleFormat::Int16));
  WriteFile(proj / "audio/en/hello.wav", snd::buildtool::WriteWav(Tone(1, 48000, 700, 600), WavSampleFormat::Int32));
  std::vector<std::uint8_t> ir(100);
  for (std::size_t i = 0; i < ir.size(); ++i) ir[i] = static_cast<std::uint8_t>(i * 7);
  WriteFile(proj / "audio/ir.bin", ir);
  return proj;
}

}  // namespace sample

#endif  // SND_BUILDTOOL_TEST_SAMPLE_PROJECT_HPP
