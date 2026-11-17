// OS-independent backend decisions (CONTRACTS.md D-61..D-64): sample
// conversion, periods and time units, device index resolution, error
// classification (ALSA errno, WASAPI HRESULT), the WASAPI stream plan, iOS
// route-change events, chunked callback delivery. These are the parts of the
// WASAPI and Core Audio backends that can be checked on Linux.
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "common/device_callback.hpp"
#include "common/owned_string.hpp"
#include "common/sample_format.hpp"
#include "common/stream_policy.hpp"
#include "snd/base/allocator.hpp"
#include "snd/ext.h"
#include "snd/platform/cpu.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform::detail;

SND_TEST(SampleFormat, ConversionClipsRoundsAndMapsNaN) {
  const float in[] = {0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 0.5f, -0.5f, NAN, 1.0f / 32767.0f, -1.0f / 32767.0f};
  constexpr std::size_t n = sizeof(in) / sizeof(in[0]);
  std::int16_t s16[n];
  ConvertFromFloat(in, s16, n, SampleFormat::Int16);
  const std::int16_t e16[] = {0, 32767, -32767, 32767, -32767, 16384, -16384, 0, 1, -1};  // 16383.5 rounds away from zero
  for (std::size_t i = 0; i < n; ++i) SND_CHECK_EQ(s16[i], e16[i]);
  std::int32_t s32[n];
  ConvertFromFloat(in, s32, n, SampleFormat::Int32);
  SND_CHECK_EQ(s32[0], 0);
  SND_CHECK_EQ(s32[1], 2147483647);
  SND_CHECK_EQ(s32[2], -2147483647);
  SND_CHECK_EQ(s32[3], 2147483647);
  SND_CHECK_EQ(s32[4], -2147483647);
  SND_CHECK_EQ(s32[5], 1073741824);  // 0.5 * (2^31 - 1) = 1073741823.5 -> away from zero
  SND_CHECK_EQ(s32[7], 0);
  float f[n];
  ConvertFromFloat(in, f, n, SampleFormat::Float32);
  SND_CHECK(std::memcmp(f, in, sizeof(in)) == 0);
  SND_CHECK_EQ(BytesPerSample(SampleFormat::Int16), 2u);
  SND_CHECK_EQ(BytesPerSample(SampleFormat::Int32), 4u);
  SND_CHECK_EQ(kFormatPreference[0], SampleFormat::Float32);
  {
    SND_RT_SCOPE();  // RT-safe
    ConvertFromFloat(in, s16, n, SampleFormat::Int16);
  }
}

SND_TEST(StreamPolicy, PeriodsAndTimeUnits) {
  SND_CHECK_EQ(DefaultPeriodFrames(48000), 256u);
  SND_CHECK_EQ(DefaultPeriodFrames(44100), 256u);
  SND_CHECK_EQ(DefaultPeriodFrames(96000), 512u);
  SND_CHECK_EQ(DefaultPeriodFrames(8000), 64u);
  SND_CHECK_EQ(FramesToHns(480, 48000), 100000);      // 10 ms
  SND_CHECK_EQ(FramesToHns(441, 44100), 100000);
  SND_CHECK_EQ(FramesToHns(1, 48000), 208);           // 208.33 -> 208
  SND_CHECK_EQ(FramesToHns(0, 0), 0);
  SND_CHECK_EQ(HnsToFrames(100000, 48000), 480u);
  SND_CHECK_EQ(HnsToFrames(30000, 44100), 132u);      // 3 ms = 132.3 frames
  SND_CHECK_EQ(HnsToFrames(-5, 48000), 0u);
  // Round trip for every period a device could report.
  for (std::uint32_t f = 1; f < 5000; f += 7) SND_CHECK_EQ(HnsToFrames(FramesToHns(f, 48000), 48000), f);
  // No overflow for huge frame counts (days of audio at 384 kHz).
  SND_CHECK(FramesToHns(1ull << 40, 384000) > 0);
  SND_CHECK_NEAR(FramesToSeconds(24000, 48000), 0.5, 1e-12);
  SND_CHECK_EQ(FramesToSeconds(1, 0), 0.0);
}

