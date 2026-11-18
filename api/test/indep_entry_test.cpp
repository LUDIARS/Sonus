// Independent tests: the C API entry (snd/sonus.h "General rules", CONTRACTS.md D-29,
// D-84, D-93, D-05, D-13, D-46, D-96).
// Properties (from the spec, before reading the implementation):
//  B1 a handle of value 0 -> SND_ERR_INVALID_HANDLE for every recording function taking
//     one, and it is checked before the argument values (0 + NaN is still INVALID_HANDLE).
//  B2 tempo and transport pitch share the rule: not finite or <= 0 -> INVALID_ARGUMENT.
//  B3 stinger id 0 -> INVALID_ARGUMENT.
//  B4 sys == NULL never crashes (recording: INVALID_ARGUMENT / invalid handle, queries:
//     their "nothing" value).
//  B5 threads: unregistered -> THREAD_NOT_REGISTERED / invalid handle; owner-only
//     functions elsewhere -> WRONG_THREAD; register is idempotent and limited to
//     max_registered_threads (owner included), unregister frees the place.
//  B6 snd_get_playback_transport is the pure D-05 mapping.
//  C1 (D-96) create + destroy of a standalone transport in one arena never runs the slots out.
//  E3 (D-93) with the live link the game keeps exactly max_registered_threads; the live
//     link's own thread always gets its place.
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "indep_rig.hpp"
#include "snd/livelink/server.hpp"

using apitest::SmallConfig;

namespace {

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

}  // namespace

