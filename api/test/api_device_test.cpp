// The output side of the composition root (2.2, 2.6, CONTRACTS.md D-66, D-95):
//  - a device whose channel layout differs from Config.outputChannels (mono, 4ch);
//  - a device at another rate: reported once through on_error (not converted);
//  - LOST -> on_error(SND_ERR_NO_DEVICE); SND_FOCUS_SILENCE mutes while the focus is lost;
//  - snd_resume -> AudioDriver::Resume for a device the OS left SUSPENDED, on the owner
//    thread at once, from another registered thread at the owner's next snd_update;
//  - snd_suspend holds the common clock (core renders silence).
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "api_rig.hpp"

using namespace apitest;

namespace {

void SND_CALL Dc(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                 const snd_render_context*) {
  const float v = *static_cast<const float*>(user);
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = v;
  }
}

snd_playback_handle PlayDc(Rig& r, const float* value) {
  snd_synth_desc d{};
  d.callback = &Dc;
  d.user = const_cast<float*>(value);
  d.channels = 1;
  d.mode = SND_SYNTH_LOW_LATENCY;
  d.volume = 1.0f;
  return snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
}

float Sample(const Rig& r, std::uint64_t frame, std::uint32_t channel) {
  const std::vector<float>& c = r.device.Captured();
  const std::size_t i = static_cast<std::size_t>(frame) * r.device.Channels() + channel;
  return i < c.size() ? c[i] : -100.0f;
}

}  // namespace

SND_TEST(ApiDevice, MonoAndFourChannelDevices) {
  static const float kValue = 0.25f;
  for (const std::uint32_t ch : {1u, 4u}) {
    snd::mock::NullDeviceConfig dc;
    dc.channels = ch;
    dc.honorRequest = false;
    Rig r(SmallConfig(), dc);
    SND_REQUIRE_EQ(r.created, SND_OK);
    SND_REQUIRE_EQ(r.device.Channels(), ch);
    SND_REQUIRE(PlayDc(r, &kValue).value != 0u);
    r.Step(20);
    const std::uint64_t f = r.Frames() - 100;
    const float centre = kValue * kCenter;  // a mono voice on the stereo master
    if (ch == 1u) {
      SND_CHECK_NEAR(Sample(r, f, 0), centre, 1e-6f);  // mean of L and R
    } else {
      SND_CHECK_NEAR(Sample(r, f, 0), centre, 1e-6f);
      SND_CHECK_NEAR(Sample(r, f, 1), centre, 1e-6f);
      SND_CHECK_EQ(Sample(r, f, 2), 0.0f);
      SND_CHECK_EQ(Sample(r, f, 3), 0.0f);
    }
    SND_CHECK_EQ(r.refillTimeouts, 0u);
  }
}

SND_TEST(ApiDevice, OtherRateIsReportedOnce) {
  snd::mock::NullDeviceConfig dc;
  dc.sampleRate = 44100;
  dc.honorRequest = false;
  Rig r(SmallConfig(), dc);
  SND_REQUIRE_EQ(r.created, SND_OK);
  r.Step(10);
  std::uint32_t n = 0;
  for (const Error& e : r.ev.errors) {
    if (e.code == SND_ERR_UNSUPPORTED && e.object == 44100u) ++n;
  }
  SND_CHECK_EQ(n, 1u);
}

SND_TEST(ApiDevice, LostAndFocus) {
  static const float kValue = 0.5f;
  snd_config c = SmallConfig();
  c.focus_mode = SND_FOCUS_SILENCE;
  Rig r(c);
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE(PlayDc(r, &kValue).value != 0u);
  r.Step(10);
  snd_device_event e{};
  e.kind = SND_DEVICE_EVENT_LOST;
  r.device.EmitEvent(e);
  r.Step(1);
  std::uint32_t lost = 0;
  for (const Error& x : r.ev.errors) lost += (x.code == SND_ERR_NO_DEVICE && x.detail == SND_DEVICE_EVENT_LOST) ? 1u : 0u;
  SND_CHECK_EQ(lost, 1u);
  // Focus lost: silence from the block after the event (blocks already rendered ahead play on).
  e.kind = SND_DEVICE_EVENT_FOCUS_LOST;
  r.device.EmitEvent(e);
  const std::uint64_t at = r.Driver()->Stats().core.blocksRendered * 256u;
  r.Step(12);
  for (std::uint64_t f = at + 256; f < at + 1024; ++f) SND_CHECK_EQ(Sample(r, f, 0), 0.0f);
  e.kind = SND_DEVICE_EVENT_FOCUS_GAINED;
  r.device.EmitEvent(e);
  const std::uint64_t back = r.Driver()->Stats().core.blocksRendered * 256u;
  r.Step(12);
  SND_CHECK_NEAR(Sample(r, back + 512, 0), kValue * kCenter, 1e-6f);
}

