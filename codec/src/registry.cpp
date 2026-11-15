// Codec lookup by fourcc (D-31).
#include "snd/codec/registry.hpp"

#include "snd/codec/codec.hpp"

namespace snd::codec {

const char* LookupFailureName(LookupFailure f) noexcept {
  switch (f) {
    case LookupFailure::None: return "none";
    case LookupFailure::NotRegistered: return "no codec registered for this fourcc";
    case LookupFailure::OpusUnavailable:
      return "Opus is not built into this runtime (no third-party library); register a plugin codec for 'OPUS'";
  }
  return "?";
}

RegistryStatus CodecRegistry::Init(const snd_codec* tables, std::uint32_t count) noexcept {
  count_ = 0;
  if (count == 0u) return RegistryStatus::Ok;
  if (count > kMaxUserCodecs) return RegistryStatus::TooManyCodecs;
  if (tables == nullptr) return RegistryStatus::InvalidEntry;
  for (std::uint32_t i = 0; i < count; ++i) {
    const snd_codec& t = tables[i];
    if (t.codec == 0u || t.state_bytes == nullptr || t.init == nullptr || t.decode == nullptr ||
        t.seek == nullptr) {
      return RegistryStatus::InvalidEntry;
    }
    for (std::uint32_t j = 0; j < i; ++j) {
      if (tables[j].codec == t.codec) return RegistryStatus::DuplicateFourcc;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const snd_codec& t = tables[i];
    fourcc_[i] = t.codec;
    // A table made by snd::ToC(ICodec&) calls straight back into an ICodec.
    const bool isBridge = t.state_bytes == &bridge::CodecStateBytes && t.init == &bridge::CodecInit &&
                          t.decode == &bridge::CodecDecode && t.seek == &bridge::CodecSeek && t.user != nullptr;
    if (isBridge) {
      user_[i] = static_cast<ICodec*>(t.user);
    } else {
      adapters_[i].Assign(t);
      user_[i] = &adapters_[i];
    }
  }
  count_ = count;
  return RegistryStatus::Ok;
}

ICodec* CodecRegistry::Find(std::uint32_t fourcc, LookupFailure* why) const noexcept {
  for (std::uint32_t i = 0; i < count_; ++i) {
    if (fourcc_[i] == fourcc) {
      if (why != nullptr) *why = LookupFailure::None;
      return user_[i];
    }
  }
  ICodec* builtin = BuiltinCodec(fourcc);
  if (why != nullptr) {
    if (builtin != nullptr) {
      *why = LookupFailure::None;
    } else {
      *why = fourcc == SND_CODEC_OPUS ? LookupFailure::OpusUnavailable : LookupFailure::NotRegistered;
    }
  }
  return builtin;
}

bool CodecRegistry::IsUserCodec(std::uint32_t fourcc) const noexcept {
  for (std::uint32_t i = 0; i < count_; ++i) {
    if (fourcc_[i] == fourcc) return true;
  }
  return false;
}

ICodec* CodecRegistry::FindForSource(void* ctx, std::uint32_t fourcc) noexcept {
  if (ctx == nullptr) return BuiltinCodec(fourcc);
  return static_cast<const CodecRegistry*>(ctx)->Find(fourcc);
}

}  // namespace snd::codec
