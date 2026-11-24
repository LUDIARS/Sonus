// Independent tests of group P: matching, the two choices and the live reflection of document
// edits (tool/test/indep_s7_live_properties.md A, B, C) against the real runtime (preview engine)
// and a hand-driven game (MiniGame). Expected values are analytic (dB ratios, the document's build
// read back from the game with ReadCell).
#include <algorithm>
#include <cmath>
#include <random>

#include "indep_s7_live_fixture.hpp"

using namespace snd::tool;
using namespace ilive;

namespace {

double PlayLevel(Rig& r, const char* event) {
  r.engine->Play(snd::HashId(event), nullptr);
  r.Frames(8);
  return r.Left();
}

// Every f32 live cell of the document's build equals the game's (ReadCell).
std::uint32_t CompareGameWithDocument(Rig& r, const std::vector<std::pair<std::string, std::string>>& members,
                                      std::string* first) {
  const DocBuild b = BuildDocument(r.doc);
  std::uint32_t diffs = 0;
  for (const LiveCell& c : F32CellsOf(b, r.doc, members)) {
    const float g = r.GameF32(c.id, c.chunk, c.column, c.index);
    if (!(g == c.value)) {
      if (diffs == 0u && first != nullptr) {
        char m[160];
        std::snprintf(m, sizeof m, "id %08x chunk %08x column %u: game %g, document %g", c.id, c.chunk, c.column,
                      static_cast<double>(g), static_cast<double>(c.value));
        *first = m;
      }
      ++diffs;
    }
  }
  return diffs;
}

const std::vector<std::pair<std::string, std::string>>& Members() {
  static const std::vector<std::pair<std::string, std::string>> m = {
      {SfxG(), "volumeDb"}, {AmbG(), "volumeDb"}, {SpotG(), "maxDistance"}, {Knob2G(), "default"}};
  return m;
}

}  // namespace

// A1
SND_TEST(IndepS7Live, MatchingBuildIsLiveAndSendsNothing) {
  Rig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const SyncCheck& c = r.sync->check();
  SND_CHECK(c.valid);
  SND_CHECK_EQ(c.helloStatus, static_cast<std::uint32_t>(ll::HelloStatus::Accepted));
  SND_CHECK_EQ(c.toolBuildHash, c.gameBuildHash);
  SND_CHECK_EQ(c.banks.size(), 2u);
  for (const BankMatch& m : c.banks) SND_CHECK_MSG(m.verdict == BankVerdict::Same, "%s", m.file.c_str());
  const LiveSyncStats& s = r.sync->stats();
  SND_CHECK_EQ(s.values + s.chunks + s.structEdits + s.reloads, 0u);
  SND_CHECK(!r.sync->pending());
  SND_CHECK(r.sync->problems().empty());
}

// A2, A5
SND_TEST(IndepS7Live, MismatchSendsNothingUntilAnsweredAndRefuseChangesNothing) {
  Rig r({}, Options(LiveDecision::Ask), [](ProjectDocument& g) { g.SetNumber(SfxG(), "volumeDb", -7.0); });
  SND_REQUIRE(r.ok);
  const double base = PlayLevel(r, "Tone");  // the game's Sfx: -7 dB
  SND_REQUIRE(base > 0.01);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Run([&] { return r.sync->pending(); }));
  SND_CHECK(r.sync->phase() == LivePhase::AwaitingDecision);
  // Edits while the user decides are not sent either.
  SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -20.0));
  SND_REQUIRE(r.doc.SetNumber(AmbG(), "volumeDb", -20.0));
  r.Frames(40);
  SND_CHECK(r.sync->pending());
  SND_CHECK_EQ(r.sync->stats().sent, 0u);
  SND_CHECK_NEAR(r.Left() / base, 1.0, 1e-6);
  SND_REQUIRE(r.sync->ChooseRefuse(r.conn));
  SND_REQUIRE(r.Run([&] { return r.sync->phase() == LivePhase::Refused; }));
  // Refused: nothing ever goes, whatever the document does.
  SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -30.0));
  r.Frames(60);
  SND_CHECK(r.sync->phase() == LivePhase::Refused);
  SND_CHECK_EQ(r.sync->stats().sent, 0u);
  SND_CHECK_NEAR(r.Left() / base, 1.0, 1e-6);
  // A second answer is not possible.
  SND_CHECK(!r.sync->ChooseSendDiffs(r.conn));
}

