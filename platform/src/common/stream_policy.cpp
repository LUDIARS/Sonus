#include "common/stream_policy.hpp"

#include <cerrno>
#include <cstdint>

#include "snd/ext.h"

namespace snd::platform::detail {

std::uint32_t DefaultPeriodFrames(std::uint32_t sampleRate) noexcept {
  const std::uint32_t want = sampleRate / 200u;
  std::uint32_t p = 64u;
  while (p < want && p < (1u << 16)) p <<= 1u;
  return p;
}

std::int64_t FramesToHns(std::uint64_t frames, std::uint32_t sampleRate) noexcept {
  if (sampleRate == 0u) return 0;
  // Split so no product overflows: rem * 1e7 < 2^32 * 1e7 < 2^56.
  const std::uint64_t q = frames / sampleRate;
  const std::uint64_t rem = frames % sampleRate;
  if (q > static_cast<std::uint64_t>(INT64_MAX) / 10000000u - 1u) return INT64_MAX;
  return static_cast<std::int64_t>(q * 10000000u + (rem * 10000000u + sampleRate / 2u) / sampleRate);
}

std::uint64_t HnsToFrames(std::int64_t hns, std::uint32_t sampleRate) noexcept {
  if (hns <= 0) return 0;
  const std::uint64_t q = static_cast<std::uint64_t>(hns) / 10000000u;
  const std::uint64_t rem = static_cast<std::uint64_t>(hns) % 10000000u;  // rem * rate < 2^56
  return q * sampleRate + (rem * sampleRate + 5000000u) / 10000000u;
}

double FramesToSeconds(std::uint64_t frames, std::uint32_t sampleRate) noexcept {
  return sampleRate == 0u ? 0.0 : static_cast<double>(frames) / static_cast<double>(sampleRate);
}

std::uint32_t ChooseEnginePeriod(std::uint32_t requested, std::uint32_t sampleRate, std::uint32_t fundamental,
                                 std::uint32_t minP, std::uint32_t maxP) noexcept {
  if (fundamental == 0u || minP > maxP || minP == 0u) return 0;
  std::uint64_t want = requested != 0u ? requested : DefaultPeriodFrames(sampleRate);
  want = (want + fundamental - 1u) / fundamental * fundamental;
  if (want < minP) want = minP;
  if (want > maxP) want = maxP;
  return static_cast<std::uint32_t>(want);
}

std::int32_t ResolveDeviceIndex(std::uint32_t deviceIndex, std::uint32_t deviceCount) noexcept {
  if (deviceIndex == 0u) return -1;
  if (deviceIndex > deviceCount) return -2;
  return static_cast<std::int32_t>(deviceIndex - 1u);
}

StreamErrorAction ClassifyAlsaError(int err) noexcept {
  if (err >= 0) return StreamErrorAction::None;
  switch (-err) {
    case EAGAIN:
    case EINTR:
    case EBUSY:
      return StreamErrorAction::Retry;
    case EPIPE:
      return StreamErrorAction::RecoverXrun;
#if defined(ESTRPIPE)
    case ESTRPIPE:
      return StreamErrorAction::Suspended;
#endif
#if defined(EBADFD)
    case EBADFD:
#endif
#if defined(ESHUTDOWN)
    case ESHUTDOWN:
#endif
    case ENODEV:
    case EIO:
    case ENXIO:
    case ENOTTY:
      return StreamErrorAction::Lost;
    default:
      return StreamErrorAction::Fatal;
  }
}

StreamErrorAction ClassifyWasapiHresult(std::uint32_t hr) noexcept {
  if ((hr & 0x80000000u) == 0u) return StreamErrorAction::None;  // SUCCEEDED (S_OK, S_FALSE)
  switch (hr) {
    case 0x88890006u:  // AUDCLNT_E_BUFFER_TOO_LARGE: asked for more than is free
    case 0x8889000Bu:  // AUDCLNT_E_BUFFER_OPERATION_PENDING
      return StreamErrorAction::Retry;
    case 0x88890004u:  // AUDCLNT_E_DEVICE_INVALIDATED (unplugged, format changed, disabled)
    case 0x88890026u:  // AUDCLNT_E_RESOURCES_INVALIDATED
    case 0x88890010u:  // AUDCLNT_E_SERVICE_NOT_RUNNING (audio service restarted)
    case 0x8889000Au:  // AUDCLNT_E_DEVICE_IN_USE (exclusive mode taken by another app)
    case 0x88890001u:  // AUDCLNT_E_NOT_INITIALIZED
      return StreamErrorAction::Lost;
    default:
      return StreamErrorAction::Fatal;
  }
}

WasapiPlan PlanWasapiStream(std::uint32_t requestedRate, std::uint32_t requestedChannels, bool exclusive,
                            bool haveClient3, std::uint32_t mixRate, std::uint32_t mixChannels) noexcept {
  WasapiPlan p;
  p.sampleRate = requestedRate != 0u ? requestedRate : mixRate;
  p.channels = requestedChannels != 0u ? requestedChannels : mixChannels;
  if (exclusive) {
    p.mode = WasapiMode::Exclusive;
  } else if (p.sampleRate == mixRate && p.channels == mixChannels) {
    p.mode = haveClient3 ? WasapiMode::SharedLowLatency : WasapiMode::SharedNative;
  } else {
    p.mode = WasapiMode::SharedAutoConvert;
  }
  return p;
}

std::uint32_t RouteChangeEvent(std::uint32_t reason, bool formatDiffers) noexcept {
  switch (reason) {
    case 1u:  // NewDeviceAvailable
    case 2u:  // OldDeviceUnavailable (headphones unplugged)
    case 4u:  // Override
    case 8u:  // RouteConfigurationChange
      return SND_DEVICE_EVENT_CHANGED;
    case 7u:  // NoSuitableRouteForCategory
      return SND_DEVICE_EVENT_LOST;
    default:  // 0 Unknown, 3 CategoryChange, 6 WakeFromSleep
      return formatDiffers ? SND_DEVICE_EVENT_CHANGED : 0u;
  }
}

}  // namespace snd::platform::detail
