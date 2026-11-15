#include "snd/buildtool/wav.hpp"

#include <cmath>
#include <cstring>

#include "snd/base/endian.hpp"

namespace snd::buildtool {
namespace {

using base::LoadLE16;  // little-endian access lives in base (A1-12)
using base::LoadLE32;
void Put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void Put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
  v.push_back(static_cast<std::uint8_t>(x));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
}

}  // namespace

bool ReadWav(const std::vector<std::uint8_t>& b, PcmData* out, std::string* error) {
  *out = PcmData{};
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
    *error = "not a RIFF/WAVE file";
    return false;
  }
  std::uint16_t format = 0;
  std::uint16_t channels = 0;
  std::uint32_t rate = 0;
  std::uint16_t bits = 0;
  std::uint16_t blockAlign = 0;
  bool haveFmt = false;
  const std::uint8_t* data = nullptr;
  std::uint64_t dataBytes = 0;
  // Sizes that point past the end of the file are errors (a truncated file
  // would otherwise silently give a shorter sound). Only the pad byte of an
  // odd-sized last chunk may be missing.
  if (static_cast<std::uint64_t>(LoadLE32(b.data() + 4)) + 8 > b.size() + 1) {
    *error = "RIFF size " + std::to_string(LoadLE32(b.data() + 4)) + " is larger than the file (truncated?)";
    return false;
  }
  std::size_t pos = 12;
  while (pos + 8 <= b.size()) {
    const std::uint8_t* h = b.data() + pos;
    const std::uint64_t size = LoadLE32(h + 4);
    const std::size_t body = pos + 8;
    const std::uint64_t avail = b.size() - body;
    if (size > avail) {
      *error = std::string("chunk '") + std::string(reinterpret_cast<const char*>(h), 4) + "' size " +
               std::to_string(size) + " runs past the end of the file (truncated?)";
      return false;
    }
    const std::uint64_t n = size;
    if (std::memcmp(h, "fmt ", 4) == 0) {
      if (n < 16) {
        *error = "fmt chunk too small";
        return false;
      }
      format = LoadLE16(b.data() + body);
      channels = LoadLE16(b.data() + body + 2);
      rate = LoadLE32(b.data() + body + 4);
      blockAlign = LoadLE16(b.data() + body + 12);
      bits = LoadLE16(b.data() + body + 14);
      if (format == 0xFFFE) {
        if (n < 40) {
          *error = "WAVE_FORMAT_EXTENSIBLE fmt chunk too small";
          return false;
        }
        format = LoadLE16(b.data() + body + 24);  // first two bytes of the sub-format GUID
      }
      haveFmt = true;
    } else if (std::memcmp(h, "data", 4) == 0) {
      data = b.data() + body;
      dataBytes = n;
    } else if (std::memcmp(h, "smpl", 4) == 0 && n >= 36 + 24) {
      const std::uint32_t loops = LoadLE32(b.data() + body + 28);
      if (loops > 0) {
        const std::uint8_t* l = b.data() + body + 36;
        out->hasLoop = true;
        out->loopStart = LoadLE32(l + 8);
        out->loopEnd = static_cast<std::uint64_t>(LoadLE32(l + 12)) + 1;  // smpl end is inclusive
      }
    }
    pos = body + static_cast<std::size_t>(n) + (n & 1u);
  }
  if (pos < b.size()) {
    *error = "trailing bytes after the last chunk (truncated chunk header)";
    return false;
  }
  if (!haveFmt || data == nullptr) {
    *error = "missing fmt or data chunk";
    return false;
  }
  if (channels == 0 || channels > 64 || rate == 0) {
    *error = "invalid channel count or sample rate";
    return false;
  }
  const bool isFloat = format == 3;
  if (!(format == 1 || isFloat)) {
    *error = "unsupported WAV format tag " + std::to_string(format) + " (PCM or IEEE float only)";
    return false;
  }
  if (isFloat ? (bits != 32 && bits != 64) : (bits != 16 && bits != 24 && bits != 32)) {
    *error = "unsupported bit depth " + std::to_string(bits);
    return false;
  }
  const std::uint32_t bytesPer = bits / 8u;
  if (blockAlign != bytesPer * channels) {
    *error = "block align does not match channels * bytes per sample";
    return false;
  }
  out->channels = channels;
  out->sampleRate = rate;
  out->frames = dataBytes / blockAlign;
  const std::uint64_t count = out->frames * channels;
  out->samples.resize(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::uint8_t* p = data + i * bytesPer;
    float v = 0.0f;
    if (isFloat && bits == 32) {
      std::memcpy(&v, p, 4);
    } else if (isFloat) {
      double d;
      std::memcpy(&d, p, 8);
      v = static_cast<float>(d);
    } else if (bits == 16) {
      v = static_cast<float>(static_cast<std::int16_t>(LoadLE16(p))) / 32768.0f;
    } else if (bits == 24) {
      std::int32_t s = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) << 8 |
                                                 static_cast<std::uint32_t>(p[1]) << 16 |
                                                 static_cast<std::uint32_t>(p[2]) << 24);
      v = static_cast<float>(s >> 8) / 8388608.0f;
    } else {
      v = static_cast<float>(static_cast<double>(static_cast<std::int32_t>(LoadLE32(p))) / 2147483648.0);
    }
    out->samples[static_cast<std::size_t>(i)] = v;
  }
  if (out->hasLoop && (out->loopStart >= out->loopEnd || out->loopEnd > out->frames)) {
    *error = "smpl loop outside the data";
    return false;
  }
  return true;
}