SND_TEST(ApiDevice, ResumeRestartsASuspendedDevice) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  r.Step(4);
  // The OS suspends the device (iOS interruption without ShouldResume, D-63).
  snd_device_event e{};
  e.kind = SND_DEVICE_EVENT_SUSPENDED;
  r.device.EmitEvent(e);
  r.device.Stop();
  SND_CHECK_EQ(r.device.Pump(1), 0u);
  // From another registered thread: recorded now, the device restarts in the owner's update.
  std::thread t([&] {
    SND_CHECK_EQ(snd_register_thread(r.sys), SND_OK);
    SND_CHECK_EQ(snd_resume(r.sys), SND_OK);
    SND_CHECK_EQ(snd_flush(r.sys), SND_OK);
    SND_CHECK_EQ(snd_unregister_thread(r.sys), SND_OK);
  });
  t.join();
  SND_CHECK(!r.device.IsStarted());
  SND_CHECK_EQ(snd_update(r.sys), SND_OK);
  SND_CHECK(r.device.IsStarted());
  SND_CHECK_EQ(r.device.Pump(1), 1u);
  // On the owner thread: at once.
  r.device.EmitEvent(e);
  r.device.Stop();
  SND_CHECK_EQ(snd_resume(r.sys), SND_OK);
  SND_CHECK(r.device.IsStarted());
  // Not suspended: snd_resume only records the command.
  SND_CHECK_EQ(snd_resume(r.sys), SND_OK);
  SND_CHECK(r.device.IsStarted());
  r.Step(4);
  SND_CHECK_EQ(r.refillTimeouts, 0u);
}

SND_TEST(ApiDevice, SuspendHoldsTheClock) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  r.Step(8);
  SND_CHECK_EQ(snd_suspend(r.sys), SND_OK);
  r.Step(8);  // applied within the render-ahead
  snd_stats a{}, b{};
  snd_update(r.sys);
  SND_CHECK_EQ(snd_get_stats(r.sys, &a), SND_OK);
  r.Step(10);
  snd_update(r.sys);
  SND_CHECK_EQ(snd_get_stats(r.sys, &b), SND_OK);
  SND_CHECK_EQ(a.clock_sample, b.clock_sample);  // held
  SND_CHECK_EQ(snd_resume(r.sys), SND_OK);
  r.Step(10);
  snd_update(r.sys);
  SND_CHECK_EQ(snd_get_stats(r.sys, &b), SND_OK);
  SND_CHECK(b.clock_sample > a.clock_sample);
}

namespace {
snd_config LifecycleConfig() {
  auto c = SmallConfig();
  c.commands.arena_bytes = 256;
  c.commands.reserve_bytes = 64;
  c.commands.overflow_param_slots = 1;
  c.commands.arenas_per_thread = 2;
  return c;
}

// 32-byte SetParam packets: eight normal, two reserve, one fallback key.
// The distinct twelfth key proves actual saturation before a lifecycle call.
void FillLifecycleArena(snd_system* sys) {
  for (snd_id i = 0; i < 11; ++i) {
    SND_REQUIRE_EQ(snd_set_param_global(sys, 0x5000u + i, 1.0f), SND_OK);
  }
  SND_REQUIRE_EQ(snd_set_param_global(sys, 0x500bu, 1.0f), SND_ERR_COMMAND_BUFFER_FULL);
}
}  // namespace

