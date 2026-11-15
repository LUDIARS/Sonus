// Helpers of the cross-module tests (stage 1.5 integration): build a project
// with buildtool, read the banks back with bank, decode assets with codec,
// size / initialise effects with dsp.
#ifndef SND_TESTS_INTEGRATION_PIPELINE_UTIL_HPP
#define SND_TESTS_INTEGRATION_PIPELINE_UTIL_HPP

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mini_project.hpp"
#include "snd/bank/bank_view.hpp"
#include "snd/base/allocator.hpp"
#include "snd/bank/prepared.hpp"
#include "snd/bank/validate.hpp"
#include "snd/codec/codec.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/test/test.hpp"

// SND_REQUIRE with a message (the test kit has SND_CHECK_MSG only).
#define SND_REQUIRE_MSG(cond, ...)    \
  do {                                \
    if (!(cond)) {                    \
      SND_CHECK_MSG(cond, __VA_ARGS__); \
      return;                         \
    }                                 \
  } while (0)

namespace pipeline {

namespace bk = snd::bank;
namespace bf = snd::bank_format;

// 64-byte aligned copy of a file image (banks are read in place).
class Image {
 public:
  Image() = default;
  explicit Image(const std::vector<std::uint8_t>& bytes) { Assign(bytes); }
  void Assign(const std::vector<std::uint8_t>& bytes) {
    size_ = bytes.size();
    mem_.reset(static_cast<std::uint8_t*>(std::aligned_alloc(64, (size_ + 63) / 64 * 64 + 64)));
    if (size_ != 0) std::memcpy(mem_.get(), bytes.data(), size_);
  }
  const std::uint8_t* data() const { return mem_.get(); }
  std::uint64_t size() const { return size_; }

