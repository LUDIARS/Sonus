// Independent tests: the output bridge of the composition root (design.md 2.2, 2.6;
// CONTRACTS.md D-66, D-95).
// Properties (from the spec, before reading the implementation):
//  D1 channel mapping: common channels copied; a mono device gets the mean of L and R
//     (checked with L != R); a mono core feeds device channels 1 and 2; extra device
//     channels are silent.
//  D2 a device at another rate: on_error(UNSUPPORTED, 0, rate) exactly once, also when
//     the rate changes again later (first time only); the output is not converted.
//  D3 underrun: the frames the device played as silence are caught up, so the common
//     clock (core blocks) runs ahead of the driver's own blocks by exactly
//     floor(lost / block) once caught up, at most underrun_catchup_blocks per block.
//  D4 LOST -> on_error(NO_DEVICE, SND_DEVICE_EVENT_LOST, 0) at the next update, once per event.
//  D5 SND_FOCUS_SILENCE: the block after FOCUS_LOST ramps linearly from the level to 0
//     over one block; silent after; FOCUS_GAINED ramps back over one block.
//     CONTINUE and PER_CATEGORY (= CONTINUE, not implemented) change nothing.
#include <cmath>
#include <thread>
#include <vector>

#include "indep_rig.hpp"

using apitest::kCenter;
using apitest::SmallConfig;

namespace {

struct Stereo {
  float l, r;
};

void SND_CALL StereoSynth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                          const snd_render_context*) {
  const Stereo v = *static_cast<const Stereo*>(user);
  for (std::uint32_t f = 0; f < frames; ++f) {
    out[0][f] = v.l;
    if (channels > 1) out[1][f] = v.r;
  }
}

snd_playback_handle PlayStereo(indep::Rig& r, const Stereo* v) {
  snd_synth_desc d{};
  d.callback = &StereoSynth;
  d.user = const_cast<Stereo*>(v);
  d.channels = 2;
  d.mode = SND_SYNTH_LOW_LATENCY;
  d.volume = 1.0f;
  return snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
}

}  // namespace

// D1: what a stereo voice with L != R looks like on a stereo, a mono and a 3-channel device.
SND_TEST(IndepDevice, ChannelMappingWithDistinctChannels) {
  static const Stereo kV{0.6f, 0.2f};
  float refL = 0, refR = 0;
  {
    indep::Rig r;  // stereo device: the reference values of L and R on the master
    SND_REQUIRE_EQ(r.created, SND_OK);
    SND_REQUIRE(PlayStereo(r, &kV).value != 0u);
    r.Step(12);
    refL = r.Raw(r.Frames() - 10, 0);
    refR = r.Raw(r.Frames() - 10, 1);
    SND_REQUIRE(std::fabs(refL - refR) > 0.1f);  // the channels really differ
  }
  for (const std::uint32_t ch : {1u, 3u}) {
    snd::mock::NullDeviceConfig dc;
    dc.channels = ch;
    dc.honorRequest = false;
    indep::Rig r(SmallConfig(), dc);
    SND_REQUIRE_EQ(r.created, SND_OK);
    SND_REQUIRE(PlayStereo(r, &kV).value != 0u);
    r.Step(12);
    const std::uint64_t f = r.Frames() - 10;
    if (ch == 1u) {
      SND_CHECK_NEAR(r.Raw(f, 0), 0.5f * (refL + refR), 1e-6f);
    } else {
      SND_CHECK_NEAR(r.Raw(f, 0), refL, 1e-6f);
      SND_CHECK_NEAR(r.Raw(f, 1), refR, 1e-6f);
      SND_CHECK_EQ(r.Raw(f, 2), 0.0f);
    }
  }
}

// D1: a mono core (Config.output_channels = 1) on a stereo device feeds both channels.
SND_TEST(IndepDevice, MonoCoreOnAStereoDevice) {
  static const float kValue = 0.4f;
  snd_config c = SmallConfig();
  c.output_channels = 1;
  snd::mock::NullDeviceConfig dc;
  dc.channels = 2;
  dc.honorRequest = false;
  indep::Rig r(c, dc);
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd_synth_desc d = indep::DcDesc(&kValue);
  SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr).value != 0u);
  r.Step(12);
  const std::uint64_t f = r.Frames() - 10;
  const float l = r.Raw(f, 0);
  SND_CHECK(std::fabs(l) > 0.1f);
  SND_CHECK_EQ(r.Raw(f, 1), l);
}

