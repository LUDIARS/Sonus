#include "convert.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "detail/fnv.hpp"

namespace snd::buildtool::detail {
namespace {

constexpr std::uint64_t kCacheVersion = 1;
constexpr std::uint32_t kCacheMagic = 0x43444E53u;  // 'SNDC'

void Put(std::vector<std::uint8_t>& v, std::uint64_t x) {
  for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void PutBytes(std::vector<std::uint8_t>& v, const std::vector<std::uint8_t>& b) {
  Put(v, b.size());
  v.insert(v.end(), b.begin(), b.end());
}
struct Reader {
  const std::vector<std::uint8_t>& b;
  std::size_t pos = 0;
  bool ok = true;
  std::uint64_t Get() {
    if (pos + 8 > b.size()) {
      ok = false;
      return 0;
    }
    std::uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<std::uint64_t>(b[pos + static_cast<std::size_t>(i)]) << (8 * i);
    pos += 8;
    return x;
  }
  std::vector<std::uint8_t> Bytes() {
    const std::uint64_t n = Get();
    if (!ok || n > b.size() - pos) {
      ok = false;
      return {};
    }
    std::vector<std::uint8_t> v(b.begin() + static_cast<std::ptrdiff_t>(pos),
                                b.begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += static_cast<std::size_t>(n);
    return v;
  }
};

std::string CachePath(const std::string& dir, std::uint64_t key) {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.sndc", static_cast<unsigned long long>(key));
  return (std::filesystem::path(dir) / name).string();
}

}  // namespace

std::vector<float> Resample(const std::vector<float>& in, std::uint32_t channels, std::uint64_t frames,
                            std::uint32_t from, std::uint32_t to, std::uint64_t* outFrames) {
  if (from == to || frames == 0) {
    *outFrames = frames;
    return in;
  }
  const std::uint64_t n = (frames * to + from - 1) / from;
  std::vector<float> out(static_cast<std::size_t>(n * channels));
  for (std::uint64_t i = 0; i < n; ++i) {
    // Exact rational source position: i * from / to.
    const std::uint64_t num = i * from;
    const std::uint64_t i0 = num / to;
    const double frac = static_cast<double>(num % to) / static_cast<double>(to);
    const std::uint64_t i1 = i0 + 1 < frames ? i0 + 1 : frames - 1;
    for (std::uint32_t c = 0; c < channels; ++c) {
      const double a = in[static_cast<std::size_t>(i0 * channels + c)];
      const double b = in[static_cast<std::size_t>(i1 * channels + c)];
      out[static_cast<std::size_t>(i * channels + c)] = static_cast<float>(a + (b - a) * frac);
    }
  }
  *outFrames = n;
  return out;
}

std::vector<float> ConvertChannels(const std::vector<float>& in, std::uint32_t from, std::uint32_t to,
                                   std::uint64_t frames) {
  if (from == to) return in;
  std::vector<float> out(static_cast<std::size_t>(frames * to), 0.0f);
  for (std::uint64_t f = 0; f < frames; ++f) {
    const float* s = in.data() + f * from;
    float* d = out.data() + f * to;
    if (from == 1) {
      for (std::uint32_t c = 0; c < to; ++c) d[c] = s[0];
    } else if (to == 1) {
      double sum = 0;
      for (std::uint32_t c = 0; c < from; ++c) sum += s[c];
      d[0] = static_cast<float>(sum / from);
    } else {
      for (std::uint32_t c = 0; c < to && c < from; ++c) d[c] = s[c];
    }
  }
  return out;
}

std::uint64_t CacheKey(const std::vector<std::uint8_t>& wav, const ConvertSettings& s, std::uint32_t encoderVersion) {
  Fnv64 h;
  h.U64(kCacheVersion).U64(s.codec).U64(encoderVersion).U64(s.sampleRate).U64(s.channels).U64(s.seekIntervalMs);
  h.U64(s.loop ? 1 : 0).U64(s.loopStart).U64(s.loopEnd);
  h.U64(wav.size()).Bytes(wav.data(), wav.size());
  return h.value();
}

bool LoadCached(const std::string& dir, std::uint64_t key, Converted* out) {
  if (dir.empty()) return false;
  std::ifstream in(CachePath(dir, key), std::ios::binary);
  if (!in) return false;
  const std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Reader r{b};
  if (r.Get() != kCacheMagic || r.Get() != kCacheVersion || r.Get() != key) return false;
  Converted c;
  c.enc.codec = static_cast<std::uint32_t>(r.Get());
  c.enc.framesPerPacket = static_cast<std::uint32_t>(r.Get());
  c.enc.blockAlign = static_cast<std::uint32_t>(r.Get());
  c.enc.preSkip = static_cast<std::uint32_t>(r.Get());
  c.enc.preRoll = static_cast<std::uint32_t>(r.Get());
  c.enc.totalFrames = r.Get();
  c.channels = static_cast<std::uint32_t>(r.Get());
  c.sampleRate = static_cast<std::uint32_t>(r.Get());
  c.sourceRate = static_cast<std::uint32_t>(r.Get());
  c.loopStart = r.Get();
  c.loopEnd = r.Get();
  c.enc.data = r.Bytes();
  c.enc.extra = r.Bytes();
  const std::uint64_t n = r.Get();
  if (!r.ok || n > b.size() / 16) return false;
  for (std::uint64_t i = 0; i < n; ++i) {
    SeekPoint p;
    p.frame = r.Get();
    p.byteOffset = r.Get();
    c.enc.seek.push_back(p);
  }
  if (!r.ok) return false;
  *out = std::move(c);
  return true;
}

void StoreCached(const std::string& dir, std::uint64_t key, const Converted& c) {
  if (dir.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::vector<std::uint8_t> b;
  for (std::uint64_t x : {std::uint64_t{kCacheMagic}, kCacheVersion, key, std::uint64_t{c.enc.codec},
                          std::uint64_t{c.enc.framesPerPacket}, std::uint64_t{c.enc.blockAlign},
                          std::uint64_t{c.enc.preSkip}, std::uint64_t{c.enc.preRoll}, c.enc.totalFrames,
                          std::uint64_t{c.channels}, std::uint64_t{c.sampleRate}, std::uint64_t{c.sourceRate},
                          c.loopStart, c.loopEnd}) {
    Put(b, x);
  }
  PutBytes(b, c.enc.data);
  PutBytes(b, c.enc.extra);
  Put(b, c.enc.seek.size());
  for (const SeekPoint& p : c.enc.seek) {
    Put(b, p.frame);
    Put(b, p.byteOffset);
  }
  const std::string path = CachePath(dir, key);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!out) return;
  }
  std::filesystem::rename(tmp, path, ec);
}

bool ConvertWav(const std::vector<std::uint8_t>& wavBytes, const ConvertSettings& s, const Encoder& encoder,
                Converted* out, std::string* error) {
  PcmData pcm;
  if (!ReadWav(wavBytes, &pcm, error)) return false;
  if (pcm.frames == 0) {
    *error = "the WAV file has no sample frames";
    return false;
  }
  std::uint64_t loopStart = s.loop ? s.loopStart : 0;
  std::uint64_t loopEnd = s.loop ? s.loopEnd : 0;
  if (!s.loop && pcm.hasLoop) {
    loopStart = pcm.loopStart;
    loopEnd = pcm.loopEnd;
  }
  if (loopEnd != 0 && (loopStart >= loopEnd || loopEnd > pcm.frames)) {
    *error = "loop [" + std::to_string(loopStart) + ", " + std::to_string(loopEnd) + ") outside the " +
             std::to_string(pcm.frames) + " source frames";
    return false;
  }
  const std::uint32_t channels = s.channels != 0 ? s.channels : pcm.channels;
  const std::uint32_t rate = s.sampleRate != 0 ? s.sampleRate : pcm.sampleRate;
  if (channels > 8) {
    *error = "more than 8 output channels";
    return false;
  }
  std::vector<float> data = ConvertChannels(pcm.samples, pcm.channels, channels, pcm.frames);
  std::uint64_t frames = pcm.frames;
  data = Resample(data, channels, pcm.frames, pcm.sampleRate, rate, &frames);
  out->channels = channels;
  out->sampleRate = rate;
  out->sourceRate = pcm.sampleRate;
  if (loopEnd != 0) {
    out->loopStart = loopStart * rate / pcm.sampleRate;
    out->loopEnd = loopEnd * rate / pcm.sampleRate;
    if (out->loopEnd > frames) out->loopEnd = frames;
    if (out->loopEnd <= out->loopStart) out->loopEnd = out->loopStart + 1;
  }
  EncodeInput in;
  in.samples = data.data();
  in.frames = frames;
  in.channels = channels;
  in.sampleRate = rate;
  in.seekIntervalFrames = s.seekIntervalMs == 0 ? 0 : static_cast<std::uint32_t>(std::uint64_t{s.seekIntervalMs} * rate / 1000);
  in.loopStart = out->loopStart;
  in.loopEnd = out->loopEnd;
  out->enc = EncodedAsset{};
  if (!encoder.encode(in, &out->enc, error)) return false;
  out->enc.codec = encoder.codec;
  return true;
}

}  // namespace snd::buildtool::detail
