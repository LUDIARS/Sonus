// Independent tests: running the composed system (design.md 3.3, 13, 14; CONTRACTS.md
// D-16, D-42, D-46, D-93).
// Properties (from the spec, before reading the implementation):
//  E4 after snd_create returns, the game allocator is not called again while the game
//     records, updates and the audio thread renders (no bank loads): "初期化後のメモリ
//     確保ゼロ". snd_destroy gives every block back.
//  E5 on_end exactly once per handle-returning play, whatever ends it (stop, completion,
//     rejection at the ring limit, destroyed user buffer, NOT_LOADED); never for
//     snd_play_one_shot / snd_play_at (D-42); none during or after snd_destroy.
//  E5' concurrently from several registered threads, with snd_flush.
#include <atomic>
#include <cmath>
#include <map>
#include <thread>
#include <vector>

#include "indep_rig.hpp"

using apitest::SmallConfig;

namespace {

// Exercises everything the C API records without a bank, for `frames` updates.
void Workload(indep::Rig& r, int frames, std::vector<snd_playback_handle>* plays) {
  static const float kValue = 0.1f;
  static float buf0[2048];
  static float* chans[1] = {buf0};
  const snd_transport_desc td{120.0f, 4, 4, 1, 0};
  for (int f = 0; f < frames; ++f) {
    const snd_synth_desc ll = indep::DcDesc(&kValue, SND_SYNTH_LOW_LATENCY);
    const snd_synth_desc bu = indep::DcDesc(&kValue, SND_SYNTH_BUFFERED);
    const snd_emitter_handle e = snd_create_emitter(r.sys);
    const snd_vec3 pos{1.0f * static_cast<float>(f % 5), 0, 0};
    snd_set_transform(r.sys, e, &pos, nullptr, nullptr);
    const snd_transport_handle t = snd_create_transport(r.sys, &td);
    const snd_when w = [&] {
      snd_when x{};
      x.kind = SND_WHEN_NEXT_BEAT;
      x.transport = t;
      return x;
    }();
    const snd_playback_handle a = snd_play_synth(r.sys, &ll, e, nullptr);
    const snd_playback_handle b = snd_play_synth(r.sys, &bu, snd_emitter_handle{0}, &w);
    const snd_playback_handle c = snd_play(r.sys, snd::HashId("NotLoaded"), e, nullptr);
    snd_play_one_shot(r.sys, snd::HashId("NotLoadedEither"), snd_emitter_handle{0}, nullptr);
    snd_play_at(r.sys, snd::HashId("NotLoadedEither"), &pos, nullptr);
    if (plays != nullptr) {
      plays->push_back(a);
      plays->push_back(b);
      plays->push_back(c);
    }
    snd_set_param_global(r.sys, snd::HashId("P"), 0.5f);
    snd_set_param_playback(r.sys, a, snd::HashId("P"), 0.5f);
    snd_set_tempo(r.sys, t, 100.0f + static_cast<float>(f), nullptr);
    snd_set_transport_pitch(r.sys, t, 1.5f, nullptr);
    snd_set_bus_volume(r.sys, 0, 0.9f, 0.01f);
    // A static user buffer that completes.
    snd_user_buffer_desc ud{chans, 1, 300, 0, SND_USER_BUFFER_STATIC, 0, 0};
    const snd_user_buffer_handle ub = snd_create_user_buffer(r.sys, &ud);
    const snd_playback_handle u = snd_play_user_buffer(r.sys, ub, 0, snd_emitter_handle{0}, nullptr);
    if (plays != nullptr) plays->push_back(u);
    r.Step(2);
    snd_stop(r.sys, a, 0.0f, nullptr);
    snd_stop(r.sys, b, 0.005f, nullptr);
    snd_destroy_user_buffer(r.sys, ub);
    snd_destroy_transport(r.sys, t);
    snd_destroy_emitter(r.sys, e);
    r.Step(2);
  }
  r.Step(8);
}

}  // namespace

