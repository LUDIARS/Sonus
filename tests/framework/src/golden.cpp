#include "snd/test/golden.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "snd/test/test.hpp"

namespace snd::test {
namespace {

constexpr std::uint32_t kGoldenMagic = 0x47444E53u;  // 'SNDG' little endian
constexpr std::uint32_t kGoldenVersion = 1;

struct GoldenHeader {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t channels;
  std::uint32_t sampleRate;
  std::uint64_t frames;
  std::uint32_t reserved[2];
};
static_assert(sizeof(GoldenHeader) == 32);

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return std::string(buf);
}

}  // namespace

AudioBuffer AudioBuffer::FromInterleaved(const float* data, std::uint32_t ch, std::uint64_t fr,
                                         std::uint32_t rate) {
  AudioBuffer b(ch, fr, rate);
  for (std::uint64_t i = 0; i < fr; ++i) {
    for (std::uint32_t c = 0; c < ch; ++c) b.Channel(c)[i] = data[i * ch + c];
  }
  return b;
}

GoldenDiff DiffAudio(const AudioBuffer& ref, const AudioBuffer& act, const GoldenTolerance& tol) {
  GoldenDiff d;
  if (ref.channels != act.channels || ref.frames != act.frames || ref.sampleRate != act.sampleRate) {
    d.shapeMismatch = true;
    d.passed = false;
    d.summary = Format("shape mismatch: reference %u ch x %llu frames @ %u Hz, actual %u ch x %llu frames @ %u Hz",
                       ref.channels, static_cast<unsigned long long>(ref.frames), ref.sampleRate,
                       act.channels, static_cast<unsigned long long>(act.frames), act.sampleRate);
    return d;
  }
  double sumSq = 0.0;
  bool haveFirst = false;
  const std::uint64_t total = static_cast<std::uint64_t>(ref.channels) * ref.frames;
  for (std::uint32_t c = 0; c < ref.channels; ++c) {
    const float* r = ref.Channel(c);
    const float* a = act.Channel(c);
    for (std::uint64_t i = 0; i < ref.frames; ++i) {
      const double diff = static_cast<double>(a[i]) - static_cast<double>(r[i]);
      const double ad = std::fabs(diff);
      const bool bad = std::isnan(diff) || ad > static_cast<double>(tol.maxAbs);
      if (std::fabs(r[i]) > d.refPeak) d.refPeak = std::fabs(r[i]);
      if (std::fabs(a[i]) > d.actPeak) d.actPeak = std::fabs(a[i]);
      if (!std::isnan(diff)) sumSq += diff * diff;
      if (bad) {
        ++d.mismatches;
        if (!haveFirst) {
          haveFirst = true;
          d.firstChannel = c;
          d.firstFrame = i;
        }
      }
      if (std::isnan(diff) || ad > d.maxAbsDiff) {
        d.maxAbsDiff = std::isnan(diff) ? INFINITY : ad;
        d.maxChannel = c;
        d.maxFrame = i;
        d.refAtMax = r[i];
        d.actAtMax = a[i];
      }
    }
  }
  d.rmsDiff = total > 0 ? std::sqrt(sumSq / static_cast<double>(total)) : 0.0;
  const bool rmsOk = tol.maxRms < 0.0f || d.rmsDiff <= static_cast<double>(tol.maxRms);
  d.passed = d.mismatches <= tol.maxMismatches && rmsOk;
  d.summary = Format(
      "%s: max |diff| %.6g at ch%u frame %llu (ref %.6g, act %.6g); rms diff %.3g%s; "
      "%llu/%llu samples exceed %.3g (allowed %llu)%s; peak ref %.6g act %.6g",
      d.passed ? "match" : "MISMATCH", d.maxAbsDiff, d.maxChannel,
      static_cast<unsigned long long>(d.maxFrame), static_cast<double>(d.refAtMax),
      static_cast<double>(d.actAtMax), d.rmsDiff, rmsOk ? "" : " (over rms tolerance)",
      static_cast<unsigned long long>(d.mismatches), static_cast<unsigned long long>(total),
      static_cast<double>(tol.maxAbs), static_cast<unsigned long long>(tol.maxMismatches),
      haveFirst ? Format(", first at ch%u frame %llu", d.firstChannel,
                         static_cast<unsigned long long>(d.firstFrame))
                      .c_str()
                : "",
      static_cast<double>(d.refPeak), static_cast<double>(d.actPeak));
  return d;
}

bool SaveGolden(const std::string& path, const AudioBuffer& b) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  GoldenHeader h{kGoldenMagic, kGoldenVersion, b.channels, b.sampleRate, b.frames, {0, 0}};
  bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1;
  if (ok && !b.samples.empty()) {
    ok = std::fwrite(b.samples.data(), sizeof(float), b.samples.size(), f) == b.samples.size();
  }
  ok = (std::fclose(f) == 0) && ok;
  return ok;
}

bool LoadGolden(const std::string& path, AudioBuffer* out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  GoldenHeader h{};
  bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && h.magic == kGoldenMagic &&
            h.version == kGoldenVersion && h.channels <= 64 && h.frames < (1ull << 32);
  if (ok) {
    AudioBuffer b(h.channels, h.frames, h.sampleRate);
    if (!b.samples.empty()) {
      ok = std::fread(b.samples.data(), sizeof(float), b.samples.size(), f) == b.samples.size();
    }
    if (ok) *out = std::move(b);
  }
  std::fclose(f);
  return ok;
}

GoldenResult CompareGolden(const char* dir, const char* name, const AudioBuffer& actual,
                           const GoldenTolerance& tol, GoldenMode mode) {
  GoldenResult r;
  r.path = std::string(dir) + "/" + name + ".sndg";
  const bool update =
      mode == GoldenMode::Update || (mode == GoldenMode::FromEnvironment && UpdateGoldenRequested());
  if (update) {
    r.updated = SaveGolden(r.path, actual);
    r.passed = r.updated;
    r.summary = r.updated ? "reference updated: " + r.path : "could not write reference: " + r.path;
    return r;
  }
  AudioBuffer ref;
  if (!LoadGolden(r.path, &ref)) {
    r.missing = true;
    r.passed = false;
    r.summary = "reference missing or unreadable: " + r.path +
                " (run the test with --update-golden or SND_UPDATE_GOLDEN=1 to create it)";
    return r;
  }
  GoldenDiff d = DiffAudio(ref, actual, tol);
  r.passed = d.passed;
  r.summary = d.summary;
  return r;
}

void CheckGolden(const char* file, int line, const char* dir, const char* name,
                 const AudioBuffer& actual, const GoldenTolerance& tol) {
  GoldenResult r = CompareGolden(dir, name, actual, tol);
  if (r.updated) {
    Note("golden '%s': %s", name, r.summary.c_str());
  } else if (!r.passed) {
    ReportFailureF(file, line, "golden", "'%s': %s", name, r.summary.c_str());
  }
}

}  // namespace snd::test
