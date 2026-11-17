// Independent tests: the override layer (design.md 11.2 上書き層) and the Ack / Error
// contract of SetValue (livelink-protocol.md §5, §6).
//
// The analytic signal is the fixture's constant "dc" asset: the master output of a
// looping event is a constant whose level follows bus / category volumes, voice
// counts and a brickwall limiter's ceiling exactly. Most checks record LiveSetValue
// directly from the owner thread (deterministic, one block per edit); the Ack / Error
// checks go through a real Server and Client over the in-memory transport.
#include <chrono>
#include <limits>
#include <memory>
#include <thread>

#include "indep_livelink_common.hpp"

using namespace indep;
using lltest::Fixture;

namespace {

double Settle(Fixture& f, std::uint32_t blocks = 24) {
  f.Steps(blocks);
  return LeftMean(f);
}

snd_bank_handle LoadAt(Fixture& f, const std::vector<std::uint8_t>& bank, const char* path) {
  return f.Load(bank, path);
}

}  // namespace

// C3: CATG.volumeDb edits are refreshed into core's category copy, and the revert
// marker puts the bank value back.
SND_TEST(IndepLivelinkOverride, CategoryVolumeEditIsAudibleAndReverts) {
  Fixture f;
  BankOpts o;
  o.withCategory = true;
  LoadAt(f, IndepBank(o), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Settle(f);
  SND_REQUIRE(base > 0.05);
  const ll::ApplyResult r =
      ApplyDirect(f, HashId("Cat"), bf::chunk::CATG, bf::col::CATG_volumeDb, ll::ValueType::F32, 0, F32Bits(-6.0f));
  SND_CHECK_EQ(r.result, SND_OK);
  SND_CHECK_EQ(r.cells, 1u);
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-6.0), 1e-3);
  const ll::ApplyResult rv = RevertDirect(f);
  SND_CHECK_EQ(rv.result, SND_OK);
  SND_CHECK_EQ(rv.revert, 1u);
  SND_CHECK_EQ(rv.cells, 1u);
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 0u);
}

