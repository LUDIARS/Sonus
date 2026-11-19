// Threads through the C API (sonus.h "Threads", D-46, D-93): several registered
// threads record at once while the owner updates and the audio thread renders.
// Every handle-returning call gets its handle at once, every playback ends exactly
// once (on_end on the owner thread), and the composition root's own cross-thread
// state (snd_resume from other threads, the device event flags) stays consistent.
// Run under TSan and repeated (ctest --repeat until-fail:20 -R api_threads_test).
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "api_rig.hpp"

using namespace apitest;

SND_TEST(ApiThreads, ConcurrentRecordingEndsEveryPlaybackOnce) {
  snd_config c = SmallConfig();
  c.max_registered_threads = 5;
  c.max_playbacks = 256;
  c.max_virtual_voices = 256;
  Rig r(c);
  SND_REQUIRE_EQ(r.created, SND_OK);
  constexpr int kThreads = 4;
  constexpr int kPlays = 60;
  std::mutex m;
  std::set<std::uint64_t> handles;
  std::atomic<int> done{0};
  std::atomic<int> refused{0};
  std::vector<std::thread> ts;
  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back([&, i] {
      if (snd_register_thread(r.sys) != SND_OK) {
        done.fetch_add(1);
        return;
      }
      const snd_emitter_handle e = snd_create_emitter(r.sys);
      for (int k = 0; k < kPlays; ++k) {
        const snd_playback_handle p = snd_play(r.sys, snd::HashId("NotLoaded") + static_cast<snd_id>(i), e, nullptr);
        if (p.value != 0u) {
          std::lock_guard<std::mutex> lock(m);
          handles.insert(p.value);
        } else {
          refused.fetch_add(1);  // no free slot right now (they come back at the owner's update)
        }
        const snd_vec3 pos{static_cast<float>(k), 0.0f, 1.0f};
        snd_set_transform(r.sys, e, &pos, nullptr, nullptr);
        snd_set_param_emitter(r.sys, e, 7u, static_cast<float>(k));
        snd_set_param_global(r.sys, 8u + static_cast<snd_id>(i), 0.5f);
        if (k % 10 == 0) {
          snd_resume(r.sys);  // non-owner: the device part waits for the owner's update
          snd_flush(r.sys);
          std::this_thread::yield();
        }
      }
      snd_destroy_emitter(r.sys, e);
      snd_flush(r.sys);
      SND_CHECK_EQ(snd_unregister_thread(r.sys), SND_OK);
      done.fetch_add(1);
    });
  }
  while (done.load() < kThreads) r.Step(1);
  for (std::thread& t : ts) t.join();
  r.Step(20);
  snd_update(r.sys);
  std::map<std::uint64_t, int> ends;
  for (const End& e : r.ev.ends) {
    ++ends[e.handle];
    SND_CHECK_EQ(e.reason, SND_END_NOT_LOADED);
  }
  SND_CHECK_EQ(ends.size(), handles.size());
  for (const std::uint64_t h : handles) SND_CHECK_MSG(ends[h] == 1, "handle %llx ended %d times", (unsigned long long)h, ends[h]);
  SND_CHECK_EQ(static_cast<int>(handles.size()) + refused.load(), kThreads * kPlays);
  SND_CHECK_EQ(r.refillTimeouts, 0u);
}

SND_TEST(ApiThreads, CreateAndDestroyRepeatedlyWhileThreadsWait) {
  for (int round = 0; round < 4; ++round) {
    Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    std::atomic<bool> stop{false};
    std::thread reader([&] {
      // Queries are "any thread" (D-16): read while the owner updates.
      snd_stats s{};
      while (!stop.load()) {
        snd_get_stats(r.sys, &s);
        snd_get_playback_state(r.sys, snd_playback_handle{SND_HANDLE_MAKE(1u, 1u)});
        std::this_thread::yield();
      }
    });
    r.Step(10);
    stop.store(true);
    reader.join();
  }
}
