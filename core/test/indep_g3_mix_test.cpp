// Stage 3 audio-level observations. The silent key output is separate from
// Master, so changes on Master measure only the continuing target playback.
#include <array>
#include "indep_g2_fixture.hpp"

namespace {
using namespace g2;
using snd::HashId;

float Linear(float db) { return std::pow(10.0f, db / 20.0f); }

void KeyBus(FxBank& bank) {
  bank.b.buses = {
      TBus{HashId("Key"), kNone, 2, bf::kBusOutput, 0, {}, {}},
      TBus{HashId("Target"), 2, 2, 0, 0, {}, {}},
      TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0, {}, {}}};
}

void SharedKey(bool compressor) {
  Fx h(G2Config());
  SND_REQUIRE(h.sys != nullptr);
  FxBank owner;
  KeyBus(owner);
  owner.b.buses.erase(owner.b.buses.begin() + 1); // The target belongs only to the survivor.
  owner.Event("OldKey", 960, 0);
  const auto first = h.Load("owner", owner.Build("Owner"));
  SND_REQUIRE_EQ(h.sys->GetBankState(first), SND_BANK_STATE_LOADED);

  FxBank survivor;
  KeyBus(survivor);
  if (compressor) {
    survivor.b.buses[1].effects = {survivor.Effect(dsp::kEffectCompressor,
      {-24, 8, 0.1f, 1, 0, 0, 0, 1})};
  }
  survivor.b.ducks.push_back(TDuck{0, 0, 1, compressor ? 0.0f : 6.0f, 1, 2, -30});
  survivor.Event("Bed", 80, 1);
  survivor.Event("NewKey", 960, 0);
  const auto second = h.Load("survivor", survivor.Build("Survivor"));
  SND_REQUIRE_EQ(h.sys->GetBankState(second), SND_BANK_STATE_LOADED);
  const auto bed = h.sys->Play(HashId("Bed"), NoEmitter(), nullptr);
  h.Steps(20);
  const float base = h.LastMean();
  SND_REQUIRE(base > 0.035f);
  h.sys->Play(HashId("OldKey"), NoEmitter(), nullptr);
  h.Steps(40);
  const float keyed = h.LastMean();
  SND_CHECK(keyed < base * 0.8f);
  SND_REQUIRE_EQ(h.sys->UnloadBank(first, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  h.Steps(90);
  const float restored = h.LastMean();
  SND_CHECK_NEAR(restored, base, 1e-4f);
  SND_CHECK_EQ(h.CountEnds(bed.value), 0u);
  h.sys->Play(HashId("NewKey"), NoEmitter(), nullptr);
  h.Steps(40);
  SND_CHECK_NEAR(h.LastMean(), keyed, 1e-4f);
  std::printf("  shared key: %.6f -> %.6f -> %.6f (baseline %.6f)\n", keyed, restored, h.LastMean(), base);
}

SND_TEST(IndepG3Shared, SurvivingKeyRedrivesCompressorAfterOwnerUnload) { SharedKey(true); }
SND_TEST(IndepG3Shared, SurvivingKeyRedrivesLevelDuckAfterOwnerUnload) { SharedKey(false); }

SND_TEST(IndepG3Snapshot, TwoWeightsReleaseIndependentlyToBusBase) {
  Fx h(G2Config());
  FxBank b;
  b.Event("Signal", 400);
  b.b.snapshots = {{HashId("A"), {{0, 0, 0, -12}}}, {HashId("B"), {{0, 0, 0, -8}}}};
  h.Load("bank", b.Build("Bank"));
  h.sys->Play(HashId("Signal"), NoEmitter(), nullptr);
  h.Steps(5);
  const float base = h.LastMean();
  SND_REQUIRE(base > 0.25f);
  h.sys->ApplySnapshot(HashId("A"), 0.5f, 0);
  h.sys->ApplySnapshot(HashId("B"), 0.25f, 0);
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean() / base, Linear(-8), 0.001f);
  h.sys->ApplySnapshot(HashId("A"), 0, 0);
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean() / base, Linear(-2), 0.001f);
  h.sys->ApplySnapshot(HashId("B"), 0, 0);
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean(), base, 1e-5f);
}

