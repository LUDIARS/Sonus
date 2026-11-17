// Independent re-check of a3dfbb6 (second round of stage-2 fixes):
//   1. a re-registering thread takes its retiring producer back: per-thread
//      order with several threads registering / unregistering while the owner
//      publishes retired producers (run it under TSan);
//   2. a timed stop of a paused (or pausing) playback ends silently; the edges:
//      X inside the pause fade, right after the pause, right after the resume;
//   3. D-15: every short fade lasts exactly shortFadeFrames per sample, across
//      blocks (steal fade-out, return from virtual, unload discard, timed stop),
//      for block sizes 64 / 256 / 1024 and 44.1 / 48 / 96 kHz; overlapping a
//      steal fade with a stop, an unload or another steal leaves no step.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "indep_fixture.hpp"

using namespace indep;
using snd::HashId;

namespace {

std::size_t ShortFade(std::uint32_t rate) { return static_cast<std::size_t>(std::lround(0.005 * double(rate))); }

snd_config Cfg(std::uint32_t block, std::uint32_t rate, std::uint32_t maxReal = 8) {
  snd_config c = Config();
  c.block_size = block;
  c.sample_rate = rate;
  c.max_real_voices = maxReal;
  c.tuning.lod_down_blocks = 0;
  return c;
}

// Frames in [from, to) whose value lies strictly between lo and hi (exclusive, eps).
std::size_t Between(const std::vector<float>& r, std::size_t from, std::size_t to, float lo, float hi) {
  std::size_t n = 0;
  const float eps = (hi - lo) * 1e-3f;
  for (std::size_t f = from; f < to && f < r.size(); ++f) n += (r[f] > lo + eps && r[f] < hi - eps) ? 1u : 0u;
  return n;
}
bool Monotone(const std::vector<float>& r, std::size_t from, std::size_t to, bool down) {
  for (std::size_t f = from + 1; f < to && f < r.size(); ++f) {
    if (down ? r[f] > r[f - 1] + 1e-6f : r[f] < r[f - 1] - 1e-6f) return false;
  }
  return true;
}
float MaxStep(const std::vector<float>& r, std::size_t from, std::size_t to) {
  float m = 0.0f;
  for (std::size_t f = from + 1; f < to && f < r.size(); ++f) m = std::fabs(r[f] - r[f - 1]) > m ? std::fabs(r[f] - r[f - 1]) : m;
  return m;
}
std::vector<float> Left(const Fx& fx) {
  std::vector<float> v(fx.Total());
  for (std::size_t f = 0; f < v.size(); ++f) v[f] = fx.At(f);
  return v;
}

}  // namespace

// 1. Three threads loop: Play, a random number of Flush (0..3), Unregister,
// Register again, Stop of that Play, sometimes a Flush. The owner keeps
// rendering and publishing retired producers. Every Play must be stopped (the
// Stop is recorded after it on the same thread) and the output silent at the end.
SND_TEST(IndepFix2, ReRegisteringThreadsKeepTheirOrder) {
  snd_config cfg = Config();
  cfg.max_registered_threads = 4;  // owner + 3: retired producers compete for numbers
  cfg.max_playbacks = 512;
  cfg.max_virtual_voices = 512;
  cfg.max_real_voices = 32;
  Bank b;
  b.Event("E", 1.0f);  // DC 0.001, endless
  Fx fx(cfg);
  fx.Load("b", b.Build("B"));
  constexpr int kThreads = 3, kIter = 120;
  std::atomic<int> finished{0};
  std::vector<std::vector<std::uint64_t>> hs(kThreads);
  std::atomic<std::uint32_t> limitRetries{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      std::uint32_t rng = 777u * (t + 1);
      auto next = [&] {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 10;
      };
      auto reg = [&] {
        while (fx.sys->RegisterThread() != SND_OK) {
          limitRetries.fetch_add(1);
          std::this_thread::yield();
        }
      };
      reg();
      for (int i = 0; i < kIter; ++i) {
        const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
        if (p.value == 0u) continue;
        hs[t].push_back(p.value);
        for (std::uint32_t k = next() % 4u; k > 0u; --k) fx.sys->Flush();
        fx.sys->UnregisterThread();
        reg();
        SND_CHECK_EQ(fx.sys->Stop(p, 0.0f, nullptr), SND_OK);
        if (next() % 2u) fx.sys->Flush();
      }
      // Leave with everything published (retire, come back, until the audio side has it all).
      fx.sys->UnregisterThread();
      finished.fetch_add(1);
    });
  }
  for (int i = 0; i < 20000 && finished.load() < kThreads; ++i) fx.Step();
  for (auto& th : ts) th.join();
  fx.Steps(40);
  std::uint32_t total = 0, stopped = 0;
  for (const auto& v : hs) {
    for (std::uint64_t h : v) {
      ++total;
      stopped += fx.Ended(h, SND_END_STOPPED) ? 1u : 0u;
    }
  }
  std::printf("%u plays, %u stopped, %u register retries\n", total, stopped, limitRetries.load());
  SND_CHECK_EQ(stopped, total);
  SND_CHECK_EQ(fx.LastMean(), 0.0f);
  SND_CHECK_EQ(fx.kind.ViolationSum(), 0u);
}