// B4
SND_TEST(IndepEntry, NullSystemIsHarmless) {
  const snd_playback_handle p{SND_HANDLE_MAKE(1, 1)};
  const snd_emitter_handle e{SND_HANDLE_MAKE(1, 1)};
  const snd_transport_handle t{SND_HANDLE_MAKE(1, 1)};
  const snd_bank_handle b{SND_HANDLE_MAKE(1, 1)};
  const snd_user_buffer_handle u{SND_HANDLE_MAKE(1, 1)};
  const snd_vec3 v{0, 0, 0};
  const snd_listener_desc l{};
  const snd_transport_desc td{120.0f, 4, 4, 1, 0};
  const snd_musical_time mt{0, 0, 0};
  snd_position pos{};
  snd_meter meter{};
  snd_stats stats{};
  const void* data = &pos;
  uint64_t bytes = 7;
  const snd_result A = SND_ERR_INVALID_ARGUMENT;
  SND_CHECK_EQ(snd_update(nullptr), A);
  SND_CHECK_EQ(snd_register_thread(nullptr), A);
  SND_CHECK_EQ(snd_unregister_thread(nullptr), A);
  SND_CHECK_EQ(snd_flush(nullptr), A);
  SND_CHECK_EQ(snd_suspend(nullptr), A);
  SND_CHECK_EQ(snd_resume(nullptr), A);
  SND_CHECK_EQ(snd_set_callbacks(nullptr, nullptr), A);
  SND_CHECK_EQ(snd_load_bank_async(nullptr, "x").value, 0u);
  SND_CHECK_EQ(snd_unload_bank(nullptr, b, SND_UNLOAD_IF_UNUSED), A);
  SND_CHECK_EQ(snd_get_bank_state(nullptr, b), SND_BANK_STATE_UNLOADED);
  SND_CHECK_EQ(snd_declare_language(nullptr, "en"), A);
  SND_CHECK_EQ(snd_set_language(nullptr, 1), A);
  SND_CHECK_EQ(snd_get_bank_chunk(nullptr, b, 1, &data, &bytes), A);
  SND_CHECK(data == nullptr);
  SND_CHECK_EQ(bytes, 0u);
  SND_CHECK_EQ(snd_play(nullptr, 1, e, nullptr).value, 0u);
  SND_CHECK_EQ(snd_play_one_shot(nullptr, 1, e, nullptr), A);
  SND_CHECK_EQ(snd_play_at(nullptr, 1, &v, nullptr), A);
  SND_CHECK_EQ(snd_stop(nullptr, p, 0, nullptr), A);
  SND_CHECK_EQ(snd_pause(nullptr, p, 0), A);
  SND_CHECK_EQ(snd_resume_playback(nullptr, p, 0), A);
  SND_CHECK_EQ(snd_set_scheduled_stop(nullptr, p, nullptr, 0), A);
  SND_CHECK_EQ(snd_create_emitter(nullptr).value, 0u);
  SND_CHECK_EQ(snd_destroy_emitter(nullptr, e), A);
  SND_CHECK_EQ(snd_set_transform(nullptr, e, &v, &v, &v), A);
  SND_CHECK_EQ(snd_set_listener(nullptr, 0, &l), A);
  SND_CHECK_EQ(snd_set_param_global(nullptr, 1, 0), A);
  SND_CHECK_EQ(snd_set_param_emitter(nullptr, e, 1, 0), A);
  SND_CHECK_EQ(snd_set_param_playback(nullptr, p, 1, 0), A);
  SND_CHECK_EQ(snd_set_switch_global(nullptr, 1, 1), A);
  SND_CHECK_EQ(snd_set_switch_emitter(nullptr, e, 1, 1), A);
  SND_CHECK_EQ(snd_set_switch_playback(nullptr, p, 1, 1), A);
  SND_CHECK_EQ(snd_set_bus_volume(nullptr, 1, 1, 0), A);
  SND_CHECK_EQ(snd_apply_snapshot(nullptr, 1, 1, 0), A);
  SND_CHECK_EQ(snd_set_category_volume(nullptr, 1, 1, 0), A);
  SND_CHECK_EQ(snd_pause_category(nullptr, 1, 1), A);
  SND_CHECK_EQ(snd_mute_category(nullptr, 1, 1), A);
  SND_CHECK_EQ(snd_set_music_state(nullptr, p, 1, nullptr), A);
  SND_CHECK_EQ(snd_prepare_music_state(nullptr, p, 1), A);
  SND_CHECK_EQ(snd_create_transport(nullptr, &td).value, 0u);
  SND_CHECK_EQ(snd_destroy_transport(nullptr, t), A);
  SND_CHECK_EQ(snd_set_transport_state(nullptr, t, SND_TRANSPORT_PLAYING, nullptr), A);
  SND_CHECK_EQ(snd_set_tempo(nullptr, t, 120, nullptr), A);
  SND_CHECK_EQ(snd_seek_transport(nullptr, t, &mt, nullptr), A);
  SND_CHECK_EQ(snd_set_transport_pitch(nullptr, t, 1, nullptr), A);
  SND_CHECK_EQ(snd_play_stinger(nullptr, p, 1, nullptr), A);
  SND_CHECK_EQ(snd_play_synth(nullptr, nullptr, e, nullptr).value, 0u);
  SND_CHECK_EQ(snd_create_user_buffer(nullptr, nullptr).value, 0u);
  SND_CHECK_EQ(snd_destroy_user_buffer(nullptr, u), A);
  SND_CHECK_EQ(snd_get_user_buffer_released(nullptr, u), 0);
  SND_CHECK_EQ(snd_write_user_buffer(nullptr, u, nullptr, 1), 0u);
  SND_CHECK_EQ(snd_play_user_buffer(nullptr, u, 0, e, nullptr).value, 0u);
  SND_CHECK_EQ(snd_play_input(nullptr, nullptr, e, nullptr).value, 0u);
  SND_CHECK_EQ(snd_get_playback_state(nullptr, p), SND_PLAYBACK_STATE_NONE);
  SND_CHECK_EQ(snd_get_position(nullptr, p, &pos), A);
  SND_CHECK_EQ(snd_get_transport_position(nullptr, t, &pos), A);
  SND_CHECK_EQ(snd_get_meter(nullptr, 0, &meter), A);
  SND_CHECK_EQ(snd_get_stats(nullptr, &stats), A);
}