SND_TEST(IndepG3Param, AllowedScopesWinInPlaybackEmitterGlobalOrder) {
  for (std::uint32_t mask = 0; mask < 8; ++mask) {
    Fx h(G2Config());
    FxBank b;
    b.Event("Signal", 400).curves = {0};
    b.b.curves.push_back({HashId("Level"), 0, 0, {{0, 0}, {1, -12}}});
    b.b.params.push_back({HashId("Level"), 0, 0, 1, 0, 0, mask});
    const auto bank = h.Load("scope", b.Build("Scopes"));
    SND_REQUIRE_EQ(h.sys->GetBankState(bank), SND_BANK_STATE_LOADED);
    const auto emitter = h.sys->CreateEmitter();
    const auto playback = h.sys->Play(HashId("Signal"), emitter, nullptr);
    h.Steps(5);
    const float base = h.LastMean();
    SND_REQUIRE(base > 0.25f);
    h.sys->SetParamGlobal(HashId("Level"), 0.25f);
    h.sys->SetParamEmitter(emitter, HashId("Level"), 0.5f);
    h.sys->SetParamPlayback(playback, HashId("Level"), 1);
    h.Steps(8);
    const float db = (mask & 4) ? -12.f : (mask & 2) ? -6.f : (mask & 1) ? -3.f : 0.f;
    SND_CHECK_MSG(std::fabs(h.LastMean() / base - Linear(db)) < 0.001f,
                  "mask %u: measured %.5f, expected %.5f", mask, h.LastMean() / base, Linear(db));
  }
}

SND_TEST(IndepG3Param, TwoCurvesAddDecibelsAndClampToEndpoints) {
  Fx h(G2Config());
  FxBank b;
  b.Event("Signal", 400).curves = {0, 1};
  b.b.params.push_back({HashId("Level"), 0, -2, 2, 0, 0});
  b.b.curves = {{HashId("Level"), 0, 0, {{-1, -6}, {0, 0}, {1, -18}}},
                {HashId("Level"), 0, 0, {{-1, -8}, {0, 0}, {1, 4}}}};
  h.Load("bank", b.Build("Bank"));
  h.sys->Play(HashId("Signal"), NoEmitter(), nullptr);
  h.Steps(5);
  const float base = h.LastMean();
  SND_REQUIRE(base > 0.25f);
  for (const float x : {-2.0f, -0.5f, 0.0f, 0.5f, 2.0f}) {
    h.sys->SetParamGlobal(HashId("Level"), x);
    h.Steps(4);
    const float attenuation = -14.0f * std::fmin(1.0f, std::fabs(x));
    SND_CHECK_NEAR(h.LastMean() / base, Linear(attenuation), 0.001f);
  }
}
} // namespace

namespace {
using namespace g2;
using snd::HashId;

SND_TEST(IndepG3Snapshot, EffectContributionTracksChangingBaseAndReleases) {
  Fx h(G2Config());
  FxBank b;
  b.b.buses[0].effects = {b.Effect(dsp::kEffectEcho, EchoGain(0.5f))};
  b.Event("Signal", 400);
  b.b.snapshots.push_back({HashId("Wet"), {{1, 0, 2, 0.25f}}});
  h.Load("effects", b.Build("Effects"));
  h.sys->Play(HashId("Signal"), NoEmitter(), nullptr);
  h.Steps(10);
  const float unity = 2 * h.LastMean();
  SND_REQUIRE(unity > 0.25f);
  h.sys->ApplySnapshot(HashId("Wet"), 0.5f, 0);
  h.Steps(10);
  SND_CHECK_NEAR(h.LastMean() / unity, 0.625, 1e-3);
  h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.75f, 0);
  h.Steps(10);
  SND_CHECK_NEAR(h.LastMean() / unity, 0.875, 1e-3);
  h.sys->ApplySnapshot(HashId("Wet"), 0, 0);
  h.Steps(10);
  SND_CHECK_NEAR(h.LastMean() / unity, 0.75, 1e-3);
}

SND_TEST(IndepG3Snapshot, SendContributionsRespectPreAndPostFaderTaps) {
  for (const auto pre : {0u, 1u}) {
    Fx h(G2Config());
    FxBank b;
    // Source is an auxiliary output: the only path to Master is its send.
    b.b.buses[0].parent = kNone;
    b.b.buses[0].flags = bf::kBusOutput;
    b.b.buses[0].sends = {{1, -12, pre}};
    b.Event("Signal", 400);
    b.b.snapshots = {{HashId("SendUp"), {{2, 0, 0, 6}}}};
    h.Load("send", b.Build("Send"));
    h.sys->Play(HashId("Signal"), NoEmitter(), nullptr);
    h.Steps(6);
    const float base = h.LastMean();
    SND_REQUIRE(base > 0.05f && base < 0.1f);
    h.sys->ApplySnapshot(HashId("SendUp"), 0.5f, 0);
    h.sys->SetBusVolume(HashId("Sfx"), Linear(-6), 0);
    h.Steps(8);
    SND_CHECK_NEAR(h.LastMean() / base, Linear(pre ? 3.f : -3.f), 1e-3);
    h.sys->ApplySnapshot(HashId("SendUp"), 0, 0);
    h.Steps(8);
    SND_CHECK_NEAR(h.LastMean() / base, Linear(pre ? 0.f : -6.f), 1e-3);
  }
}

