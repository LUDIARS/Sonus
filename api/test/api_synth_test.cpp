// Synth callbacks through the C API (5.2, CONTRACTS.md D-68, D-90, D-94):
//  - snd_render_context.transport_beats counts QUARTER NOTES of the transport the
//    synth follows (the transport of its start When): checked analytically against
//    clock_sample for 4/4 and 6/8 at 120 BPM, after snd_set_tempo and after
//    snd_set_transport_pitch; -1 without a transport;
//  - LowLatency: one call per block, clock_sample = block clocks, FIRST_BLOCK once,
//    the output is exactly what the callback wrote;
//  - Buffered: the render-ahead job's contexts follow the same time line (clock and
//    beats extrapolated), the output arrives after the documented latency;
//  - the C++ wrapper's callback (snd::PlaySynth) gets the same context;
//  - user buffers: a static buffer plays its frames, a stream buffer what was written.
#include <atomic>
#include <cmath>
#include <vector>

#include "api_rig.hpp"

using namespace apitest;

namespace {

struct Call {
  std::uint64_t clock;
  double beats;
  std::uint32_t frames;
  std::uint32_t flags;
};

// Written from the callback (RT worker / audio thread / normal worker): preallocated.
struct Recorder {
  static constexpr std::uint32_t kMax = 4096;
  Call calls[kMax];
  std::atomic<std::uint32_t> count{0};
  float value = 0.25f;  // what the callback writes
  void Add(const snd_render_context& c, std::uint32_t frames) {
    const std::uint32_t i = count.load(std::memory_order_relaxed);
    if (i >= kMax) return;
    calls[i] = Call{c.clock_sample, c.transport_beats, frames, c.flags};
    count.store(i + 1u, std::memory_order_release);
  }
  std::vector<Call> Calls() const {
    const std::uint32_t n = count.load(std::memory_order_acquire);
    return std::vector<Call>(calls, calls + n);
  }
};

void SND_CALL Synth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
                    const snd_render_context* ctx) {
  auto* r = static_cast<Recorder*>(user);
  r->Add(*ctx, frames);
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint32_t f = 0; f < frames; ++f) out[c][f] = r->value;
  }
}

void CppSynth(void* user, float* const* out, std::uint32_t channels, std::uint32_t frames,
              const snd::RenderContext& ctx) {
  Synth(user, out, channels, frames, &ctx);
}

snd_synth_desc Desc(Recorder* rec, snd_synth_mode mode) {
  snd_synth_desc d{};
  d.callback = &Synth;
  d.user = rec;
  d.channels = 1;
  d.mode = mode;
  d.volume = 1.0f;
  return d;
}

snd_when Follow(snd_transport_handle t) {
  snd_when w{};
  w.kind = SND_WHEN_IMMEDIATE;
  w.transport = t;  // D-90: the synth follows this transport (any kind)
  return w;
}

// Quarter notes per sample at `bpm` (D-68: BPM counts quarter notes).
double QuarterPerSample(double bpm) { return bpm / 60.0 / 48000.0; }

}  // namespace

SND_TEST(ApiSynth, TransportBeatsCountQuarterNotesLowLatency) {
  for (const std::uint16_t unit : {std::uint16_t{4}, std::uint16_t{8}}) {  // 4/4 and 6/8
    Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    Recorder rec;
    const snd_transport_desc td{120.0f, static_cast<std::uint16_t>(unit == 4 ? 4 : 6), unit, 1u, 0u};
    const snd_transport_handle t = snd_create_transport(r.sys, &td);
    const snd_synth_desc d = Desc(&rec, SND_SYNTH_LOW_LATENCY);
    const snd_when w = Follow(t);
    const snd_playback_handle p = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, &w);
    SND_REQUIRE(t.value != 0u && p.value != 0u);
    r.Step(40);
    const std::vector<Call> calls = rec.Calls();
    SND_REQUIRE(calls.size() >= 30u);
    // Transport and synth come from the same arena: the transport's tick 0 is the first
    // block of the synth, so transport_beats = (clock - first clock) x 120 / 60 / 48000.
    const std::uint64_t c0 = calls[0].clock;
    SND_CHECK_NEAR(calls[0].beats, 0.0, 1e-12);
    SND_CHECK((calls[0].flags & SND_RENDER_FLAG_FIRST_BLOCK) != 0u);
    for (std::size_t i = 0; i < calls.size(); ++i) {
      const Call& c = calls[i];
      SND_CHECK_EQ(c.frames, 256u);
      SND_CHECK_EQ(c.clock, c0 + 256u * i);  // one call per block, in order
      SND_CHECK((c.flags & SND_RENDER_FLAG_LOW_LATENCY) != 0u);
      if (i > 0) SND_CHECK((c.flags & SND_RENDER_FLAG_FIRST_BLOCK) == 0u);
      // Quarter notes, not beats of the 6/8 eighth (D-68): the same value for both signatures.
      SND_CHECK_NEAR(c.beats, static_cast<double>(c.clock - c0) * QuarterPerSample(120.0), 1e-9);
    }
    // The output is the callback's value (mono at the centre of the stereo master).
    const std::uint64_t at = c0 + 2048;
    SND_CHECK_NEAR(r.At(at), rec.value, 1e-6f);
    snd_stop(r.sys, p, 0.0f, nullptr);
    r.Step(4);
  }
}