// C3: VLGR.maxVoices edits change the voice limit for later plays.
SND_TEST(IndepLivelinkOverride, VoiceLimitEditChangesHowManyVoicesPlay) {
  Fixture f;
  BankOpts o;
  o.withGroup = true;
  o.groupMax = 4;
  LoadAt(f, IndepBank(o), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double one = Settle(f);
  snd_playback_handle tone{};
  (void)tone;
  // Control: two ToneG voices under a limit of 4 add up (Tone + 2 x ToneG = 3 units).
  const snd_playback_handle g1 = f.sys->Play(HashId("ToneG"), snd_emitter_handle{0}, nullptr);
  const snd_playback_handle g2 = f.sys->Play(HashId("ToneG"), snd_emitter_handle{0}, nullptr);
  SND_CHECK_NEAR(Settle(f) / one, 3.0, 1e-3);
  f.sys->Stop(g1, 0.0f, nullptr);
  f.sys->Stop(g2, 0.0f, nullptr);
  SND_CHECK_NEAR(Settle(f, 40) / one, 1.0, 1e-3);
  const ll::ApplyResult r =
      ApplyDirect(f, HashId("Grp"), bf::chunk::VLGR, bf::col::VLGR_maxVoices, ll::ValueType::U32, 0, 1u);
  SND_CHECK_EQ(r.result, SND_OK);
  f.sys->Play(HashId("ToneG"), snd_emitter_handle{0}, nullptr);
  f.sys->Play(HashId("ToneG"), snd_emitter_handle{0}, nullptr);
  // At most one ToneG voice is audible now: Tone + 1.
  SND_CHECK_NEAR(Settle(f, 40) / one, 2.0, 1e-3);
}

// C3 / C6: EFPV (bus id, effect << 16 | param) reaches the running limiter: its
// ceiling bounds the constant output exactly; the revert restores the bank ceiling.
SND_TEST(IndepLivelinkOverride, EffectParameterEditMovesTheLimiterCeiling) {
  Fixture f;
  BankOpts o;
  o.withLimiter = true;
  o.limiterDb = 0.0f;  // the dc level stays below 0 dBFS: unlimited
  LoadAt(f, IndepBank(o), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double open = Settle(f, 40);
  SND_REQUIRE(open > DbRatio(-12.0) * 1.05);  // the limiter has something to do at -12 dB
  auto ceiling = [&](float db, std::uint32_t index = 0u) {
    return ApplyDirect(f, HashId("Sfx"), bf::chunk::EFPV, bf::col::EFPV_value, ll::ValueType::F32, index, F32Bits(db));
  };
  SND_CHECK_EQ(ceiling(-12.0f).result, SND_OK);
  SND_CHECK_NEAR(Settle(f, 60), DbRatio(-12.0), 2e-3);
  SND_CHECK_EQ(ceiling(-20.0f).result, SND_OK);
  SND_CHECK_NEAR(Settle(f, 60), DbRatio(-20.0), 2e-3);
  // Out of range addressing: no second effect, no fifth parameter.
  SND_CHECK_EQ(ceiling(-6.0f, 1u << 16).result, SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(ceiling(-6.0f, 4u).result, SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(RevertDirect(f).result, SND_OK);
  SND_CHECK_NEAR(Settle(f, 60), open, 2e-3);
}

// C7: an object copied into two dev banks is written in both (cells == 2); with one
// copy in a release bank nothing is written (all or nothing).
SND_TEST(IndepLivelinkOverride, DuplicatedObjectsAreWrittenEverywhereOrNowhere) {
  {
    Fixture f;
    LoadAt(f, IndepBank(), "a.sndb");
    BankOpts two;
    two.name = "Two";
    two.extraEvent = "Other";
    LoadAt(f, IndepBank(two), "b.sndb");
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    const double base = Settle(f);
    const ll::ApplyResult r = VolumeDirect(f, "Sfx", -6.0f);
    SND_CHECK_EQ(r.result, SND_OK);
    SND_CHECK_EQ(r.cells, 2u);
    SND_CHECK_EQ(f.rt->applier().Remembered(), 2u);
    SND_CHECK_NEAR(Settle(f) / base, DbRatio(-6.0), 1e-3);
    // Event only in bank "Two": one cell.
    const ll::ApplyResult e = ApplyDirect(f, HashId("Other"), bf::chunk::EVNT, bf::col::EVNT_fadeOutMs,
                                          ll::ValueType::F32, 0, F32Bits(25.0f));
    SND_CHECK_EQ(e.result, SND_OK);
    SND_CHECK_EQ(e.cells, 1u);
    const ll::ApplyResult rv = RevertDirect(f);
    SND_CHECK_EQ(rv.cells, 3u);
    SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
  }
  {
    Fixture f;
    LoadAt(f, IndepBank(), "a.sndb");
    BankOpts two;
    two.name = "Two";
    two.dev = false;  // release bank: no override layer
    LoadAt(f, IndepBank(two), "b.sndb");
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    const double base = Settle(f);
    const ll::ApplyResult r = VolumeDirect(f, "Sfx", -6.0f);
    SND_CHECK_EQ(r.result, SND_ERR_UNSUPPORTED);
    SND_CHECK_EQ(r.cells, 0u);
    SND_CHECK_EQ(f.rt->applier().Remembered(), 0u);
    SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-6);
  }
}

// C11 / C12: the first write keeps the original; writing the value a cell already
// holds is not an edit; the revert empties the table.
SND_TEST(IndepLivelinkOverride, FirstWriteKeepsTheOriginalAndNoOpWritesAreNotRemembered) {
  Fixture f;
  LoadAt(f, IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Settle(f);
  ll::ApplyResult r = VolumeDirect(f, "Sfx", 0.0f);  // the bank value
  SND_CHECK_EQ(r.result, SND_OK);
  SND_CHECK_EQ(r.cells, 0u);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 0u);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -3.0f).cells, 1u);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -9.0f).cells, 1u);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", 0.0f).cells, 1u);  // back to the bank value: still remembered
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -9.0f).cells, 1u);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 1u);
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-9.0), 1e-3);
  SND_CHECK_EQ(RevertDirect(f).cells, 1u);
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-4);
  // A second revert has nothing to do.
  const ll::ApplyResult again = RevertDirect(f);
  SND_CHECK_EQ(again.result, SND_OK);
  SND_CHECK_EQ(again.cells, 0u);
}