// B1: handle 0 first, whatever the other arguments are (NaN, bad when, stinger 0).
SND_TEST(IndepEntry, HandleZeroBeatsEveryOtherArgument) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  snd_system* s = r.sys;
  const snd_playback_handle p0{0};
  const snd_emitter_handle e0{0};
  const snd_transport_handle t0{0};
  const snd_bank_handle b0{0};
  const snd_user_buffer_handle u0{0};
  snd_when bad{};
  bad.kind = 200;  // invalid kind
  const snd_musical_time mt{0, 0, 0};
  const snd_vec3 nanv{kNaN, kNaN, kNaN};
  const snd_result H = SND_ERR_INVALID_HANDLE;
  SND_CHECK_EQ(snd_unload_bank(s, b0, 77u), H);
  SND_CHECK_EQ(snd_stop(s, p0, kNaN, &bad), H);
  SND_CHECK_EQ(snd_pause(s, p0, kNaN), H);
  SND_CHECK_EQ(snd_resume_playback(s, p0, kNaN), H);
  SND_CHECK_EQ(snd_set_scheduled_stop(s, p0, &bad, kNaN), H);
  SND_CHECK_EQ(snd_destroy_emitter(s, e0), H);
  SND_CHECK_EQ(snd_set_transform(s, e0, &nanv, &nanv, &nanv), H);
  SND_CHECK_EQ(snd_set_param_emitter(s, e0, 1, kNaN), H);
  SND_CHECK_EQ(snd_set_param_playback(s, p0, 1, kNaN), H);
  SND_CHECK_EQ(snd_set_switch_emitter(s, e0, 1, 1), H);
  SND_CHECK_EQ(snd_set_switch_playback(s, p0, 1, 1), H);
  SND_CHECK_EQ(snd_set_music_state(s, p0, 1, &bad), H);
  SND_CHECK_EQ(snd_prepare_music_state(s, p0, 1), H);
  SND_CHECK_EQ(snd_play_stinger(s, p0, 0, &bad), H);
  SND_CHECK_EQ(snd_destroy_transport(s, t0), H);
  SND_CHECK_EQ(snd_set_transport_state(s, t0, 99u, &bad), H);
  SND_CHECK_EQ(snd_set_tempo(s, t0, kNaN, &bad), H);
  SND_CHECK_EQ(snd_set_transport_pitch(s, t0, -1.0f, &bad), H);
  SND_CHECK_EQ(snd_seek_transport(s, t0, &mt, &bad), H);
  SND_CHECK_EQ(snd_destroy_user_buffer(s, u0), H);
  // Queries of handle 0.
  snd_position pos{};
  SND_CHECK_EQ(snd_get_playback_state(s, p0), SND_PLAYBACK_STATE_NONE);
  SND_CHECK_EQ(snd_get_user_buffer_released(s, u0), 0);
  SND_CHECK_EQ(snd_write_user_buffer(s, u0, nullptr, 4), 0u);
  SND_CHECK(snd_get_position(s, p0, &pos) != SND_OK);
  SND_CHECK(snd_get_transport_position(s, t0, &pos) != SND_OK);
  // Nothing was recorded (no rejected / applied command leaves a trace).
  r.Step(4);
  SND_CHECK(r.ev.ends.empty());
}