SND_TEST(ApiSynth, TransportBeatsFollowTempoAndPitch) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  Recorder rec;
  const snd_transport_desc td{120.0f, 4, 4, 1u, 0u};
  const snd_transport_handle t = snd_create_transport(r.sys, &td);
  const snd_synth_desc d = Desc(&rec, SND_SYNTH_LOW_LATENCY);
  const snd_when w = Follow(t);
  SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, &w).value != 0u);
  r.Step(10);
  SND_CHECK_EQ(snd_set_tempo(r.sys, t, 60.0f, nullptr), SND_OK);
  r.Step(20);
  SND_CHECK_EQ(snd_set_transport_pitch(r.sys, t, 2.0f, nullptr), SND_OK);  // 60 BPM x 2
  r.Step(20);
  const std::vector<Call> calls = rec.Calls();
  SND_REQUIRE(calls.size() >= 45u);
  // Slopes between consecutive blocks: 120 BPM, then 60, then 60 x 2 = 120 again. The
  // change takes effect at a block boundary (reservation "now"); find it from the data.
  std::vector<double> slope;
  for (std::size_t i = 1; i < calls.size(); ++i) {
    slope.push_back((calls[i].beats - calls[i - 1].beats) / static_cast<double>(calls[i].clock - calls[i - 1].clock));
  }
  std::size_t k = 0;
  while (k < slope.size() && std::fabs(slope[k] - QuarterPerSample(120.0)) < 1e-12) ++k;
  SND_REQUIRE(k >= 5u && k < slope.size());
  std::size_t k2 = k;
  // At the block of the change the slope may be mixed (the change lands inside it).
  if (std::fabs(slope[k2] - QuarterPerSample(60.0)) > 1e-12) ++k2;
  std::size_t n60 = 0;
  while (k2 < slope.size() && std::fabs(slope[k2] - QuarterPerSample(60.0)) < 1e-12) {
    ++k2;
    ++n60;
  }
  SND_CHECK(n60 >= 10u);
  if (k2 < slope.size() && std::fabs(slope[k2] - QuarterPerSample(120.0)) > 1e-12) ++k2;
  std::size_t nPitch = 0;
  while (k2 < slope.size() && std::fabs(slope[k2] - QuarterPerSample(120.0)) < 1e-12) {
    ++k2;
    ++nPitch;
  }
  SND_CHECK(nPitch >= 10u);
  SND_CHECK_EQ(k2, slope.size());  // nothing else
  // The transport's own position agrees (snd_get_transport_position).
  snd_update(r.sys);
  snd_position pos{};
  SND_CHECK_EQ(snd_get_transport_position(r.sys, t, &pos), SND_OK);
  SND_CHECK_EQ(pos.has_musical, 1u);
}

SND_TEST(ApiSynth, NoTransportMeansMinusOne) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  Recorder rec;
  const snd_synth_desc d = Desc(&rec, SND_SYNTH_LOW_LATENCY);
  SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr).value != 0u);
  r.Step(8);
  const std::vector<Call> calls = rec.Calls();
  SND_REQUIRE(calls.size() >= 4u);
  for (const Call& c : calls) SND_CHECK_EQ(c.beats, -1.0);
}

