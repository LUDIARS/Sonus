// Independent tests: synth callbacks, transport_beats and user buffers through the C
// API (design.md 5.1, 5.2; CONTRACTS.md D-68, D-88, D-90, D-94).
// Properties (from the spec, before reading the implementation):
//  C2 transport_beats counts quarter notes: at bpm B it grows by B/60/sr per sample,
//     whatever the signature (6/8 included).
//  C2' a synth started with NEXT_BAR on a transport begins exactly on a bar line: its
//     first context has clock_sample = the bar's sample and transport_beats = 4k (4/4).
//  C3 snd_set_transport_pitch multiplies the slope; values are clamped to [1/16, 16].
//  C3' a STOPPED transport has a position (0) but does not advance: transport_beats is
//     constant 0, not -1; after PLAYING it starts at 0 and grows.
//  F1 Buffered: frame f of the voice is heard f frames after its start (lockstep), the
//     first block is silent: every heard sample equals what the callback produced for
//     that common-clock sample, or 0.
//  F2 Buffered rings: min(max_real_voices, 32); a start beyond them ends with
//     SND_END_DETAIL_VOICE_LIMIT, the others play.
//  F3 LowLatency: one call per block with the block's clock, FIRST_BLOCK once.
//  F4 UserBuffer Stream: one playback per stream buffer at a time (second: VOICE_LIMIT);
//     what is heard is exactly what was written, in order, then silence (never ends);
//     destroying the buffer ends a playing voice within a block and releases it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>

#include "indep_rig.hpp"

using apitest::kCenter;
using apitest::SmallConfig;

namespace {

struct Ctx {
  std::uint64_t clock;
  double beats;
  std::uint32_t frames;
  std::uint32_t flags;
};

struct Log {
  static constexpr std::uint32_t kMax = 8192;
  Ctx calls[kMax];
  std::atomic<std::uint32_t> count{0};
  void Add(const snd_render_context& c, std::uint32_t frames) {
    const std::uint32_t i = count.load(std::memory_order_relaxed);
    if (i >= kMax) return;
    calls[i] = Ctx{c.clock_sample, c.transport_beats, frames, c.flags};
    count.store(i + 1u, std::memory_order_release);
  }
  std::vector<Ctx> Calls() const {
    const std::uint32_t n = count.load(std::memory_order_acquire);
    return std::vector<Ctx>(calls, calls + n);
  }
};

// Value the ramp synth writes for common-clock sample c: distinct for every sample of a
// period of 1000 (a lockstep error of any number of frames below 1000 changes it).
float RampAt(std::uint64_t c) { return 0.1f + 0.0008f * static_cast<float>(c % 1000u); }

void SND_CALL RampSynth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                        const snd_render_context* ctx) {
  static_cast<Log*>(user)->Add(*ctx, frames);
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = RampAt(ctx->clock_sample + f);
  }
}

void SND_CALL LogSynth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                       const snd_render_context* ctx) {
  static_cast<Log*>(user)->Add(*ctx, frames);
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = 0.25f;
  }
}

snd_synth_desc Desc(snd_synth_callback cb, Log* log, snd_synth_mode mode) {
  snd_synth_desc d{};
  d.callback = cb;
  d.user = log;
  d.channels = 1;
  d.mode = mode;
  d.volume = 1.0f;
  return d;
}

snd_when On(snd_transport_handle t, snd_when_kind kind = SND_WHEN_IMMEDIATE) {
  snd_when w{};
  w.kind = static_cast<std::uint8_t>(kind);
  w.transport = t;
  return w;
}

double Qps(double bpm) { return bpm / 60.0 / 48000.0; }

}  // namespace

// C2 for 6/8 at 90 BPM, LowLatency and Buffered: the slope between any two contexts.
SND_TEST(IndepSynth, QuarterNotesInSixEightAtNinety) {
  for (const snd_synth_mode mode : {SND_SYNTH_LOW_LATENCY, SND_SYNTH_BUFFERED}) {
    indep::Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    Log log;
    const snd_transport_desc td{90.0f, 6, 8, 1, 0};
    const snd_transport_handle t = snd_create_transport(r.sys, &td);
    const snd_synth_desc d = Desc(&LogSynth, &log, mode);
    const snd_when w = On(t);
    SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, &w).value != 0u);
    r.Step(40);
    const std::vector<Ctx> c = log.Calls();
    SND_REQUIRE(c.size() >= 20u);
    for (std::size_t i = 1; i < c.size(); ++i) {
      SND_REQUIRE(c[i].beats >= 0.0);
      const double slope = (c[i].beats - c[0].beats) / static_cast<double>(c[i].clock - c[0].clock);
      SND_CHECK_NEAR(slope, Qps(90.0), 1e-12);
    }
  }
}

