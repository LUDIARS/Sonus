// Properties 07, 09..13. Measure each path independently at the master output.
#include <limits>

#include "indep_g1_scene.hpp"
#include "indep_g2_fixture.hpp"
#include "snd/test/rt_guard.hpp"

namespace {
using namespace indep_g1;

snd_emitter_handle Zone(Scene& s, snd_vec3 position = {}, float amount = 1, float fadeDistance = 4,
                        snd::Id bus = kWet) {
  const auto zone = s.h.sys->CreateEmitter();
  const snd_vec3 size{1, 1, 1};
  SND_CHECK_EQ(s.h.sys->SetTransform(zone, &position, &kFront, nullptr), SND_OK);
  SND_CHECK_EQ(s.h.sys->SetEmitterShape(zone, SND_EMITTER_SHAPE_BOX, &size), SND_OK);
  SND_CHECK_EQ(s.h.sys->SetReverbZone(zone, bus, amount, fadeDistance), SND_OK);
  return zone;
}

// TestBank does not expose EVNT.curveReverbSend. Supply an event table built
// from the frozen format and copy the remaining serialized fixture tables.
std::vector<std::uint8_t> WithDistanceSend(float amount, bool sendRow = true) {
  auto bank = Signal(true);
  bank.buses[0].sends.clear();
  if (sendRow) bank.buses[0].sends.push_back(TBus::Send{1, -120.0f, 0});
  Curve(bank, bf::CurveTarget::ReverbSend, amount, amount);
  const auto bytes = bank.Build(HashId("Scene"));
  bf::FileHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  bt::BankWriter writer;
  for (std::uint32_t i = 0; i < header.chunkCount; ++i) {
    bf::ChunkEntry entry;
    std::memcpy(&entry, bytes.data() + header.chunkTableOffset + i * sizeof(entry), sizeof(entry));
    if (entry.fourcc == bf::chunk::EVNT) continue;
    const auto* first = bytes.data() + entry.offset;
    writer.AddBlob(entry.fourcc, {first, first + entry.bytes}, entry.flags);
  }
  bt::TableBuilder event(bf::chunk::EVNT);
  auto row = event.AddRow();
  event.U32(bf::col::EVNT_id, row, kEvent);
  event.U32(bf::col::EVNT_trackFirst, row, 0);
  event.U32(bf::col::EVNT_trackCount, row, 1);
  event.U32(bf::col::EVNT_bus, row, 0);
  event.U32(bf::col::EVNT_flags, row, bf::kEvent3D);
  event.U32(bf::col::EVNT_curveReverbSend, row, 0);
  writer.AddTable(event);
  bt::BankHeaderInfo info;
  info.bankId = HashId("Scene");
  return writer.Finish(info);
}

SND_TEST(IndepG1Routing, DistanceSendUsesFirstBusSendAndSaturatesWithZone) {
  auto level = [](float send, float zone, bool present) {
    Scene s(WithDistanceSend(send, present));
    if (zone > 0) Zone(s, {}, zone);
    s.Start();
    return s.Level();
  };
  const double residual = level(0, 0, true);
  const double full = level(1, 0, true) - residual;
  SND_REQUIRE(full > 0.04);
  SND_CHECK_NEAR((level(0.25f, 0, true) - residual) / full, 0.25, 0.002);
  SND_CHECK_NEAR((level(0.75f, 0.5f, true) - residual) / full, 1.0, 0.002);
  SND_CHECK_EQ(level(1, 0, false), 0.0);
}

SND_TEST(IndepG1Routing, ListenerPositionControlsContinuousZoneFade) {
  auto b = Signal(true);
  b.events[0].curveVolume = Curve(b, bf::CurveTarget::Volume, 0, 0);
  Scene s(b);
  Zone(s, {}, 0.8f, 4);
  s.Position({0, 0, 8});  // stays outside the zone at every position
  s.Start();
  const double inside = s.Level();
  SND_REQUIRE(inside > 0.04);
  for (float z : {1.0f, 3.0f, 5.0f, 6.0f}) {
    auto listener = Listener({0, 0, z});
    SND_CHECK_EQ(s.h.sys->SetListener(0, &listener), SND_OK);
    s.h.Steps(8);
    const double weight = std::max(0.0, 1.0 - (z - 1.0) / 4.0);
    SND_CHECK_NEAR(s.Level() / inside, weight, 0.003);
  }
}

SND_TEST(IndepG1Routing, EachListenerContributesOnlyWhenInsideTheZone) {
  auto gain = [](snd_listener_mode mode, float inside, float outside, bool single) {
    auto b = Signal(true);
    b.events[0].curveVolume = Curve(b, bf::CurveTarget::Volume, 0, 0);
    auto cfg = Config();
    cfg.listener_mode = mode;
    Scene s(b, cfg);
    Zone(s, {}, 1, 0);
    const auto first = Listener({}, inside);
    SND_CHECK_EQ(s.h.sys->SetListener(0, &first), SND_OK);
    if (!single) {
      const auto second = Listener({0, 0, 4}, outside);
      SND_CHECK_EQ(s.h.sys->SetListener(1, &second), SND_OK);
    }
    s.Position({0, 0, 8});  // in front of both, equal distance-curve gain
    s.Start();
    return s.Level();
  };
  const double full = gain(SND_LISTENER_SPLIT, 1, 1, true);
  SND_REQUIRE(full > 0.05);
  SND_CHECK_NEAR(gain(SND_LISTENER_SPLIT, 1, 1, false) / full, 0.5, 0.002);
  SND_CHECK_NEAR(gain(SND_LISTENER_WEIGHTED, 3, 1, false) / full, 0.75, 0.002);
  SND_CHECK_NEAR(gain(SND_LISTENER_WEIGHTED, 1, 3, false) / full, 0.25, 0.002);
}

SND_TEST(IndepG1Routing, OnlyListenersInsideZoneDetermineSendPan) {
  enum class Seats { LeftOnly, BothInside, LeftInside };
  auto stereo = [](Seats seats, snd_listener_mode mode) {
    auto cfg = Config();
    cfg.listener_mode = mode;
    Scene s(Signal(true), cfg);
    auto left = Listener({-1, 0, 0});
    auto right = Listener({1, 0, 0});
    SND_CHECK_EQ(s.h.sys->SetListener(0, &left), SND_OK);
    if (seats != Seats::LeftOnly) SND_CHECK_EQ(s.h.sys->SetListener(1, &right), SND_OK);
    // The left seat hears a source to its right; the right seat hears it to its left.
    Zone(s, seats == Seats::BothInside ? kOrigin : snd_vec3{-1, 0, 0}, 1, 0);
    s.Position(kOrigin);
    s.Start();
    return std::pair{s.Level(0), s.Level(1)};
  };
  for (auto mode : {SND_LISTENER_SPLIT, SND_LISTENER_WEIGHTED}) {
    const auto solo = stereo(Seats::LeftOnly, mode);
    SND_REQUIRE(solo.second > 0.08 && solo.first < 0.001);
    const auto all = stereo(Seats::BothInside, mode);
    SND_CHECK_NEAR(all.first + all.second, solo.second, 0.001);
    SND_CHECK_NEAR(all.first, all.second, 0.001);
    const auto one = stereo(Seats::LeftInside, mode);
    SND_CHECK_NEAR(one.first, 0, 0.001);
    SND_CHECK_NEAR(one.second, 0.5 * solo.second, 0.001);
  }
}

SND_TEST(IndepG1Routing, ClearingZoneAndReusingEmitterRemoveTheSend) {
  Scene s(Signal(true));
  auto z = Zone(s);
  s.Start();
  const double on = s.Level();
  SND_REQUIRE(on > 0.05);
  SND_CHECK_EQ(s.h.sys->SetReverbZone(z, SND_ID_NONE, 0, 0), SND_OK);
  s.h.Steps(8);
  SND_CHECK_EQ(s.Level(), 0.0);
  SND_CHECK_EQ(s.h.sys->SetReverbZone(z, kWet, 0.5f, 0), SND_OK);
  s.h.Steps(8);
  SND_CHECK_NEAR(s.Level(), 0.5 * on, 0.001);
  SND_CHECK_EQ(s.h.sys->DestroyEmitter(z), SND_OK);
  s.h.Steps(8);
  auto next = s.h.sys->CreateEmitter();
  SND_REQUIRE(next.value != z.value && next.value != 0);
  s.h.Steps(8);
  SND_CHECK_EQ(s.Level(), 0.0);
}

SND_TEST(IndepG1Routing, TwoDimensionalEventNeverFeedsTheZone) {
  auto b = Signal(true);
  b.events[0].flags = 0;
  Scene s(b);
  Zone(s);
  s.Start();
  SND_CHECK_EQ(s.Level(), 0.0);
}

SND_TEST(IndepG1Routing, StrongestFourZonesKeepIndependentDestinations) {
  // Four stronger zones feed an unrelated output. The fifth would be audible
  // at the master if selected. Raising its weight must displace the fourth.
  auto b = Signal(true);
  Scene s(b);
  for (float amount : {0.9f, 0.8f, 0.7f, 0.6f}) Zone(s, {}, amount, 4, kDry);
  const auto fifth = Zone(s, {}, 0.5f, 4);
  s.Start();
  SND_CHECK_EQ(s.Level(), 0.0);
  SND_CHECK_EQ(s.h.sys->SetReverbZone(fifth, kWet, 1.0f, 4), SND_OK);
  s.h.Steps(8);
  SND_CHECK(s.Level() > 0.05);
}

SND_TEST(IndepG1Routing, RemovingZoneRampsWithinTheBlock) {
  Scene s(Signal(true));
  const auto zone = Zone(s);
  s.Start();
  const float before = s.h.out[s.h.out.size() - 2];
  SND_REQUIRE(before > 0.04f);
  SND_CHECK_EQ(s.h.sys->SetReverbZone(zone, SND_ID_NONE, 0, 0), SND_OK);
  s.h.Step();
  const auto halfway = s.h.out[s.h.Frames()];
  SND_CHECK(s.h.out[0] > 0.9f * before);
  SND_CHECK(halfway > 0.1f * before && halfway < 0.9f * before);
  s.h.Steps(3);
  SND_CHECK_EQ(s.Level(), 0.0);
}

SND_TEST(IndepG1Routing, PartialZoneCommandNeverChangesItsDestination) {
  auto cfg = Config();
  cfg.commands.arena_bytes = 4096;
  cfg.commands.reserve_bytes = 256;
  Scene s(Signal(true), cfg);
  const auto zone = Zone(s);
  s.Start();
  const double before = s.Level();
  SND_REQUIRE(before > 0.05);
  // 32-byte parameter records: 128 fill normal storage, seven leave one
  // record in the reserve. A zone change needs two records.
  for (int n = 0; n < 135; ++n)
    SND_CHECK_EQ(s.h.sys->SetParamGlobal(HashId("unused"), 0.5f), SND_OK);
  SND_CHECK_EQ(s.h.sys->SetReverbZone(zone, kDry, 0.25f, 0), SND_ERR_COMMAND_BUFFER_FULL);
  s.h.Steps(4);
  SND_CHECK_NEAR(s.Level(), before, 0.0001);
}

SND_TEST(IndepG1Routing, PausedAndVirtualPlaybackLeaveNoZoneSignal) {
  auto cfg = Config();
  cfg.max_real_voices = 1;
  auto bank = Signal(true);
  auto other = SimpleEvent("Competing2D", 0);
  other.priority = 255;
  other.tracks[0].clips[0].loopCount = 0;
  bank.events.push_back(other);
  Scene s(bank, cfg);
  Zone(s);
  const auto p = s.Play();
  s.h.Steps(8);
  const double on = s.Level();
  SND_REQUIRE(on > 0.04);
  SND_CHECK_EQ(s.h.sys->Pause(p, 0), SND_OK);
  s.h.Steps(10);
  SND_CHECK_EQ(s.Level(), 0.0);
  SND_CHECK_EQ(s.h.sys->ResumePlayback(p, 0), SND_OK);
  s.h.Steps(10);
  SND_CHECK_NEAR(s.Level(), on, 0.001);
  // The 2D winner never feeds the zone. Keep it playing until the loser
  // has completed its steal fade and released the real slot.
  const auto competitor = s.h.sys->Play(HashId("Competing2D"), {}, nullptr);
  SND_REQUIRE(competitor.value != 0u);
  s.h.Steps(6);
  snd_stats stats{};
  SND_CHECK_EQ(s.h.sys->GetStats(&stats), SND_OK);
  SND_REQUIRE(stats.virtual_voices > 0);
  SND_CHECK_EQ(s.Level(), 0.0);
  SND_CHECK_EQ(s.h.sys->Stop(competitor, 0, nullptr), SND_OK);
  s.h.Steps(8);
  SND_CHECK_NEAR(s.Level(), on, 0.001);
}

double FilteredLevel(bool wet, const char* parameter, float hz) {
  Scene s(Signal(wet, hz));
  if (wet) Zone(s);
  if (parameter) SND_CHECK_EQ(s.h.sys->SetParamEmitter(s.em, HashId(parameter), 1), SND_OK);
  s.Start();
  s.h.Steps(24);
  return s.Level();
}

SND_TEST(IndepG1Routing, ObstructionFiltersOnlyDirectWhileOcclusionFiltersBoth) {
  for (bool wet : {false, true}) {
    const double low = FilteredLevel(wet, nullptr, 375);
    const double high = FilteredLevel(wet, nullptr, 6000);
    SND_REQUIRE(low > 0.02 && high > 0.02);
    for (const auto* parameter : {"snd.occlusion", "snd.obstruction"}) {
      const double l = FilteredLevel(wet, parameter, 375) / low;
      const double h = FilteredLevel(wet, parameter, 6000) / high;
      if (wet && std::strcmp(parameter, "snd.obstruction") == 0) {
        SND_CHECK_NEAR(l, 1.0, 0.001);
        SND_CHECK_NEAR(h, 1.0, 0.001);
      } else {
        SND_CHECK(l < 0.9 && l > 0.01);
        SND_CHECK(h < l * 0.75);
      }
    }
  }
}

SND_TEST(IndepG1Routing, DistanceAndOcclusionChooseTheLowerCutoff) {
  // The lower, pre-existing 300 Hz cutoff already dominates. Additional
  // obstruction or occlusion changes the gain, but not the relative spectrum.
  auto ratio = [](bool wet, const char* parameter) {
    auto level = [&](float hz) {
      auto b = Signal(wet, hz);
      b.events[0].curveLowpass = Curve(b, bf::CurveTarget::Lowpass, 300, 300);
      Scene s(b);
      if (wet) Zone(s);
      if (parameter) SND_CHECK_EQ(s.h.sys->SetParamEmitter(s.em, HashId(parameter), 1), SND_OK);
      s.Start();
      s.h.Steps(32);
      return s.Level();
    };
    return level(1500) / level(187.5f);
  };
  for (bool wet : {false, true}) {
    const auto baseline = ratio(wet, nullptr);
    SND_REQUIRE(baseline > 1e-6 && baseline < 0.2);
    SND_CHECK_NEAR(ratio(wet, "snd.occlusion") / baseline, 1, 0.03);
    if (!wet) SND_CHECK_NEAR(ratio(false, "snd.obstruction") / baseline, 1, 0.03);
  }
}

SND_TEST(IndepG1Routing, InvalidZoneAndShapeDoNotOverwriteValidSettings) {
  Scene s(Signal(true));
  auto zone = Zone(s);
  s.Start();
  const double initial = s.Level();
  SND_REQUIRE(initial > 0.04);
  for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    const snd_vec3 extent{invalid, 1, 1};
    SND_CHECK_NE(s.h.sys->SetEmitterShape(zone, SND_EMITTER_SHAPE_BOX, &extent), SND_OK);
    SND_CHECK_NE(s.h.sys->SetReverbZone(zone, kWet, invalid, 4), SND_OK);
    SND_CHECK_NE(s.h.sys->SetReverbZone(zone, kWet, 0, invalid), SND_OK);
    s.h.Steps(8);
    SND_CHECK_NEAR(s.Level(), initial, 0.0001);
  }
}

