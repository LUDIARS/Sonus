// Decisions of the api acceptance (CONTRACTS.md D-95, D-97):
//  - a synth's `user` lifetime: on_end comes only after the last call of the playback's
//    callback returned (a render-ahead job still running holds it back);
//  - notices that came before the game installed its callbacks (rate mismatch, LOST) are
//    delivered once, at the first snd_update after on_error is installed;
//  - Buffered synths beyond the rings wait silently (as if virtual) and sound once a
//    ring is free; none is refused.
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "api_rig.hpp"

using namespace apitest;

namespace {

struct Slow {
  std::atomic<bool> slow{false};
  std::atomic<int> inCall{0};
  std::atomic<bool> ended{false};
  std::atomic<int> callsAfterEnd{0};
  std::atomic<int> endsWhileInCall{0};
  std::atomic<int> ends{0};
};

void SND_CALL SlowSynth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                        const snd_render_context*) {
  auto* s = static_cast<Slow*>(user);
  if (s->ended.load()) s->callsAfterEnd.fetch_add(1);
  s->inCall.fetch_add(1);
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = 0.1f;
  }
  if (s->slow.load()) std::this_thread::sleep_for(std::chrono::milliseconds(40));  // a normal worker
  s->inCall.fetch_sub(1);
}

void SND_CALL Dc(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                 const snd_render_context*) {
  const float v = *static_cast<const float*>(user);
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = v;
  }
}

}  // namespace

SND_TEST(ApiLifetime, OnEndComesAfterTheLastCallbackCall) {
  for (int round = 0; round < 3; ++round) {
    Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    r.settle = false;  // the job runs on its own while the game updates
    Slow s;
    snd_callbacks cb{};
    cb.user = &s;
    cb.on_end = [](void* u, snd_playback_handle, snd_end_reason, snd_end_detail) {
      auto* x = static_cast<Slow*>(u);
      if (x->inCall.load() != 0) x->endsWhileInCall.fetch_add(1);
      x->ended.store(true);
      x->ends.fetch_add(1);
    };
    SND_CHECK_EQ(snd_set_callbacks(r.sys, &cb), SND_OK);
    snd_synth_desc d{};
    d.callback = &SlowSynth;
    d.user = &s;
    d.channels = 1;
    d.mode = SND_SYNTH_BUFFERED;
    d.volume = 1.0f;
    const snd_playback_handle p = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
    SND_REQUIRE(p.value != 0u);
    r.Step(10);
    s.slow.store(true);  // the next render-ahead job takes 40 ms per call
    r.Step(2);
    SND_CHECK_EQ(snd_stop(r.sys, p, 0.0f, nullptr), SND_OK);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (s.ends.load() == 0 && std::chrono::steady_clock::now() < until) r.Step(1);
    SND_CHECK_EQ(s.ends.load(), 1);
    SND_CHECK_EQ(s.endsWhileInCall.load(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    r.Step(4);
    SND_CHECK_EQ(s.callsAfterEnd.load(), 0);  // `user` could have been freed in on_end
    SND_CHECK_EQ(s.ends.load(), 1);
  }
}

SND_TEST(ApiLifetime, NoticesBeforeTheCallbacksAreDeliveredOnce) {
  snd::mock::NullDeviceConfig dc;
  dc.sampleRate = 44100;
  dc.honorRequest = false;
  snd::mock::NullOutputDevice dev(dc);
  snd::mock::MemoryFileSystem fs;
  snd_output_device dt = snd::ToC(dev);
  snd_file_system ft = snd::ToC(fs);
  snd_config c = SmallConfig();
  c.output.device = &dt;
  snd_system* sys = nullptr;
  SND_REQUIRE_EQ(snd_create(&c, nullptr, &ft, &sys), SND_OK);
  for (int i = 0; i < 4; ++i) {
    snd_update(sys);
    dev.Pump(1);
  }
  snd_device_event e{};
  e.kind = SND_DEVICE_EVENT_LOST;
  dev.EmitEvent(e);
  snd_update(sys);  // no callbacks yet: kept
  Events ev;
  snd_callbacks cb{};
  cb.user = &ev;
  cb.on_error = [](void* u, snd_result r, std::uint32_t d, std::uint64_t o) {
    static_cast<Events*>(u)->errors.push_back(Error{r, d, o});
  };
  SND_CHECK_EQ(snd_set_callbacks(sys, &cb), SND_OK);
  snd_update(sys);
  snd_update(sys);
  std::uint32_t rate = 0, lost = 0;
  for (const Error& x : ev.errors) {
    rate += (x.code == SND_ERR_UNSUPPORTED && x.object == 44100u) ? 1u : 0u;
    lost += (x.code == SND_ERR_NO_DEVICE && x.detail == SND_DEVICE_EVENT_LOST) ? 1u : 0u;
  }
  SND_CHECK_EQ(rate, 1u);
  SND_CHECK_EQ(lost, 1u);
  snd_destroy(sys);
}

SND_TEST(ApiLifetime, BufferedSynthsBeyondTheRingsWaitForOne) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 40;
  cfg.max_virtual_voices = 128;
  cfg.max_playbacks = 128;
  Rig r(cfg);
  SND_REQUIRE_EQ(r.created, SND_OK);
  static const float zero = 0.0f;
  static const float tenth = 0.1f;
  auto play = [&](const float* v) {
    snd_synth_desc d{};
    d.callback = &Dc;
    d.user = const_cast<float*>(v);
    d.channels = 1;
    d.mode = SND_SYNTH_BUFFERED;
    d.volume = 1.0f;
    return snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
  };
  std::vector<snd_playback_handle> first;
  for (int i = 0; i < 32; ++i) first.push_back(play(&zero));  // every ring
  r.Step(4);
  std::vector<snd_playback_handle> extra;
  for (int i = 0; i < 3; ++i) extra.push_back(play(&tenth));
  r.Step(12);
  const std::uint64_t quiet = r.Frames();
  for (std::uint64_t f = quiet - 1024; f < quiet; ++f) SND_CHECK_EQ(r.At(f), 0.0f);  // waiting: silent
  for (const snd_playback_handle& p : extra) SND_CHECK_EQ(snd_get_playback_state(r.sys, p), SND_PLAYBACK_STATE_PLAYING);
  SND_CHECK(r.ev.ends.empty());  // none refused
  for (int i = 0; i < 3; ++i) snd_stop(r.sys, first[i], 0.0f, nullptr);
  r.Step(30);
  // The three waiting synths got the freed rings: 3 x 0.1 on the centre.
  SND_CHECK_NEAR(r.At(r.Frames() - 10), 0.3f, 1e-5f);
  const snd::compose::Parts parts = snd::compose::PartsOf(r.sys);
  SND_REQUIRE(parts.sources != nullptr);
  SND_CHECK(parts.sources->Stats().callbackRejected >= 3u);
  for (const snd_playback_handle& p : first) snd_stop(r.sys, p, 0.0f, nullptr);
  for (const snd_playback_handle& p : extra) snd_stop(r.sys, p, 0.0f, nullptr);
  r.Step(6);
}