// C2': NEXT_BAR start lands on the bar line with an integral number of bars of quarters.
SND_TEST(IndepSynth, NextBarStartIsOnTheBarLine) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  Log log;
  const snd_transport_desc td{120.0f, 4, 4, 1, 0};  // a bar = 96000 samples
  const snd_transport_handle t = snd_create_transport(r.sys, &td);
  r.Step(30);  // the transport runs: the next bar line is not at the current block
  const snd_synth_desc d = Desc(&LogSynth, &log, SND_SYNTH_LOW_LATENCY);
  const snd_when w = On(t, SND_WHEN_NEXT_BAR);
  const snd_playback_handle p = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, &w);
  SND_REQUIRE(p.value != 0u);
  r.Step(400);  // > one bar
  const std::vector<Ctx> c = log.Calls();
  SND_REQUIRE(!c.empty());
  // On the transport's own line: its origin is where beats = 0; the bar line is a multiple of 4.
  const double bars = c[0].beats / 4.0;
  SND_CHECK_MSG(std::fabs(bars - std::round(bars)) < 1e-6, "first context at %.12f quarter notes", c[0].beats);
  SND_CHECK(c[0].beats >= 4.0 - 1e-6);
  // The first call renders from the bar line (frames < block: it starts inside a block) and
  // what is heard starts there: silence before, the value from that sample on.
  const std::uint64_t bar = c[0].clock;
  SND_CHECK(r.Raw(bar - 1) == 0.0f);
  SND_CHECK_NEAR(r.Raw(bar) / kCenter, 0.25f, 1e-6f);
  // The bar line agrees with on_beat (bar start: beat 0).
  bool beatAtBar = false;
  for (const apitest::Beat& b : r.ev.beats) beatAtBar = beatAtBar || (b.transport == t.value && b.beat == 0 && b.clock == bar);
  SND_CHECK(beatAtBar);
}

// C3: the pitch multiplies the slope and is clamped to [1/16, 16].
SND_TEST(IndepSynth, TransportPitchIsClamped) {
  struct Case {
    float pitch;
    double factor;
  };
  const Case cases[] = {{100.0f, 16.0}, {16.0f, 16.0}, {0.5f, 0.5}, {1e-4f, 1.0 / 16.0}};
  for (const Case& k : cases) {
    indep::Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    Log log;
    const snd_transport_desc td{120.0f, 4, 4, 1, 0};
    const snd_transport_handle t = snd_create_transport(r.sys, &td);
    SND_CHECK_EQ(snd_set_transport_pitch(r.sys, t, k.pitch, nullptr), SND_OK);  // same arena as the create
    const snd_synth_desc d = Desc(&LogSynth, &log, SND_SYNTH_LOW_LATENCY);
    const snd_when w = On(t);
    SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, &w).value != 0u);
    r.Step(20);
    const std::vector<Ctx> c = log.Calls();
    SND_REQUIRE(c.size() >= 10u);
    // After the first block the slope is the clamped one.
    const double slope = (c.back().beats - c[2].beats) / static_cast<double>(c.back().clock - c[2].clock);
    SND_CHECK_MSG(std::fabs(slope - Qps(120.0) * k.factor) < 1e-12, "pitch %g: slope %g, want %g",
                  static_cast<double>(k.pitch), slope, Qps(120.0) * k.factor);
  }
}