SND_TEST(IndepG1Routing, UnknownParameterVariantLeavesOrdinaryEmitterParameterUntouched) {
  enum class Command { NoChange, Public, Legacy, Unknown };
  auto wave = [](Command command) {
    g2::RawFx rig(Config());
    g2::FxBank bank;
    bank.Event("E", 300.0f).flags = bf::kEvent3D;
    rig.Load("raw", bank.Build("Raw"));
    const auto em = rig.sys->CreateEmitter();
    const auto listener = Listener();
    SND_CHECK_EQ(rig.sys->SetListener(0, &listener), SND_OK);
    SND_CHECK_EQ(rig.sys->SetTransform(em, &kFront, nullptr, nullptr), SND_OK);
    rig.sys->Play(HashId("E"), em, nullptr);
    rig.Steps(4);
    if (command == Command::Public) {
      SND_CHECK_EQ(rig.sys->SetParamEmitter(em, HashId("snd.occlusion"), 1), SND_OK);
    } else if (command != Command::NoChange) {
      snd::cmd::SetParam param{};
      param.target = snd::HandleRef::FromValue(em.value);
      param.scope = static_cast<std::uint8_t>(snd::ParamScope::Emitter);
      param.param = HashId("snd.occlusion");
      param.value = 1;
      param.reserved[0] = command == Command::Unknown ? 255 : 0;
      rig.Queue(snd::cmd::CommandType::SetParam, param);
    }
    rig.Steps(8);
    return rig.all;
  };
  const auto ordinary = wave(Command::Public);
  const auto original = wave(Command::NoChange);
  SND_REQUIRE(ordinary != original);
  SND_CHECK(ordinary == wave(Command::Legacy));
  SND_CHECK(original == wave(Command::Unknown));
}

