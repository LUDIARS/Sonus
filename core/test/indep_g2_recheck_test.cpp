// Independent tests, re-check 1 (implementation 192bace): the places the fixes changed.
//  - snd_set_event_voice_effect all or none at every arena boundary (normal region exactly
//    full, one command short, reserve only) and from several threads at once;
//  - per-playback values from the first block of a slot with two stages;
//  - the raw SetBusVolume variant 1 (contract commands.hpp) equals the public call;
//  - determinism, no allocation and no RT violation on the new paths (re-acquire on return,
//    [init] warnings).
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "indep_g2_fixture.hpp"
#include "snd/internal/commands.hpp"
#include "snd/test/rt_guard.hpp"

namespace {

using namespace g2;
using snd::HashId;

constexpr std::size_t kDelay = 480;

snd_config SmallArena() {
  snd_config c = G2Config();
  c.max_voice_effects = 4;
  c.commands.arena_bytes = 2048;
  c.commands.reserve_bytes = 512;
  c.voice_effect_bytes = 1u << 20;
  return c;
}

// 32-byte filler: one SetSwitch variant 1 (an Event that is never played).
snd_result Filler(Fx& h) { return h.sys->SetEventVoiceEffect(HashId("X"), 1, dsp::kEffectEcho, nullptr, 0); }

std::uint32_t ArenaCapacity() {
  Fx h(SmallArena());
  if (h.sys == nullptr) return 0;
  FxBank fb;
  fb.Event("E", 500.0f);
  h.Load("v", fb.Build("V"));
  std::uint32_t n = 0;
  while (n < 10000 && Filler(h) == SND_OK) ++n;
  return n;
}

}  // namespace

// k fillers, then a 6-command call (1 + 5 values). OK exactly when it fits (k <= n - 6: the normal
// region, or the reserve when the normal region is short); refused leaves nothing behind (E dry,
// the space is still there for a smaller call).
SND_TEST(IndepG2Recheck, AllOrNoneAtEveryArenaBoundary) {
  const std::uint32_t n = ArenaCapacity();
  SND_REQUIRE(n > 40u);
  std::printf("  arena capacity: %u commands of 32 bytes\n", n);
  const std::vector<float> p = EchoDelay(10.0f);
  for (std::uint32_t k = n - 30u; k <= n; ++k) {
    Fx h(SmallArena());
    SND_REQUIRE(h.sys != nullptr);
    FxBank fb;
    fb.Event("E", 500.0f);
    h.Load("v", fb.Build("V"));
    for (std::uint32_t i = 0; i < k; ++i) Filler(h);
    const snd_result r = h.sys->SetEventVoiceEffect(HashId("E"), 0, dsp::kEffectEcho, p.data(), 5);
    const bool fits = k + 6u <= n;
    SND_CHECK_MSG(r == (fits ? SND_OK : SND_ERR_COMMAND_BUFFER_FULL), "k %u: result %d", k, r);
    std::uint32_t more = 0;  // what is left after the call
    while (more < 100 && Filler(h) == SND_OK) ++more;
    SND_CHECK_MSG(more == (fits ? n - k - 6u : n - k), "k %u: %u commands left after the call", k, more);
    h.Steps(2);
    const std::size_t f0 = h.Total();
    h.sys->Play(HashId("E"), NoEmitter(), nullptr);
    h.Steps(4);
    const float early = h.Mean(f0 + 8, f0 + kDelay);
    const float late = h.Mean(f0 + kDelay, f0 + 1024);
    if (fits) {
      SND_CHECK_MSG(std::fabs(early) < 1e-6f && std::fabs(late - 0.5f * kCenter) < 1e-5f, "k %u: not the full setting",
                    k);
    } else {
      SND_CHECK_MSG(std::fabs(early - 0.5f * kCenter) < 1e-5f, "k %u: refused call left a setting (early %.4f)", k,
                    early);
    }
  }
}