// C3': a stopped transport is at 0 and stays there; after PLAYING it advances from 0.
SND_TEST(IndepSynth, StoppedTransportHoldsZero) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  Log log;
  const snd_transport_desc td{120.0f, 4, 4, 0, 0};  // Stopped
  const snd_transport_handle t = snd_create_transport(r.sys, &td);
  const snd_synth_desc d = Desc(&LogSynth, &log, SND_SYNTH_LOW_LATENCY);
  const snd_when w = On(t);
  SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, &w).value != 0u);
  r.Step(10);
  const std::uint32_t stoppedCalls = log.count.load();
  SND_REQUIRE(stoppedCalls >= 5u);
  for (const Ctx& c : log.Calls()) SND_CHECK_EQ(c.beats, 0.0);
  SND_CHECK_EQ(snd_set_transport_state(r.sys, t, SND_TRANSPORT_PLAYING, nullptr), SND_OK);
  r.Step(20);
  const std::vector<Ctx> c = log.Calls();
  std::size_t k = stoppedCalls;
  while (k < c.size() && c[k].beats == 0.0) ++k;
  SND_REQUIRE(k < c.size() && k > 0);
  // From the start point (the last zero context, possibly mid-block) on: the 120 BPM slope.
  for (std::size_t i = k + 1; i < c.size(); ++i) {
    const double slope = (c[i].beats - c[k].beats) / static_cast<double>(c[i].clock - c[k].clock);
    SND_CHECK_NEAR(slope, Qps(120.0), 1e-12);
  }
  SND_CHECK(c[k].beats <= Qps(120.0) * 256.0 + 1e-9);  // started at 0 within this block
}

// F1: Buffered lockstep with a ramp (a constant cannot show an offset).
SND_TEST(IndepSynth, BufferedIsInLockstepWithTheClock) {
  for (const std::uint32_t ahead : {1u, 3u, 6u}) {
    indep::Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    Log log;
    snd_synth_desc d = Desc(&RampSynth, &log, SND_SYNTH_BUFFERED);
    d.ahead_blocks = ahead;
    const std::uint64_t startBlock = r.RenderClock();
    const snd_playback_handle p = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
    SND_REQUIRE(p.value != 0u);
    r.Step(60);
    std::uint64_t heard = 0, wrong = 0, firstHeard = UINT64_MAX;
    for (std::uint64_t c = 0; c < r.Frames(); ++c) {
      const float x = r.Raw(c) / kCenter;
      if (std::fabs(x) < 1e-7f) continue;
      if (firstHeard == UINT64_MAX) firstHeard = c;
      ++heard;
      if (std::fabs(x - RampAt(c)) > 1e-5f) {
        if (wrong < 3u) snd::test::Note("ahead %u: at %llu heard %g, the callback wrote %g for it", ahead,
                                        static_cast<unsigned long long>(c), static_cast<double>(x),
                                        static_cast<double>(RampAt(c)));
        ++wrong;
      }
    }
    SND_CHECK_EQ(wrong, 0u);
    SND_CHECK(heard >= 40u * 256u);
    // The first block of the voice is silent (nothing could be rendered ahead of it).
    SND_CHECK(firstHeard >= startBlock + 256u);
    // The job's contexts never overlap and cover a contiguous range (frames are not repeated).
    const std::vector<Ctx> c = log.Calls();
    for (std::size_t i = 1; i < c.size(); ++i) SND_CHECK(c[i].clock >= c[i - 1].clock + c[i - 1].frames);
    SND_CHECK_EQ(c.empty() ? 0u : (c[0].flags & SND_RENDER_FLAG_FIRST_BLOCK), SND_RENDER_FLAG_FIRST_BLOCK);
    snd_stop(r.sys, p, 0.0f, nullptr);
    r.Step(4);
    SND_CHECK_EQ(r.EndsOf(p.value), 1u);
  }
}

// F3: LowLatency, same ramp: exact output from the first block on.
SND_TEST(IndepSynth, LowLatencyOutputIsExact) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  Log log;
  const snd_synth_desc d = Desc(&RampSynth, &log, SND_SYNTH_LOW_LATENCY);
  SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr).value != 0u);
  r.Step(20);
  const std::vector<Ctx> c = log.Calls();
  SND_REQUIRE(c.size() >= 10u);
  for (std::size_t i = 0; i + 1 < c.size(); ++i) {
    SND_CHECK_EQ(c[i + 1].clock, c[i].clock + 256u);
    SND_CHECK_EQ(c[i].frames, 256u);
    SND_CHECK_EQ((c[i].flags & SND_RENDER_FLAG_FIRST_BLOCK) != 0u, i == 0);
  }
  for (std::uint64_t s = c[0].clock; s < c[0].clock + 2560; ++s) {
    SND_CHECK_NEAR(r.Raw(s) / kCenter, RampAt(s), 1e-5f);
  }
}