// A3, A4: verdicts per bank, then "send every diff" makes every cell of the game the document's.
SND_TEST(IndepS7Live, VerdictsPerBankAndSendDiffsMakesTheGameTheDocument) {
  Rig r({}, Options(LiveDecision::Ask), [](ProjectDocument& g) {
    g.SetNumber(SfxG(), "volumeDb", -7.0);
    g.SetNumber(SpotG(), "maxDistance", 33.0);
    g.Remove(Knob2G(), true);  // the game has no Aux bank
    g.Remove(G(219), true);
    g.Create("bank", "Extra");  // ... but a bank the tool does not have
    const json::Value p = Json("{\"bank\": \"Extra\", \"min\": 0, \"max\": 1}");
    g.Create("parameter", "Lone", &p);
  });
  SND_REQUIRE(r.ok);
  SND_REQUIRE_EQ(r.gameBuild.banks.size(), 2u);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Run([&] { return r.sync->pending() && r.sync->check().banksKnown; }));
  auto verdicts = [&](const char* when) {
    const SyncCheck& c = r.sync->check();
    SND_CHECK(c.toolBuildHash != c.gameBuildHash);
    std::uint32_t same = 0, differs = 0, toolOnly = 0, gameOnly = 0;
    for (const BankMatch& m : c.banks) {
      if (m.verdict == BankVerdict::Same) ++same;
      if (m.verdict == BankVerdict::Differs) {
        ++differs;
        SND_CHECK_MSG(m.file == "Main.sndb", "%s: %s differs", when, m.file.c_str());
      }
      if (m.verdict == BankVerdict::ToolOnly) {
        ++toolOnly;
        SND_CHECK_MSG(m.file == "Aux.sndb", "%s: %s tool only", when, m.file.c_str());
      }
      if (m.verdict == BankVerdict::GameOnly) {
        ++gameOnly;
        SND_CHECK_EQ(m.bankId, snd::HashId("Extra"));
      }
    }
    SND_CHECK_MSG(same == 0u && differs == 1u && toolOnly == 1u && gameOnly == 1u, "%s: same %u differs %u tool %u game %u",
                  when, same, differs, toolOnly, gameOnly);
  };
  verdicts("asking");
  SND_REQUIRE(r.sync->ChooseSendDiffs(r.conn));
  SND_REQUIRE(r.Settle());
  verdicts("after");
  std::string first;
  SND_CHECK_MSG(CompareGameWithDocument(r, Members(), &first) == 0u, "%s", first.c_str());
  // The tool-only bank was loaded with the document's content.
  const DocBuild b = BuildDocument(r.doc);
  bool aux = false;
  for (const ll::BankEntry& e : r.GameBanks()) {
    if (e.bankId == snd::HashId("Aux")) aux = e.contentHash == b.FindBank("Aux")->contentHash;
  }
  SND_CHECK(aux);
  SND_CHECK(r.sync->problems().empty());
}

// B1, B2, B11: a value edit reaches the game (level and read-back); Undo and Redo too; saving
// sends nothing.
SND_TEST(IndepS7Live, ValueEditUndoRedoReachTheGame) {
  Rig r;
  SND_REQUIRE(r.ok);
  const double base = PlayLevel(r, "Tone");
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -1.0 - 6.0206));
  SND_REQUIRE(r.Settle());
  SND_CHECK_NEAR(r.Left() / base, 0.5, 1e-4);
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), static_cast<float>(-1.0 - 6.0206));
  SND_REQUIRE(r.doc.Undo());
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -1.0f);
  SND_CHECK_NEAR(r.Left() / base, 1.0, 1e-5);
  SND_REQUIRE(r.doc.Redo());
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), static_cast<float>(-1.0 - 6.0206));
  const std::uint64_t sent = r.sync->stats().sent;
  r.doc.MarkSaved();
  r.Frames(10);
  SND_CHECK_EQ(r.sync->stats().sent, sent);
  SND_CHECK(r.sync->phase() == LivePhase::Live);
  const LiveItem* it = r.sync->Find(SfxG(), "volumeDb");
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->route == LiveRoute::SetValue);
  SND_CHECK(it->state == LiveState::Live);
}