// C14: LIMIT_REACHED exactly past maxEdits; nothing is written then; rewriting a
// remembered cell still works at the limit; a command needing 2 cells with 1 left fails whole.
SND_TEST(IndepLivelinkOverride, LimitReachedBoundaryIsExactAndAllOrNothing) {
  ll::RuntimeDesc rd;
  rd.maxEdits = 3;
  Fixture f(lltest::Hooks::Live, lltest::Config(), nullptr, rd);
  SND_REQUIRE_EQ(f.created, SND_OK);
  LoadAt(f, IndepBank(), "a.sndb");
  BankOpts two;
  two.name = "Two";
  two.extraEvent = "Other";
  LoadAt(f, IndepBank(two), "b.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Settle(f);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -6.0f).cells, 2u);  // 2 of 3
  // Master is in both banks too: 2 more cells do not fit -> refused, nothing written.
  ll::ApplyResult r = VolumeDirect(f, "Master", -6.0f);
  SND_CHECK_EQ(r.result, SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(r.cells, 0u);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 2u);
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-6.0), 1e-3);
  // "Other" is only in bank Two: one cell fits exactly (3 of 3).
  r = ApplyDirect(f, HashId("Other"), bf::chunk::EVNT, bf::col::EVNT_fadeOutMs, ll::ValueType::F32, 0, F32Bits(5.0f));
  SND_CHECK_EQ(r.result, SND_OK);
  SND_CHECK_EQ(r.cells, 1u);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 3u);
  // Full: any new cell is refused, a remembered one is not.
  r = ApplyDirect(f, HashId("Other"), bf::chunk::EVNT, bf::col::EVNT_retriggerMs, ll::ValueType::F32, 0, F32Bits(5.0f));
  SND_CHECK_EQ(r.result, SND_ERR_LIMIT_REACHED);
  r = ApplyDirect(f, HashId("Other"), bf::chunk::EVNT, bf::col::EVNT_fadeOutMs, ll::ValueType::F32, 0, F32Bits(7.0f));
  SND_CHECK_EQ(r.result, SND_OK);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -12.0f).result, SND_OK);
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-12.0), 1e-3);
}

// C14 (single bank): edits up to maxEdits succeed, the next new cell is refused, an
// already remembered cell can still be rewritten.
SND_TEST(IndepLivelinkOverride, LimitReachedSingleBank) {
  ll::RuntimeDesc rd;
  rd.maxEdits = 2;
  Fixture f(lltest::Hooks::Live, lltest::Config(), nullptr, rd);
  SND_REQUIRE_EQ(f.created, SND_OK);
  LoadAt(f, IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Settle(f);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -6.0f).result, SND_OK);
  SND_CHECK_EQ(VolumeDirect(f, "Master", -6.0f).result, SND_OK);
  const ll::ApplyResult r =
      ApplyDirect(f, HashId("Tone"), bf::chunk::CLIP, bf::col::CLIP_volumeDb, ll::ValueType::F32, 0, F32Bits(-3.0f));
  SND_CHECK_EQ(r.result, SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 2u);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -12.0f).result, SND_OK);  // remembered cell: fine at the limit
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-18.0), 1e-3);
  SND_CHECK_EQ(RevertDirect(f).cells, 2u);
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-4);
  // After the revert the table is empty again: two new cells fit.
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -1.0f).result, SND_OK);
  SND_CHECK_EQ(ApplyDirect(f, HashId("Tone"), bf::chunk::CLIP, bf::col::CLIP_volumeDb, ll::ValueType::F32, 0,
                           F32Bits(-3.0f))
                   .result,
               SND_OK);
}

// C13: a cell of a bank that was unloaded is not restored by the revert (the bank is
// gone); its entry must not affect anything else.
SND_TEST(IndepLivelinkOverride, RevertSkipsBanksThatWereUnloaded) {
  Fixture f;
  const snd_bank_handle h = LoadAt(f, IndepBank(), "a.sndb");
  BankOpts two;
  two.name = "Two";
  two.extraEvent = "Other";
  LoadAt(f, IndepBank(two), "b.sndb");
  f.sys->Play(HashId("Other"), snd_emitter_handle{0}, nullptr);
  const double base = Settle(f);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -6.0f).cells, 2u);
  SND_CHECK_EQ(f.sys->UnloadBank(h, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  SND_REQUIRE(f.StepUntil([&] {
    f.sys->Update();
    return f.sys->GetBankState(h) == SND_BANK_STATE_UNLOADED;
  }));
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-6.0), 1e-3);  // bank Two still holds the edit
  const ll::ApplyResult rv = RevertDirect(f);
  SND_CHECK_EQ(rv.result, SND_OK);
  SND_CHECK_EQ(rv.cells, 1u);  // only bank Two's cell is restored
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
}