// 2. Timed stop at X of a paused / pausing / just resumed playback.
SND_TEST(IndepFix2, TimedStopAroundPauseAndResume) {
  const std::size_t N = ShortFade(48000);
  // (a) X inside a 0.1 s pause fade: the level only goes down from the pause
  //     on, and the playback ends silently by the end of the block with X.
  {
    Fx fx;
    Bank b;
    b.Event("E", 500.0f);
    fx.Load("b", b.Build("B"));
    const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
    fx.Steps(3);
    const std::size_t s = fx.Total();
    const std::size_t x = s + 2 * 256 + 77;
    const snd_when w = AtClock(x);
    fx.sys->Pause(p, 0.1f);
    fx.sys->Stop(p, 0.0f, &w);
    fx.Steps(10);
    const auto r = Left(fx);
    SND_CHECK(Monotone(r, s, fx.Total(), true));
    SND_CHECK(r[x - 1] > 0.0f);  // still fading towards the pause at X
    const std::size_t blockEnd = (x / 256 + 1) * 256;
    SND_CHECK_MSG(fx.MaxAbs(blockEnd, fx.Total()) == 0.0f, "(a) audible after the block with X");
    SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
  }
  // (b) X right after the pause completed: silent all along.
  {
    Fx fx;
    Bank b;
    b.Event("E", 500.0f);
    fx.Load("b", b.Build("B"));
    const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
    fx.Steps(3);
    fx.sys->Pause(p, 0.0f);
    fx.Steps(3);
    fx.sys->Update();
    SND_REQUIRE_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PAUSED);
    const std::size_t s = fx.Total();
    const snd_when w = AtClock(s + 3);
    fx.sys->Stop(p, 0.0f, &w);
    fx.Steps(6);
    SND_CHECK_EQ(fx.MaxAbs(s - 256, fx.Total()), 0.0f);
    SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
  }
  // (c) X right after a resume: it plays again (fading in), then releases from X
  //     over exactly N frames; never above the full level.
  {
    Fx fx;
    Bank b;
    b.Event("E", 500.0f);
    fx.Load("b", b.Build("B"));
    const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
    fx.Steps(3);
    fx.sys->Pause(p, 0.0f);
    fx.Steps(4);
    const std::size_t s = fx.Total();
    fx.sys->ResumePlayback(p, 0.0f);
    const std::size_t x = s + 3 * 256 + 50;
    const snd_when w = AtClock(x);
    fx.sys->Stop(p, 0.0f, &w);
    fx.Steps(10);
    const auto r = Left(fx);
    SND_CHECK(fx.MaxAbs(s, fx.Total()) <= kCenter * 0.5f + 1e-6f);
    SND_CHECK_NEAR(r[x - 1], kCenter * 0.5f, 1e-5f);
    SND_CHECK(Monotone(r, x, fx.Total(), true));
    const std::size_t n = Between(r, x, fx.Total(), 0.0f, kCenter * 0.5f);
    SND_CHECK_MSG(n + 2 >= N && n <= N, "(c) release of %zu frames, shortFadeFrames %zu", n, N);
    SND_CHECK_EQ(fx.MaxAbs(x + N + 1, fx.Total()), 0.0f);
    SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
  }
}