// B3: a group of 50 steps, one Undo back.
SND_TEST(IndepS7Live, GroupSendsTheLastValueAndOneUndoGoesBack) {
  Rig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  r.doc.BeginGroup();
  for (int i = 1; i <= 50; ++i) {
    SND_REQUIRE(r.doc.SetNumber(AmbG(), "volumeDb", -2.0 - 0.25 * i));
    if (i % 3 == 0) r.Frame();
  }
  r.doc.EndGroup();
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Amb"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -14.5f);
  SND_REQUIRE(r.doc.Undo());
  SND_CHECK(!r.doc.CanUndo());
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Amb"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -2.0f);
}

// B4: thinning: fewer sends than steps, the last value arrives and nothing older overtakes it.
SND_TEST(IndepS7Live, ThinningKeepsTheLastValue) {
  LiveSyncOptions o = Options();
  o.throttleMs = 25;
  Rig r({}, o);
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  for (int i = 1; i <= 200; ++i) {
    SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -0.125 * i, 77u));
    r.Frame();
  }
  // No FlushNow: the last state must be built and sent once the interval has passed.
  SND_REQUIRE(r.Run([&] { return r.sync->phase() == LivePhase::Live; }));
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -25.0f);
  SND_CHECK_MSG(r.sync->stats().values < 200u, "values %llu", static_cast<unsigned long long>(r.sync->stats().values));
  SND_CHECK(r.sync->stats().values >= 1u);
}

// B5: a new event (rows change): the game's bank becomes the document's build.
SND_TEST(IndepS7Live, NewEventReachesTheGameByReload) {
  Rig r;
  SND_REQUIRE(r.ok);
  const double hum = PlayLevel(r, "Hum");  // same bus, same material
  r.engine->StopAll();
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const json::Value init = Json("{\"bank\": \"Main\", \"bus\": \"Amb\", \"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": 0}]}]}");
  SND_REQUIRE(!r.doc.Create("event", "Fresh", &init).empty());
  SND_REQUIRE(r.Settle());
  const DocBuild b = BuildDocument(r.doc);
  std::uint64_t mainHash = 0;
  for (const ll::BankEntry& e : r.GameBanks()) {
    if (e.bankId == snd::HashId("Main")) mainHash = e.contentHash;
  }
  SND_CHECK_EQ(mainHash, b.FindBank("Main")->contentHash);
  SND_CHECK(r.sync->problems().empty());
  SND_REQUIRE(r.engine->Play(snd::HashId("Fresh"), nullptr) != 0u);
  r.Frames(8);
  SND_CHECK_NEAR(r.Left(), hum, 1e-6);
}