SND_TEST(StreamPolicy, EnginePeriodChoice) {
  // IAudioClient3 typical: fundamental 48 (1 ms), min 48, max 480 at 48 kHz.
  SND_CHECK_EQ(ChooseEnginePeriod(0, 48000, 48, 48, 480), 288u);    // 256 rounded up to 6 x 48
  SND_CHECK_EQ(ChooseEnginePeriod(128, 48000, 48, 48, 480), 144u);
  SND_CHECK_EQ(ChooseEnginePeriod(1, 48000, 48, 96, 480), 96u);     // clamped to min
  SND_CHECK_EQ(ChooseEnginePeriod(4096, 48000, 48, 48, 480), 480u); // clamped to max
  SND_CHECK_EQ(ChooseEnginePeriod(480, 48000, 480, 480, 480), 480u);  // only the default available
  SND_CHECK_EQ(ChooseEnginePeriod(256, 48000, 0, 48, 480), 0u);
  SND_CHECK_EQ(ChooseEnginePeriod(256, 48000, 48, 480, 48), 0u);
}

SND_TEST(StreamPolicy, DeviceIndexResolution) {
  SND_CHECK_EQ(ResolveDeviceIndex(0, 0), -1);
  SND_CHECK_EQ(ResolveDeviceIndex(0, 5), -1);
  SND_CHECK_EQ(ResolveDeviceIndex(1, 5), 0);
  SND_CHECK_EQ(ResolveDeviceIndex(5, 5), 4);
  SND_CHECK_EQ(ResolveDeviceIndex(6, 5), -2);
  SND_CHECK_EQ(ResolveDeviceIndex(1, 0), -2);
}

SND_TEST(StreamPolicy, AlsaErrorClassification) {
  SND_CHECK_EQ(ClassifyAlsaError(0), StreamErrorAction::None);
  SND_CHECK_EQ(ClassifyAlsaError(12), StreamErrorAction::None);
  SND_CHECK_EQ(ClassifyAlsaError(-EAGAIN), StreamErrorAction::Retry);
  SND_CHECK_EQ(ClassifyAlsaError(-EINTR), StreamErrorAction::Retry);
  SND_CHECK_EQ(ClassifyAlsaError(-EPIPE), StreamErrorAction::RecoverXrun);
#if defined(ESTRPIPE)
  SND_CHECK_EQ(ClassifyAlsaError(-ESTRPIPE), StreamErrorAction::Suspended);
#endif
  SND_CHECK_EQ(ClassifyAlsaError(-ENODEV), StreamErrorAction::Lost);
  SND_CHECK_EQ(ClassifyAlsaError(-EIO), StreamErrorAction::Lost);
#if defined(EBADFD)
  SND_CHECK_EQ(ClassifyAlsaError(-EBADFD), StreamErrorAction::Lost);
#endif
  SND_CHECK_EQ(ClassifyAlsaError(-EINVAL), StreamErrorAction::Fatal);
}

SND_TEST(StreamPolicy, WasapiHresultClassification) {
  SND_CHECK_EQ(ClassifyWasapiHresult(0x00000000u), StreamErrorAction::None);  // S_OK
  SND_CHECK_EQ(ClassifyWasapiHresult(0x00000001u), StreamErrorAction::None);  // S_FALSE
  SND_CHECK_EQ(ClassifyWasapiHresult(0x88890004u), StreamErrorAction::Lost);  // DEVICE_INVALIDATED
  SND_CHECK_EQ(ClassifyWasapiHresult(0x88890026u), StreamErrorAction::Lost);  // RESOURCES_INVALIDATED
  SND_CHECK_EQ(ClassifyWasapiHresult(0x88890010u), StreamErrorAction::Lost);  // SERVICE_NOT_RUNNING
  SND_CHECK_EQ(ClassifyWasapiHresult(0x8889000Au), StreamErrorAction::Lost);  // DEVICE_IN_USE
  SND_CHECK_EQ(ClassifyWasapiHresult(0x88890006u), StreamErrorAction::Retry); // BUFFER_TOO_LARGE
  SND_CHECK_EQ(ClassifyWasapiHresult(0x80070057u), StreamErrorAction::Fatal); // E_INVALIDARG
}