// F2 (D-97, accepted decision): rings = min(max_real_voices, 32). With 40 real voices, 35
// buffered synths: 32 get a ring and sound, 3 are not refused but wait in silence; when
// rings come free (3 synths stopped), the waiting ones get them and sound. Each synth
// writes 0.01, so the output counts the sounding synths: n x 0.01 x centre pan.
SND_TEST(IndepSynth, BufferedRingLimit) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 40;
  cfg.max_virtual_voices = 128;
  cfg.max_playbacks = 128;
  indep::Rig r(cfg);
  SND_REQUIRE_EQ(r.created, SND_OK);
  static const float kOne = 0.01f;
  std::vector<snd_playback_handle> ps;
  const snd_synth_desc d = indep::DcDesc(&kOne, SND_SYNTH_BUFFERED);
  for (std::uint32_t i = 0; i < 35u; ++i) ps.push_back(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr));
  for (const snd_playback_handle& p : ps) SND_REQUIRE(p.value != 0u);
  // Sounding synths, from the last captured frame (all synths write a constant).
  auto sounding = [&] { return static_cast<int>(std::lround(r.Raw(r.Frames() - 1) / kCenter / kOne)); };
  // Wait by quantity: until 32 sound (the render-ahead jobs have filled their rings).
  int n = 0;
  for (int i = 0; i < 2000 && (n = sounding()) != 32; ++i) r.Step(1);
  SND_CHECK_EQ(n, 32);
  r.Step(20);
  SND_CHECK_EQ(sounding(), 32);  // not more: 3 still wait
  SND_CHECK(r.ev.ends.empty());  // none refused (D-97)
  // Stop 3 synths: rings come free, the 3 waiting ones take them: 32 again.
  for (int k = 0; k < 3; ++k) SND_CHECK_EQ(snd_stop(r.sys, ps[static_cast<std::size_t>(k)], 0.0f, nullptr), SND_OK);
  bool dipped = false, back = false;
  for (int i = 0; i < 2000 && !back; ++i) {
    r.Step(1);
    const int m = sounding();
    dipped = dipped || m < 32;
    back = dipped && m == 32;
  }
  SND_CHECK(back);
  for (int k = 0; k < 3; ++k) SND_CHECK_EQ(r.EndsOf(ps[static_cast<std::size_t>(k)].value), 1u);
  // Stop 10 more: 22 sound (no one is waiting any more).
  for (int k = 3; k < 13; ++k) snd_stop(r.sys, ps[static_cast<std::size_t>(k)], 0.0f, nullptr);
  for (int i = 0; i < 2000 && (n = sounding()) != 22; ++i) r.Step(1);
  SND_CHECK_EQ(n, 22);
  r.Step(10);
  SND_CHECK_EQ(sounding(), 22);
  for (const snd_playback_handle& p : ps) snd_stop(r.sys, p, 0.0f, nullptr);
  for (int i = 0; i < 200 && r.ev.ends.size() < ps.size(); ++i) r.Step(1);
  for (const snd_playback_handle& p : ps) SND_CHECK_EQ(r.EndsOf(p.value), 1u);
  snd::compose::Parts parts = snd::compose::PartsOf(r.sys);
  SND_REQUIRE(parts.sources != nullptr);
  SND_CHECK_EQ(parts.sources->Stats().callbackRejected, 3u);  // the 3 that had to wait at their start
}