SND_TEST(ApiDevice, A005_FullArenaAlternatesSilenceAndAudio) {
  static const float value = 0.25f;
  Rig r(LifecycleConfig());
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE(PlayDc(r, &value).value != 0u);
  r.Step(10);
  for (int cycle = 0; cycle < 6; ++cycle) {
    FillLifecycleArena(r.sys);
    // These all belong to one render boundary. The final suspend must win.
    for (int i = 0; i < 16; ++i) {
      SND_REQUIRE_EQ(snd_suspend(r.sys), SND_OK);
      SND_REQUIRE_EQ(snd_resume(r.sys), SND_OK);
    }
    SND_REQUIRE_EQ(snd_suspend(r.sys), SND_OK);
    r.Step(12);
    snd_stats a{}, b{};
    SND_REQUIRE_EQ(snd_update(r.sys), SND_OK);
    SND_REQUIRE_EQ(snd_get_stats(r.sys, &a), SND_OK);
    for (int i = 0; i < 1000; ++i) SND_REQUIRE_EQ(snd_update(r.sys), SND_OK);
    r.Step(8);
    SND_REQUIRE_EQ(snd_update(r.sys), SND_OK);
    SND_REQUIRE_EQ(snd_get_stats(r.sys, &b), SND_OK);
    SND_CHECK_EQ(b.clock_sample, a.clock_sample);
    SND_CHECK_EQ(Sample(r, r.Frames() - 1u, 0), 0.0f);

    FillLifecycleArena(r.sys);
    SND_REQUIRE_EQ(snd_suspend(r.sys), SND_OK);
    SND_REQUIRE_EQ(snd_resume(r.sys), SND_OK);
    r.Step(12);
    SND_REQUIRE_EQ(snd_update(r.sys), SND_OK);
    SND_REQUIRE_EQ(snd_get_stats(r.sys, &b), SND_OK);
    SND_CHECK(b.clock_sample > a.clock_sample);
    SND_CHECK_NEAR(Sample(r, r.Frames() - 1u, 0), value * kCenter, 1e-6f);
  }
  SND_CHECK_EQ(r.refillTimeouts, 0u);
}

SND_TEST(ApiDevice, A005_RetiringWorkerResumesStoppedDeviceAtSaturation) {
  Rig r(LifecycleConfig());
  SND_REQUIRE_EQ(r.created, SND_OK);
  SND_REQUIRE_EQ(snd_suspend(r.sys), SND_OK);
  r.Step(12);
  SND_REQUIRE(r.WaitRefill());
  SND_REQUIRE_EQ(snd_update(r.sys), SND_OK);
  snd_stats before{}, after{};
  SND_REQUIRE_EQ(snd_get_stats(r.sys, &before), SND_OK);
  snd_device_event event{};
  event.kind = SND_DEVICE_EVENT_SUSPENDED;
  r.device.EmitEvent(event);
  r.device.Stop();
  std::thread worker([&] {
    SND_REQUIRE_EQ(snd_resume(r.sys), SND_ERR_THREAD_NOT_REGISTERED);
    SND_REQUIRE_EQ(snd_register_thread(r.sys), SND_OK);
    SND_REQUIRE_EQ(snd_suspend(r.sys), SND_OK);
    SND_REQUIRE_EQ(snd_flush(r.sys), SND_OK);  // one in flight, one still writable
    FillLifecycleArena(r.sys);
    const auto result = snd_resume(r.sys);
    SND_CHECK_EQ(result, SND_OK);
    SND_CHECK_EQ(snd_unregister_thread(r.sys), SND_OK);
  });
  worker.join();
  SND_CHECK(!r.device.IsStarted());
  SND_REQUIRE_EQ(snd_update(r.sys), SND_OK);
  SND_REQUIRE(r.device.IsStarted());
  r.Step(16);
  SND_REQUIRE_EQ(snd_update(r.sys), SND_OK);
  SND_REQUIRE_EQ(snd_get_stats(r.sys, &after), SND_OK);
  SND_CHECK(after.clock_sample > before.clock_sample);
  SND_CHECK_EQ(r.refillTimeouts, 0u);
}
