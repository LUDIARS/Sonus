// OS-independent decisions shared by the backends (CONTRACTS.md D-61..D-64).
// Pure functions: unit-tested on every OS (platform_stream_policy_test).
#ifndef SND_PLATFORM_SRC_COMMON_STREAM_POLICY_HPP
#define SND_PLATFORM_SRC_COMMON_STREAM_POLICY_HPP

#include <cstdint>

#include "snd/ext/types.hpp"

namespace snd::platform::detail {

// --- periods and time units ---------------------------------------------------
// Period used when the DeviceDesc asks for 0: the power of two closest above
// rate / 200 (5 ms): 256 at 44.1 / 48 kHz, 512 at 96 kHz (one 2.2 block).
std::uint32_t DefaultPeriodFrames(std::uint32_t sampleRate) noexcept;
// Frames <-> 100 ns units (REFERENCE_TIME), rounded to nearest.
std::int64_t FramesToHns(std::uint64_t frames, std::uint32_t sampleRate) noexcept;
std::uint64_t HnsToFrames(std::int64_t hns, std::uint32_t sampleRate) noexcept;
double FramesToSeconds(std::uint64_t frames, std::uint32_t sampleRate) noexcept;
// IAudioClient3 shared-mode period: `requested` (0 = DefaultPeriodFrames)
// rounded UP to a multiple of `fundamental`, clamped to [minP, maxP]. Returns
// 0 when the inputs are unusable (fundamental 0 or minP > maxP).
std::uint32_t ChooseEnginePeriod(std::uint32_t requested, std::uint32_t sampleRate, std::uint32_t fundamental,
                                 std::uint32_t minP, std::uint32_t maxP) noexcept;

// --- device selection (D-64) --------------------------------------------------
// DeviceDesc::device_index -> -1 = system default, otherwise the entry index
// in the enumeration; -2 = out of range (Open fails).
std::int32_t ResolveDeviceIndex(std::uint32_t deviceIndex, std::uint32_t deviceCount) noexcept;

// --- stream errors ---------------------------------------------------------------
enum class StreamErrorAction : std::uint8_t {
  None = 0,         // not an error
  Retry = 1,        // transient (EAGAIN / EINTR / buffer busy): try again later
  RecoverXrun = 2,  // underrun: prepare / restart, count an xrun, keep going
  Suspended = 3,    // system suspend: post SUSPENDED, wait for resume, post RESUMED
  Lost = 4,         // device gone / invalidated: post LOST, reopen (D-63)
  Fatal = 5,        // configuration error: post LOST, do not reopen
};
// ALSA return codes (negative errno).
StreamErrorAction ClassifyAlsaError(int err) noexcept;
// WASAPI HRESULTs (as unsigned 32-bit values).
StreamErrorAction ClassifyWasapiHresult(std::uint32_t hr) noexcept;

// --- WASAPI stream plan (D-63) ---------------------------------------------------
enum class WasapiMode : std::uint8_t {
  SharedLowLatency = 0,  // IAudioClient3::InitializeSharedAudioStream in the mix format
  SharedNative = 1,      // IAudioClient::Initialize in the mix format (no IAudioClient3)
  SharedAutoConvert = 2, // IAudioClient::Initialize + AUTOCONVERTPCM | SRC_DEFAULT_QUALITY
  Exclusive = 3,         // IAudioClient::Initialize(EXCLUSIVE), event driven
};
struct WasapiPlan {
  WasapiMode mode = WasapiMode::SharedNative;
  std::uint32_t sampleRate = 0;
  std::uint32_t channels = 0;
};
// Requested 0 = the mix format's value. Shared mode keeps the mix format when
// the request matches it (lowest latency, IAudioClient3 if present) and lets
// WASAPI convert rate and channels otherwise; the callback always sees the
// requested format.
WasapiPlan PlanWasapiStream(std::uint32_t requestedRate, std::uint32_t requestedChannels, bool exclusive,
                            bool haveClient3, std::uint32_t mixRate, std::uint32_t mixChannels) noexcept;

// --- route changes (iOS AVAudioSessionRouteChangeReason, D-63) --------------------
// Returns the event to post for a route change with `reason`
// (AVAudioSessionRouteChangeReason value), or 0 for none. NewDeviceAvailable,
// OldDeviceUnavailable, Override and RouteConfigurationChange -> CHANGED;
// NoSuitableRouteForCategory -> LOST; CategoryChange / WakeFromSleep / Unknown
// -> CHANGED only when the format differs.
std::uint32_t RouteChangeEvent(std::uint32_t reason, bool formatDiffers) noexcept;

}  // namespace snd::platform::detail

#endif  // SND_PLATFORM_SRC_COMMON_STREAM_POLICY_HPP
