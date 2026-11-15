// Bank validation (bank-format.md §8). See validate.hpp for the list of checks.
// The rules themselves are the contract's shared header snd/format/bank_check.hpp
// (contract 1.1, A1-13), which the build tool's self-check uses too.
#include "snd/bank/validate.hpp"

#include <cstdio>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "snd/base/crc32c.hpp"
#include "snd/format/bank_check.hpp"
#include "view_access.hpp"

namespace snd::bank {

std::uint32_t RuntimePlatform() noexcept {
#if defined(_WIN32)
  return bf::kPlatformWindows;
#elif defined(__ANDROID__)
  return bf::kPlatformAndroid;
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
  return bf::kPlatformIos;
#else
  return bf::kPlatformMac;
#endif
#elif defined(__linux__)
  return bf::kPlatformLinux;
#else
  return bf::kPlatformAny;
#endif
}

snd_result ParseBank(const void* data, std::uint64_t bytes, const ParseOptions& options, BankView* out,
                     ParseError* error) noexcept {
  if (error != nullptr) *error = ParseError{};
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  *out = BankView{};
  // The rules are the contract's shared checker (A1-13); base's CRC is faster
  // and bit-identical to the reference.
  bf::check::CheckOptions opt;
  opt.platform = options.platform;
  bf::check::CheckedBank checked;
  const snd_result r = bf::check::CheckBank(data, bytes, opt, &checked, error, &base::Crc32cUpdate);
  if (r != SND_OK) return r;
  ViewAccess::File(*out) = static_cast<const std::uint8_t*>(data);
  ViewAccess::Header(*out) = checked.header;
  for (std::uint32_t i = 0; i < kKnownChunkCount; ++i) {
    ViewAccess::Chunks(*out)[i] = checked.chunks[i];
    ViewAccess::Tables(*out)[i] = checked.tables[i];
  }
  ViewAccess::Writable(*out) = false;
  return SND_OK;
}

}  // namespace snd::bank