// B6, C3: a bus / an effect added without a structural editor in core goes by reload; with an
// editor an appended effect is a StructEdit within the reserve (effects 1, bytes 64 + 8 x args)
// and UndoStructEdit gives it back.
SND_TEST(IndepS7Live, StructuralEditsTakeTheReserveAndUndoGivesItBack) {
  const json::Value bus = Json("{\"bank\": \"Main\", \"parent\": \"Master\", \"volumeDb\": -4}");
  const json::Value fx = Json("{\"type\": \"snd.fx.delay\", \"params\": [10, 0, 0, -96, 20000, 0, 100]}");
  {  // no structural editor in core (C-01): a reload, no problem, nothing taken
    Rig r;
    SND_REQUIRE(r.ok);
    SND_REQUIRE(r.Connect());
    SND_REQUIRE(r.Settle());
    SND_REQUIRE(!r.doc.Create("bus", "Extra", &bus).empty());
    SND_REQUIRE(r.Settle());
    SND_REQUIRE(!r.sync->reloads().empty());
    SND_CHECK(r.sync->reloads().back().phase == ll::ReloadPhase::Done);
    SND_REQUIRE(r.doc.Insert(AmbG(), "effects", 0, fx));
    SND_REQUIRE(r.Settle());
    SND_CHECK(r.sync->reloads().back().phase == ll::ReloadPhase::Done);
    SND_CHECK(r.sync->problems().empty());
    SND_CHECK_EQ(r.sync->reserve().buses, 0u);
    SND_CHECK_EQ(r.sync->reserve().effects, 0u);
  }
  AcceptAll editor;
  preview::PreviewEngineDesc ed;
  ed.structEditor = &editor;
  Rig r(ed);
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const ReserveUsage none = r.sync->reserve();
  SND_CHECK_EQ(none.limits.buses, 16u);
  SND_CHECK_EQ(none.limits.effects, 32u);
  SND_CHECK_EQ(none.limits.bytes, 1ull << 20);
  SND_REQUIRE(r.doc.Insert(AmbG(), "effects", 0, fx));
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(editor.applied, 1u);
  SND_REQUIRE_EQ(r.sync->structEdits().size(), 1u);
  const StructEditInfo s = r.sync->structEdits()[0];
  SND_CHECK(s.op == ll::StructOp::AddEffect);
  SND_CHECK_EQ(s.target, snd::HashId("Amb"));
  SND_CHECK(s.applied);
  SND_REQUIRE(s.args.size() >= 2u);
  SND_CHECK_EQ(s.args[1], 0xFFFFFFFFull);  // appended
  const ReserveUsage u = r.sync->reserve();
  SND_CHECK_EQ(u.effects, 1u);
  SND_CHECK_EQ(u.buses, 0u);
  SND_CHECK_EQ(u.bytes, 64u + 8u * s.args.size());  // protocol §8.4
  SND_REQUIRE(r.sync->UndoStructEdit(0));
  SND_REQUIRE(r.Settle());
  SND_CHECK(r.sync->structEdits()[0].undone);
  SND_CHECK_EQ(editor.undone + editor.applied, 2u);
  SND_CHECK_EQ(r.sync->reserve().effects, 0u);
  SND_CHECK_EQ(r.sync->reserve().bytes, 0u);
  SND_CHECK(!r.sync->UndoStructEdit(0));   // twice: nothing to undo
  SND_CHECK(!r.sync->UndoStructEdit(99));  // out of range
}

// Records today's behaviour (a spec gap, notes): an added bus always moves the master bus's row
// (BUSS rows are children first), so events on the master change (EVNT.bus) and the edit is not
// "buses only": it goes by reload even with a structural editor in core; StructEdit AddBus is
// never chosen for this project.
SND_TEST(IndepS7Live, Record_AddedBusGoesByReloadEvenWithAnEditor) {
  AcceptAll editor;
  preview::PreviewEngineDesc ed;
  ed.structEditor = &editor;
  Rig r(ed);
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const json::Value bus = Json("{\"bank\": \"Main\", \"parent\": \"Master\"}");
  SND_REQUIRE(!r.doc.Create("bus", "Zed", &bus).empty());
  SND_REQUIRE(r.Settle());
  std::printf("record: AddBus: struct edits %u, editor applied %u, reloads %u\n",
              static_cast<unsigned>(r.sync->structEdits().size()), editor.applied, static_cast<unsigned>(r.sync->reloads().size()));
  SND_CHECK(r.sync->problems().empty());
  // The new bus is in the game either way.
  bool zed = false;
  for (const ll::BankEntry& e : r.GameBanks()) {
    if (e.bankId == snd::HashId("Main")) zed = e.contentHash == BuildDocument(r.doc).FindBank("Main")->contentHash || editor.applied == 1u;
  }
  SND_CHECK(zed);
}