// F4: stream user buffer.
SND_TEST(IndepSynth, StreamUserBufferOrderOneReaderAndRelease) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  std::vector<float> mem(1000, 0.0f);
  float* chans[1] = {mem.data()};
  const snd_user_buffer_desc bd{chans, 1, 1000, 0, SND_USER_BUFFER_STREAM, 0, 0};
  const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &bd);
  SND_REQUIRE(b.value != 0u);
  // Write a ramp of 3000 frames over time (the ring holds 1000): value k for frame k.
  auto value = [](std::uint64_t k) { return 0.05f + 0.0002f * static_cast<float>(k % 3000u); };
  std::uint64_t written = 0;
  auto feed = [&] {
    float tmp[512];
    std::uint32_t n = 0;
    for (; n < 512 && written + n < 3000u; ++n) tmp[n] = value(written + n);
    const float* src[1] = {tmp};
    written += snd_write_user_buffer(r.sys, b, src, n);
  };
  feed();
  const snd_playback_handle p1 = snd_play_user_buffer(r.sys, b, 0u, snd_emitter_handle{0}, nullptr);
  const snd_playback_handle p2 = snd_play_user_buffer(r.sys, b, 0u, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p1.value != 0u && p2.value != 0u);
  for (int i = 0; i < 60; ++i) {
    feed();
    r.Step(1);
  }
  SND_CHECK_EQ(written, 3000u);
  SND_CHECK_EQ(r.EndsOf(p1.value), 0u);  // a stream never ends by itself
  bool p2Limit = false;
  for (const apitest::End& e : r.ev.ends) p2Limit = p2Limit || (e.handle == p2.value && e.detail == SND_END_DETAIL_VOICE_LIMIT);
  SND_CHECK(p2Limit);
  // What was heard: the written frames in order (ratio 1: the resampler passes them; allow
  // its tiny error), each once, then silence.
  std::vector<float> heard;
  for (std::uint64_t c = 0; c < r.Frames(); ++c) {
    const float x = r.Raw(c) / kCenter;
    if (std::fabs(x) > 1e-4f) heard.push_back(x);
  }
  SND_CHECK_MSG(heard.size() >= 2990u && heard.size() <= 3032u, "heard %llu frames",
                static_cast<unsigned long long>(heard.size()));
  // Align the heard run on the written frames (the resampler may add a few frames of
  // transient at the edges), then compare frame by frame in the middle.
  int best = 0;
  double bestErr = 1e30;
  for (int o = -8; o <= 8; ++o) {
    double e = 0;
    for (std::size_t i = 100; i < 2900 && i < heard.size(); ++i) {
      const long k = static_cast<long>(i) + o;
      if (k < 0) continue;
      e += std::fabs(heard[i] - value(static_cast<std::uint64_t>(k)));
    }
    if (e < bestErr) {
      bestErr = e;
      best = o;
    }
  }
  std::uint32_t bad = 0;
  for (std::size_t i = 32; i < 2960 && i < heard.size(); ++i) {
    if (std::fabs(heard[i] - value(static_cast<std::uint64_t>(static_cast<long>(i) + best))) > 1e-3f) ++bad;
  }
  SND_CHECK_MSG(bad == 0u, "%u frames differ (offset %d)", bad, best);
  // Stop, destroy: released.
  SND_CHECK_EQ(snd_stop(r.sys, p1, 0.0f, nullptr), SND_OK);
  for (int i = 0; i < 10 && r.EndsOf(p1.value) == 0u; ++i) r.Step(1);
  SND_CHECK_EQ(r.EndsOf(p1.value), 1u);
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, b), SND_OK);
  for (int i = 0; i < 20 && snd_get_user_buffer_released(r.sys, b) == 0; ++i) r.Step(1);
  SND_CHECK_EQ(snd_get_user_buffer_released(r.sys, b), 1);
  // A second play of a stream buffer works once the first reader is gone.
  const snd_user_buffer_handle b2 = snd_create_user_buffer(r.sys, &bd);
  SND_REQUIRE(b2.value != 0u);
  const snd_playback_handle q1 = snd_play_user_buffer(r.sys, b2, 0u, snd_emitter_handle{0}, nullptr);
  r.Step(3);
  SND_CHECK_EQ(snd_stop(r.sys, q1, 0.0f, nullptr), SND_OK);
  r.Step(3);
  const snd_playback_handle q2 = snd_play_user_buffer(r.sys, b2, 0u, snd_emitter_handle{0}, nullptr);
  r.Step(3);
  SND_CHECK_EQ(r.EndsOf(q2.value), 0u);
  SND_CHECK_EQ(snd_get_playback_state(r.sys, q2), SND_PLAYBACK_STATE_PLAYING);
  snd_stop(r.sys, q2, 0.0f, nullptr);
  r.Step(3);
}