std::vector<std::uint8_t> WriteWav(const PcmData& pcm, WavSampleFormat format) {
  std::uint16_t bits = 16;
  std::uint16_t tag = 1;
  switch (format) {
    case WavSampleFormat::Int16: bits = 16; break;
    case WavSampleFormat::Int24: bits = 24; break;
    case WavSampleFormat::Int32: bits = 32; break;
    case WavSampleFormat::Float32:
      bits = 32;
      tag = 3;
      break;
  }
  const std::uint16_t bytesPer = static_cast<std::uint16_t>(bits / 8);
  const std::uint32_t dataBytes = static_cast<std::uint32_t>(pcm.samples.size() * bytesPer);
  std::vector<std::uint8_t> v;
  v.insert(v.end(), {'R', 'I', 'F', 'F'});
  Put32(v, 0);
  v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  Put32(v, 16);
  Put16(v, tag);
  Put16(v, static_cast<std::uint16_t>(pcm.channels));
  Put32(v, pcm.sampleRate);
  Put32(v, pcm.sampleRate * pcm.channels * bytesPer);
  Put16(v, static_cast<std::uint16_t>(pcm.channels * bytesPer));
  Put16(v, bits);
  if (pcm.hasLoop) {
    v.insert(v.end(), {'s', 'm', 'p', 'l'});
    Put32(v, 60);
    for (int i = 0; i < 7; ++i) Put32(v, 0);
    Put32(v, 1);  // loops
    Put32(v, 0);  // sampler data
    Put32(v, 0);  // cue id
    Put32(v, 0);  // type forward
    Put32(v, static_cast<std::uint32_t>(pcm.loopStart));
    Put32(v, static_cast<std::uint32_t>(pcm.loopEnd - 1));
    Put32(v, 0);
    Put32(v, 0);
  }
  v.insert(v.end(), {'d', 'a', 't', 'a'});
  Put32(v, dataBytes);
  for (float f : pcm.samples) {
    const double c = f < -1.0f ? -1.0 : (f > 1.0f ? 1.0 : static_cast<double>(f));
    switch (format) {
      case WavSampleFormat::Int16:
        Put16(v, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(c * 32767.0))));
        break;
      case WavSampleFormat::Int24: {
        const std::int32_t s = static_cast<std::int32_t>(std::lround(c * 8388607.0));
        for (int i = 0; i < 3; ++i) v.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(s) >> (8 * i)));
        break;
      }
      case WavSampleFormat::Int32:
        Put32(v, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::llround(c * 2147483647.0))));
        break;
      case WavSampleFormat::Float32: {
        std::uint32_t u;
        std::memcpy(&u, &f, 4);
        Put32(v, u);
        break;
      }
    }
  }
  if (dataBytes & 1u) v.push_back(0);
  const std::uint32_t riff = static_cast<std::uint32_t>(v.size() - 8);
  std::memcpy(v.data() + 4, &riff, 4);
  return v;
}

}  // namespace snd::buildtool