// Several threads record settings of their own Event at once (each through its own arena):
// whatever was accepted last per thread is what applies; nothing partial.
SND_TEST(IndepG2Recheck, AllOrNoneFromSeveralThreads) {
  Fx h(SmallArena());
  SND_REQUIRE(h.sys != nullptr);
  FxBank fb;
  const char* names[3] = {"T0", "T1", "T2"};
  for (const char* nm : names) fb.Event(nm, 500.0f);
  h.Load("v", fb.Build("V"));
  std::atomic<std::uint32_t> go{0};
  float lastOk[3] = {0, 0, 0};
  std::uint32_t refused[3] = {0, 0, 0};
  std::vector<std::thread> th;
  for (int t = 0; t < 3; ++t) {
    th.emplace_back([&, t] {
      if (h.sys->RegisterThread() != SND_OK) return;
      while (go.load() == 0u) std::this_thread::yield();
      for (int i = 0; i < 400; ++i) {
        const float ms = 5.0f + float((i * 7 + t * 3) % 20);
        const std::vector<float> p = {ms, 0.0f, 0.0f, 1.0f, 30.0f};
        const snd_result r = h.sys->SetEventVoiceEffect(HashId(names[t]), 0, dsp::kEffectEcho, p.data(), 5);
        if (r == SND_OK) lastOk[t] = ms;
        else ++refused[t];
        if (i % 97 == 96) h.sys->Flush();
      }
      h.sys->Flush();
      h.sys->UnregisterThread();
    });
  }
  go.store(1u);
  for (int i = 0; i < 400; ++i) h.Step();  // the owner keeps applying meanwhile
  for (auto& x : th) x.join();
  h.Steps(3);
  for (int t = 0; t < 3; ++t) {
    SND_REQUIRE(lastOk[t] > 0.0f);
    const std::size_t f0 = h.Total();
    const snd_playback_handle p = h.sys->Play(HashId(names[t]), NoEmitter(), nullptr);
    h.Steps(8);
    const std::size_t on = FirstAbove(h, f0, 0.1f);
    const std::size_t exp = static_cast<std::size_t>(lastOk[t] * 48.0f + 0.5f);
    SND_CHECK_MSG(on != static_cast<std::size_t>(-1) && on - f0 == exp, "thread %d: onset %lld, last accepted %.0f ms (%u refused)",
                  t, on == static_cast<std::size_t>(-1) ? -1LL : static_cast<long long>(on - f0), lastOk[t], refused[t]);
    h.sys->Stop(p, 0.0f, nullptr);
    h.Steps(3);
  }
}

// Per-playback values on two stages, set while the voice is virtual: the first block of the slot
// already has both values (start == end), on both stages.
SND_TEST(IndepG2Recheck, FirstBlockHasThePlaybackValuesOnEveryStage) {
  Log().Reset();
  const snd_dsp_effect probe = ProbePlugin();
  snd_config c = G2Config();
  c.max_voice_effects = 4;
  c.max_real_voices = 1;
  c.plugins.effect_count = 1;
  c.plugins.effects = &probe;
  Fx h(c);
  SND_REQUIRE(h.sys != nullptr);
  FxBank fb;
  fb.Event("H", 200.0f, 0, 200);
  fb.Event("A", 500.0f, 0, 100);
  h.Load("v", fb.Build("V"));
  const float one[] = {1.0f, 0.0f, 0.0f};
  SND_REQUIRE_EQ(h.sys->SetEventVoiceEffect(HashId("A"), 0, kProbeId, one, 3), SND_OK);
  SND_REQUIRE_EQ(h.sys->SetEventVoiceEffect(HashId("A"), 1, kProbeId, one, 3), SND_OK);
  h.Steps(1);
  const snd_playback_handle hp = h.sys->Play(HashId("H"), NoEmitter(), nullptr);
  h.Steps(2);
  const snd_playback_handle a = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(2);
  h.sys->SetPlaybackEffectParam(a, 0, 0, 0.25f);
  h.sys->SetPlaybackEffectParam(a, 1, 0, 0.5f);
  h.sys->SetPlaybackEffectParam(a, 1, 2, 7.0f);  // a second value on stage 1 (unused by the probe)
  h.Steps(4);
  SND_CHECK_EQ(Log().Count(), 0u);
  h.sys->Stop(hp, 0.0f, nullptr);
  h.Steps(4);
  SND_REQUIRE(Log().Count() >= 2u);
  const ProbeCall& c0 = Log().calls[0];
  const ProbeCall& c1 = Log().calls[1];
  std::printf("  first calls: (%.2f -> %.2f) (%.2f -> %.2f)\n", c0.start0, c0.end0, c1.start0, c1.end0);
  SND_CHECK(c0.stateId != c1.stateId);
  SND_CHECK_EQ(c0.start0, 0.25f);
  SND_CHECK_EQ(c0.end0, 0.25f);
  SND_CHECK_EQ(c1.start0, 0.5f);
  SND_CHECK_EQ(c1.end0, 0.5f);
  SND_CHECK_NEAR(h.LastMean(0), 0.125f * 0.5f * kCenter, 1e-6);
}

// The raw SetBusVolume variant 1 (commands.hpp BusEffectParamReserved) equals the public call bit
// for bit; an effect index past the chain is NOT_FOUND (object = bus id).
SND_TEST(IndepG2Recheck, RawVariantOneEqualsThePublicCall) {
  auto run = [](int mode) {
    RawFx h;
    std::vector<float> out;
    if (h.sys == nullptr) return out;
    FxBank fb;
    fb.b.buses[0].effects = {fb.Effect(dsp::kEffectEcho, EchoGain(1.0f))};
    fb.Event("E", 500.0f);
    h.Load("g", fb.Build("G"));
    h.sys->Play(HashId("E"), NoEmitter(), nullptr);
    h.Steps(4);
    const std::size_t f0 = h.all.size();
    if (mode == 0) h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.3f, 0.02f);
    if (mode >= 1) {
      snd::cmd::SetBusVolume p{};
      p.bus = HashId("Sfx");
      p.volume = 0.3f;
      p.fadeSeconds = 0.02f;
      p.reserved = snd::cmd::BusEffectParamReserved(mode == 1 ? 0u : 200u, 2u);
      h.Queue(snd::cmd::CommandType::SetBusVolume, p);
    }
    h.errors.clear();
    h.Steps(8);
    if (mode == 2) {
      const bool warned = h.errors.size() == 1u && h.errors[0].code == SND_ERR_NOT_FOUND && h.errors[0].object == HashId("Sfx");
      if (!warned) return std::vector<float>{};
    }
    out.assign(h.all.begin() + static_cast<std::ptrdiff_t>(f0), h.all.end());
    return out;
  };
  const std::vector<float> api = run(0), raw = run(1), far = run(2);
  SND_REQUIRE(!api.empty());
  SND_CHECK(api == raw);
  SND_CHECK(!far.empty());  // warned once with the bus id
}