// B2 / B3 on a live handle: the values the rule refuses.
SND_TEST(IndepEntry, TempoPitchAndStingerValues) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd_transport_desc td{120.0f, 4, 4, 1, 0};
  const snd_transport_handle t = snd_create_transport(r.sys, &td);
  SND_REQUIRE(t.value != 0u);
  const float refused[] = {0.0f, -0.0f, -1.0f, kNaN, kInf, -kInf};
  for (const float v : refused) {
    SND_CHECK_MSG(snd_set_tempo(r.sys, t, v, nullptr) == SND_ERR_INVALID_ARGUMENT, "bpm %g", static_cast<double>(v));
    SND_CHECK_MSG(snd_set_transport_pitch(r.sys, t, v, nullptr) == SND_ERR_INVALID_ARGUMENT, "pitch %g",
                  static_cast<double>(v));
  }
  // Positive finite values are accepted, however extreme (clamped later, D-85 / D-88).
  const float accepted[] = {std::numeric_limits<float>::denorm_min(), 1e-6f, 1.0f / 16.0f, 16.0f, 1e6f,
                            std::numeric_limits<float>::max()};
  for (const float v : accepted) {
    SND_CHECK_MSG(snd_set_transport_pitch(r.sys, t, v, nullptr) == SND_OK, "pitch %g", static_cast<double>(v));
    SND_CHECK_MSG(snd_set_tempo(r.sys, t, v, nullptr) == SND_OK, "bpm %g", static_cast<double>(v));
  }
  snd_when w{};
  w.kind = SND_WHEN_DELAY_US + 1u;
  SND_CHECK_EQ(snd_set_transport_pitch(r.sys, t, 1.0f, &w), SND_ERR_INVALID_ARGUMENT);
  w.kind = SND_WHEN_NEXT_BAR;
  w.flags = 0x02;  // unknown flag
  SND_CHECK_EQ(snd_set_transport_pitch(r.sys, t, 1.0f, &w), SND_ERR_INVALID_ARGUMENT);
  // A playback (any live handle) with stinger 0.
  float v = 0.0f;
  const snd_synth_desc d = indep::DcDesc(&v);
  const snd_playback_handle p = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  SND_CHECK_EQ(snd_play_stinger(r.sys, p, 0u, nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_play_stinger(r.sys, p, 1u, nullptr), SND_OK);  // not a music playback: plays nothing
  // The transport of a playback: the pitch / tempo functions take it (D-05, D-88).
  const snd_transport_handle pt = snd_get_playback_transport(p);
  SND_CHECK_EQ(snd_set_transport_pitch(r.sys, pt, kNaN, nullptr), SND_ERR_INVALID_ARGUMENT);
  r.Step(6);
  // And a playback-owned transport cannot be destroyed (sonus.h): not SND_OK.
  SND_CHECK(snd_destroy_transport(r.sys, pt) != SND_OK);
  snd_stop(r.sys, p, 0.0f, nullptr);
  r.Step(4);
  SND_CHECK_EQ(r.EndsOf(p.value), 1u);
}

// B6: the pure mapping (D-05), any thread, no system.
SND_TEST(IndepEntry, PlaybackTransportMapping) {
  for (const std::uint32_t slot : {0u, 1u, 1023u, 0x7FFFFFFFu}) {
    for (const std::uint32_t gen : {1u, 2u, 0x7FFFFFFFu}) {
      const snd_transport_handle t = snd_get_playback_transport(snd_playback_handle{SND_HANDLE_MAKE(slot, gen)});
      SND_CHECK_EQ(SND_HANDLE_SLOT(t.value), slot | SND_TRANSPORT_PLAYBACK_BIT);
      SND_CHECK_EQ(SND_HANDLE_GENERATION(t.value), gen);
    }
  }
  SND_CHECK_EQ(snd_get_playback_transport(snd_playback_handle{0}).value, 0u);
}