// D2: the other rate is reported once, even after another CHANGED with yet another rate.
SND_TEST(IndepDevice, OtherRateReportedOnceAcrossChanges) {
  snd::mock::NullDeviceConfig dc;
  dc.sampleRate = 44100;
  dc.honorRequest = false;
  indep::Rig r(SmallConfig(), dc);
  SND_REQUIRE_EQ(r.created, SND_OK);
  r.Step(6);
  snd_device_event e{};
  e.kind = SND_DEVICE_EVENT_CHANGED;
  e.sample_rate = 96000;
  e.channels = 2;
  r.device.EmitEvent(e);
  r.Step(10);
  std::uint32_t n = 0;
  for (const apitest::Error& x : r.ev.errors) n += x.code == SND_ERR_UNSUPPORTED ? 1u : 0u;
  SND_CHECK_EQ(n, 1u);
  SND_CHECK_EQ(r.refillTimeouts, 0u);
}

// D2 with the callbacks installed after the first updates (accepted decision, D-95 note):
// a notice that came before is delivered once, at the first update after they were set.
SND_TEST(IndepDevice, OtherRateWithLateCallbacks) {
  snd::mock::NullDeviceConfig dc;
  dc.sampleRate = 44100;
  dc.honorRequest = false;
  indep::Rig r(SmallConfig(), dc, /*installCallbacks=*/false);
  SND_REQUIRE_EQ(r.created, SND_OK);
  r.Step(4);  // updates without callbacks
  r.InstallCallbacks();
  r.Step(4);
  std::uint32_t n = 0;
  for (const apitest::Error& x : r.ev.errors) n += x.code == SND_ERR_UNSUPPORTED ? 1u : 0u;
  SND_CHECK_EQ(n, 1u);
}

// D4: one report per LOST event, at the next update.
SND_TEST(IndepDevice, LostReportedOncePerEvent) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  r.Step(4);
  snd_device_event e{};
  e.kind = SND_DEVICE_EVENT_LOST;
  r.device.EmitEvent(e);
  r.device.EmitEvent(e);
  snd_update(r.sys);
  std::uint32_t n = 0;
  for (const apitest::Error& x : r.ev.errors) {
    if (x.code == SND_ERR_NO_DEVICE) {
      ++n;
      SND_CHECK_EQ(x.detail, SND_DEVICE_EVENT_LOST);
      SND_CHECK_EQ(x.object, 0u);
    }
  }
  SND_CHECK_EQ(n, 2u);
  snd_update(r.sys);
  snd_update(r.sys);
  n = 0;
  for (const apitest::Error& x : r.ev.errors) n += x.code == SND_ERR_NO_DEVICE ? 1u : 0u;
  SND_CHECK_EQ(n, 2u);
}

// D5: the exact ramp.
SND_TEST(IndepDevice, FocusSilenceRampsOverOneBlock) {
  static const float kValue = 0.5f;
  snd_config c = SmallConfig();
  c.focus_mode = SND_FOCUS_SILENCE;
  indep::Rig r(c);
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd_synth_desc d = indep::DcDesc(&kValue);
  SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr).value != 0u);
  r.Step(10);
  const float level = r.Raw(r.Frames() - 1);
  SND_REQUIRE(std::fabs(level - kValue * kCenter) < 1e-6f);
  snd_device_event e{};
  e.kind = SND_DEVICE_EVENT_FOCUS_LOST;
  const std::uint64_t from = r.Frames();
  r.device.EmitEvent(e);
  r.Step(12);
  // Find the block where it starts to fall (block aligned), check the line, then zeros.
  std::uint64_t b = from;
  while (b < r.Frames() && std::fabs(r.Raw(b) - level) < 1e-6f) ++b;
  SND_REQUIRE(b < r.Frames() - 512);
  SND_CHECK_EQ(b % 256u, 0u);
  for (std::uint32_t f = 0; f < 256; ++f) {
    const float want = level * (1.0f - static_cast<float>(f + 1) / 256.0f);
    SND_CHECK_NEAR(r.Raw(b + f), want, 2e-6f);
  }
  for (std::uint64_t f = b + 256; f < r.Frames(); ++f) SND_CHECK_EQ(r.Raw(f), 0.0f);
  e.kind = SND_DEVICE_EVENT_FOCUS_GAINED;
  const std::uint64_t back = r.Frames();
  r.device.EmitEvent(e);
  r.Step(12);
  std::uint64_t u = back;
  while (u < r.Frames() && r.Raw(u) == 0.0f) ++u;
  SND_REQUIRE(u < r.Frames() - 512);
  SND_CHECK_EQ(u % 256u, 0u);
  for (std::uint32_t f = 0; f < 256; ++f) {
    const float want = level * static_cast<float>(f + 1) / 256.0f;
    SND_CHECK_NEAR(r.Raw(u + f), want, 2e-6f);
  }
  for (std::uint64_t f = u + 256; f < r.Frames(); ++f) SND_CHECK_NEAR(r.Raw(f), level, 1e-6f);
}

