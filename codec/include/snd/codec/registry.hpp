// Codec lookup table (design.md 6.4, CONTRACTS.md D-31).
//
// Holds the built-in codecs and the user codecs from Config.plugins.codecs.
// A user codec with the same fourcc as a built-in wins. Two user codecs with
// the same fourcc are rejected at Init (DuplicateFourcc).
//
// Threads:
//   - Init: owner thread (snd_create), before the registry is shared.
//   - Find / FindForSource: any thread, including the audio thread and RT
//     workers. Read-only scan of a fixed array: no allocation, no lock.
//
// SourceServices::findCodec (snd/internal/source_if.hpp) is satisfied by
//   services.findCodec = &CodecRegistry::FindForSource;
//   services.codecCtx  = &registry;
#ifndef SND_CODEC_REGISTRY_HPP
#define SND_CODEC_REGISTRY_HPP

#include <cstdint>

#include "snd/ext.h"
#include "snd/ext/codec.hpp"

namespace snd::codec {

enum class RegistryStatus : std::uint32_t {
  Ok = 0,
  TooManyCodecs = 1,    // count > kMaxUserCodecs
  InvalidEntry = 2,     // fourcc 0 or a null function pointer
  DuplicateFourcc = 3,  // two user codecs with the same fourcc
};

enum class LookupFailure : std::uint32_t {
  None = 0,
  NotRegistered = 1,    // no built-in or user codec for this fourcc
  OpusUnavailable = 2,  // SND_CODEC_OPUS without a plugin: not compiled into this build (A0-3)
};
const char* LookupFailureName(LookupFailure f) noexcept;

class CodecRegistry {
 public:
  static constexpr std::uint32_t kMaxUserCodecs = 32;  // = kMaxPluginsPerKind (D-39)

  CodecRegistry() noexcept = default;  // built-ins only
  CodecRegistry(const CodecRegistry&) = delete;
  CodecRegistry& operator=(const CodecRegistry&) = delete;

  // Copies the tables (snd_codec.codec is the fourcc). On failure the registry
  // keeps only the built-ins. Tables that are bridges made by snd::ToC(ICodec&)
  // are unwrapped to the ICodec (no double indirection, D-06).
  RegistryStatus Init(const snd_codec* tables, std::uint32_t count) noexcept;

  // nullptr when nothing decodes `fourcc`; `why` (optional) tells the reason.
  ICodec* Find(std::uint32_t fourcc, LookupFailure* why = nullptr) const noexcept;
  bool IsUserCodec(std::uint32_t fourcc) const noexcept;
  std::uint32_t UserCodecCount() const noexcept { return count_; }

  // Signature of SourceServices::findCodec. ctx = CodecRegistry*.
  static ICodec* FindForSource(void* ctx, std::uint32_t fourcc) noexcept;

 private:
  CCodec adapters_[kMaxUserCodecs];  // contract 1.1: default constructible (D-52)
  ICodec* user_[kMaxUserCodecs] = {};
  std::uint32_t fourcc_[kMaxUserCodecs] = {};
  std::uint32_t count_ = 0;
};

}  // namespace snd::codec

#endif  // SND_CODEC_REGISTRY_HPP