// B5: thread rules.
SND_TEST(IndepEntry, ThreadRules) {
  indep::Rig r;
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd_transport_desc td{120.0f, 4, 4, 1, 0};
  std::thread t([&] {
    // Unregistered: nothing is recorded.
    float v = 0.0f;
    const snd_synth_desc d = indep::DcDesc(&v);
    SND_CHECK_EQ(snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr).value, 0u);
    SND_CHECK_EQ(snd_create_transport(r.sys, &td).value, 0u);
    SND_CHECK_EQ(snd_create_emitter(r.sys).value, 0u);
    SND_CHECK_EQ(snd_set_param_global(r.sys, 1, 0.5f), SND_ERR_THREAD_NOT_REGISTERED);
    SND_CHECK_EQ(snd_suspend(r.sys), SND_ERR_THREAD_NOT_REGISTERED);
    SND_CHECK_EQ(snd_flush(r.sys), SND_ERR_THREAD_NOT_REGISTERED);
    // Owner-only functions.
    SND_CHECK_EQ(snd_update(r.sys), SND_ERR_WRONG_THREAD);
    SND_CHECK_EQ(snd_set_callbacks(r.sys, nullptr), SND_ERR_WRONG_THREAD);
    const void* data = nullptr;
    uint64_t bytes = 0;
    SND_CHECK_EQ(snd_get_bank_chunk(r.sys, snd_bank_handle{SND_HANDLE_MAKE(0, 1)}, 1, &data, &bytes),
                 SND_ERR_WRONG_THREAD);
    // Registered: idempotent; the queries are "any".
    SND_CHECK_EQ(snd_register_thread(r.sys), SND_OK);
    SND_CHECK_EQ(snd_register_thread(r.sys), SND_OK);
    const snd_transport_handle h = snd_create_transport(r.sys, &td);
    SND_CHECK(h.value != 0u);
    SND_CHECK_EQ(snd_set_tempo(r.sys, h, 90.0f, nullptr), SND_OK);
    SND_CHECK_EQ(snd_update(r.sys), SND_ERR_WRONG_THREAD);  // still not the owner
    snd_stats st{};
    SND_CHECK(snd_get_stats(r.sys, &st) == SND_OK || snd_get_stats(r.sys, &st) == SND_ERR_NOT_READY);
    SND_CHECK_EQ(snd_unregister_thread(r.sys), SND_OK);
    // Unregistered again.
    SND_CHECK_EQ(snd_set_param_global(r.sys, 1, 0.5f), SND_ERR_THREAD_NOT_REGISTERED);
  });
  t.join();
  // The callbacks are still the rig's (the refused set_callbacks changed nothing).
  float v = 0.0f;
  const snd_synth_desc d = indep::DcDesc(&v);
  const snd_playback_handle p = snd_play_synth(r.sys, &d, snd_emitter_handle{0}, nullptr);
  snd_stop(r.sys, p, 0.0f, nullptr);
  r.Step(6);
  SND_CHECK_EQ(r.EndsOf(p.value), 1u);
}

// E3: max_registered_threads counts the owner; unregistering frees the place for another thread.
SND_TEST(IndepEntry, RegisteredThreadLimitAndReuse) {
  snd_config c = SmallConfig();
  c.max_registered_threads = 2;
  indep::Rig r(c);
  SND_REQUIRE_EQ(r.created, SND_OK);
  for (int round = 0; round < 5; ++round) {
    snd_result first = SND_ERR_INVALID_ARGUMENT, second = SND_ERR_INVALID_ARGUMENT;
    std::atomic<int> phase{0};
    std::thread a([&] {
      first = snd_register_thread(r.sys);
      phase.store(1);
      while (phase.load() != 2) std::this_thread::yield();
      if (first == SND_OK) snd_unregister_thread(r.sys);
      phase.store(3);
    });
    while (phase.load() != 1) std::this_thread::yield();
    std::thread b([&] { second = snd_register_thread(r.sys); });
    b.join();
    phase.store(2);
    a.join();
    SND_CHECK_EQ(first, SND_OK);
    SND_CHECK_EQ(second, SND_ERR_LIMIT_REACHED);
    r.Step(2);  // the owner's update may be what returns a slot
  }
}

