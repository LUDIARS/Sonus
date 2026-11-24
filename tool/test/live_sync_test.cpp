// LiveSync (group P, design.md 11.2) against a real runtime: the preview engine (core, source
// kinds, music, the live link server on the in-process transport), connected with an ordinary
// Connection. The DC material of live_fixture.hpp makes the levels analytic.
#include <cmath>
#include <cstring>

#include "live_fixture.hpp"
#include "snd/buildtool/json.hpp"
#include "snd/buildtool/patch.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/livelink/client.hpp"
#include "snd/tool/session.hpp"

using namespace snd::tool;
using livetest::Ratio;
using livetest::Rig;
namespace json = snd::buildtool::json;
namespace bf = snd::bank_format;

namespace {

json::Value Json(const std::string& text) {
  json::Value v;
  json::ParseError e{};
  json::Parse(text, &v, &e);
  return v;
}

// Plays Tone and returns its steady level.
double PlayTone(Rig& r) {
  r.engine->Play(snd::HashId("Tone"), nullptr);
  r.Frames(8);
  return r.Left();
}

}  // namespace

SND_TEST(LiveSync, MatchingBuildIsLiveWithNothingToSend) {
  Rig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  SND_CHECK(r.sync->check().valid);
  SND_CHECK(r.sync->check().banksKnown);
  SND_CHECK_EQ(r.sync->check().peerMinor, 1u);
  SND_REQUIRE_EQ(r.sync->check().banks.size(), 1u);
  SND_CHECK(r.sync->check().banks[0].verdict == BankVerdict::Same);
  SND_CHECK_EQ(r.sync->stats().sent, 0u);
  for (const LiveFeature& f : r.sync->features()) SND_CHECK_MSG(f.available, "%s: %s", f.name, f.reason.c_str());
}

SND_TEST(LiveSync, ValueEditUndoRedoAndThinningReachTheGame) {
  LiveSyncOptions o = livetest::MakeOptions();
  o.throttleMs = 50;
  Rig r(false, {}, o);
  SND_REQUIRE(r.ok);
  const double base = PlayTone(r);
  SND_REQUIRE(base > 0.05);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  // A drag: 40 steps in quick frames, one undo step; the last value must arrive.
  r.doc.BeginGroup();
  for (int i = 1; i <= 40; ++i) {
    SND_REQUIRE(r.doc.SetNumber(livetest::SfxGuid(), "volumeDb", -0.3 * i));
    r.Frame();
  }
  r.doc.EndGroup();
  r.sync->FlushNow();
  SND_REQUIRE(r.Settled());
  SND_CHECK_NEAR(r.Left() / base, Ratio(-12.0), 1e-5);
  SND_CHECK(r.sync->stats().values < 40u);  // thinned
  SND_CHECK(r.sync->stats().values >= 1u);
  const LiveItem* it = r.sync->Find(livetest::SfxGuid(), "volumeDb");
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->route == LiveRoute::SetValue);
  SND_CHECK(it->state == LiveState::Live);
  // Undo and redo are edits like any other.
  SND_REQUIRE(r.doc.Undo());
  SND_REQUIRE(r.Settled());
  SND_CHECK_NEAR(r.Left() / base, 1.0, 1e-5);
  SND_REQUIRE(r.doc.Redo());
  SND_REQUIRE(r.Settled());
  SND_CHECK_NEAR(r.Left() / base, Ratio(-12.0), 1e-5);
  // Saving changes nothing live (an in-memory document: MarkSaved).
  const std::uint64_t sent = r.sync->stats().sent;
  r.doc.MarkSaved();
  r.Frames(4);
  SND_CHECK_EQ(r.sync->stats().sent, sent);
  SND_CHECK(r.sync->problems().empty());
}

namespace {
// Reads one cell of the game through the tool's client.
bool ReadGameCell(Rig& r, std::uint32_t id, std::uint32_t chunk, std::uint32_t column, snd::livelink::CellValue* v) {
  snd::livelink::SetValue cell{};
  cell.runtimeId = id;
  cell.chunk = chunk;
  cell.column = column;
  snd::livelink::Client* c = r.conn.client();
  const std::uint32_t seq = c->ReadCell(cell);
  return seq != 0u && r.Run([&] { return c->TakeCell(seq, v); });
}
}  // namespace