// C13 / C11 (defect candidate): after a bank is unloaded and loaded again, edits of the
// new registration must be remembered with the new bank's original and reverted.
SND_TEST(IndepLivelinkOverride, RevertCoversEditsAfterBankReload) {
  Fixture f;
  const snd_bank_handle h = LoadAt(f, IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Settle(f);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -6.0f).cells, 1u);
  SND_CHECK_EQ(f.sys->UnloadBank(h, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  SND_REQUIRE(f.StepUntil([&] {
    f.sys->Update();
    return f.sys->GetBankState(h) == SND_BANK_STATE_UNLOADED;
  }));
  LoadAt(f, IndepBank(), "a.sndb");  // fresh registration (same slot, new generation)
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);  // the bank value again
  const ll::ApplyResult r = VolumeDirect(f, "Sfx", -12.0f);
  SND_CHECK_EQ(r.result, SND_OK);
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-12.0), 1e-3);
  const ll::ApplyResult rv = RevertDirect(f);
  SND_CHECK_EQ(rv.result, SND_OK);
  SND_CHECK_MSG(rv.cells == 1u, "revert restored %u cells of the reloaded bank (expected 1)", rv.cells);
  const double after = Settle(f) / base;
  SND_CHECK_MSG(std::fabs(after - 1.0) < 1e-3, "after the revert the level is %.4f of the bank level (expected 1)",
                after);
}

// C8 / C9 / C10 more columns: every u32 rule's range boundary, non-listed u32 columns
// and other types are refused and change nothing.
SND_TEST(IndepLivelinkOverride, U32RangesAndRefusedColumns) {
  Fixture f;
  BankOpts o;
  o.withGroup = true;
  LoadAt(f, IndepBank(o), "a.sndb");
  auto set = [&](snd::Id id, std::uint32_t chunk, std::uint32_t col, std::uint64_t v, std::uint32_t index = 0) {
    return ApplyDirect(f, id, chunk, col, ll::ValueType::U32, index, v).result;
  };
  SND_CHECK_EQ(set(HashId("Grp"), bf::chunk::VLGR, bf::col::VLGR_stealMode, 3u), SND_OK);
  SND_CHECK_EQ(set(HashId("Grp"), bf::chunk::VLGR, bf::col::VLGR_stealMode, 4u), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_virtualReturn, 2u), SND_OK);
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_virtualReturn, 3u), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(set(HashId("Knob"), bf::chunk::PARM, bf::col::PARM_scopeMask, 3u), SND_OK);  // bank default 7
  SND_CHECK_EQ(set(HashId("Knob"), bf::chunk::PARM, bf::col::PARM_scopeMask, 8u), SND_ERR_INVALID_ARGUMENT);
  // References / layout / flags / ids are not live-editable.
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_bus, 1u), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_trackCount, 2u), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_flags, 1u), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_id, 5u), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_priority, 200u), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_priority, 128u), SND_OK);  // unchanged value
  SND_CHECK_EQ(set(HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_channels, 1u), SND_ERR_UNSUPPORTED);
  // TRAK (1-level list) and CLIP (2-level) out-of-range indices.
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::TRAK, bf::col::TRAK_layerParam, 1u, 1u), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::CLIP, bf::col::CLIP_loopCount, 2u, 1u << 16), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::CLIP, bf::col::CLIP_loopCount, 2u, 0u), SND_OK);
  // A value with high bits set for a 32-bit column.
  SND_CHECK_EQ(set(HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_maxInstances, 1ull << 32), SND_ERR_INVALID_ARGUMENT);
  // Unknown object.
  SND_CHECK_EQ(set(HashId("Nope"), bf::chunk::EVNT, bf::col::EVNT_maxInstances, 1u), SND_ERR_NOT_FOUND);
  // DUCK: no rule targets Sfx in this bank.
  SND_CHECK_EQ(ApplyDirect(f, HashId("Sfx"), bf::chunk::DUCK, bf::col::DUCK_amountDb, ll::ValueType::F32, 0,
                           F32Bits(-6.0f))
                   .result,
               SND_ERR_NOT_FOUND);
  // NaN / Inf at the applier (bypassing the wire check).
  SND_CHECK_EQ(ApplyDirect(f, HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb, ll::ValueType::F32, 0,
                           F32Bits(std::numeric_limits<float>::infinity()))
                   .result,
               SND_ERR_INVALID_ARGUMENT);
  // Edited: stealMode, virtualReturn, scopeMask, loopCount.
  SND_CHECK_EQ(f.rt->applier().Remembered(), 4u);
}

// --- through the server ------------------------------------------------------------------