// Determinism (serial vs 2 RT workers), no allocation, no RT violation, with voices going virtual
// and back, the Event's kind changed / removed in between, [init] warnings, all-or-none refusals.
namespace {
std::vector<float> Script(std::uint32_t workers, CountingAllocator* alloc, std::uint64_t* allocs, std::uint32_t* rt) {
  static const snd_dsp_effect probe = ProbePlugin();
  snd_config c = G2Config();
  c.max_voice_effects = 3;
  c.max_real_voices = 3;
  c.voice_effect_chain = 2;
  c.jobs.realtime_workers = workers;
  c.jobs.voices_per_chunk = 1;
  c.plugins.effect_count = 1;
  c.plugins.effects = &probe;
  Fx h(c, {}, alloc);
  std::vector<float> out;
  if (h.sys == nullptr) return out;
  FxBank fb;
  fb.b.buses[0].effects = {fb.Effect(dsp::kEffectEcho, {7.0f, 0.3f, 1.0f, 0.5f, 20.0f})};
  fb.Event("H", 150.0f, 0, 200);
  fb.Event("A", 1000.0f, 0, 100);
  fb.Event("B", 2000.0f + 6000.0f, 0, 120);
  h.Load("v", fb.Build("V"));
  const std::vector<float> d10 = EchoDelay(10.0f);
  const float half = 0.5f;
  h.sys->SetEventVoiceEffect(HashId("A"), 0, dsp::kEffectEcho, d10.data(), 5);
  h.sys->SetEventVoiceEffect(HashId("B"), 1, kProbeId, &half, 1);
  h.Steps(2);
  const std::uint64_t before = alloc != nullptr ? alloc->allocations.load() : 0u;
  snd::test::rt::Reset();
  snd_playback_handle hs[3] = {}, as[2] = {};
  for (int b = 0; b < 60; ++b) {
    if (b == 0 || b == 1) as[b] = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
    if (b == 2) h.sys->Play(HashId("B"), NoEmitter(), nullptr);
    if (b % 12 == 4) {
      for (auto& x : hs) x = h.sys->Play(HashId("H"), NoEmitter(), nullptr);  // A, B -> virtual
    }
    if (b % 12 == 7) h.sys->SetEventVoiceEffect(HashId("A"), 0, b % 24 == 7 ? kProbeId : SND_ID_NONE, &half, 1);
    if (b % 12 == 9) {
      for (auto& x : hs) h.sys->Stop(x, 0.0f, nullptr);  // back to real: re-read
    }
    if (b % 12 == 10) {
      h.sys->SetEffectParam(HashId("Sfx"), 0, 4, 1.0f, 0.0f);         // [init]: warning
      h.sys->SetPlaybackEffectParam(as[0], 0, 0, 0.1f * float(b % 7));  // probe gain or echo delay
      h.sys->SetEventVoiceEffect(HashId("A"), 0, dsp::kEffectEcho, d10.data(), 5);
    }
    h.sys->Update();
    tAudioThread = true;
    {
      SND_RT_SCOPE();
      h.sys->RenderBlock(h.out.data());
    }
    h.all.insert(h.all.end(), h.out.begin(), h.out.end());
  }
  if (allocs != nullptr && alloc != nullptr) *allocs = alloc->allocations.load() - before;
  if (rt != nullptr) *rt = snd::test::rt::UnexpectedTotal();
  return h.all;
}
}  // namespace

SND_TEST(IndepG2Recheck, ReacquireAndWarningsAreDeterministicAllocationFreeAndRtSafe) {
  const std::vector<float> a = Script(0, nullptr, nullptr, nullptr), b = Script(0, nullptr, nullptr, nullptr);
  CountingAllocator alloc;
  std::uint64_t allocs = 1;
  std::uint32_t rt = 1;
  const std::vector<float> c = Script(2, &alloc, &allocs, &rt);
  SND_REQUIRE(!a.empty());
  SND_CHECK(AllFinite(a));
  SND_CHECK(MaxAbsAll(a) > 0.1f);
  SND_CHECK(a == b);
  SND_CHECK(a == c);
  SND_CHECK_EQ(allocs, 0u);
  SND_CHECK_EQ(rt, 0u);
}