// F4 (concurrency): the game writes a stream buffer from another (unregistered) thread
// while the audio thread reads it: every frame is heard once and in order.
SND_TEST(IndepSynth, StreamUserBufferWrittenFromAnotherThread) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  r.settle = false;
  std::vector<float> mem(2047, 0.0f);  // not a power of two
  float* chans[1] = {mem.data()};
  const snd_user_buffer_desc bd{chans, 1, 2047, 0, SND_USER_BUFFER_STREAM, 0, 0};
  const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &bd);
  SND_REQUIRE(b.value != 0u);
  const std::uint64_t total = 20000;
  auto value = [](std::uint64_t k) { return 0.05f + 0.00004f * static_cast<float>(k % 20000u); };  // increasing
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> done{0};
  std::thread writer([&] {  // an unregistered thread: snd_write_user_buffer is "any"
    std::uint64_t w = 0;
    float tmp[97];
    while (w < total && !stop.load()) {
      std::uint32_t n = 0;
      for (; n < 97 && w + n < total; ++n) tmp[n] = value(w + n);
      const float* src[1] = {tmp};
      w += snd_write_user_buffer(r.sys, b, src, n);
      done.store(w);
      std::this_thread::yield();
    }
  });
  // The reader never starves while frames remain: before every device period, the writer
  // is at least 1536 frames (the render-ahead and one period, with room) past what has been
  // heard. Waiting by quantity: no assumption on how fast either thread runs.
  auto waitAhead = [&](std::uint64_t heard) {
    const std::uint64_t want = std::min<std::uint64_t>(total, heard + 1536u);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (done.load() < want && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    return done.load() >= want;
  };
  SND_REQUIRE(waitAhead(0));
  const snd_playback_handle p = snd_play_user_buffer(r.sys, b, 0u, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  std::uint64_t scanned = 0, heardSoFar = 0;
  bool ahead = true;
  while (heardSoFar + 16u < total) {
    ahead = ahead && waitAhead(heardSoFar);
    if (!ahead) break;
    r.Step(1);
    for (; scanned < r.Frames(); ++scanned) heardSoFar += std::fabs(r.Raw(scanned)) >= 1e-4f ? 1u : 0u;
  }
  SND_CHECK(ahead);
  r.Step(10);
  stop.store(true);
  writer.join();
  // One run of non-silent frames (no starvation before the end: it would leave a gap). Away from its edges (resampler transients) the ramp only
  // goes up, so no frame repeats or goes back, and the run has the frames written.
  std::uint64_t runs = 0, back = 0, sum = 0;
  std::uint64_t c = 0;
  while (c < r.Frames()) {
    while (c < r.Frames() && std::fabs(r.Raw(c)) < 1e-4f) ++c;
    const std::uint64_t a = c;
    while (c < r.Frames() && std::fabs(r.Raw(c)) >= 1e-4f) ++c;
    if (c == a) break;
    ++runs;
    sum += c - a;
    for (std::uint64_t i = a + 33; i + 32 < c; ++i) back += r.Raw(i) + 1e-5f < r.Raw(i - 1) ? 1u : 0u;
  }
  SND_CHECK_EQ(runs, 1u);
  SND_CHECK_EQ(back, 0u);
  SND_CHECK_MSG(sum + 16u >= total && sum <= total + 32u, "heard %llu frames", static_cast<unsigned long long>(sum));
  snd_stop(r.sys, p, 0.0f, nullptr);
  r.Step(3);
}

// F4 / D-94 "バッファが解放されたら次のブロックで終わる", sonus.h snd_destroy_user_buffer
// ("the game may free the memory after the next Update in which released returns 1"):
// destroying a buffer that a stream (or looping static) playback is reading ends that
// playback and releases the buffer. A stream never ends by itself, so if the release
// waited for the playback, neither would ever happen.
SND_TEST(IndepSynth, Defect_DestroyingAPlayingUserBufferEndsItsPlaybackAndReleasesIt) {
  for (const snd_user_buffer_mode mode : {SND_USER_BUFFER_STREAM, SND_USER_BUFFER_STATIC}) {
    indep::Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    std::vector<float> mem(1000, 0.25f);
    float* chans[1] = {mem.data()};
    const snd_user_buffer_desc bd{chans, 1, 1000, 0, mode, mode == SND_USER_BUFFER_STATIC ? 1u : 0u, 0};
    const snd_user_buffer_handle b = snd_create_user_buffer(r.sys, &bd);
    SND_REQUIRE(b.value != 0u);
    const float* src[1] = {mem.data()};
    snd_write_user_buffer(r.sys, b, src, 1000);
    const snd_playback_handle p = snd_play_user_buffer(r.sys, b, 0u, snd_emitter_handle{0}, nullptr);
    SND_REQUIRE(p.value != 0u);
    r.Step(8);
    SND_REQUIRE_EQ(r.EndsOf(p.value), 0u);
    SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, b), SND_OK);
    r.Step(40);
    SND_CHECK_MSG(r.EndsOf(p.value) == 1u, "mode %u: the playback did not end after the destroy", mode);
    SND_CHECK_MSG(snd_get_user_buffer_released(r.sys, b) == 1, "mode %u: the buffer is never released", mode);
    snd_stop(r.sys, p, 0.0f, nullptr);  // so that the rig can end
    r.Step(4);
  }
}
