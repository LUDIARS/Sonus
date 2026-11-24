// Stage 3 audio-level observations. The silent key output is separate from
// Master, so changes on Master measure only the continuing target playback.
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

// Replace one table in the test writer's output. The normal writer recomputes
// layout and both checksums, including for scope masks omitted in old banks.
std::vector<std::uint8_t> WithScopes(const FxBank& bank, std::uint32_t mask) {
  const auto bytes = bank.Build("Scopes");
  bf::FileHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  bt::BankWriter writer;
  for (std::uint32_t i = 0; i < header.chunkCount; ++i) {
    bf::ChunkEntry entry{};
    std::memcpy(&entry, bytes.data() + header.chunkTableOffset + sizeof(entry) * i, sizeof(entry));
    if (entry.fourcc != bf::chunk::PARM) {
      const auto* p = bytes.data() + entry.offset;
      writer.AddBlob(entry.fourcc, {p, p + entry.bytes}, entry.flags);
    }
  }
  bt::TableBuilder params(bf::chunk::PARM);
  const auto row = params.AddRow();
  params.U32(bf::col::PARM_id, row, HashId("Level"));
  params.U32(bf::col::PARM_scopeMask, row, mask);
  params.F32(bf::col::PARM_maxValue, row, 1);
  params.F32(bf::col::PARM_defaultValue, row, 0);
  writer.AddTable(params);
  bt::BankHeaderInfo info;
  info.bankId = header.bankId;
  return writer.Finish(info);
}

SND_TEST(IndepG3Param, AllowedScopesWinInPlaybackEmitterGlobalOrder) {
  for (std::uint32_t mask = 0; mask < 8; ++mask) {
    Fx h(G2Config());
    FxBank b;
    b.Event("Signal", 400).curves = {0};
    b.b.curves.push_back({HashId("Level"), 0, 0, {{0, 0}, {1, -12}}});
    const auto bank = h.Load("scope", WithScopes(b, mask));
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