SND_TEST(IndepG3Ducking, LevelTracksDifferentOverThresholdInputsAndCaps) {
  Fx h(G2Config());
  FxBank b;
  KeyBus(b);
  b.Event("Key", 960, 0);
  b.Event("Bed", 80, 1);
  b.b.ducks.push_back({0, 0, 1, 12, 1, 1, -30});
  h.Load("duck", b.Build("Duck"));
  h.sys->Play(HashId("Bed"), NoEmitter(), nullptr);
  h.Steps(8);
  const float base = h.LastMean();
  SND_REQUIRE(base > 0.04f);
  h.sys->SetBusVolume(HashId("Key"), Linear(-40), 0);
  const auto key = h.sys->Play(HashId("Key"), NoEmitter(), nullptr);
  h.Steps(20);
  SND_CHECK_NEAR(h.LastMean(), base, 1e-4);
  h.sys->SetBusVolume(HashId("Key"), Linear(-23), 0);
  h.Steps(20);
  const float middle = h.LastMean();
  h.sys->SetBusVolume(HashId("Key"), 1, 0);
  h.Steps(20);
  SND_CHECK_MSG(middle < base && middle > h.LastMean(), "base %.6f middle %.6f full %.6f", base, middle, h.LastMean());
  SND_CHECK_NEAR(h.LastMean() / base, Linear(-12), 0.005);
  h.sys->Stop(key, 0, nullptr);
  h.Steps(80);
  SND_CHECK_NEAR(h.LastMean(), base, 1e-4);
}

SND_TEST(IndepG3Ducking, LastCategoryVoiceReleasesRule) {
  Fx h(G2Config());
  FxBank b;
  KeyBus(b);
  b.Event("Key", 960, 0).category = 0;
  b.Event("Bed", 80, 1);
  b.b.categories.push_back({HashId("Dialog"), kNone, 0, 0});
  b.b.ducks.push_back({1, 0, 1, 8, 1, 3, 0});
  h.Load("duck", b.Build("Duck"));
  h.sys->Play(HashId("Bed"), NoEmitter(), nullptr);
  h.Steps(8);
  const float base = h.LastMean();
  SND_REQUIRE(base > 0.04f);
  const auto one = h.sys->Play(HashId("Key"), NoEmitter(), nullptr);
  const auto two = h.sys->Play(HashId("Key"), NoEmitter(), nullptr);
  h.Steps(20);
  SND_CHECK_NEAR(h.LastMean() / base, Linear(-8), 0.002);
  h.sys->Stop(one, 0, nullptr);
  h.Steps(30);
  SND_CHECK_NEAR(h.LastMean() / base, Linear(-8), 0.002);
  h.sys->Stop(two, 0, nullptr);
  h.Steps(30);
  SND_CHECK_NEAR(h.LastMean() / base, 1, 0.002);
}
} // namespace