namespace {
struct LiveSession {
  Fixture f;
  std::unique_ptr<Tool> tool;
  double base = 0;
  explicit LiveSession(const snd_config& cfg = lltest::Config()) : f(lltest::Hooks::Live, cfg) {
    f.Load(IndepBank(), "a.sndb");
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    base = Settle(f);
    if (f.StartServer() == SND_OK) tool = std::make_unique<Tool>(f);
  }
};
}  // namespace

// D3: the SetValue Ack is sent only after the audio thread applied the edit: no
// block, no Ack; when the Ack arrives the edit is in the override table.
SND_TEST(IndepLivelinkAck, SetValueAckComesOnlyAfterTheAudioThreadApplied) {
  LiveSession s;
  SND_REQUIRE(s.tool && s.tool->Ready());
  const std::uint32_t seq = s.tool->c->SetValue(lltest::BusVolume("Sfx", -6.0f), true);
  SND_REQUIRE(seq != 0u);
  // No block rendered: the server records and flushes, nothing is applied.
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (s.f.server->Stats().commandsRecorded < 1u && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  SND_REQUIRE_EQ(s.f.server->Stats().commandsRecorded, 1u);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  std::int32_t r = 99;
  SND_CHECK(!s.tool->c->TakeAck(seq, &r));
  SND_CHECK_EQ(s.f.rt->applier().Remembered(), 0u);
  // Render until the Ack: the cell is remembered by then.
  bool rememberedAtAck = false;
  SND_CHECK(s.f.StepUntil([&] {
    if (!s.tool->c->TakeAck(seq, &r)) return false;
    rememberedAtAck = s.f.rt->applier().Remembered() == 1u;
    return true;
  }));
  SND_CHECK_EQ(r, SND_OK);
  SND_CHECK(rememberedAtAck);
  SND_CHECK_NEAR(Settle(s.f) / s.base, DbRatio(-6.0), 1e-3);
}

// D2: wantAck = 0: a success is silent, a failure answers Error(code, seq).
SND_TEST(IndepLivelinkAck, WithoutWantAckOnlyFailuresAreAnswered) {
  LiveSession s;
  SND_REQUIRE(s.tool && s.tool->Ready());
  ll::Client& c = *s.tool->c;
  const std::uint32_t ok = c.SetValue(lltest::BusVolume("Sfx", -6.0f), false);
  ll::SetValue bad = lltest::BusVolume("Sfx", -6.0f);
  bad.runtimeId = HashId("NoSuchBus");
  const std::uint32_t nf = c.SetValue(bad, false);  // refused when applied
  ll::SetValue badCol = lltest::BusVolume("Sfx", -6.0f);
  badCol.column = 200;  // refused at once (wire check)
  const std::uint32_t bc = c.SetValue(badCol, false);
  SND_REQUIRE(ok != 0u && nf != 0u && bc != 0u);
  ll::Error errs[8];
  SND_CHECK(s.f.StepUntil([&] { return c.CopyErrors(errs, 8) >= 2u; }));
  s.f.Steps(20);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  s.f.Steps(4);
  const std::uint32_t n = c.CopyErrors(errs, 8);
  SND_CHECK_EQ(n, 2u);
  bool sawNf = false, sawBc = false;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (errs[i].seq == nf) sawNf = errs[i].code == SND_ERR_NOT_FOUND;
    if (errs[i].seq == bc) sawBc = errs[i].code == SND_ERR_NOT_FOUND;
    SND_CHECK(errs[i].seq != ok);
  }
  SND_CHECK(sawNf);
  SND_CHECK(sawBc);
  std::int32_t r = 0;
  SND_CHECK(!c.TakeAck(ok, &r));
  SND_CHECK(!c.TakeAck(nf, &r));
  SND_CHECK_NEAR(Settle(s.f) / s.base, DbRatio(-6.0), 1e-3);
}