// C1: edits while a reload runs are kept, in order (the last value wins), in both banks.
SND_TEST(IndepS7Live, EditsDuringAReloadKeepTheirOrder) {
  Rig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const json::Value init = Json("{\"bank\": \"Main\", \"bus\": \"Sfx\"}");
  SND_REQUIRE(!r.doc.Create("event", "Fresh", &init).empty());
  r.Frame();
  SND_REQUIRE(r.Run([&] { return r.sync->reloading(); }, 5000));
  for (int i = 1; i <= 3; ++i) {
    SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -3.0 * i));
    SND_REQUIRE(r.doc.SetNumber(Knob2G(), "default", 1.0 + i));
    r.Frame();
  }
  SND_REQUIRE(r.Settle());
  std::string first;
  SND_CHECK_MSG(CompareGameWithDocument(r, Members(), &first) == 0u, "%s", first.c_str());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -9.0f);
  SND_CHECK(r.sync->problems().empty());
  for (const LiveProblem& p : r.sync->problems()) std::printf("problem: %s\n", p.message.c_str());
}

// C4: no writer for rebuilt banks: what needs a reload is listed with the reason.
SND_TEST(IndepS7Live, ReloadWithoutWriterIsListed) {
  LiveSyncOptions o = Options();
  o.writer = [](const BankImage&, std::string*, std::string*) { return false; };
  Rig r({}, o);
  SND_REQUIRE(r.ok);
  // the Rig installs its own writer only when none is given: replace it by "none"
  r.sync = std::make_unique<LiveSync>(r.doc, Options());
  r.sync->SetBaseline(r.gameBuild);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const json::Value init = Json("{\"bank\": \"Main\", \"bus\": \"Sfx\"}");
  SND_REQUIRE(!r.doc.Create("event", "Fresh", &init).empty());
  r.Frames(10);
  bool listed = false;
  for (const UnsupportedItem& u : r.sync->unsupported()) listed = listed || (u.bank == "Main.sndb" && !u.reason.empty());
  SND_CHECK(listed);
  bool reloadFeature = true;
  for (const LiveFeature& f : r.sync->features()) {
    if (std::string(f.name).find("hot reload") != std::string::npos) reloadFeature = f.available || f.reason.empty();
  }
  SND_CHECK(!reloadFeature);
}

// B12: non-finite values never leave the document; a value that does not build is a problem and
// the next good value still goes out.
SND_TEST(IndepS7Live, NonFiniteAndUnbuildableValues) {
  Rig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const std::uint64_t sent = r.sync->stats().sent;
  SND_CHECK(!r.doc.SetNumber(SfxG(), "volumeDb", std::nan("")));
  SND_CHECK(!r.doc.SetNumber(SfxG(), "volumeDb", INFINITY));
  SND_CHECK(r.doc.SetNumber(SfxG(), "volumeDb", -1.0));  // the same value: nothing
  r.Frames(8);
  SND_CHECK_EQ(r.sync->stats().sent, sent);
  // A huge value: either the game holds it or the user is told (never silently dropped).
  SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", 1.0e6));
  r.Frames(10);
  r.Run([&] { return r.sync->phase() == LivePhase::Live || !r.sync->problems().empty(); }, 5000);
  const float g = r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb);
  SND_CHECK_MSG(g == 1.0e6f || !r.sync->problems().empty(), "game %g, %u problems", static_cast<double>(g),
                static_cast<unsigned>(r.sync->problems().size()));
  SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -5.0));
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -5.0f);
}

