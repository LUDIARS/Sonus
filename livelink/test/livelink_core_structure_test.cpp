#include <chrono>
#include <cstdio>
#include "snd/internal/rt_check.hpp"
#include "livelink_v11_common.hpp"
#include "test_bank.hpp"

using namespace v11;
namespace {
constexpr auto sfx = HashId("Sfx"), master = HashId("Master"), aux = HashId("Aux");
ll::RuntimeDesc RealEditor() {
  ll::RuntimeDesc rd;
  rd.structEditor = &ll::CoreStructEditor();
  return rd;
}
int Edit(Session& s, ll::StructOp op, snd::Id id, std::initializer_list<std::uint64_t> args) {
  return s.Ack(s.client->StructEdit(op, id, args.begin(), static_cast<std::uint32_t>(args.size())));
}
struct Capacity {
  std::uint32_t buses, effects;
  std::uint64_t bytes;
  bool operator==(const Capacity&) const = default;
};
Capacity Left(snd::core::System& sys) {
  Capacity c;
  sys.LiveReserveLeft(&c.buses, &c.effects, nullptr, &c.bytes);
  return c;
}
}  // namespace

SND_TEST(CoreStructure, CyclesCapacityAndDisconnectLeaveAudibleRoutingIntact) {
  using O = ll::StructOp;
  auto cfg = Config();
  cfg.dev_reserve.buses = 1;
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  Session s(cfg, RealEditor(), sd);
  SND_REQUIRE(s.Ready());
  const auto initial = Left(*s.f.sys);
  SND_REQUIRE_EQ(Edit(s, O::AddBus, aux, {master, 2, Bits(-6)}), SND_OK);
  const auto afterBus = Left(*s.f.sys);
  SND_CHECK_EQ(Edit(s, O::AddBus, HashId("OneTooMany"), {master, 2}), SND_ERR_OUT_OF_BUDGET);
  SND_CHECK_EQ(Edit(s, O::AddSend, master, {aux, Bits(0)}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(Left(*s.f.sys) == afterBus);
  SND_CHECK_NEAR(s.Level(), 1, 1e-5);
  SND_REQUIRE_EQ(Edit(s, O::AddSend, sfx, {aux, Bits(0)}), SND_OK);
  SND_CHECK_NEAR(s.Level(), 1 + Ratio(-6), 1e-5);
  const auto afterSend = Left(*s.f.sys);
  SND_CHECK_EQ(Edit(s, O::AddSend, aux, {sfx, Bits(-6)}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(Edit(s, O::RemoveBus, aux, {}), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(Left(*s.f.sys) == afterSend);
  s.client->Disconnect();
  SND_REQUIRE(s.f.StepUntil([&] { return Left(*s.f.sys) == initial; }));
  SND_CHECK_NEAR(s.Level(), 1, 1e-5);
}

SND_TEST(CoreStructure, RemovingPlayingAuthoredBusReroutesAndUndoRestoresItsFader) {
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  Session s(Config(), RealEditor(), sd);
  SND_REQUIRE(s.Ready());
  SND_REQUIRE_EQ(s.f.sys->SetBusVolume(sfx, 0.25f, 0), SND_OK);
  s.f.Steps(4);
  SND_CHECK_NEAR(s.Level(), .25, 1e-5);
  SND_REQUIRE_EQ(Edit(s, ll::StructOp::RemoveBus, sfx, {}), SND_OK);
  SND_CHECK_NEAR(s.Level(), 1, 1e-5);
  ll::VoiceInfo voice{};
  SND_REQUIRE_EQ(s.f.sys->LiveVoices(&voice, 1), 1u);
  SND_CHECK_EQ(voice.busId, master);
  s.client->Disconnect();
  SND_REQUIRE(s.f.StepUntil([&] { return std::fabs(s.Level() - .25) < 1e-5; }));
  SND_REQUIRE_EQ(s.f.sys->LiveVoices(&voice, 1), 1u);
  SND_CHECK_EQ(voice.busId, sfx);
}

SND_TEST(CoreStructure, TailOwnsPhysicalSlotUntilSilenceAndReleasesIt) {
  auto cfg = Config();
  cfg.dev_reserve.effects = 1;
  Fixture f(Hooks::Live, cfg);
  f.Load(DevBank());
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(4);
  const float dry = f.LastLeft();
  auto& sys = *f.sys;
  const auto initial = Left(sys);
  const float p[]{10, 0, 0, 1, 20};
  SND_REQUIRE_EQ(sys.LiveAddEffect(sfx, HashId("snd.fx.echo"), 0, p, 5, 1), SND_OK);
  f.Steps(16);
  SND_REQUIRE_EQ(sys.LiveRemoveEffect(sfx, 0, 2), SND_OK);
  SND_CHECK_EQ(Left(sys).effects, 0u);
  SND_CHECK_EQ(sys.LiveAddEffect(sfx, HashId("snd.fx.echo"), 0, p, 5, 3), SND_ERR_LIMIT_REACHED);
  f.Steps(1);
  SND_CHECK_NEAR(f.LastLeft() / dry, 2, 1e-4);  // direct tone plus delayed sound from before removal
  f.Steps(80);
  SND_CHECK_NEAR(f.LastLeft() / dry, 1, 1e-5);
  SND_CHECK(Left(sys) == initial);
}

SND_TEST(CoreStructure, FailedDspSizingPreservesBothBudgetsAndChain) {
  auto cfg = Config();
  cfg.dev_reserve.bytes = 512;
  Session s(cfg, RealEditor());
  SND_REQUIRE(s.Ready());
  const auto before = Left(*s.f.sys);
  SND_REQUIRE_EQ(Edit(s, ll::StructOp::AddEffect, sfx, {HashId("snd.fx.delay"), 0}), SND_ERR_LIMIT_REACHED);
  SND_CHECK(Left(*s.f.sys) == before);
  SND_CHECK_NEAR(s.Level(), 1, 1e-5);
  SND_REQUIRE_EQ(Edit(s, ll::StructOp::AddBus, aux, {master, 2}), SND_OK);
}

SND_TEST(CoreStructure, RemoveEffectCancelsItsOwnAddWithShiftedIndices) {
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  Session s(Config(), RealEditor(), sd);
  SND_REQUIRE(s.Ready());
  const auto before = Left(*s.f.sys);
  using O = ll::StructOp;
  SND_REQUIRE_EQ(Edit(s, O::AddEffect, sfx, {HashId("snd.fx.distortion"), 0}), SND_OK);
  const auto first = s.f.sys->LiveOrigin(sfx, true, 0);
  SND_REQUIRE_EQ(Edit(s, O::AddEffect, sfx, {HashId("snd.fx.distortion"), 0, Bits(.25f)}), SND_OK);
  const auto second = s.f.sys->LiveOrigin(sfx, true, 0);
  SND_REQUIRE(first != second);
  SND_REQUIRE_EQ(Edit(s, O::RemoveEffect, sfx, {1}), SND_OK);
  SND_CHECK_EQ(s.f.sys->LiveOrigin(sfx, true, 0), second);
  s.client->Disconnect();
  SND_REQUIRE(s.f.StepUntil([&] { return Left(*s.f.sys) == before; }));
  SND_CHECK_NEAR(s.Level(), 1, 1e-5);
}

SND_TEST(CoreStructure, AuthoredEffectUndoRestoresOriginalIndexAndParameters) {
  Fixture f;
  f.Load(DevBank(kProjectHash, true, true));
  auto& sys = *f.sys;
  sys.Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(8);
  const auto original = f.LastLeft();
  SND_REQUIRE(original > 0);
  SND_REQUIRE_EQ(sys.LiveAddEffect(sfx, HashId("snd.fx.distortion"), 0, nullptr, 0, 1), SND_OK);
  SND_REQUIRE_EQ(sys.LiveRemoveEffect(sfx, 1, 2), SND_OK);
  SND_REQUIRE_EQ(sys.LiveUndoEdit(2), SND_OK);
  SND_CHECK_EQ(sys.LiveOrigin(sfx, true, 0), 1u);
  SND_REQUIRE_EQ(sys.LiveUndoEdit(1), SND_OK);
  f.Steps(40);
  SND_CHECK_NEAR(f.LastLeft(), original, 1e-6);
  SND_REQUIRE_EQ(sys.LiveRemoveEffect(sfx, 0, 3), SND_OK);
  f.Steps(40);
  SND_CHECK_NEAR(f.LastLeft(), 4 * original, 1e-6);
}

SND_TEST(CoreStructure, BankRetirementReleasesBothLedgersAndOldUndoCannotTouchReplacement) {
  using O = ll::StructOp;
  Session s(Config(), RealEditor());
  SND_REQUIRE(s.Ready());
  const auto before = Left(*s.f.sys);
  SND_REQUIRE_EQ(Edit(s, O::AddBus, aux, {master, 2}), SND_OK);
  const auto old = s.f.sys->LiveOrigin(aux, false, 0);
  SND_REQUIRE_EQ(Edit(s, O::AddEffect, sfx, {HashId("snd.fx.echo"), 0}), SND_OK);
  SND_REQUIRE_EQ(Edit(s, O::RemoveEffect, sfx, {0}), SND_OK);
  SND_REQUIRE_EQ(s.f.sys->UnloadBank(snd_bank_handle{s.f.sys->LiveBusBank(sfx)}, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  SND_REQUIRE(s.f.StepUntil([&] { return s.f.sys->LiveBanks(nullptr, 0) == 0; }));
  SND_CHECK(Left(*s.f.sys) == before);
  SND_CHECK_EQ(s.f.rt->ReserveUsage().buses, 0u);
  s.f.Steps(8);
  s.f.Load(DevBank());
  SND_REQUIRE_EQ(Edit(s, O::AddBus, aux, {master, 2}), SND_OK);
  const auto fresh = s.f.sys->LiveOrigin(aux, false, 0);
  SND_REQUIRE(fresh != old);
  SND_CHECK_EQ(s.f.sys->LiveUndoEdit(old), SND_OK);
  SND_CHECK_EQ(s.f.sys->LiveOrigin(aux, false, 0), fresh);
}

SND_TEST(CoreStructure, AddedBusCellsFollowTheGraphAndOldCellCannotAliasAReusedRow) {
  Fixture f;
  f.Load(DevBank());
  auto& sys = *f.sys;
  sys.Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(5);
  const auto dry = f.LastLeft();
  SND_REQUIRE_EQ(sys.LiveAddBus(aux, master, 2, 0, 1), SND_OK);
  SND_REQUIRE_EQ(sys.LiveAddSend(sfx, aux, 0, true, 2), SND_OK);
  snd::core::LiveCell saved{};
  SND_REQUIRE_EQ(sys.LiveFindCells(aux, bf::chunk::BUSS, 0, &saved, 1), 1u);
  SND_REQUIRE_EQ(sys.LiveWriteCell(saved, bf::chunk::BUSS, bf::col::BUSS_volumeDb, Bits(-6), nullptr, true), SND_OK);
  SND_REQUIRE_EQ(sys.SetBusVolume(sfx, 0, 0), SND_OK);
  f.Steps(5);
  SND_CHECK_NEAR(f.LastLeft() / dry, Ratio(-6), 1e-5);  // pre-fader send
  snd::core::LiveCell send{};
  SND_REQUIRE_EQ(sys.LiveFindCells(sfx, bf::chunk::SEND, 0, &send, 1), 1u);
  SND_REQUIRE_EQ(sys.LiveWriteCell(send, bf::chunk::SEND, bf::col::SEND_preFader, 0, nullptr, true), SND_OK);
  f.Steps(4);
  SND_CHECK_EQ(f.LastLeft(), 0.0f);  // post-fader send
  SND_REQUIRE_EQ(sys.LiveUndoEdit(2), SND_OK);
  SND_REQUIRE_EQ(sys.LiveUndoEdit(1), SND_OK);
  SND_REQUIRE_EQ(sys.LiveAddBus(aux, master, 2, 0, 3), SND_OK);
  SND_CHECK_EQ(sys.LiveWriteCell(saved, bf::chunk::BUSS, bf::col::BUSS_volumeDb, Bits(-12), nullptr, true),
               SND_ERR_NOT_FOUND);
}

SND_TEST(CoreStructure, CrossBankSendExpiresBeforeDestinationSlotReuse) {
  Session s(Config(), RealEditor());
  SND_REQUIRE(s.Ready());
  coretest::TestBank other;
  coretest::TBus returnBus;
  returnBus.id = aux;
  returnBus.channels = 2;
  other.buses.push_back(returnBus);
  const auto handle = s.f.Load(other.Build(HashId("ReturnBank")), "return.sndb");
  SND_REQUIRE_EQ(s.f.sys->GetBankState(handle), SND_BANK_STATE_LOADED);
  const auto initial = Left(*s.f.sys);
  SND_REQUIRE_EQ(Edit(s, ll::StructOp::AddSend, sfx, {aux, Bits(0)}), SND_OK);
  SND_CHECK_NEAR(s.Level(), 2, 1e-5);
  SND_REQUIRE_EQ(s.f.sys->UnloadBank(handle, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  SND_REQUIRE(s.f.StepUntil([&] { return s.f.rt->ReserveUsage().bytes == 0; }));
  SND_CHECK(Left(*s.f.sys) == initial);
  SND_CHECK_NEAR(s.Level(), 1, 1e-5);
  s.f.Steps(8);
  s.f.Load(other.Build(HashId("ReturnBank")), "return.sndb");
  s.f.Steps(3);
  SND_CHECK_NEAR(s.Level(), 1, 1e-5);
}

SND_TEST(CoreStructure, FullReserveEditsAndTeardownReturnAllMixerAllocations) {
  snd::base::AccountingAllocator accounting(snd::base::DefaultAllocator());
  auto cfg = Config();
  cfg.dev_reserve.buses = 64;
  cfg.dev_reserve.effects = 32;
  constexpr snd::Id first = 10000;
  {
    Fixture f(Hooks::Live, cfg, nullptr, RealEditor(), accounting);
    f.Load(DevBank());
    auto& sys = *f.sys;
    const auto initial = Left(sys);
    std::uint64_t token = 0;
    for (unsigned i = 0; i < cfg.dev_reserve.buses; ++i) {
      SND_RT_SCOPE();
      SND_REQUIRE_EQ(sys.LiveAddBus(first + i, master, 2, 0, ++token), SND_OK);
      if (i) SND_REQUIRE_EQ(sys.LiveAddSend(first + i - 1, first + i, -6, false, ++token), SND_OK);
      if (i < cfg.dev_reserve.effects)
        SND_REQUIRE_EQ(sys.LiveAddEffect(first + i, HashId("snd.fx.lowpass"), 0, nullptr, 0, ++token), SND_OK);
    }
    SND_CHECK_EQ(Left(sys).buses, 0u);
    SND_CHECK_EQ(Left(sys).effects, 0u);
    // Measure edit plus rendering under saturation; machine scheduling is not a pass criterion.
    std::int64_t samples[32]{};
    for (auto& sample : samples) {
      const auto begin = std::chrono::steady_clock::now();
      {
        SND_RT_SCOPE();
        SND_REQUIRE_EQ(sys.LiveAddSend(first + 63, first, 0, false, ++token), SND_ERR_INVALID_ARGUMENT);
        sys.RenderBlock(f.out.data());
      }
      sample = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    }
    std::sort(std::begin(samples), std::end(samples));
    std::printf("C-01 full reserve: 64 buses, 32 effects, 63 sends, 32 blocks; median %lld ns, max %lld ns, period %llu ns\n",
                static_cast<long long>(samples[16]), static_cast<long long>(samples[31]),
                static_cast<unsigned long long>(1000000000ull * cfg.block_size / cfg.sample_rate));
    while (token) SND_REQUIRE_EQ(sys.LiveUndoEdit(token--), SND_OK);
    f.Steps(4);
    SND_CHECK(Left(sys) == initial);
  }
  SND_CHECK_EQ(accounting.TotalUsed(), 0u);
}