SND_TEST(LiveSync, ShapePreservingEditGoesAsReplaceChunkElseReload) {
  Rig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  // EVNT.fadeOutMs is 0 on every event: the build leaves the column out. Setting it adds the
  // column: the table keeps its rows, so the whole table goes (ReplaceChunk), not a cell.
  SND_REQUIRE(r.doc.SetNumber(livetest::ToneGuid(), "fadeOutMs", 50));
  SND_REQUIRE(r.Settled());
  SND_CHECK_EQ(r.sync->stats().chunks, 1u);
  const LiveItem* it = r.sync->Find(livetest::ToneGuid());
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->route == LiveRoute::ReplaceChunk);
  SND_CHECK_MSG(it->state == LiveState::Live, "%s %s", LiveStateName(it->state), it->reason.c_str());
  SND_CHECK(r.sync->reloads().empty());
  snd::livelink::CellValue v{};
  SND_REQUIRE(ReadGameCell(r, snd::HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_fadeOutMs, &v));
  SND_CHECK_EQ(v.result, SND_OK);
  float f = 0.0f;
  const std::uint32_t bits = static_cast<std::uint32_t>(v.current);
  std::memcpy(&f, &bits, 4);
  SND_CHECK_EQ(f, 50.0f);
  // EVNT.priority: the runtime refuses it live (core CheckEdit), cell or table: the bank is
  // rebuilt and hot reloaded, without a problem shown.
  SND_REQUIRE(r.doc.SetNumber(livetest::ToneGuid(), "priority", 200));
  SND_REQUIRE(r.Settled());
  it = r.sync->Find(livetest::ToneGuid());
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->route == LiveRoute::Reload);
  SND_CHECK_MSG(it->state == LiveState::Live, "%s %s", LiveStateName(it->state), it->reason.c_str());
  SND_REQUIRE_EQ(r.sync->reloads().size(), 1u);
  SND_CHECK_EQ(r.sync->reloads()[0].result, SND_OK);
  SND_CHECK(r.sync->problems().empty());
  SND_REQUIRE(ReadGameCell(r, snd::HashId("Tone"), bf::chunk::EVNT, bf::col::EVNT_priority, &v));
  SND_CHECK_EQ(v.current, 200u);
}

SND_TEST(LiveSync, NewObjectGoesAsHotReloadAndEditsMeanwhileFollow) {
  Rig r;
  SND_REQUIRE(r.ok);
  const double base = PlayTone(r);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  // A new event: rows change -> rebuild and hot reload (the engine's rebuilt-bank writer).
  const json::Value init = Json("{\"bank\": \"Main\", \"bus\": \"Sfx\", \"tracks\": [{\"clips\": "
                                "[{\"asset\": \"dc\", \"loopCount\": 0}]}]}");
  const std::string g = r.doc.Create("event", "Tone2", &init);
  SND_REQUIRE(!g.empty());
  r.Frame();  // the reload is queued / sent
  // An edit right after: waits for the reload (never NOT_READY to the user), then goes out.
  SND_REQUIRE(r.doc.SetNumber(livetest::SfxGuid(), "volumeDb", -6.0));
  SND_REQUIRE(r.Settled());
  SND_REQUIRE(!r.sync->reloads().empty());
  const ReloadProgress& p = r.sync->reloads().back();
  SND_CHECK(p.finished);
  SND_CHECK_EQ(p.result, SND_OK);
  SND_CHECK(p.phase == snd::livelink::ReloadPhase::Done);
  SND_CHECK(!p.detached);
  SND_CHECK(r.sync->problems().empty());
  for (const LiveProblem& q : r.sync->problems()) std::printf("problem: %s\n", q.message.c_str());
  // The reload stopped the bank's sounds; the new event plays at the edited bus volume.
  r.Frames(4);
  SND_CHECK_EQ(r.Left(), 0.0f);
  SND_REQUIRE(r.engine->Play(snd::HashId("Tone2"), nullptr) != 0u);
  r.Frames(8);
  SND_CHECK_NEAR(r.Left() / base, Ratio(-6.0), 1e-5);
}