// B9: the mixer holds a cell: the document's value waits; once released, Resync sends it (案 8).
SND_TEST(IndepS7Live, MixerHeldCellWaitsUntilResync) {
  bool held = true;
  LiveSyncOptions o = Options();
  o.heldByMixer = [&](snd::Id id, std::uint32_t chunk, std::uint32_t column, std::uint32_t) {
    return held && id == snd::HashId("Sfx") && chunk == bf::chunk::BUSS && column == bf::col::BUSS_volumeDb;
  };
  Rig r({}, o);
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -9.0));
  SND_REQUIRE(r.doc.SetNumber(AmbG(), "volumeDb", -9.0));  // not held: goes
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -1.0f);
  SND_CHECK_EQ(r.GameF32(snd::HashId("Amb"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -9.0f);
  const LiveItem* it = r.sync->Find(SfxG(), "volumeDb");
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->state == LiveState::HeldByMixer);
  held = false;
  r.sync->Resync();
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -9.0f);
}

// B8 / A7 (the game reverts on disconnect): edits made while offline reach the game after the
// reconnect.
SND_TEST(IndepS7Live, EditsWhileOfflineReachTheGameAfterReconnect) {
  preview::PreviewEngineDesc ed;
  ed.revertOnDisconnect = true;
  Rig r(ed, Options(LiveDecision::SendDiffs));
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -6.0));
  SND_REQUIRE(r.Settle());
  r.conn.Close();
  r.Frames(30);
  SND_CHECK(r.sync->phase() == LivePhase::Offline);
  SND_REQUIRE(r.doc.SetNumber(AmbG(), "volumeDb", -11.0));
  SND_REQUIRE(r.doc.Undo());  // Amb back
  SND_REQUIRE(r.doc.SetNumber(SpotG(), "maxDistance", 7.0));
  SND_REQUIRE(r.doc.SetNumber(Knob2G(), "default", 4.0));
  r.Frames(5);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  std::string first;
  SND_CHECK_MSG(CompareGameWithDocument(r, Members(), &first) == 0u, "%s", first.c_str());
}

// B10: a random edit sequence (fixed seed) with undo, redo and groups: the game ends as the
// document's build.
SND_TEST(IndepS7Live, RandomEditSequenceEndsWithTheGameEqualToTheDocument) {
  LiveSyncOptions o = Options();
  o.throttleMs = 5;
  Rig r({}, o);
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  std::mt19937 rng(20261124u);
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
  auto value = [&](double lo, double hi) { return lo + (hi - lo) * (static_cast<double>(rng() % 10000u) / 9999.0); };
  for (int step = 0; step < 160; ++step) {
    switch (pick(7)) {
      case 0: r.doc.SetNumber(SfxG(), "volumeDb", std::round(value(-30, 6) * 4) / 4); break;
      case 1: r.doc.SetNumber(AmbG(), "volumeDb", std::round(value(-30, 6) * 4) / 4); break;
      case 2: r.doc.SetNumber(SpotG(), "maxDistance", std::round(value(1, 50))); break;
      case 3: r.doc.SetNumber(Knob2G(), "default", std::round(value(0, 10))); break;
      case 4: r.doc.Undo(); break;
      case 5: r.doc.Redo(); break;
      default:
        r.doc.BeginGroup();
        for (int k = 0; k < 3; ++k) r.doc.SetNumber(SfxG(), "volumeDb", -1.0 - pick(20));
        r.doc.EndGroup();
        break;
    }
    const int frames = pick(3);
    for (int f = 0; f < frames; ++f) r.Frame();
  }
  r.sync->FlushNow();
  SND_REQUIRE(r.Settle());
  std::string first;
  SND_CHECK_MSG(CompareGameWithDocument(r, Members(), &first) == 0u, "%s", first.c_str());
  for (const LiveProblem& p : r.sync->problems()) std::printf("problem: %s\n", p.message.c_str());
  SND_CHECK(r.sync->problems().empty());
}