SND_TEST(StreamPolicy, WasapiPlan) {
  // Shared, request matches the mix format -> low latency when IAudioClient3 exists.
  WasapiPlan p = PlanWasapiStream(0, 0, false, true, 48000, 2);
  SND_CHECK_EQ(p.mode, WasapiMode::SharedLowLatency);
  SND_CHECK_EQ(p.sampleRate, 48000u);
  SND_CHECK_EQ(p.channels, 2u);
  p = PlanWasapiStream(48000, 2, false, false, 48000, 2);
  SND_CHECK_EQ(p.mode, WasapiMode::SharedNative);
  // Different rate or channel count -> WASAPI converts (AUTOCONVERTPCM).
  p = PlanWasapiStream(44100, 2, false, true, 48000, 2);
  SND_CHECK_EQ(p.mode, WasapiMode::SharedAutoConvert);
  SND_CHECK_EQ(p.sampleRate, 44100u);
  p = PlanWasapiStream(0, 2, false, true, 48000, 6);
  SND_CHECK_EQ(p.mode, WasapiMode::SharedAutoConvert);
  SND_CHECK_EQ(p.channels, 2u);
  SND_CHECK_EQ(p.sampleRate, 48000u);
  // Exclusive: the request (or the mix values) as is.
  p = PlanWasapiStream(96000, 0, true, true, 48000, 2);
  SND_CHECK_EQ(p.mode, WasapiMode::Exclusive);
  SND_CHECK_EQ(p.sampleRate, 96000u);
  SND_CHECK_EQ(p.channels, 2u);
}

SND_TEST(StreamPolicy, SpeakerMasks) {
  SND_CHECK_EQ(SpeakerMaskForChannels(1), 0x4u);
  SND_CHECK_EQ(SpeakerMaskForChannels(2), 0x3u);
  SND_CHECK_EQ(SpeakerMaskForChannels(6), 0x3Fu);
  SND_CHECK_EQ(SpeakerMaskForChannels(8), 0x63Fu);
  SND_CHECK_EQ(SpeakerMaskForChannels(9), 0u);
  // Every mask has exactly `channels` bits set.
  for (std::uint32_t c = 1; c <= 8; ++c) {
    std::uint32_t bits = 0;
    for (std::uint32_t m = SpeakerMaskForChannels(c); m != 0u; m &= m - 1u) ++bits;
    SND_CHECK_EQ(bits, c);
  }
}

SND_TEST(StreamPolicy, RouteChangeEvents) {
  SND_CHECK_EQ(RouteChangeEvent(1, false), SND_DEVICE_EVENT_CHANGED);  // NewDeviceAvailable
  SND_CHECK_EQ(RouteChangeEvent(2, false), SND_DEVICE_EVENT_CHANGED);  // OldDeviceUnavailable
  SND_CHECK_EQ(RouteChangeEvent(4, false), SND_DEVICE_EVENT_CHANGED);  // Override
  SND_CHECK_EQ(RouteChangeEvent(8, false), SND_DEVICE_EVENT_CHANGED);  // RouteConfigurationChange
  SND_CHECK_EQ(RouteChangeEvent(7, false), SND_DEVICE_EVENT_LOST);     // NoSuitableRouteForCategory
  SND_CHECK_EQ(RouteChangeEvent(3, false), 0u);                        // CategoryChange, same format
  SND_CHECK_EQ(RouteChangeEvent(3, true), SND_DEVICE_EVENT_CHANGED);
  SND_CHECK_EQ(RouteChangeEvent(6, false), 0u);                        // WakeFromSleep
  SND_CHECK_EQ(RouteChangeEvent(0, true), SND_DEVICE_EVENT_CHANGED);
}

namespace {
struct ChunkProbe {
  std::vector<std::uint32_t> sizes;
  std::uint32_t channels = 0;
  bool ftz = true;
  bool rt = true;
  float* expectNext = nullptr;
  bool contiguous = true;
  static void Callback(void* user, float* out, std::uint32_t frames, std::uint32_t channels) {
    auto* p = static_cast<ChunkProbe*>(user);
    p->sizes.reserve(64);  // capacity reserved by the test: no allocation here
    if (p->sizes.size() < p->sizes.capacity()) p->sizes.push_back(frames);
    p->channels = channels;
    if (snd::platform::GetCpuFeatures().denormalControl) p->ftz = p->ftz && snd::platform::FlushDenormalsEnabled();
    p->rt = p->rt && snd::rt::InRtScope() == snd::rt::kEnabled;
    p->contiguous = p->contiguous && out == p->expectNext;
    p->expectNext = out + static_cast<std::size_t>(frames) * channels;
  }
};
}  // namespace

