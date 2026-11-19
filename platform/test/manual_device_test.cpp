// ManualOutputDevice (manual_output_device.hpp, CONTRACTS.md D-102): the
// callback runs only inside Advance, for whole periods, on the calling thread;
// the output is what the callback wrote; Stop / Close / Advance from inside the
// callback are refused and counted.
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/test/test.hpp"

using snd::platform::ManualDeviceConfig;
using snd::platform::ManualOutputDevice;
using snd::platform::OutputBackend;

namespace {

struct Feed {
  ManualOutputDevice* device = nullptr;
  std::uint64_t frames = 0;   // frames written so far: sample value = frame index + channel / 10
  std::uint32_t calls = 0;
  std::uint32_t lastFrames = 0;
  std::thread::id thread{};
  bool misuse = false;        // call Stop / Close / Advance from inside the callback
  std::uint32_t before = 0;   // BeforePeriod calls
};

void SND_CALL Callback(void* user, float* out, std::uint32_t frames, std::uint32_t channels) {
  Feed& f = *static_cast<Feed*>(user);
  for (std::uint32_t i = 0; i < frames; ++i) {
    for (std::uint32_t c = 0; c < channels; ++c) {
      out[i * channels + c] = static_cast<float>(f.frames + i) + 0.1f * static_cast<float>(c);
    }
  }
  f.frames += frames;
  ++f.calls;
  f.lastFrames = frames;
  f.thread = std::this_thread::get_id();
  if (f.misuse) {
    f.device->Stop();
    f.device->Close();
    f.device->Advance(1, nullptr);
  }
}

void Before(void* user) noexcept { ++static_cast<Feed*>(user)->before; }

}  // namespace

SND_TEST(ManualDevice, AdvanceRunsWholePeriodsOnTheCallingThread) {
  ManualDeviceConfig mc;
  mc.periodFrames = 64;
  ManualOutputDevice dev(snd::base::DefaultAllocator(), mc);
  SND_CHECK(dev.Backend() == OutputBackend::Manual);
  SND_CHECK(snd::platform::OutputBackendAvailable(OutputBackend::Manual));
  Feed f;
  f.device = &dev;
  dev.SetBeforePeriod(&Before, &f);
  SND_REQUIRE(dev.Open(snd::DeviceDesc{44100, 3, 0, 0}, &Callback, &f));
  SND_CHECK_EQ(dev.SampleRate(), 44100u);
  SND_CHECK_EQ(dev.Channels(), 3u);
  SND_CHECK_EQ(dev.PeriodFrames(), 64u);
  SND_CHECK_NEAR(dev.OutputLatencySeconds(), 64.0 / 44100.0, 1e-12);
  SND_CHECK_EQ(dev.Advance(2, nullptr), 0u);  // not started: nothing runs
  SND_CHECK_EQ(f.calls, 0u);
  SND_REQUIRE(dev.Start());
  std::vector<float> out(5u * 64u * 3u, -1.0f);
  SND_CHECK_EQ(dev.Advance(5, out.data()), 5u);
  SND_CHECK_EQ(f.calls, 5u);
  SND_CHECK_EQ(f.before, 5u);
  SND_CHECK_EQ(f.lastFrames, 64u);
  SND_CHECK(f.thread == std::this_thread::get_id());
  SND_CHECK_EQ(dev.FramesDelivered(), 320u);
  bool exact = true;
  for (std::uint32_t i = 0; i < 320u; ++i) {
    for (std::uint32_t c = 0; c < 3u; ++c) {
      exact = exact && out[i * 3u + c] == static_cast<float>(i) + 0.1f * static_cast<float>(c);
    }
  }
  SND_CHECK(exact);
  snd::platform::OutputClockSample cs;
  SND_REQUIRE(dev.OutputClock(&cs));
  SND_CHECK_EQ(cs.framesWritten, 320u);
  SND_CHECK_EQ(cs.sampleRate, 44100u);
  // Real time does not move it: no callback runs between Advance calls.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  SND_CHECK_EQ(f.calls, 5u);
  dev.Stop();
  SND_CHECK_EQ(dev.Advance(1, nullptr), 0u);
  SND_CHECK_EQ(f.calls, 5u);
  dev.Close();
  SND_CHECK(!dev.IsOpen());
}

SND_TEST(ManualDevice, StopCloseAndAdvanceFromTheCallbackAreRefused) {
  ManualOutputDevice dev(snd::base::DefaultAllocator());
  Feed f;
  f.device = &dev;
  f.misuse = true;
  SND_REQUIRE(dev.Open(snd::DeviceDesc{0, 0, 0, 0}, &Callback, &f));
  SND_CHECK_EQ(dev.SampleRate(), 48000u);  // the config's defaults for 0
  SND_CHECK_EQ(dev.Channels(), 2u);
  SND_CHECK_EQ(dev.PeriodFrames(), 256u);
  SND_REQUIRE(dev.Start());
  SND_CHECK_EQ(dev.Advance(2, nullptr), 2u);
  SND_CHECK_EQ(f.calls, 2u);  // the nested Advance ran nothing
  SND_CHECK_EQ(dev.MisuseFromCallback(), 6u);
  SND_CHECK(dev.IsOpen() && dev.IsStarted());
  f.misuse = false;
  dev.Close();
}

SND_TEST(ManualDevice, FactoryAndOpenLimits) {
  snd::platform::PlatformOutputDevice* d =
      snd::platform::CreateOutputDevice(OutputBackend::Manual, snd::base::DefaultAllocator());
  SND_REQUIRE(d != nullptr);
  SND_CHECK(d->Backend() == OutputBackend::Manual);
  SND_CHECK(std::string(snd::platform::OutputBackendName(d->Backend())) == "manual");
  Feed f;
  SND_CHECK(!d->Open(snd::DeviceDesc{48000, SND_MAX_CHANNELS + 1u, 0, 0}, &Callback, &f));
  SND_CHECK(!d->Open(snd::DeviceDesc{48000, 2, 0, 0}, nullptr, &f));
  SND_CHECK(d->Open(snd::DeviceDesc{48000, 2, 128, 0}, &Callback, &f));
  SND_CHECK(!d->Open(snd::DeviceDesc{48000, 2, 128, 0}, &Callback, &f));  // already open
  SND_CHECK_EQ(d->PeriodFrames(), 128u);
  d->Close();
  snd::platform::DestroyOutputDevice(d, snd::base::DefaultAllocator());
  snd::platform::OutputDeviceInfo info[2];
  SND_CHECK_EQ(snd::platform::EnumerateOutputDevices(OutputBackend::Manual, info, 2), 1u);
  SND_CHECK(std::string(info[0].id) == "manual");
}