// B14
SND_TEST(IndepS7Live, LiveSupportTable) {
  SND_CHECK(LiveSupportOf(bf::chunk::BUSS, bf::col::BUSS_volumeDb).supported);
  SND_CHECK(LiveSupportOf(bf::chunk::EVNT, bf::col::EVNT_maxDistance).supported);
  const LiveSupport phys = LiveSupportOf(bf::chunk::PHYS);
  SND_CHECK(!phys.supported && phys.blocking && !phys.reason.empty());
  const LiveSupport ptch = LiveSupportOf(bf::chunk::PTCH);
  SND_CHECK(!ptch.supported && ptch.blocking && !ptch.reason.empty());
  // Every unsupported answer has a reason.
  for (const bf::ChunkInfo& ci : bf::kChunks) {
    const LiveSupport s = LiveSupportOf(ci.fourcc);
    SND_CHECK(s.supported || !s.reason.empty());
    SND_CHECK(!s.blocking || !s.supported);
  }
}

// A6: a protocol 1.0 game answers a mismatch; the tool asks without waiting for a bank list.
SND_TEST(IndepS7Live, OneZeroGameMismatchAsksWithoutABankList) {
  ProjectDocument doc;
  Load(doc);
  LiveSync sync(doc, Options(LiveDecision::Ask));
  MiniGame g;
  SND_REQUIRE(g.ok);
  g.minor = 0;
  g.status = ll::HelloStatus::BuildMismatch;
  g.echoHash = false;
  Connection conn;
  ConnectionDesc d;
  d.connector = &g.link.connector();
  d.profilerMask = 0;
  d.reconnectDelayMs = 20;
  SND_REQUIRE_EQ(conn.Open(sync.Prepare(d)), SND_OK);
  auto run = [&](auto pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > end) return false;
      g.Pump();
      sync.Tick(conn, conn.Poll(), Now());
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    return true;
  };
  SND_REQUIRE(run([&] { return sync.pending(); }));
  SND_CHECK(!sync.check().note.empty() || !sync.check().banksKnown);
  SND_CHECK(g.Of(ll::MessageType::QueryBanks).empty());
  SND_REQUIRE(sync.ChooseSendDiffs(conn));
  SND_REQUIRE(run([&] { return sync.phase() == LivePhase::Live; }));
  for (const LiveFeature& f : sync.features()) {
    const std::string n = f.name;
    if (n.find("ReloadBank") != std::string::npos || n.find("RevertValue") != std::string::npos ||
        n.find("ReadCell") != std::string::npos || n.find("QueryBanks") != std::string::npos ||
        n.find("StructEdit") != std::string::npos || n.find("stop all") != std::string::npos) {
      SND_CHECK_MSG(!f.available && !f.reason.empty(), "%s", f.name);
    }
  }
  SND_REQUIRE(doc.SetNumber(SfxG(), "volumeDb", -12.0));
  SND_REQUIRE(run([&] { return !g.Of(ll::MessageType::SetValue).empty() && sync.phase() == LivePhase::Live; }));
  const ll::SetValue v = g.Of(ll::MessageType::SetValue).back().As<ll::SetValue>();
  SND_CHECK_EQ(v.runtimeId, snd::HashId("Sfx"));
  SND_CHECK_EQ(F32Of(v.value), -12.0f);
  for (const MiniFrame& f : g.frames) {
    SND_CHECK_MSG(f.h.type < 0x0019u || f.h.type > 0x001Cu, "1.1 request %#x to a 1.0 game", f.h.type);
  }
  conn.Close();
}