 private:
  struct Free {
    void operator()(std::uint8_t* p) const { std::free(p); }
  };
  std::unique_ptr<std::uint8_t, Free> mem_;
  std::uint64_t size_ = 0;
};

// A parsed bank (BankView over an aligned copy).
struct ReadBank {
  Image image;
  bk::BankView view;
  bk::ParseError error;
  bool ok = false;
  explicit ReadBank(const std::vector<std::uint8_t>& bytes) : image(bytes) {
    bk::ParseOptions o;
    o.platform = bf::kPlatformAny;
    ok = bk::ParseBank(image.data(), image.size(), o, &view, &error) == SND_OK;
  }
};

// snd_codec_header of an ASST row (the columns named like the header fields).
inline snd::CodecHeader AssetHeader(const bk::BankView& v, std::uint32_t row) {
  snd::CodecHeader h{};
  h.codec = v.Column<bk::col::ASST_codec>()[row];
  h.channels = v.Column<bk::col::ASST_channels>()[row];
  h.sample_rate = v.Column<bk::col::ASST_sampleRate>()[row];
  h.frames_per_packet = v.Column<bk::col::ASST_framesPerPacket>()[row];
  h.total_frames = v.Column<bk::col::ASST_totalFrames>()[row];
  h.pre_skip = v.Column<bk::col::ASST_preSkip>()[row];
  h.pre_roll = v.Column<bk::col::ASST_preRoll>()[row];
  h.block_align = v.Column<bk::col::ASST_blockAlign>()[row];
  return h;
}

// Decodes `bytes` with the built-in codec of `h` in `chunk`-byte pieces into
// planar float (total_frames per channel). false on any decode error.
inline bool DecodeAll(const snd::CodecHeader& h, const std::uint8_t* bytes, std::uint64_t n, std::uint32_t chunk,
                      std::vector<std::vector<float>>* out) {
  snd::ICodec* c = snd::codec::BuiltinCodec(h.codec);
  if (c == nullptr) return false;
  const std::uint32_t stateBytes = c->StateBytes(h);
  std::vector<std::uint64_t> state((stateBytes + 7) / 8 + 8);
  auto* st = reinterpret_cast<std::uint8_t*>(state.data());
  while (reinterpret_cast<std::uintptr_t>(st) % 64 != 0) ++st;
  c->Init(st, h);
  out->assign(h.channels, {});
  constexpr std::uint32_t kMax = 256;
  std::vector<std::vector<float>> tmp(h.channels, std::vector<float>(kMax));
  std::vector<float*> dst(h.channels);
  for (std::uint32_t ch = 0; ch < h.channels; ++ch) dst[ch] = tmp[ch].data();
  auto take = [&](std::uint32_t got) {
    for (std::uint32_t ch = 0; ch < h.channels; ++ch) {
      (*out)[ch].insert((*out)[ch].end(), tmp[ch].begin(), tmp[ch].begin() + got);
    }
  };
  std::uint64_t pos = 0;
  while (pos < n) {
    const std::uint32_t len = static_cast<std::uint32_t>(n - pos < chunk ? n - pos : chunk);
    std::uint32_t got = c->Decode(st, bytes + pos, len, dst.data(), kMax);
    if (got == SND_DECODE_ERROR) return false;
    take(got);
    while (got == kMax) {  // the caller's duty (snd/ext.h): drain before the next input
      got = c->Decode(st, nullptr, 0, dst.data(), kMax);
      if (got == SND_DECODE_ERROR) return false;
      take(got);
    }
    pos += len;
  }
  return (*out)[0].size() == h.total_frames;
}

// Effect descs of a bus as core would build them from the bank (EFFX + EFPV),
// for `channels` and the runtime's rate / block size. `params` keeps the values.
inline std::vector<snd::EffectDesc> BusEffectDescs(const bk::BankView& v, std::uint32_t busRow, std::uint32_t channels,
                                                   std::uint32_t rate, std::uint32_t block,
                                                   std::vector<std::vector<float>>* params) {
  std::vector<snd::EffectDesc> out;
  const std::uint32_t first = v.Column<bk::col::BUSS_effectFirst>()[busRow];
  const std::uint32_t count = v.Column<bk::col::BUSS_effectCount>()[busRow];
  const bk::ByteSpan efbl = v.Chunk(bf::chunk::EFBL);
  params->reserve(params->size() + count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t fx = first + i;
    std::vector<float> p;
    const std::uint32_t pf = v.Column<bk::col::EFFX_paramFirst>()[fx];
    const std::uint32_t pc = v.Column<bk::col::EFFX_paramCount>()[fx];
    for (std::uint32_t k = 0; k < pc; ++k) p.push_back(v.Column<bk::col::EFPV_value>()[pf + k]);
    params->push_back(p);
    snd::EffectDesc d{};
    d.effect_type = v.Column<bk::col::EFFX_effectType>()[fx];
    d.channels = channels;
    d.max_frames = block;
    d.param_count = pc;
    d.initial_params = params->back().empty() ? nullptr : params->back().data();
    const std::uint64_t bb = v.Column<bk::col::EFFX_blobBytes>()[fx];
    d.blob = bb != 0 ? efbl.data + v.Column<bk::col::EFFX_blobOffset>()[fx] : nullptr;
    d.blob_bytes = bb;
    d.sample_rate = rate;
    out.push_back(d);
  }
  return out;
}

// Signal-to-noise ratio of `got` against `ref` in dB.
inline double SnrDb(const std::vector<float>& ref, const std::vector<float>& got) {
  double s = 0, e = 0;
  for (std::size_t i = 0; i < ref.size() && i < got.size(); ++i) {
    s += static_cast<double>(ref[i]) * ref[i];
    const double d = static_cast<double>(ref[i]) - got[i];
    e += d * d;
  }
  return e == 0 ? 300.0 : 10.0 * std::log10(s / e);
}

}  // namespace pipeline

#endif  // SND_TESTS_INTEGRATION_PIPELINE_UTIL_HPP