namespace {
using namespace g2;
using snd::HashId;

std::array<float, 2> PanSample(float spread, std::initializer_list<float> values, unsigned compose) {
  Fx h(G2Config());
  FxBank bank;
  auto& event = bank.Event("Signal", 400);
  event.tracks[0].clips[0].randPan = spread;
  for (const float value : values) {
    event.curves.push_back(static_cast<unsigned>(bank.b.curves.size()));
    bank.b.curves.push_back({HashId("Position"), 4, compose, {{0, value}, {1, value}}});
  }
  h.Load("pan", bank.Build("Pan"));
  h.sys->Play(HashId("Signal"), NoEmitter(), nullptr);
  h.Steps(8);
  return {h.out[0], h.out[1]};
}

SND_TEST(IndepG3Param, LegacyPanAddsCurvesToClipInStoredOrder) {
  // The common binary32 spacing around 2^24 is at least 1. Every possible
  // randomized pan in [-0.25,0.25] disappears on the first addition.
  auto middle = PanSample(0, {}, 0);
  auto initial = PanSample(0.25f, {}, 0);
  auto combined = PanSample(0.25f, {16777216.0f, -16777216.0f}, 0);
  SND_REQUIRE(middle[0] > 0.25f);
  SND_REQUIRE(initial != middle);
  SND_CHECK(std::memcmp(combined.data(), middle.data(), sizeof(middle)) == 0);
}

SND_TEST(IndepG3Param, ExplicitPanMultiplyCombinesContributions) {
  auto composed = PanSample(0, {0.2f, 0.5f}, 4);
  auto reference = PanSample(0, {0.1f}, 0);
  SND_REQUIRE(reference[1] > reference[0]);
  SND_CHECK(std::memcmp(composed.data(), reference.data(), sizeof(reference)) == 0);
}

SND_TEST(IndepG3Param, RampRateFollowsElapsedTimeRegardlessOfVoiceCount) {
  for (unsigned count : {1u, 3u}) {
    Fx h(G2Config());
    FxBank b;
    b.b.params.push_back({HashId("Level"), 0, 0, 1, 0, 2});
    b.b.curves = {{HashId("Level"), 0, 0, {{0, 0}, {1, -10}}}};
    b.Event("Signal", 80).curves = {0};
    h.Load("rate", b.Build("Rate"));
    for (unsigned i = 0; i < count; ++i) h.sys->Play(HashId("Signal"), NoEmitter(), nullptr);
    h.Steps(8);
    const float base = h.LastMean();
    SND_REQUIRE(base > 0.04f);
    h.sys->SetParamGlobal(HashId("Level"), 1);
    h.Steps(24); // 128 ms at 2 units/s: gain -2.56 dB, within one block of publication.
    SND_CHECK_NEAR(Db(h.LastMean() / base), -2.56, 0.17);
    h.sys->SetParamGlobal(HashId("Level"), 0);
    h.Steps(12);
    SND_CHECK_NEAR(Db(h.LastMean() / base), -1.28, 0.17);
    h.Steps(18);
    SND_CHECK_NEAR(h.LastMean(), base, 1e-4);
  }
}
} // namespace

namespace {
using namespace g2;
using snd::HashId;

SND_TEST(IndepG3Snapshot, RetargetedWeightContinuesFromCurrentLevel) {
  Fx h(G2Config());
  FxBank b;
  b.Event("Signal", 400);
  b.b.snapshots = {{HashId("Weight"), {{0, 0, 0, -18}}}};
  h.Load("weight", b.Build("WeightBank"));
  h.sys->Play(HashId("Signal"), NoEmitter(), nullptr);
  h.Steps(8);
  const float base = h.LastMean();
  h.sys->ApplySnapshot(HashId("Weight"), 1, 0.128f);
  h.Steps(12);
  const auto mid = h.Total();
  const float last = h.At(mid - 1);
  SND_CHECK_NEAR(Db(last / base), -9, 0.9);
  h.sys->ApplySnapshot(HashId("Weight"), 0, 0.064f);
  h.Steps(16);
  SND_CHECK_NEAR(h.At(mid), last, 0.001);
  SND_CHECK_NEAR(Db(h.At(mid + 6 * h.Frames()) / base), -4.5, 0.9);
  SND_CHECK_NEAR(h.LastMean(), base, 1e-4);
}

SND_TEST(IndepG3Category, IndependentOutputIsAbsentFromMasterButPublishesItsOwnPeak) {
  Fx h(G2Config());
  FxBank b;
  KeyBus(b);
  b.b.buses[0].flags |= bf::kBusMeter;
  b.Event("SoloOutput", 700, 0);
  b.Event("MainOutput", 80, 1);
  h.Load("out", b.Build("Out"));
  h.sys->Play(HashId("SoloOutput"), NoEmitter(), nullptr);
  h.Steps(10);
  snd_meter meter{};
  SND_REQUIRE_EQ(h.sys->GetMeter(HashId("Key"), &meter), SND_OK);
  SND_CHECK(meter.peak[0] > 0.4f);
  SND_CHECK_NEAR(h.LastMean(), 0, 1e-7);
  h.sys->Play(HashId("MainOutput"), NoEmitter(), nullptr);
  h.Steps(8);
  SND_CHECK_NEAR(h.LastMean(), 0.08f * kCenter, 1e-4);
}
} // namespace