SND_TEST(LiveSync, StructuralEditWithinTheReserveOrFallsBackToReload) {
  const std::string effect = "{\"type\": \"snd.fx.delay\", \"params\": [10, 0, 0, -96, 20000, 0, 100]}";
  {  // An editor in core (test double for C-01): StructEdit, the reserve is used.
    livetest::YesEditor editor;
    snd::tool::preview::PreviewEngineDesc ed;
    ed.structEditor = &editor;
    Rig r(false, ed);
    SND_REQUIRE(r.ok);
    SND_REQUIRE(r.Connect());
    SND_REQUIRE(r.Settled());
    SND_REQUIRE(r.doc.Insert(livetest::G(4), "effects", 0, Json(effect)));
    SND_REQUIRE(r.Settled());
    SND_CHECK_EQ(editor.applied, 1u);
    SND_REQUIRE_EQ(r.sync->structEdits().size(), 1u);
    SND_CHECK(r.sync->structEdits()[0].op == snd::livelink::StructOp::AddEffect);
    SND_CHECK(r.sync->structEdits()[0].applied);
    SND_CHECK_EQ(r.sync->reserve().effects, 1u);
    SND_CHECK(r.sync->reloads().empty());
    // Undo of the live structural edit (RemoveEffect).
    SND_REQUIRE(r.sync->UndoStructEdit(0));
    SND_REQUIRE(r.Settled());
    SND_CHECK(r.sync->structEdits()[0].undone);
    SND_CHECK_EQ(r.sync->reserve().effects, 0u);
  }
  {  // No editor (today's core): UNSUPPORTED -> rebuild and hot reload, without a problem shown.
    Rig r;
    SND_REQUIRE(r.ok);
    SND_REQUIRE(r.Connect());
    SND_REQUIRE(r.Settled());
    SND_REQUIRE(r.doc.Insert(livetest::G(4), "effects", 0, Json(effect)));
    SND_REQUIRE(r.Settled());
    SND_REQUIRE(!r.sync->reloads().empty());
    SND_CHECK_EQ(r.sync->reloads().back().result, SND_OK);
    SND_CHECK(r.sync->problems().empty());
    SND_CHECK_EQ(r.sync->reserve().effects, 0u);
  }
}

SND_TEST(LiveSync, MismatchAsksThenSendsEveryDiff) {
  Rig r;
  SND_REQUIRE(r.ok);
  const double base = PlayTone(r);
  // The tool's document moved on (unsaved edits) before it connects.
  SND_REQUIRE(r.doc.SetNumber(livetest::SfxGuid(), "volumeDb", -6.0));
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Run([&] { return r.sync->pending(); }));
  const SyncCheck& ck = r.sync->check();
  SND_CHECK(ck.valid);
  SND_CHECK(ck.helloStatus == static_cast<std::uint32_t>(snd::livelink::HelloStatus::BuildMismatch));
  SND_CHECK(ck.toolBuildHash != ck.gameBuildHash);
  SND_REQUIRE(r.Run([&] { return r.sync->check().banksKnown; }));
  SND_REQUIRE_EQ(r.sync->check().banks.size(), 1u);
  SND_CHECK(r.sync->check().banks[0].verdict == BankVerdict::Differs);
  SND_CHECK(r.sync->check().banks[0].knownBase);
  // Nothing is sent while the user decides.
  r.Frames(16);
  SND_CHECK_EQ(r.sync->stats().sent, 0u);
  SND_CHECK_NEAR(r.Left() / base, 1.0, 1e-6);
  SND_REQUIRE(r.sync->ChooseSendDiffs(r.conn));
  SND_REQUIRE(r.Settled());
  SND_CHECK_NEAR(r.Left() / base, Ratio(-6.0), 1e-5);
  SND_CHECK(r.conn.mismatch());
}

SND_TEST(LiveSync, MismatchRefusedSendsNothing) {
  Rig r;
  SND_REQUIRE(r.ok);
  const double base = PlayTone(r);
  SND_REQUIRE(r.doc.SetNumber(livetest::SfxGuid(), "volumeDb", -6.0));
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Run([&] { return r.sync->pending(); }));
  SND_REQUIRE(r.sync->ChooseRefuse(r.conn));
  SND_REQUIRE(r.Run([&] { return r.sync->phase() == LivePhase::Refused; }));
  SND_CHECK(r.conn.state() == LinkState::Rejected);
  r.Frames(16);
  SND_CHECK_EQ(r.sync->stats().sent, 0u);
  SND_CHECK_NEAR(r.Left() / base, 1.0, 1e-6);
}