std::vector<float> Replay(std::uint32_t workers) {
  auto cfg = Config();
  cfg.jobs.realtime_workers = workers;
  cfg.jobs.voices_per_chunk = 2;
  cfg.listener_mode = SND_LISTENER_SPLIT;
  CountingAllocator alloc;
  Scene s(Signal(true, 375), cfg, &alloc);
  Zone(s, {}, 0.7f);
  const auto second = Listener({0.5f, 0, 0});
  SND_CHECK_EQ(s.h.sys->SetListener(1, &second), SND_OK);
  auto before = alloc.allocations.load();
  std::vector<snd_emitter_handle> emitters;
  for (int i = 0; i < 10; ++i) {
    const auto em = s.h.sys->CreateEmitter();
    emitters.push_back(em);
    const snd_vec3 half{0.2f, 0.3f, 0.4f};
    const snd_vec3 pos{static_cast<float>(i) * 0.2f, 0, 1};
    SND_CHECK_EQ(s.h.sys->SetEmitterShape(em, SND_EMITTER_SHAPE_BOX, &half), SND_OK);
    SND_CHECK_EQ(s.h.sys->SetTransform(em, &pos, &kFront, nullptr), SND_OK);
    SND_CHECK(s.h.sys->Play(kEvent, em, nullptr).value != 0);
  }
  s.h.Clear();
  for (int i = 0; i < 12; ++i) {
    snd_vec3 vel{1, 0, -4};
    snd_vec3 pos{1, 0, 2.0f - static_cast<float>(i) * 0.02f};
    auto em = emitters[static_cast<std::size_t>(i) % emitters.size()];
    SND_CHECK_EQ(s.h.sys->SetTransform(em, &pos, &kFront, &vel), SND_OK);
    SND_CHECK_EQ(s.h.sys->SetParamEmitter(em, HashId("snd.occlusion"), 0.3f), SND_OK);
    s.h.Step();
  }
  snd_stats stats{};
  s.h.sys->Update();
  SND_CHECK_EQ(s.h.sys->GetStats(&stats), SND_OK);
  SND_CHECK_EQ(stats.late_chunks, 0u);
  SND_CHECK_EQ(alloc.allocations.load(), before);
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  SND_CHECK(s.Level() > 0.01);
  return s.h.all;
}

SND_TEST(IndepG1Routing, SpatialChangesRepeatBitForBitAcrossWorkers) {
  const auto first = Replay(0);
  for (auto workers : {0u, 2u}) {
    const auto next = Replay(workers);
    SND_REQUIRE_EQ(first.size(), next.size());
    SND_CHECK_EQ(std::memcmp(first.data(), next.data(), first.size() * sizeof(float)), 0);
  }
}
}  // namespace