SND_TEST(DeviceCallback, DeliverFramesSplitsIntoChunksWithFtzAndRtScope) {
  std::vector<float> buf(1000 * 3);
  std::atomic<std::uint64_t> callbacks{0}, frames{0};
  ChunkProbe p;
  p.sizes.reserve(64);
  p.expectNext = buf.data();
  DeliverFrames(&ChunkProbe::Callback, &p, buf.data(), 1000, 3, 256, callbacks, frames);
  SND_REQUIRE_EQ(p.sizes.size(), 4u);
  SND_CHECK_EQ(p.sizes[0], 256u);
  SND_CHECK_EQ(p.sizes[1], 256u);
  SND_CHECK_EQ(p.sizes[2], 256u);
  SND_CHECK_EQ(p.sizes[3], 232u);
  SND_CHECK(p.contiguous);
  SND_CHECK_EQ(p.channels, 3u);
  SND_CHECK(p.ftz);
  SND_CHECK(p.rt);
  SND_CHECK_EQ(callbacks.load(), 4u);
  SND_CHECK_EQ(frames.load(), 1000u);
  SND_CHECK(!snd::rt::InRtScope());  // scope ends with the delivery
  SND_CHECK_EQ(ChunkCount(1000, 256), 4u);
  SND_CHECK_EQ(ChunkCount(256, 256), 1u);
  SND_CHECK_EQ(ChunkCount(10, 0), 1u);
  SND_CHECK_EQ(ChunkCount(0, 256), 0u);
  // Zero frames / channels / null callback: nothing happens.
  DeliverFrames(&ChunkProbe::Callback, &p, buf.data(), 0, 3, 256, callbacks, frames);
  DeliverFrames(nullptr, &p, buf.data(), 10, 3, 256, callbacks, frames);
  SND_CHECK_EQ(callbacks.load(), 4u);
}

namespace {
struct AllocatingCallback {
  static void Callback(void*, float*, std::uint32_t, std::uint32_t) {
    int* leak = new int(1);  // forbidden in the callback: must be reported
    delete leak;
  }
};
}  // namespace

SND_TEST(DeviceCallback, AllocationInCallbackIsReported) {
  if (!snd::rt::kEnabled) return;
  std::vector<float> buf(64);
  std::atomic<std::uint64_t> callbacks{0}, frames{0};
  snd::test::rt::ExpectViolations expect;
  snd::test::rt::Reset();
  DeliverFrames(&AllocatingCallback::Callback, nullptr, buf.data(), 32, 2, 32, callbacks, frames);
  SND_CHECK(snd::test::rt::Count(snd::rt::Violation::Alloc) >= 1u);
  snd::test::rt::Reset();
}

SND_TEST(DeviceCallback, CallbackMarkerNests) {
  int a = 0, b = 0;
  SND_CHECK(!CalledFromCallback(&a));
  {
    CallbackMarker ma(&a);
    SND_CHECK(CalledFromCallback(&a));
    {
      CallbackMarker mb(&b);
      SND_CHECK(CalledFromCallback(&b));
      SND_CHECK(!CalledFromCallback(&a));
    }
    SND_CHECK(CalledFromCallback(&a));
  }
  SND_CHECK(!CalledFromCallback(&a));
}

SND_TEST(OwnedString, CopiesAndTruncatesUtf8) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  {
    OwnedString s;
    SND_CHECK(s.Empty());
    SND_REQUIRE(s.Assign(acc, "hw:0,0"));
    SND_CHECK(std::strcmp(s.Get(), "hw:0,0") == 0);
    SND_REQUIRE(s.Assign(acc, nullptr));
    SND_CHECK(s.Get() == nullptr);
    SND_REQUIRE(s.Assign(acc, "plughw:1"));
  }
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
  char buf[6];
  CopyTruncatedUtf8(buf, sizeof(buf), "abcdefgh");
  SND_CHECK(std::strcmp(buf, "abcde") == 0);
  CopyTruncatedUtf8(buf, sizeof(buf), "ab\xE3\x81\x82\xE3\x81\x84");  // "abあい": never split a sequence
  SND_CHECK(std::strcmp(buf, "ab\xE3\x81\x82") == 0);
  CopyTruncatedUtf8(buf, sizeof(buf), nullptr);
  SND_CHECK_EQ(buf[0], '\0');
}