namespace {
using namespace g2;
using snd::HashId;

SND_TEST(IndepG3Snapshot, SharedBusEffectOffsetsResolveInEachBanksOwnRowSpace) {
  Fx h(G2Config());
  FxBank owner;
  owner.b.buses[0].effects = {owner.Effect(dsp::kEffectEcho, EchoGain(0.5f))};
  owner.Event("Signal", 400);
  auto a = h.Load("definition", owner.Build("Definition"));
  SND_REQUIRE_EQ(h.sys->GetBankState(a), SND_BANK_STATE_LOADED);
  FxBank contributor;
  // The other bank's same bus has local EFFX row 1; dummy's EFFX row 0
  // must never be used as the target of the snapshot contribution.
  contributor.b.buses = {{HashId("Dummy"), 2, 2, 0, 0, {}, {}},
                         {HashId("Sfx"), 2, 2, 0, 0, {}, {}},
                         {HashId("Master"), kNone, 2, bf::kBusMaster, 0, {}, {}}};
  contributor.b.buses[0].effects = {contributor.Effect(dsp::kEffectLowpass, {2000, 0.707f})};
  contributor.b.buses[1].effects = {contributor.Effect(dsp::kEffectEcho, EchoGain(0.5f))};
  contributor.b.snapshots = {{HashId("InOtherBank"), {{1, 1, 2, 0.25f}}}};
  const auto b = h.Load("contributor", contributor.Build("Contributor"));
  SND_REQUIRE_EQ(h.sys->GetBankState(b), SND_BANK_STATE_LOADED);
  const auto playback = h.sys->Play(HashId("Signal"), NoEmitter(), nullptr);
  h.Steps(10);
  const float base = h.LastMean();
  SND_REQUIRE(std::fabs(base - 0.2f * kCenter) < 0.001f);
  h.sys->ApplySnapshot(HashId("InOtherBank"), 1, 0);
  h.Steps(10);
  SND_CHECK_NEAR(h.LastMean() / base, 1.5f, 0.001f);
  h.sys->UnloadBank(b, SND_UNLOAD_STOP_AND_UNLOAD);
  h.Steps(20);
  SND_CHECK(!h.Ended(playback.value, SND_END_STOPPED));
  SND_CHECK_NEAR(h.LastMean(), base, 1e-4f);
}

SND_TEST(IndepG3Curve, ExponentBelongsOnlyToItsOutgoingInterval) {
  Fx h(G2Config());
  FxBank bank;
  bank.Event("Signal", 400).curves = {0};
  bank.b.params.push_back({HashId("X"), 0, 0, 2, 0, 0});
  bank.b.curves.push_back({HashId("X"), 0, 0, {{0, 0}, {1, -16}, {2, 0}}});
  auto original = bank.Build("Shaped");
  bf::FileHeader header{};
  std::memcpy(&header, original.data(), sizeof(header));
  bt::BankWriter writer;
  for (unsigned i = 0; i < header.chunkCount; ++i) {
    bf::ChunkEntry entry{};
    std::memcpy(&entry, original.data() + header.chunkTableOffset + i * sizeof(entry), sizeof(entry));
    if (entry.fourcc == bf::chunk::CPNT) continue;
    const auto* data = original.data() + entry.offset;
    writer.AddBlob(entry.fourcc, {data, data + entry.bytes}, entry.flags);
  }
  bt::TableBuilder points(bf::chunk::CPNT);
  for (unsigned i = 0; i < 3; ++i) {
    auto row = points.AddRow();
    points.F32(bf::col::CPNT_x, row, static_cast<float>(i));
    points.F32(bf::col::CPNT_y, row, i == 1 ? -16.f : 0.f);
    if (i == 0) points.F32(bf::col::CPNT_shape, row, 2);
  }
  writer.AddTable(points);
  bt::BankHeaderInfo info;
  info.bankId = header.bankId;
  const auto b = h.Load("shaped", writer.Finish(info));
  SND_REQUIRE_EQ(h.sys->GetBankState(b), SND_BANK_STATE_LOADED);
  h.sys->Play(HashId("Signal"), NoEmitter(), nullptr);
  h.Steps(6);
  const float base = h.LastMean();
  SND_REQUIRE(base > 0.25f);
  h.sys->SetParamGlobal(HashId("X"), 0.5f);
  h.Steps(8);
  SND_CHECK_NEAR(Db(h.LastMean() / base), -4, 0.005);
  h.sys->SetParamGlobal(HashId("X"), 1.5f);
  h.Steps(8);
  SND_CHECK_NEAR(Db(h.LastMean() / base), -8, 0.005);
}
} // namespace