// E3 with the live link: the game gets max_registered_threads (owner included) and no more,
// even when its threads register right after snd_create returned, and the live link's
// thread is registered (it can record: Flush succeeds in its loop). D-93: "core には
// max_registered_threads + 1 を渡す" - the extra place is the live link's, not the game's.
SND_TEST(IndepEntry, Defect_LiveLinkThreadPlaceCannotBeTakenByTheGame) {
#if SND_LIVELINK
  std::uint32_t gameGotMore = 0;
  for (int trial = 0; trial < 40; ++trial) {
    snd_config c = SmallConfig();
    c.livelink.enabled = 1;
    c.livelink.port = 0;
    c.max_registered_threads = 2;  // the owner + 1 game thread
    std::atomic<int> go{0};
    std::atomic<int> ok{0};
    snd_system* volatile target = nullptr;
    std::vector<std::thread> ts;
    for (int i = 0; i < 2; ++i) {
      ts.emplace_back([&] {
        while (go.load(std::memory_order_acquire) == 0) {
        }
        if (snd_register_thread(target) == SND_OK) ok.fetch_add(1);
      });
    }
    indep::Rig r(c);
    SND_REQUIRE_EQ(r.created, SND_OK);
    target = r.sys;
    go.store(1, std::memory_order_release);  // right after snd_create returned
    for (std::thread& t : ts) t.join();
    if (ok.load() > 1) ++gameGotMore;
    // Unregistering happens on the threads, which ended: they leave their places taken;
    // the rig is destroyed with them (snd_destroy does not need them).
  }
  if (gameGotMore != 0u) snd::test::Note("the game registered 2 threads besides the owner in %u of 40 trials", gameGotMore);
  SND_CHECK_EQ(gameGotMore, 0u);
#endif
}

// C1 (D-96): create + destroy in one arena, many more times than there are slots; also
// mixed with transports that live across blocks.
SND_TEST(IndepEntry, TransportSlotsComeBackWhenCreatedAndDestroyedInOneBlock) {
  snd_config c = SmallConfig();
  c.max_transports = 3;
  indep::Rig r(c);
  SND_REQUIRE_EQ(r.created, SND_OK);
  const snd_transport_desc td{120.0f, 4, 4, 1, 0};
  const snd_transport_handle keep = snd_create_transport(r.sys, &td);  // one stays all along
  SND_REQUIRE(keep.value != 0u);
  r.Step(2);
  std::uint32_t invalid = 0;
  for (int frame = 0; frame < 40; ++frame) {
    // Two per frame (both free slots), destroyed in the same arena.
    for (int k = 0; k < 2; ++k) {
      const snd_transport_handle t = snd_create_transport(r.sys, &td);
      if (t.value == 0u) {
        ++invalid;
        continue;
      }
      SND_CHECK_EQ(snd_destroy_transport(r.sys, t), SND_OK);
      // Stale on the calling side at once? No: it is current until the slot comes back.
    }
    r.Step(2);  // applied in one block, then the owner's update returns the slots
    r.Step(1);
  }
  SND_CHECK_EQ(invalid, 0u);
  // Alternating: created in one block, destroyed in the next (the normal path), and again
  // created + destroyed in one block.
  for (int frame = 0; frame < 20; ++frame) {
    const snd_transport_handle t = snd_create_transport(r.sys, &td);
    SND_REQUIRE(t.value != 0u);
    r.Step(2);
    SND_CHECK_EQ(snd_destroy_transport(r.sys, t), SND_OK);
    const snd_transport_handle u = snd_create_transport(r.sys, &td);
    SND_REQUIRE(u.value != 0u);
    SND_CHECK_EQ(snd_destroy_transport(r.sys, u), SND_OK);
    r.Step(3);
    // The destroyed handles become stale for the recording side.
    SND_CHECK_EQ(snd_set_tempo(r.sys, t, 100.0f, nullptr), SND_ERR_INVALID_HANDLE);
    SND_CHECK_EQ(snd_set_tempo(r.sys, u, 100.0f, nullptr), SND_ERR_INVALID_HANDLE);
  }
  // The kept transport is unaffected and still runs.
  snd_position pos{};
  snd_update(r.sys);
  SND_CHECK_EQ(snd_get_transport_position(r.sys, keep, &pos), SND_OK);
  SND_CHECK_EQ(pos.has_musical, 1u);
  SND_CHECK(pos.musical.bar > 0 || pos.musical.beat > 0);
}