SND_TEST(ApiSynth, TransportDestroyedUnderTheSynth) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  Recorder rec;
  const snd_transport_desc td{120.0f, 4, 4, 1u, 0u};
  const snd_transport_handle t = snd_create_transport(r.sys, &td);
  const snd_synth_desc d = Desc(&rec, SND_SYNTH_LOW_LATENCY);
  const snd_when w = Follow(t);
  SND_REQUIRE(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, &w).value != 0u);
  r.Step(8);
  SND_CHECK_EQ(snd_destroy_transport(r.sys, t), SND_OK);
  r.Step(12);
  const std::vector<Call> calls = rec.Calls();
  SND_REQUIRE(calls.size() >= 16u);
  SND_CHECK(calls[2].beats > 0.0);
  SND_CHECK_EQ(calls.back().beats, -1.0);  // the transport is gone: no position
}

SND_TEST(ApiSynth, BufferedRendersAheadOnTheSameTimeLine) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  Recorder rec;
  rec.value = 0.5f;
  const snd_transport_desc td{120.0f, 4, 4, 1u, 0u};
  const snd_transport_handle t = snd_create_transport(r.sys, &td);
  snd_synth_desc d = Desc(&rec, SND_SYNTH_BUFFERED);
  d.ahead_blocks = 2;
  const snd_when w = Follow(t);
  const snd_playback_handle p = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, &w);
  SND_REQUIRE(p.value != 0u);
  r.Step(60);
  const std::vector<Call> calls = rec.Calls();
  SND_REQUIRE(calls.size() >= 20u);
  SND_CHECK((calls[0].flags & SND_RENDER_FLAG_FIRST_BLOCK) != 0u);
  SND_CHECK((calls[0].flags & SND_RENDER_FLAG_LOW_LATENCY) == 0u);
  // Contiguous frames: each call starts where the previous one ended (voice time).
  for (std::size_t i = 1; i < calls.size(); ++i) {
    SND_CHECK_EQ(calls[i].clock, calls[i - 1].clock + calls[i - 1].frames);
    SND_CHECK((calls[i].flags & SND_RENDER_FLAG_FIRST_BLOCK) == 0u);
  }
  // Beats on the same time line: (clock - transport start) x quarters per sample. The
  // synth's first block is the transport's first block (same arena); the job extrapolates
  // from the last rendered block at its tempo (constant here: exact).
  std::uint64_t first = 0;
  for (std::uint64_t c = 0; c < r.Frames(); ++c) {
    if (std::fabs(r.At(c)) > 1e-6f) {
      first = c;
      break;
    }
  }
  SND_REQUIRE(first != 0u);
  const double bps = QuarterPerSample(120.0);
  const double origin = static_cast<double>(calls[0].clock) - calls[0].beats / bps;  // transport tick 0
  for (const Call& c : calls) SND_CHECK_NEAR(c.beats, (static_cast<double>(c.clock) - origin) * bps, 1e-9);
  // The first block of a buffered voice is silent (5.2: nothing rendered ahead yet), the
  // output then starts at a block boundary with exactly the callback's value.
  SND_CHECK(first >= static_cast<std::uint64_t>(origin) + 256u);
  SND_CHECK_EQ(first % 256u, 0u);
  // Every sample is the callback's value or, after an underrun (the job came too late on
  // a loaded machine; 5.2 fills with silence), 0 - never anything else.
  snd::compose::Parts parts = snd::compose::PartsOf(r.sys);
  SND_REQUIRE(parts.sources != nullptr);
  const bool underran = parts.sources->Stats().callbackUnderruns != 0u;
  std::uint64_t value = 0;
  for (std::uint64_t c = first; c < first + 4096; ++c) {
    const float x = r.At(c);
    if (!underran) SND_CHECK_NEAR(x, rec.value, 1e-6f);
    SND_CHECK(std::fabs(x - rec.value) < 1e-6f || std::fabs(x) < 1e-6f);
    value += std::fabs(x - rec.value) < 1e-6f ? 1u : 0u;
  }
  SND_CHECK(value >= 2048u);
  // The rendered frames are the ones the callback produced for that clock: the frame heard
  // at `first` was rendered by the call whose range holds `first` (lockstep, D-94).
  bool covered = false;
  for (const Call& c : calls) covered = covered || (c.clock <= first && first < c.clock + c.frames);
  SND_CHECK(covered);
  SND_CHECK_EQ(parts.sources->Stats().callbackRejected, 0u);
}