SND_TEST(LiveSync, FailedReloadIsAProblemAndTheBankIsDetached) {
  LiveSyncOptions o = livetest::MakeOptions(LiveDecision::SendDiffs);
  o.writer = [](const BankImage&, std::string* path, std::string*) {
    *path = "no/such/bank.sndb";  // the game cannot read it
    return true;
  };
  Rig r(false, {}, o);
  SND_REQUIRE(r.ok);
  PlayTone(r);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  const json::Value init = Json("{\"bank\": \"Main\", \"bus\": \"Sfx\"}");
  SND_REQUIRE(!r.doc.Create("event", "Tone2", &init).empty());
  SND_REQUIRE(r.Run([&] { return !r.sync->reloads().empty() && r.sync->reloads().back().finished; }));
  const ReloadProgress& p = r.sync->reloads().back();
  SND_CHECK(p.result != SND_OK);
  SND_CHECK(p.phase == snd::livelink::ReloadPhase::Failed);
  SND_CHECK(p.detached);
  SND_REQUIRE(!r.sync->problems().empty());
  const LiveProblem& q = r.sync->problems().back();
  SND_CHECK(q.route == LiveRoute::Reload);
  SND_CHECK(q.message.find("detached") != std::string::npos);
  SND_CHECK(q.revision != 0u);
  r.Frames(4);
  SND_CHECK_EQ(r.Left(), 0.0f);  // the bank was unloaded and did not come back
}

SND_TEST(LiveSync, ReconnectSendsTheUnsavedStateAgain) {
  snd::tool::preview::PreviewEngineDesc ed;
  ed.revertOnDisconnect = true;  // the game puts its bank values back when the tool leaves
  Rig r(false, ed, livetest::MakeOptions(LiveDecision::SendDiffs));
  SND_REQUIRE(r.ok);
  const double base = PlayTone(r);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  SND_REQUIRE(r.doc.SetNumber(livetest::SfxGuid(), "volumeDb", -6.0));
  SND_REQUIRE(r.Settled());
  SND_CHECK_NEAR(r.Left() / base, Ratio(-6.0), 1e-5);
  const std::uint64_t valuesBefore = r.sync->stats().values;
  r.conn.Drop();
  SND_REQUIRE(r.Run([&] { return r.conn.sessions() >= 2u; }));
  SND_REQUIRE(r.Settled());
  SND_CHECK_NEAR(r.Left() / base, Ratio(-6.0), 1e-5);
  SND_CHECK(r.sync->stats().values > valuesBefore);  // sent again in the new session
}

SND_TEST(LiveSync, MixerMonitoringValueWinsOverTheDocument) {
  bool held = true;
  LiveSyncOptions o = livetest::MakeOptions();
  o.heldByMixer = [&](snd::Id id, std::uint32_t chunk, std::uint32_t column, std::uint32_t) {
    return held && id == snd::HashId("Sfx") && chunk == bf::chunk::BUSS && column == bf::col::BUSS_volumeDb;
  };
  Rig r(false, {}, o);
  SND_REQUIRE(r.ok);
  const double base = PlayTone(r);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  SND_REQUIRE(r.doc.SetNumber(livetest::SfxGuid(), "volumeDb", -6.0));
  SND_REQUIRE(r.Settled());
  const LiveItem* it = r.sync->Find(livetest::SfxGuid(), "volumeDb");
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->state == LiveState::HeldByMixer);
  SND_CHECK_NEAR(r.Left() / base, 1.0, 1e-6);
}

