// Independent tests: banks (10.3), A1-16 (shared objects unified by id), the
// release epoch (3.3: source jobs posted before the unregistration finish
// before the memory goes back), IF_UNUSED / STOP_AND_UNLOAD.
#include <cmath>
#include <cstdio>
#include <vector>

#include "indep_fixture.hpp"

using namespace indep;
using snd::HashId;

namespace {
float Db(float db) { return std::pow(10.0f, db / 20.0f); }

// Bank with shared objects: bus "Shared" (send -6 dB pre-fader to "Aux"),
// "Aux", category "Cat", and one event of its own.
std::vector<std::uint8_t> SharedBank(const char* id, const char* event, float f) {
  Bank b;
  b.b.buses.clear();
  b.b.buses.push_back(TBus{HashId("Shared"), 2u, 0, 0, 0.0f, {}, {TBus::Send{1u, -6.0f, 1u}}});
  b.b.buses.push_back(TBus{HashId("Aux"), 2u, 0, 0, 0.0f, {}, {}});
  b.b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  b.b.categories.push_back(TCategory{HashId("Cat"), kNone, 0u, 0.0f});
  b.Event(event, f, 128, 0, 0);
  return b.Build(id);
}
}  // namespace

// N1 / N2: load once, STOP_AND_UNLOAD fades the playing sound and ends it
// DISCARDED / BANK_UNLOADED; the event is not loaded any more afterwards.
SND_TEST(IndepBank, StopAndUnloadDiscardsWithAShortFade) {
  Bank b;
  b.Event("E", 500.0f);
  Fx fx;
  const snd_bank_handle h = fx.Load("b", b.Build("B"));
  SND_REQUIRE_EQ(fx.sys->GetBankState(h), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(fx.loads.size(), 1u);
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  SND_CHECK_EQ(fx.sys->UnloadBank(h, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  const std::size_t s = fx.Total();
  fx.Steps(30);
  SND_CHECK(fx.Ended(p.value, SND_END_DISCARDED, SND_END_DETAIL_BANK_UNLOADED));
  SND_CHECK_EQ(fx.CountEnds(p.value), 1u);
  SND_CHECK_EQ(fx.MaxAbs(s + 3 * 256, fx.Total()), 0.0f);
  SND_CHECK(fx.At(s) > 0.0f);  // faded, not cut before the block
  SND_CHECK_EQ(fx.sys->GetBankState(h), SND_BANK_STATE_UNLOADED);
  const snd_playback_handle q = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  SND_CHECK(fx.Ended(q.value, SND_END_NOT_LOADED));
  SND_CHECK_EQ(fx.loads.size(), 1u);
}

// N3: IF_UNUSED while referenced fails with BANK_IN_USE (on_error, object =
// bank handle) and leaves the bank and the sound alone; once unused it unloads.
SND_TEST(IndepBank, IfUnusedRefusesWhileReferenced) {
  Bank b;
  b.Event("E", 500.0f);
  Fx fx;
  const snd_bank_handle h = fx.Load("b", b.Build("B"));
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  fx.sys->UnloadBank(h, SND_UNLOAD_IF_UNUSED);
  fx.Steps(6);
  bool inUse = false;
  for (const Err& e : fx.errors) inUse = inUse || (e.code == SND_ERR_BANK_IN_USE && e.object == h.value);
  SND_CHECK(inUse);
  SND_CHECK_EQ(fx.sys->GetBankState(h), SND_BANK_STATE_LOADED);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f, 1e-5f);
  SND_CHECK_EQ(fx.CountEnds(p.value), 0u);
  fx.sys->Stop(p, 0.0f, nullptr);
  fx.Steps(4);
  SND_CHECK_EQ(fx.sys->UnloadBank(h, SND_UNLOAD_IF_UNUSED), SND_OK);
  fx.Steps(10);
  SND_CHECK_EQ(fx.sys->GetBankState(h), SND_BANK_STATE_UNLOADED);
}

// N4 (A1-16): shared bus / send / category defined by two banks are one object;
// unloading the bank loaded first keeps them working for the other bank,
// including the runtime volume set before.
SND_TEST(IndepBank, SharedObjectsSurviveTheirFirstDefinersUnload) {
  Fx fx;
  const snd_bank_handle a = fx.Load("a", SharedBank("A", "EA", 100.0f));
  const snd_bank_handle b = fx.Load("b", SharedBank("B", "EB", 400.0f));
  SND_REQUIRE_EQ(fx.sys->GetBankState(a), SND_BANK_STATE_LOADED);
  SND_REQUIRE_EQ(fx.sys->GetBankState(b), SND_BANK_STATE_LOADED);
  fx.sys->Play(HashId("EA"), NoEmitter(), nullptr);
  fx.sys->Play(HashId("EB"), NoEmitter(), nullptr);
  fx.Steps(3);
  const float send = 1.0f + Db(-6.0f);  // direct + pre-fader send through Aux
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f * send, 1e-5f);
  fx.sys->SetCategoryVolume(HashId("Cat"), 0.5f, 0.0f);
  fx.sys->SetBusVolume(HashId("Aux"), 0.0f, 0.0f);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f * 0.5f, 1e-5f);  // only one send, not two
  fx.sys->SetBusVolume(HashId("Aux"), 1.0f, 0.0f);
  fx.sys->UnloadBank(a, SND_UNLOAD_STOP_AND_UNLOAD);
  fx.Steps(30);
  SND_CHECK_EQ(fx.sys->GetBankState(a), SND_BANK_STATE_UNLOADED);
  // EB still goes through Shared -> (send) Aux, at the category volume.
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.4f * 0.5f * send, 1e-5f);
  fx.sys->SetBusVolume(HashId("Shared"), 0.0f, 0.0f);  // pre-fader send survives the fader
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.4f * 0.5f * Db(-6.0f), 1e-5f);
  snd_meter m{};
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetMeter(HashId("Shared"), &m), SND_OK);
  fx.sys->UnloadBank(b, SND_UNLOAD_STOP_AND_UNLOAD);
  fx.Steps(30);
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetMeter(HashId("Shared"), &m), SND_ERR_NOT_FOUND);
}

// N5 (3.3 epoch): a source job posted before the unregistration keeps the bank
// memory alive (state UNLOADING) until it has run.
SND_TEST(IndepBank, ReleaseWaitsForSourceJobs) {
  Bank b;
  b.Event("E", 500.0f);
  Fx fx;
  const snd_bank_handle h = fx.Load("b", b.Build("B"));
  fx.jobs.SetMode(snd::mock::JobMode::Manual);
  fx.kind.postJobs.store(3);
  fx.Steps(2);  // posted in Publish, submitted by the next Update
  fx.sys->Update();
  SND_REQUIRE(fx.jobs.Pending() >= 3u);
  fx.sys->UnloadBank(h, SND_UNLOAD_STOP_AND_UNLOAD);
  fx.Steps(30);
  SND_CHECK_EQ(fx.sys->GetBankState(h), SND_BANK_STATE_UNLOADING);
  fx.jobs.RunAll();
  fx.Steps(5);
  SND_CHECK_EQ(fx.sys->GetBankState(h), SND_BANK_STATE_UNLOADED);
  SND_CHECK_EQ(fx.kind.jobsRun.load(), 3u);
}
