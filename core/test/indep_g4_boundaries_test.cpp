#include <algorithm>

#include "indep_g4_fixture.hpp"
#include "snd/music/music_system.hpp"

using namespace g4;

namespace {
struct LoopAudio {
  std::vector<float> sound;
  bool completed;
  std::uint32_t ends;
};

LoopAudio PlayLoop(std::uint32_t loops) {
  Rig r;
  B b;
  std::vector<float> pcm(600, 0.25f);
  std::fill_n(pcm.begin(), 64, 0.75f);
  auto clip = B::AssetClip(b.Asset("cycle", pcm));
  clip.loopCount = loops;
  TParam p;
  p.id = HashId("speed");
  b.b.params.push_back(p);
  b.b.curves.push_back(TCurve{p.id, 1, 1, {{0, 0}, {1, 1200}}});
  b.Event("Loop", {clip}).curves = {0};
  r.Load("loop", b.Build("bank"));
  const auto start = r.Now();
  const auto h = r.sys->Play(HashId("Loop"), NoEm(), nullptr);
  r.Steps(5);
  r.sys->SetParamPlayback(h, p.id, 1.0f);
  r.Steps(6);
  LoopAudio result{{}, r.Ended(h.value, SND_END_COMPLETED, SND_END_DETAIL_NONE), r.CountEnds(h.value)};
  for (auto f = start; f < r.Now(); ++f) result.sound.push_back(r.At(f));
  return result;
}

TAction PulseAt(float ms) {
  return TAction{ms, 0, HashId("Pulse"), 0.0f};
}
}  // namespace

SND_TEST(IndepG4Loop, PitchChangeCutsFiniteLoopAtMeasuredWrap) {
  const auto reference = PlayLoop(0);
  const auto finite = PlayLoop(3);
  std::vector<std::size_t> wraps;
  const float threshold = 0.5f * kC;
  for (std::size_t f = 1; f < reference.sound.size(); ++f) {
    if (reference.sound[f] > threshold && reference.sound[f - 1] <= threshold) wraps.push_back(f);
  }
  // Ignore startup gain. The first material wrap occurs one 600-frame cycle after start.
  const auto first = std::find_if(wraps.begin(), wraps.end(), [](std::size_t f) { return f > 500; });
  SND_REQUIRE(wraps.end() - first >= 3);
  SND_CHECK_EQ(first[1] - first[0], 600u);
  SND_CHECK(first[2] - first[1] < 600u);
  const auto cut = first[2];
  snd::test::Note("infinite loop wraps at %zu, %zu, %zu output frames", first[0], first[1], cut);
  SND_REQUIRE_EQ(reference.sound.size(), finite.sound.size());
  SND_CHECK(finite.sound[cut - 1] > 0.1f * kC);
  for (std::size_t f = cut; f < finite.sound.size(); ++f) SND_REQUIRE_EQ(finite.sound[f], 0.0f);
  SND_CHECK_EQ(reference.ends, 0u);
  SND_CHECK_EQ(finite.ends, 1u);
  SND_CHECK(finite.completed);
}

SND_TEST(IndepG4Recording, PlayModesLeaveReservedBitsClear) {
  struct Capture {
    std::uint32_t flags[6]{};
    std::size_t count = 0;
  } capture;
  snd::core::LiveTap tap{};
  tap.user = &capture;
  tap.arena = [](void* user, const snd::core::LiveArenaView& arena) noexcept {
    auto& log = *static_cast<Capture*>(user);
    auto observe = [&](const snd::cmd::CommandHeader& h, const std::uint8_t* bytes) {
      if (h.type != static_cast<std::uint16_t>(snd::cmd::CommandType::Play)) return;
      snd::cmd::Play play{};
      std::memcpy(&play, bytes, sizeof(play));
      if (log.count < 6) log.flags[log.count] = play.flags;
      ++log.count;
    };
    if (arena.normalBytes) snd::cmd::ForEachCommand(arena.normal, arena.normal + arena.normalBytes, observe);
    if (arena.reserveBytes) snd::cmd::ForEachCommand(arena.reserve, arena.reserve + arena.reserveBytes, observe);
  };
  Rig r(Config(), {}, nullptr, nullptr, tap);
  B bank;
  const auto id = HashId("Tone");
  bank.Event("Tone", {B::SynthClip(bank.Synth(100.0f))});
  r.Load("b", bank.Build("B"));
  const snd_vec3 pos{};
  SND_REQUIRE(r.sys->Play(id, NoEm(), nullptr).value != 0u);
  SND_REQUIRE_EQ(r.sys->PlayOneShot(id, NoEm(), nullptr), SND_OK);
  SND_REQUIRE_EQ(r.sys->PlayAt(id, &pos, nullptr), SND_OK);
  for (float fade : {0.0f, 0.123f, 70.0f}) SND_REQUIRE(r.sys->PlayFade(id, NoEm(), nullptr, fade).value != 0u);
  r.Step();
  const std::uint32_t expected[] = {0u, 1u, 3u, 0u, 123u << 16, 65535u << 16};
  SND_REQUIRE_EQ(capture.count, 6u);
  for (std::size_t i = 0; i < 6; ++i) SND_CHECK_EQ(capture.flags[i], expected[i]);
}

SND_TEST(IndepG4Action, ScheduledStopCancelsAtItsFrame) {
  for (int delta : {-1, 0, 1}) {
    Rig r;
    B b;
    b.Event("Parent", {B::SynthClip(b.Synth(0.0f))}).tracks[0].actions = {PulseAt(10.0f)};
    b.Event("Pulse", {B::AssetClip(b.Asset("dc", Constant(32, 0.5f)))});
    r.Load("b", b.Build("B"));
    const auto start = r.Now();
    const auto parent = r.sys->Play(HashId("Parent"), NoEm(), nullptr);
    r.Step();
    const auto stop = AtClock(start + static_cast<std::size_t>(480 + delta));
    SND_REQUIRE_EQ(r.sys->SetScheduledStop(parent, &stop, 0.0f), SND_OK);
    r.Steps(5);
    const auto heard = r.CountAbove(start, r.Now(), 0.25f * kC);
    SND_CHECK_EQ(heard, delta > 0 ? 32u : 0u);
    if (delta > 0) SND_CHECK_EQ(r.FirstAbove(start, r.Now(), 0.25f * kC), start + 480);
  }
}

SND_TEST(IndepG4Action, MusicalReservationAnchorsTrackTime) {
  snd::music::MusicSystem music;
  Rig r(Config(), {}, nullptr, nullptr, {}, &music);
  B b;
  b.Event("Parent", {B::SynthClip(b.Synth(0.0f))}).tracks[0].actions = {PulseAt(10.0f)};
  b.Event("Pulse", {B::AssetClip(b.Asset("dc", Constant(32, 0.5f)))});
  r.Load("b", b.Build("B"));
  const auto start = r.Now();
  const snd_transport_desc desc{120.0f, 4, 4, 1, 0};
  const auto tr = r.sys->CreateTransport(&desc);
  SND_REQUIRE(tr.value != 0u);
  r.Steps(3);
  snd_when next{};
  next.kind = SND_WHEN_NEXT_BEAT;
  next.transport = tr;
  SND_REQUIRE(r.sys->Play(HashId("Parent"), NoEm(), &next).value != 0u);
  r.Steps(100);
  SND_CHECK_EQ(r.FirstAbove(start, r.Now(), 0.25f * kC), start + 24000 + 480);
  SND_CHECK_EQ(r.CountAbove(start, r.Now(), 0.25f * kC), 32u);
}