SND_TEST(LiveSync, WhatTheRuntimeCannotTakeIsListedWithAReason) {
  const LiveSupport phys = LiveSupportOf(bf::chunk::PHYS);
  SND_CHECK(!phys.supported && phys.blocking && phys.reason.find("C-11") != std::string::npos);
  const LiveSupport patch = LiveSupportOf(snd::buildtool::patch::kChunkFourcc);
  SND_CHECK(!patch.supported && patch.blocking && patch.reason.find("C-10") != std::string::npos);
  const LiveSupport compose = LiveSupportOf(bf::chunk::CURV, bf::col::CURV_compose, 2, true);
  SND_CHECK(!compose.supported && !compose.blocking);
  const LiveSupport send = LiveSupportOf(bf::chunk::CURV, bf::col::CURV_target,
                                         static_cast<std::uint32_t>(bf::CurveTarget::Send), true);
  SND_CHECK(!send.supported && !send.blocking);
  SND_CHECK(LiveSupportOf(bf::chunk::CURV, bf::col::CURV_target, static_cast<std::uint32_t>(bf::CurveTarget::Volume), true).supported);
  SND_CHECK(!LiveSupportOf(bf::chunk::SNPV, bf::col::SNPV_targetKind, 1, true).supported);
  SND_CHECK(LiveSupportOf(bf::chunk::BUSS, bf::col::BUSS_volumeDb).supported);
  // Offline: no game; every 1.1 feature says why it is not available.
  ProjectDocument doc;
  livetest::LoadLive(doc);
  LiveSync s(doc);
  for (const LiveFeature& f : s.features()) {
    if (!f.available) SND_CHECK(!f.reason.empty());
  }
  SND_CHECK(s.phase() == LivePhase::Offline);
}

SND_TEST(LiveSync, SessionCarriesTheDocumentAndTheDetailProfiler) {
  Rig r;
  SND_REQUIRE(r.ok);
  const double base = PlayTone(r);
  Project project;
  SND_REQUIRE(project.Load(r.doc));
  Session s;
  s.UseProject(project);
  LiveSyncOptions o = livetest::MakeOptions(LiveDecision::SendDiffs);
  snd::tool::preview::PreviewEngine* e = r.engine.get();
  o.writer = [e](const BankImage& b, std::string* path, std::string*) {
    *path = e->PutRebuiltBank(b.file, b.bytes);
    return true;
  };
  s.AttachDocument(&r.doc, o);
  s.live()->SetBaseline(r.gameBuild);
  s.EnableProfilerStreams(kViewCpu | kViewMemory);
  ConnectionDesc d;
  d.connector = r.engine->connector();
  d.reconnectDelayMs = 20;
  SND_REQUIRE_EQ(s.Connect(d), SND_OK);
  auto run = [&](auto pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > end) return false;
      r.out.clear();
      r.engine->Render(1, &r.out);
      s.Tick();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
  };
  SND_REQUIRE(run([&] { return s.live()->phase() == LivePhase::Live; }));
  SND_REQUIRE(r.doc.SetNumber(livetest::SfxGuid(), "volumeDb", -6.0));
  s.Tick();
  SND_REQUIRE(run([&] { return s.live()->phase() == LivePhase::Live; }));
  r.Frames(6);
  SND_CHECK_NEAR(r.Left() / base, Ratio(-6.0), 1e-5);
  // The mixer's monitoring value on the same cell wins over later document edits.
  SND_REQUIRE(s.mixer().Set(Target{Control::BusFader, project.FindBus(snd::HashId("Sfx")), 0, 0}, -12.0f));
  s.Tick();  // CellSync sends the mixer's value in a Tick (the condition below holds before it)
  SND_REQUIRE(s.sync().stats().sent >= 1u);
  SND_REQUIRE(run([&] { return s.sync().Count(CellState::Pending) == 0u && s.sync().Count(CellState::Unsent) == 0u; }));
  SND_REQUIRE(r.doc.SetNumber(livetest::SfxGuid(), "volumeDb", -3.0));
  s.Tick();
  SND_REQUIRE(run([&] {
    return s.live()->phase() == LivePhase::Live && s.sync().Count(CellState::Pending) == 0u &&
           s.sync().Count(CellState::Unsent) == 0u;
  }));
  r.Frames(8);
  SND_CHECK_NEAR(r.Left() / base, Ratio(-12.0), 1e-5);
  const LiveItem* it = s.live()->Find(livetest::SfxGuid(), "volumeDb");
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->state == LiveState::HeldByMixer);
  SND_REQUIRE(run([&] { return s.streams().hasMemory() && !s.streams().cpu().empty(); }));
}