// B7: a refused value is a problem naming the edit, the object and the member.
SND_TEST(IndepS7Live, RefusedValueIsAProblemWithItsEdit) {
  ProjectDocument doc;
  Load(doc);
  LiveSync sync(doc, Options(LiveDecision::SendDiffs));
  MiniGame g;
  SND_REQUIRE(g.ok);
  g.banks.clear();
  g.ackResult = [](const MiniFrame& f) { return f.Is(ll::MessageType::SetValue) ? SND_ERR_NOT_FOUND : SND_OK; };
  Connection conn;
  ConnectionDesc d;
  d.connector = &g.link.connector();
  d.profilerMask = 0;
  SND_REQUIRE_EQ(conn.Open(sync.Prepare(d)), SND_OK);
  const DocBuild b = BuildDocument(doc);
  for (const BankImage& bank : b.banks) {
    ll::BankEntry e{};
    e.bankId = bank.bankId;
    e.projectBuildHash = b.buildHash;
    e.contentHash = bank.contentHash;
    e.writable = 1;
    g.banks.push_back(e);
  }
  auto run = [&](auto pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > end) return false;
      g.Pump();
      sync.Tick(conn, conn.Poll(), Now());
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    return true;
  };
  SND_REQUIRE(run([&] { return sync.phase() == LivePhase::Live; }));
  SND_REQUIRE(doc.SetNumber(SfxG(), "volumeDb", -12.0));
  const std::uint64_t rev = doc.revision();
  SND_REQUIRE(run([&] { return !sync.problems().empty(); }));
  const LiveProblem& p = sync.problems().back();
  SND_CHECK_EQ(p.revision, rev);
  SND_CHECK_EQ(p.guid, SfxG());
  SND_CHECK_EQ(p.member, std::string("volumeDb"));
  SND_CHECK(p.route == LiveRoute::SetValue);
  SND_CHECK_EQ(p.result, SND_ERR_NOT_FOUND);
  SND_CHECK(!p.message.empty());
  const LiveItem* it = sync.Find(SfxG(), "volumeDb");
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->state == LiveState::Failed);
  conn.Close();
}

// Records today's behaviour (spec gap, notes, 案 3): the answer to a mismatch is remembered for
// the reconnect of the same LiveSync (the user is not asked again).
SND_TEST(IndepS7Live, Record_MismatchAnswerIsRememberedOnReconnect) {
  Rig r({}, Options(LiveDecision::Ask), [](ProjectDocument& g) { g.SetNumber(SfxG(), "volumeDb", -7.0); });
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Run([&] { return r.sync->pending(); }));
  SND_REQUIRE(r.sync->ChooseSendDiffs(r.conn));
  SND_REQUIRE(r.Settle());
  r.conn.Drop();
  SND_REQUIRE(r.Run([&] { return r.conn.sessions() >= 2u || r.sync->pending(); }));
  bool askedAgain = false;
  r.Run([&] {
    askedAgain = askedAgain || r.sync->pending();
    return r.sync->phase() == LivePhase::Live;
  }, 5000);
  std::printf("record: after a reconnect the mismatch was %s\n", askedAgain ? "asked again" : "answered as before");
  SND_CHECK(r.sync->phase() == LivePhase::Live || askedAgain);
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -1.0f);
}

// Records today's behaviour (spec gap, notes): an effect added live by StructEdit (an editor in
// core that changes nothing, C-01) and then a parameter edit of that effect: the SetValue targets
// a cell the game's tables do not have.
SND_TEST(IndepS7Live, Record_ValueEditOfALiveAddedEffect) {
  AcceptAll editor;
  preview::PreviewEngineDesc ed;
  ed.structEditor = &editor;
  Rig r(ed);
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const json::Value fx = Json("{\"type\": \"snd.fx.delay\", \"params\": [10, 0, 0, -96, 20000, 0, 100]}");
  SND_REQUIRE(r.doc.Insert(AmbG(), "effects", 0, fx));
  SND_REQUIRE(r.Settle());
  SND_REQUIRE_EQ(editor.applied, 1u);
  SND_REQUIRE(r.doc.SetNumber(AmbG(), "effects[0].params[0]", 20.0));
  r.Settle();
  r.Frames(10);
  const LiveItem* it = r.sync->Find(AmbG(), "effects[#].params[#]");
  std::printf("record: param edit of a live-added effect: %u problems, %u reloads, item %s\n",
              static_cast<unsigned>(r.sync->problems().size()), static_cast<unsigned>(r.sync->reloads().size()),
              it != nullptr ? LiveStateName(it->state) : "(none)");
  for (const LiveProblem& p : r.sync->problems()) std::printf("  problem: %s (%s)\n", p.message.c_str(), p.member.c_str());
  // Whatever the route, the user is not told "live" while the game has no such cell.
  SND_CHECK(!r.sync->problems().empty() || !r.sync->reloads().empty());
}
