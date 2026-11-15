// Golden audio comparison (design.md 14 "ゴールデン音声").
//
// Reference files: <golden dir>/<name>.sndg, float32 little endian:
//   offset 0  u32 magic 'SNDG'
//          4  u32 version (1)
//          8  u32 channels
//         12  u32 sampleRate
//         16  u64 frames
//         24  u32 reserved[2] (0)
//         32  float32 samples, channel-major (all frames of channel 0, then 1, ...)
// The golden directory of a test target is <test source dir>/golden unless
// snd_add_test(... GOLDEN_DIR <dir>) says otherwise (macro SND_GOLDEN_DIR).
//
// Updating references: run the test executable with --update-golden, or set
// SND_UPDATE_GOLDEN=1 (e.g. `SND_UPDATE_GOLDEN=1 ctest -R dsp`). The actual
// output is written as the new reference and the check passes with a note.
// A missing reference fails with a hint unless an update is requested.
#ifndef SND_TEST_GOLDEN_HPP
#define SND_TEST_GOLDEN_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace snd::test {

// Non-interleaved float32 audio owned by the test.
struct AudioBuffer {
  std::uint32_t channels = 0;
  std::uint32_t sampleRate = 48000;
  std::uint64_t frames = 0;
  std::vector<float> samples;  // channel-major: samples[c * frames + i]

  AudioBuffer() = default;
  AudioBuffer(std::uint32_t ch, std::uint64_t fr, std::uint32_t rate = 48000)
      : channels(ch), sampleRate(rate), frames(fr), samples(static_cast<std::size_t>(ch * fr), 0.0f) {}
  float* Channel(std::uint32_t c) { return samples.data() + static_cast<std::size_t>(c * frames); }
  const float* Channel(std::uint32_t c) const {
    return samples.data() + static_cast<std::size_t>(c * frames);
  }
  // Builds from interleaved data (e.g. NullOutputDevice::Captured()).
  static AudioBuffer FromInterleaved(const float* data, std::uint32_t channels, std::uint64_t frames,
                                     std::uint32_t sampleRate = 48000);
};

struct GoldenTolerance {
  float maxAbs = 1e-6f;          // per-sample absolute tolerance
  float maxRms = -1.0f;          // RMS of the difference; < 0 = not checked
  std::uint64_t maxMismatches = 0;  // samples allowed to exceed maxAbs
};

struct GoldenDiff {
  bool shapeMismatch = false;
  std::uint64_t mismatches = 0;  // samples with |diff| > maxAbs (NaN counts as mismatch)
  double maxAbsDiff = 0.0;
  std::uint32_t maxChannel = 0;
  std::uint64_t maxFrame = 0;
  float refAtMax = 0.0f;
  float actAtMax = 0.0f;
  std::uint32_t firstChannel = 0;
  std::uint64_t firstFrame = 0;
  double rmsDiff = 0.0;
  float refPeak = 0.0f;
  float actPeak = 0.0f;
  bool passed = false;
  std::string summary;  // human-readable, one line
};

GoldenDiff DiffAudio(const AudioBuffer& reference, const AudioBuffer& actual,
                     const GoldenTolerance& tol);

bool SaveGolden(const std::string& path, const AudioBuffer& buffer);
bool LoadGolden(const std::string& path, AudioBuffer* out);

enum class GoldenMode { Compare, Update, FromEnvironment };

struct GoldenResult {
  bool passed = false;
  bool updated = false;
  bool missing = false;
  std::string path;
  std::string summary;
};

// Compares `actual` with <dir>/<name>.sndg. FromEnvironment = Update if
// --update-golden or SND_UPDATE_GOLDEN=1, else Compare.
GoldenResult CompareGolden(const char* dir, const char* name, const AudioBuffer& actual,
                           const GoldenTolerance& tol, GoldenMode mode = GoldenMode::FromEnvironment);

void CheckGolden(const char* file, int line, const char* dir, const char* name,
                 const AudioBuffer& actual, const GoldenTolerance& tol);

}  // namespace snd::test

#ifndef SND_GOLDEN_DIR
#define SND_GOLDEN_DIR "golden"
#endif

// Fails the running test with the diff summary when the comparison fails.
#define SND_CHECK_GOLDEN(name, buffer, tolerance) \
  ::snd::test::CheckGolden(__FILE__, __LINE__, SND_GOLDEN_DIR, (name), (buffer), (tolerance))

#endif  // SND_TEST_GOLDEN_HPP