// 3. Fade lengths: steal fade-out, return from virtual (fade-in), unload
// discard, timed stop release: exactly shortFadeFrames, linear, monotone.
SND_TEST(IndepFix2, ShortFadesLastExactlyShortFadeFrames) {
  const std::uint32_t cases[][2] = {{64, 48000}, {256, 48000}, {1024, 48000}, {256, 44100}, {256, 96000}};
  for (const auto& c : cases) {
    const std::uint32_t B = c[0], rate = c[1];
    const std::size_t N = ShortFade(rate);
    const float a = kCenter * 0.2f;  // Low's contribution
    const float h = kCenter * 0.5f;  // High's contribution
    // Steal fade-out and the return from virtual.
    {
      Fx fx(Cfg(B, rate, 1));
      Bank bk;
      bk.Event("Low", 200.0f, 100);
      const std::size_t highFrames = 20 * N + 3 * B;
      bk.Event("High", 2000.0f + float(highFrames), 200);  // DC 1.0 x 0.5 dB? (level 1.0)
      fx.Load("b", bk.Build("B"));
      fx.sys->Play(HashId("Low"), NoEmitter(), nullptr);
      fx.Steps(4);
      const std::size_t s = fx.Total();  // High starts here (fresh: full level)
      fx.sys->Play(HashId("High"), NoEmitter(), nullptr);
      fx.Steps(static_cast<std::uint32_t>((highFrames + 3 * N) / B + 4));
      std::vector<float> r = Left(fx);
      for (std::size_t f = s; f < s + highFrames && f < r.size(); ++f) r[f] -= 2.0f * h;  // High = DC 1.0
      const std::size_t out = Between(r, s, s + highFrames, 0.0f, a);
      SND_CHECK_MSG(out + 2 >= N && out <= N, "[%u/%u] steal fade-out %zu frames, shortFadeFrames %zu", B, rate, out, N);
      SND_CHECK(Monotone(r, s, s + highFrames, true));
      SND_CHECK_MSG(MaxStep(r, s, s + highFrames) < 1.5f * a / float(N), "[%u/%u] step in the steal fade", B, rate);
      const std::size_t in = Between(r, s + highFrames, r.size(), 0.0f, a);
      SND_CHECK_MSG(in + 2 >= N && in <= N, "[%u/%u] return fade-in %zu frames, shortFadeFrames %zu", B, rate, in, N);
      SND_CHECK(Monotone(r, s + highFrames, r.size(), false));
      SND_CHECK_NEAR(r.back(), a, 1e-5f);
    }
    // Unload discard and timed stop release.
    for (int variant = 0; variant < 2; ++variant) {
      Fx fx(Cfg(B, rate));
      Bank bk;
      bk.Event("Low", 200.0f);
      const snd_bank_handle bh = fx.Load("b", bk.Build("B"));
      const snd_playback_handle p = fx.sys->Play(HashId("Low"), NoEmitter(), nullptr);
      fx.Steps(4);
      const std::size_t s = fx.Total();
      std::size_t from = s;
      if (variant == 0) {
        fx.sys->UnloadBank(bh, SND_UNLOAD_STOP_AND_UNLOAD);
      } else {
        from = s + B + B / 3;
        const snd_when w = AtClock(from);
        fx.sys->Stop(p, 0.0f, &w);
      }
      fx.Steps(static_cast<std::uint32_t>(N / B + 6));
      const auto r = Left(fx);
      const std::size_t n = Between(r, from, r.size(), 0.0f, a);
      SND_CHECK_MSG(n + 2 >= N && n <= N, "[%u/%u] %s release %zu frames, shortFadeFrames %zu", B, rate,
                    variant == 0 ? "unload" : "timed stop", n, N);
      SND_CHECK(Monotone(r, s, r.size(), true));
      SND_CHECK_EQ(fx.MaxAbs(from + N + 1, fx.Total()), 0.0f);
      if (variant == 1) SND_CHECK_NEAR(r[from - 1], a, 1e-6f);
      SND_CHECK(fx.Ended(p.value, variant == 0 ? SND_END_DISCARDED : SND_END_STOPPED));
    }
  }
}

