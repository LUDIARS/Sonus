// Reference decode of a built asset (bank or stream file) with the codec
// module directly, for exact comparison with what the source kinds play.
#ifndef SND_SOURCE_TEST_REFERENCE_HPP
#define SND_SOURCE_TEST_REFERENCE_HPP

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "snd/bank/bank_view.hpp"
#include "snd/bank/validate.hpp"
#include "snd/codec/codec.hpp"
#include "snd/dsp/resampler.hpp"
#include "snd/format/bank_format.hpp"
#include "source_harness.hpp"

namespace srctest {

struct Decoded {
  bool ok = false;
  snd::CodecHeader h{};
  std::vector<std::vector<float>> ch;  // planar, total_frames each
  std::uint64_t loopStart = 0, loopEnd = 0;
};

// Decodes asset row `row` of the built bank.
inline Decoded Reference(const Built& b, std::uint32_t row) {
  namespace bk = snd::bank;
  namespace bf = snd::bank_format;
  Decoded d;
  const std::size_t n = b.bank.size();
  std::unique_ptr<std::uint8_t, void (*)(void*)> img(static_cast<std::uint8_t*>(std::aligned_alloc(64, (n + 127) / 64 * 64)),
                                                     &std::free);
  std::memcpy(img.get(), b.bank.data(), n);
  bk::BankView v;
  bk::ParseOptions po;
  po.platform = bf::kPlatformAny;
  bk::ParseError err;
  if (bk::ParseBank(img.get(), n, po, &v, &err) != SND_OK || row >= v.Rows(bf::chunk::ASST)) return d;
  snd::CodecHeader& h = d.h;
  h.codec = v.Column<bk::col::ASST_codec>()[row];
  h.channels = v.Column<bk::col::ASST_channels>()[row];
  h.sample_rate = v.Column<bk::col::ASST_sampleRate>()[row];
  h.frames_per_packet = v.Column<bk::col::ASST_framesPerPacket>()[row];
  h.total_frames = v.Column<bk::col::ASST_totalFrames>()[row];
  h.pre_skip = v.Column<bk::col::ASST_preSkip>()[row];
  h.pre_roll = v.Column<bk::col::ASST_preRoll>()[row];
  h.block_align = v.Column<bk::col::ASST_blockAlign>()[row];
  const bk::ByteSpan extra = v.AssetExtra(row);
  std::vector<std::uint8_t> extraCopy(extra.data, extra.data + extra.bytes);
  h.extra = extraCopy.empty() ? nullptr : extraCopy.data();
  h.extra_bytes = static_cast<std::uint32_t>(extraCopy.size());
  d.loopStart = v.Column<bk::col::ASST_loopStart>()[row];
  d.loopEnd = v.Column<bk::col::ASST_loopEnd>()[row];
  std::vector<std::uint8_t> data;
  const std::uint32_t strm = v.Column<bk::col::ASST_stream>()[row];
  if (strm == bf::kNone) {
    const bk::ByteSpan w = v.WaveData(row);
    data.assign(w.data, w.data + w.bytes);
  } else {
    const std::string path = v.StreamPath(strm);
    for (const auto& f : b.files) {
      if (f.first != path || f.second.size() < sizeof(bf::StreamFileHeader)) continue;
      bf::StreamFileHeader sh{};
      std::memcpy(&sh, f.second.data(), sizeof(sh));
      data.assign(f.second.begin() + static_cast<std::ptrdiff_t>(sh.dataOffset),
                  f.second.begin() + static_cast<std::ptrdiff_t>(sh.dataOffset + sh.dataBytes));
    }
  }
  snd::ICodec* c = snd::codec::BuiltinCodec(h.codec);
  if (c == nullptr || data.empty()) return d;
  std::vector<std::uint64_t> state((c->StateBytes(h) + 127) / 8);
  void* st = reinterpret_cast<void*>((reinterpret_cast<std::uintptr_t>(state.data()) + 63) & ~std::uintptr_t{63});
  c->Init(st, h);
  constexpr std::uint32_t kMax = 512;
  std::vector<std::vector<float>> tmp(h.channels, std::vector<float>(kMax));
  std::vector<float*> dst(h.channels);
  for (std::uint32_t k = 0; k < h.channels; ++k) dst[k] = tmp[k].data();
  d.ch.assign(h.channels, {});
  auto take = [&](std::uint32_t got) {
    for (std::uint32_t k = 0; k < h.channels; ++k) d.ch[k].insert(d.ch[k].end(), tmp[k].begin(), tmp[k].begin() + got);
  };
  std::size_t pos = 0;
  while (pos < data.size()) {
    const std::uint32_t len = static_cast<std::uint32_t>(data.size() - pos < 4096 ? data.size() - pos : 4096);
    std::uint32_t got = c->Decode(st, data.data() + pos, len, dst.data(), kMax);
    if (got == SND_DECODE_ERROR) return d;
    take(got);
    while (got == kMax) {
      got = c->Decode(st, nullptr, 0, dst.data(), kMax);
      if (got == SND_DECODE_ERROR) return d;
      take(got);
    }
    pos += len;
  }
  d.ok = d.ch[0].size() == h.total_frames;
  return d;
}

// What core's voice resampler (sinc, maxPitchRatio 4) makes of the source frames
// `x` (zeros after the end) at a constant ratio: `outFrames` output frames. The
// resampler output does not depend on how the frames are split into calls.
inline std::vector<float> Resampled(const std::vector<float>& x, std::size_t outFrames, float ratio = 1.0f,
                                    float maxRatio = 4.0f) {
  using namespace snd::dsp;
  std::vector<std::uint8_t> st(ResamplerStateBytes(kResamplerHigh, 1, maxRatio) + 64);
  void* s = reinterpret_cast<void*>((reinterpret_cast<std::uintptr_t>(st.data()) + 63) & ~std::uintptr_t{63});
  ResamplerInit(s, kResamplerHigh, 1, maxRatio);
  const std::uint32_t pad = ResamplerPadFrames(kResamplerHigh, maxRatio);
  std::vector<float> out;
  std::size_t pos = 0;
  std::vector<float> in, o(256);
  while (out.size() < outFrames) {
    const std::uint32_t n = ResamplerInputFrames(s, 256, ratio, ratio);
    in.assign(pad + n + 16, 0.0f);
    for (std::uint32_t i = 0; i < n; ++i) in[pad + i] = pos + i < x.size() ? x[pos + i] : 0.0f;
    float* ip = in.data();
    float* op = o.data();
    ResamplerProcess(s, &ip, n, &op, 256, ratio, ratio);
    pos += n;
    out.insert(out.end(), o.begin(), o.end());
  }
  out.resize(outFrames);
  return out;
}

// Fit of the output against `x` as the resampler plays it (ratio 1).
inline Fit FitPlayed(const Rig& r, std::uint32_t c, std::size_t at, const std::vector<float>& x, std::size_t extra = 64) {
  return FitGain(r, c, at, Resampled(x, x.size() + extra));
}

}  // namespace srctest

#endif  // SND_SOURCE_TEST_REFERENCE_HPP
