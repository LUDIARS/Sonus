// ICodec (design.md 6.4, 12.3). FROZEN CONTRACT.
#ifndef SND_EXT_CODEC_HPP
#define SND_EXT_CODEC_HPP

#include <cstdint>

#include "snd/ext.h"
#include "snd/ext/types.hpp"

namespace snd {

// Called from normal workers (prefetch lane). Decoder state lives in the
// decoder pool (64-byte aligned, Config.memory.decoderSlotBytes per slot).
// One state is used by one job at a time; different states concurrently.
// See snd_codec in snd/ext.h for the exact Decode/Seek semantics.
struct ICodec {
  virtual ~ICodec() = default;
  virtual std::uint32_t StateBytes(const CodecHeader& header) const = 0;
  virtual void Init(void* state, const CodecHeader& header) = 0;
  // Returns frames written or kDecodeError.
  virtual std::uint32_t Decode(void* state, const std::uint8_t* src, std::uint32_t srcBytes,
                               float* const* dst, std::uint32_t maxFrames) = 0;
  virtual void Seek(void* state, std::uint64_t frame) = 0;  // pre-roll per header metadata
};

// Registration entry used by snd::Config::plugins.
struct CodecPlugin {
  std::uint32_t fourcc;
  ICodec* codec;
};

namespace bridge {
inline std::uint32_t SND_CALL CodecStateBytes(void* u, const snd_codec_header* h) {
  return static_cast<ICodec*>(u)->StateBytes(*h);
}
inline void SND_CALL CodecInit(void* u, void* s, const snd_codec_header* h) {
  static_cast<ICodec*>(u)->Init(s, *h);
}
inline std::uint32_t SND_CALL CodecDecode(void* u, void* s, const std::uint8_t* src,
                                          std::uint32_t n, float* const* dst, std::uint32_t max) {
  return static_cast<ICodec*>(u)->Decode(s, src, n, dst, max);
}
inline void SND_CALL CodecSeek(void* u, void* s, std::uint64_t f) {
  static_cast<ICodec*>(u)->Seek(s, f);
}
}  // namespace bridge

inline snd_codec ToC(ICodec& c, std::uint32_t fourcc) noexcept {
  return snd_codec{&c,
                   fourcc,
                   0u,
                   &bridge::CodecStateBytes,
                   &bridge::CodecInit,
                   &bridge::CodecDecode,
                   &bridge::CodecSeek};
}

// ICodec over a copied snd_codec table. 1.1: default constructible (an empty
// table; Assign() before use) so that registries can keep fixed arrays of them.
class CCodec final : public ICodec {
 public:
  CCodec() noexcept : c_{} {}
  explicit CCodec(const snd_codec& c) noexcept : c_(c) {}
  void Assign(const snd_codec& c) noexcept { c_ = c; }
  const snd_codec& Table() const noexcept { return c_; }
  std::uint32_t StateBytes(const CodecHeader& h) const override { return c_.state_bytes(c_.user, &h); }
  void Init(void* s, const CodecHeader& h) override { c_.init(c_.user, s, &h); }
  std::uint32_t Decode(void* s, const std::uint8_t* src, std::uint32_t n, float* const* dst,
                       std::uint32_t max) override {
    return c_.decode(c_.user, s, src, n, dst, max);
  }
  void Seek(void* s, std::uint64_t f) override { c_.seek(c_.user, s, f); }
  std::uint32_t fourcc() const noexcept { return c_.codec; }

 private:
  snd_codec c_;
};

}  // namespace snd

#endif  // SND_EXT_CODEC_HPP