// D1 / D4 / D5: a flood of edits while the audio thread is not consuming fills the
// live link's arenas: the overflow is refused with SND_ERR_COMMAND_BUFFER_FULL, every
// seq is answered exactly once, and the last accepted value wins.
SND_TEST(IndepLivelinkAck, CommandBufferFullIsAnsweredPerSeqAndLaterEditsWork) {
  snd_config cfg = lltest::Config();
  cfg.commands.arena_bytes = 4u << 10;
  cfg.commands.reserve_bytes = 1u << 10;
  LiveSession s(cfg);
  SND_REQUIRE(s.tool && s.tool->Ready());
  ll::Client& c = *s.tool->c;
  constexpr int kN = 600;
  std::vector<std::uint32_t> seqs;
  for (int i = 0; i < kN; ++i) {
    const float db = -1.0f - static_cast<float>(i % 7);
    const std::uint32_t q = c.SetValue(lltest::BusVolume("Sfx", db), true);
    SND_REQUIRE(q != 0u);
    seqs.push_back(q);
  }
  // Without rendering, the refused ones are answered.
  std::uint32_t full = 0;
  std::vector<std::int32_t> res(kN, 1);
  std::vector<int> got(kN, 0);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline && s.f.server->Stats().commandsRecorded +
                                                                s.f.server->Stats().commandsRefused <
                                                            static_cast<std::uint32_t>(kN)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  for (int i = 0; i < kN; ++i) {
    std::int32_t r = 0;
    if (c.TakeAck(seqs[i], &r)) {
      ++got[i];
      res[i] = r;
      full += r == SND_ERR_COMMAND_BUFFER_FULL ? 1u : 0u;
    }
  }
  SND_CHECK(full > 0u);
  SND_CHECK_EQ(full, s.f.server->Stats().commandsRefused);
  // Render: the rest is applied and acknowledged.
  int lastOk = -1;
  SND_CHECK(s.f.StepUntil([&] {
    bool all = true;
    for (int i = 0; i < kN; ++i) {
      std::int32_t r = 0;
      if (got[i] == 0 && c.TakeAck(seqs[i], &r)) {
        ++got[i];
        res[i] = r;
      }
      all = all && got[i] != 0;
    }
    return all;
  }));
  for (int i = 0; i < kN; ++i) {
    SND_CHECK(res[i] == SND_OK || res[i] == SND_ERR_COMMAND_BUFFER_FULL);
    std::int32_t r = 0;
    SND_CHECK(!c.TakeAck(seqs[i], &r));  // exactly once
    if (res[i] == SND_OK) lastOk = i;
  }
  SND_REQUIRE(lastOk >= 0);
  const float lastDb = -1.0f - static_cast<float>(lastOk % 7);
  SND_CHECK_NEAR(Settle(s.f) / s.base, DbRatio(lastDb), 1e-3);
  // The link still works.
  SND_CHECK_EQ(s.tool->Ack(c.SetValue(lltest::BusVolume("Sfx", -20.0f), true)), SND_OK);
  SND_CHECK_NEAR(Settle(s.f) / s.base, DbRatio(-20.0), 1e-3);
  SND_CHECK_EQ(c.Stats().connects, 1u);
}

// D1 (defect candidate): a burst of acknowledged edits applied in one block (a diff
// sync after BuildMismatch, protocol §4) must not make the server drop a tool that
// reads normally: every Ack arrives and the connection stays.
SND_TEST(IndepLivelinkAck, AckBurstKeepsTheTool) {
  snd_config cfg = lltest::Config();
  cfg.commands.arena_bytes = 64u << 10;
  LiveSession s(cfg);
  SND_REQUIRE(s.tool && s.tool->Ready());
  ll::Client& c = *s.tool->c;
  constexpr int kN = 900;  // below resultEntries (1024) and the arena capacity
  std::vector<std::uint32_t> seqs;
  for (int i = 0; i < kN; ++i) {
    const std::uint32_t q = c.SetValue(lltest::BusVolume("Sfx", -1.0f - static_cast<float>(i % 5)), true);
    SND_REQUIRE(q != 0u);
    seqs.push_back(q);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline &&
         s.f.server->Stats().commandsRecorded < static_cast<std::uint32_t>(kN)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  SND_REQUIRE_EQ(s.f.server->Stats().commandsRecorded, static_cast<std::uint32_t>(kN));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));  // flushed
  s.f.Step();  // all of them in one block
  std::vector<int> got(kN, 0);
  int n = 0;
  s.f.StepUntil(
      [&] {
        for (int i = 0; i < kN; ++i) {
          std::int32_t r = 0;
          if (got[i] == 0 && c.TakeAck(seqs[i], &r)) {
            got[i] = 1;
            ++n;
          }
        }
        return n == kN;
      },
      3000);
  SND_CHECK_MSG(n == kN, "%d of %d Acks arrived", n, kN);
  SND_CHECK_MSG(c.Stats().disconnects == 0u, "the server dropped the tool %u time(s)", c.Stats().disconnects);
  SND_CHECK_EQ(s.f.server->Stats().protocolErrors, 0u);
}