// E4 + E5.
SND_TEST(IndepRuntime, NoAllocationAfterCreateAndEveryEndOnce) {
  std::int64_t liveAfterCreate = 0;
  std::uint64_t callsAfterCreate = 0;
  std::uint64_t callsAfterRun = 0;
  {
    indep::Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    liveAfterCreate = r.alloc.live.load();
    callsAfterCreate = r.alloc.calls.load();
    std::vector<snd_playback_handle> plays;
    Workload(r, 24, &plays);
    callsAfterRun = r.alloc.calls.load();
    SND_CHECK_EQ(callsAfterRun, callsAfterCreate);
    SND_CHECK_EQ(r.alloc.live.load(), liveAfterCreate);
    // Every handle-returning play ended exactly once; nothing else ended.
    std::map<std::uint64_t, std::uint32_t> n;
    for (const apitest::End& e : r.ev.ends) ++n[e.handle];
    for (const snd_playback_handle& p : plays) {
      if (p.value == 0u) continue;
      SND_CHECK_MSG(n[p.value] == 1u, "handle %llx ended %u times", static_cast<unsigned long long>(p.value),
                    n[p.value]);
    }
    std::size_t valid = 0;
    for (const snd_playback_handle& p : plays) valid += p.value != 0u ? 1u : 0u;
    SND_CHECK_EQ(r.ev.ends.size(), valid);  // the one-shots did not report (D-42)
    const std::size_t before = r.ev.ends.size();
    // Plays still running at destroy: no callback during snd_destroy.
    static const float kValue = 0.1f;
    const snd_synth_desc ll = indep::DcDesc(&kValue);
    snd_play_synth(r.sys, &ll, snd_emitter_handle{0}, nullptr);
    r.Step(2);
    snd_destroy(r.sys);
    r.sys = nullptr;
    SND_CHECK_EQ(r.ev.ends.size(), before);
    SND_CHECK_EQ(r.alloc.live.load(), 0);
  }
}

// E5': several registered threads record and flush while the owner updates.
SND_TEST(IndepRuntime, EndsFromConcurrentProducersAreExactlyOnce) {
  snd_config c = SmallConfig();
  c.max_registered_threads = 4;
  c.max_playbacks = 256;
  c.max_real_voices = 48;
  c.max_virtual_voices = 256;
  indep::Rig r(c);
  SND_REQUIRE_EQ(r.created, SND_OK);
  r.settle = false;
  const std::uint64_t allocCalls = r.alloc.calls.load();
  constexpr int kThreads = 3;
  constexpr int kPerThread = 300;
  std::vector<std::vector<std::uint64_t>> handles(kThreads);
  std::atomic<int> done{0};
  std::vector<std::thread> ts;
  static const float kValue = 0.01f;
  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back([&, i] {
      SND_CHECK_EQ(snd_register_thread(r.sys), SND_OK);
      const snd_synth_desc d = indep::DcDesc(&kValue, i == 0 ? SND_SYNTH_BUFFERED : SND_SYNTH_LOW_LATENCY);
      std::vector<snd_playback_handle> mine;
      for (int k = 0; k < kPerThread; ++k) {
        const snd_playback_handle p = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
        if (p.value != 0u) {
          handles[i].push_back(p.value);
          mine.push_back(p);
        }
        if (mine.size() > 8u) {
          snd_stop(r.sys, mine.front(), 0.0f, nullptr);
          mine.erase(mine.begin());
        }
        if (k % 7 == 0) snd_flush(r.sys);
        if (k % 13 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
      for (const snd_playback_handle& p : mine) snd_stop(r.sys, p, 0.0f, nullptr);
      snd_flush(r.sys);
      snd_unregister_thread(r.sys);  // publishes what is left (snd/sonus.h)
      done.fetch_add(1);
    });
  }
  while (done.load() < kThreads) r.Step(1);
  for (std::thread& t : ts) t.join();
  r.Step(30);
  std::map<std::uint64_t, std::uint32_t> n;
  for (const apitest::End& e : r.ev.ends) ++n[e.handle];
  std::size_t total = 0, wrong = 0;
  for (const auto& v : handles) {
    for (const std::uint64_t h : v) {
      ++total;
      if (n[h] != 1u) ++wrong;
    }
  }
  SND_CHECK(total > 100u);
  SND_CHECK_MSG(wrong == 0u, "%llu of %llu handles did not end exactly once", static_cast<unsigned long long>(wrong),
                static_cast<unsigned long long>(total));
  SND_CHECK_EQ(r.ev.ends.size(), total);
  SND_CHECK_EQ(r.alloc.calls.load(), allocCalls);
}