SND_TEST(ApiSynth, CppWrapperCallbackGetsTheContext) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  Recorder rec;
  rec.value = 0.125f;
  snd::SynthDesc d;
  d.callback = &CppSynth;
  d.user = &rec;
  d.mode = snd::SynthMode::LowLatency;
  const snd::PlaybackHandle p = snd::PlaySynth(r.sys, d);
  SND_REQUIRE(p.IsValid());
  r.Step(10);
  const std::vector<Call> calls = rec.Calls();
  SND_REQUIRE(calls.size() >= 6u);
  SND_CHECK_EQ(calls[0].beats, -1.0);
  SND_CHECK_NEAR(r.At(calls[2].clock + 10), 0.125f, 1e-6f);
}

SND_TEST(ApiSynth, UserBuffersPlayTheGameMemory) {
  Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  // Static: 1000 frames of a ramp, played once at 48 kHz (ratio 1).
  std::vector<float> ramp(1000);
  for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = 0.3f;
  float* chans[1] = {ramp.data()};
  snd_user_buffer_desc sd{chans, 1, 1000, 48000, SND_USER_BUFFER_STATIC, 0, 0};
  const snd_user_buffer_handle sb = snd_create_user_buffer(r.sys, &sd);
  SND_REQUIRE(sb.value != 0u);
  const snd_playback_handle sp = snd_play_user_buffer(r.sys, sb, 0u, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(sp.value != 0u);
  r.Step(30);
  // 1000 frames of 0.3 somewhere in the capture (the sinc resampler at ratio 1 passes DC).
  std::uint64_t on = 0, n = 0;
  for (std::uint64_t c = 0; c < r.Frames(); ++c) {
    if (std::fabs(r.At(c) - 0.3f) < 1e-3f) {
      if (n == 0) on = c;
      ++n;
    }
  }
  SND_CHECK(n >= 980u && n <= 1000u);
  SND_CHECK(on != 0u);
  bool ended = false;
  for (const End& e : r.ev.ends) ended = ended || (e.handle == sp.value && e.reason == SND_END_COMPLETED);
  SND_CHECK(ended);
  // Stream: the game writes 512 frames of 0.2; exactly those are heard, then silence.
  std::vector<float> ring(2048, 0.0f);
  float* rc[1] = {ring.data()};
  snd_user_buffer_desc qd{rc, 1, 2048, 48000, SND_USER_BUFFER_STREAM, 0, 0};
  const snd_user_buffer_handle qb = snd_create_user_buffer(r.sys, &qd);
  SND_REQUIRE(qb.value != 0u);
  std::vector<float> data(512, 0.2f);
  const float* src[1] = {data.data()};
  SND_CHECK_EQ(snd_write_user_buffer(r.sys, qb, src, 512), 512u);
  const std::uint64_t from = r.Frames();
  const snd_playback_handle qp = snd_play_user_buffer(r.sys, qb, 0u, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(qp.value != 0u);
  r.Step(30);
  SND_CHECK_EQ(snd_get_playback_state(r.sys, qp), SND_PLAYBACK_STATE_PLAYING);  // a stream never ends by itself
  SND_CHECK_EQ(snd_stop(r.sys, qp, 0.0f, nullptr), SND_OK);
  std::uint64_t heard = 0;
  for (std::uint64_t c = from; c < r.Frames(); ++c) heard += std::fabs(r.At(c) - 0.2f) < 1e-3f ? 1u : 0u;
  SND_CHECK(heard >= 490u && heard <= 512u);
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, sb), SND_OK);
  SND_CHECK_EQ(snd_destroy_user_buffer(r.sys, qb), SND_OK);
  for (int i = 0; i < 40 && (snd_get_user_buffer_released(r.sys, sb) == 0 || snd_get_user_buffer_released(r.sys, qb) == 0); ++i) {
    r.Step();
  }
  SND_CHECK_EQ(snd_get_user_buffer_released(r.sys, sb), 1);
  SND_CHECK_EQ(snd_get_user_buffer_released(r.sys, qb), 1);
}