SND_TEST(IndepDevice, FocusContinueAndPerCategoryChangeNothing) {
  static const float kValue = 0.5f;
  for (const snd_focus_mode m : {SND_FOCUS_CONTINUE, SND_FOCUS_PER_CATEGORY}) {
    snd_config c = SmallConfig();
    c.focus_mode = m;
    indep::Rig r(c);
    SND_REQUIRE_EQ(r.created, SND_OK);
    const snd_synth_desc d = indep::DcDesc(&kValue);
    SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr).value != 0u);
    r.Step(6);
    const std::uint64_t from = r.Frames();
    snd_device_event e{};
    e.kind = SND_DEVICE_EVENT_FOCUS_LOST;
    r.device.EmitEvent(e);
    r.Step(10);
    for (std::uint64_t f = from; f < r.Frames(); ++f) SND_CHECK_NEAR(r.Raw(f), kValue * kCenter, 1e-6f);
  }
}

// D3: catch-up accounting after underruns. The underrun is made deterministically: a
// LowLatency synth callback holds the audio thread (spinning, no system call) while the
// test pulls periods from the device, so the ring runs dry for a known number of periods.
namespace {
struct Gate {
  std::atomic<int> hold{0};
  std::atomic<int> inside{0};
};
void SND_CALL GateSynth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                        const snd_render_context*) {
  auto* g = static_cast<Gate*>(user);
  if (g->hold.load(std::memory_order_acquire) != 0) {
    g->inside.store(1, std::memory_order_release);
    while (g->hold.load(std::memory_order_acquire) != 0) {
    }
    g->inside.store(0, std::memory_order_release);
  }
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = 0.0f;
  }
}
}  // namespace

SND_TEST(IndepDevice, UnderrunsAreCaughtUpOnTheCommonClock) {
  snd_config c = SmallConfig();
  c.tuning.underrun_catchup_blocks = 2;
  indep::Rig r(c);
  SND_REQUIRE_EQ(r.created, SND_OK);
  Gate gate;
  snd_synth_desc d{};
  d.callback = &GateSynth;
  d.user = &gate;
  d.channels = 1;
  d.mode = SND_SYNTH_LOW_LATENCY;
  d.volume = 1.0f;
  SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr).value != 0u);
  r.Step(8);
  snd::platform::AudioDriver* drv = r.Driver();
  SND_REQUIRE(drv != nullptr);
  const std::uint64_t lost0 = drv->Stats().core.lostFrames;
  SND_REQUIRE_EQ(lost0, 0u);
  // Hold the audio thread inside the next block it renders.
  gate.hold.store(1, std::memory_order_release);
  r.device.Pump(1);  // wakes it: it renders, enters the callback and stays there
  while (gate.inside.load(std::memory_order_acquire) == 0) r.device.Pump(1);
  // The ring holds at most the render-ahead target: 40 more periods surely run it dry.
  r.device.Pump(40);
  const std::uint64_t drvAtRelease = drv->Stats().core.blocksRendered;  // the held block not yet counted
  gate.hold.store(0, std::memory_order_release);
  // Wait until the lost frames are known (the driver counts them when the callback finds
  // the ring short; that happened during the pumps above).
  const std::uint64_t lost = drv->Stats().core.lostFrames;
  SND_REQUIRE(lost >= 20u * 256u);
  // Catch up: normal periods with refills, until core is ahead of the driver by
  // floor(lost / 256) blocks (bounded wait by quantity, not time).
  snd_stats st{};
  const std::uint64_t want = lost / 256u;
  std::uint64_t steps = 0;
  for (; steps < 4000; ++steps) {
    r.Step(1);
    r.WaitRefill();
    snd_update(r.sys);
    if (snd_get_stats(r.sys, &st) != SND_OK) continue;
    const std::uint64_t drvBlocks = drv->Stats().core.blocksRendered;
    if (st.block_index >= drvBlocks + want) break;
  }
  SND_CHECK(steps < 4000u);
  const snd::platform::AudioDriverStats ds = drv->Stats();
  SND_CHECK_EQ(ds.core.lostFrames, lost);  // no further underrun while catching up
  // Exactly floor(lost / 256) discarded blocks: never more (the core is not running ahead).
  // The published stats may lag the driver's counter by the block being rendered.
  SND_CHECK(st.block_index <= ds.core.blocksRendered + want);
  SND_CHECK(st.block_index + 1u >= ds.core.blocksRendered + want);
  SND_CHECK_EQ(st.clock_sample, st.block_index * 256u);
  // At most underrun_catchup_blocks (2) extra blocks per driver block: with `want` extra
  // blocks the catch-up took at least ceil(want / 2) driver blocks after the release.
  SND_CHECK_MSG(2u * (ds.core.blocksRendered - drvAtRelease) >= want, "%llu extra blocks in %llu driver blocks",
                static_cast<unsigned long long>(want),
                static_cast<unsigned long long>(ds.core.blocksRendered - drvAtRelease));
}