// 3. Overlaps with block 64 (a short fade spans ~4 blocks): while Low fades
// out (stolen by High), (a) Low is stopped, (b) Low's bank is unloaded, (c)
// High is stolen by High2. The fading voices only go down, without a step,
// and Low is gone by N frames after its fade started.
SND_TEST(IndepFix2, OverlapsDuringAStealFadeHaveNoStep) {
  const std::uint32_t B = 64, rate = 48000;
  const std::size_t N = ShortFade(rate);
  const float a = kCenter * 0.2f, h = kCenter * 0.3f, h2 = kCenter * 0.6f;
  for (int variant = 0; variant < 3; ++variant) {
    Fx fx(Cfg(B, rate, 1));
    Bank lowBank;
    lowBank.Event("Low", 200.0f, 100);
    Bank highBank;
    highBank.Event("High", 300.0f, 200);
    highBank.Event("High2", 600.0f, 250);
    const snd_bank_handle lb = fx.Load("l", lowBank.Build("L"));
    fx.Load("h", highBank.Build("H"));
    const snd_playback_handle low = fx.sys->Play(HashId("Low"), NoEmitter(), nullptr);
    fx.Steps(4);
    const std::size_t s = fx.Total();
    fx.sys->Play(HashId("High"), NoEmitter(), nullptr);
    fx.Step();  // Low's steal fade runs (N > B)
    std::size_t s2 = 0;
    if (variant == 0) fx.sys->Stop(low, 0.0f, nullptr);
    if (variant == 1) fx.sys->UnloadBank(lb, SND_UNLOAD_STOP_AND_UNLOAD);
    if (variant == 2) {
      s2 = fx.Total();
      fx.sys->Play(HashId("High2"), NoEmitter(), nullptr);
    }
    fx.Steps(static_cast<std::uint32_t>(3 * N / B + 8));
    std::vector<float> r = Left(fx);
    // Residual of the fading voices: remove the newcomers (fresh starts, full
    // level from their first frame). In variant 2 High is a fading voice from s2 on.
    for (std::size_t f = s; f < r.size(); ++f) {
      if (variant != 2 || f < s2) r[f] -= h;
      if (variant == 2 && f >= s2) r[f] -= h2;
    }
    const std::size_t split = variant == 2 ? s2 : r.size();
    const float total = variant == 2 ? a + h : a;
    SND_CHECK_MSG(Monotone(r, s, split, true) && Monotone(r, split, r.size(), true),
                  "variant %d: the fading voices go up again", variant);
    const float step = std::max(MaxStep(r, s, split), MaxStep(r, split, r.size()));
    // Two short fades on one voice multiply (steal fade x release): the slope is
    // at most the sum of both, 2 x ramp step; a discontinuity is far larger.
    SND_CHECK_MSG(step < 2.1f * total / float(N), "variant %d: step %f (ramp step %f)", variant,
                  static_cast<double>(step), static_cast<double>(total / float(N)));
    // Low (fading from s) is gone N frames later; in variant 2 High (fading from s2) too.
    SND_CHECK_MSG(std::fabs(r[(variant == 2 ? s2 : s) + N + 1]) < 1e-6f, "variant %d: residual %f after the fades",
                  variant, static_cast<double>(r[(variant == 2 ? s2 : s) + N + 1]));
    if (variant == 0) SND_CHECK(fx.Ended(low.value, SND_END_STOPPED));
    if (variant == 1) SND_CHECK(fx.Ended(low.value, SND_END_DISCARDED));
    SND_CHECK_EQ(fx.kind.ViolationSum(), 0u);
  }
}
