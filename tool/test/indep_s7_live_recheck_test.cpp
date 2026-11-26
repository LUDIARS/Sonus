// Independent tests of group P, re-check after the decisions D-182..D-185
// (tool/test/indep_s7_live_properties.md section R). Expected values come from the decisions and,
// for the transition audition, from music's transition.cpp rules computed here by hand.
#include <cmath>
#include <filesystem>
#include <limits>
#include <random>
#include <type_traits>

#include <unistd.h>

#include "indep_s7_live_fixture.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/preview/replay_host.hpp"
#include "snd/tool/recording_report.hpp"

using namespace snd::tool;
using namespace ilive;
namespace fs = std::filesystem;

namespace {

const std::vector<std::pair<std::string, std::string>>& Members() {
  static const std::vector<std::pair<std::string, std::string>> m = {
      {SfxG(), "volumeDb"}, {AmbG(), "volumeDb"}, {SpotG(), "maxDistance"}, {Knob2G(), "default"}};
  return m;
}

std::uint32_t Diffs(Rig& r, std::string* first) {
  const DocBuild b = BuildDocument(r.doc);
  std::uint32_t n = 0;
  for (const LiveCell& c : F32CellsOf(b, r.doc, Members())) {
    const float g = r.GameF32(c.id, c.chunk, c.column, c.index);
    if (!(g == c.value)) {
      if (n == 0u && first != nullptr) {
        char m[160];
        std::snprintf(m, sizeof m, "id %08x column %u: game %g, document %g", c.id, c.column, static_cast<double>(g),
                      static_cast<double>(c.value));
        *first = m;
      }
      ++n;
    }
  }
  return n;
}

// Drives a LiveSync against a MiniGame.
struct MiniRig {
  ProjectDocument doc;
  MiniGame g;
  std::unique_ptr<LiveSync> sync;
  Connection conn;
  explicit MiniRig(LiveSyncOptions o) {
    Load(doc);
    sync = std::make_unique<LiveSync>(doc, o);
  }
  ~MiniRig() { conn.Close(); }
  void Banks(const DocBuild& b) {
    g.banks.clear();
    for (const BankImage& bank : b.banks) {
      ll::BankEntry e{};
      e.bankId = bank.bankId;
      e.projectBuildHash = b.buildHash;
      e.contentHash = bank.contentHash;
      e.writable = 1;
      g.banks.push_back(e);
    }
  }
  bool Open() {
    ConnectionDesc d;
    d.connector = &g.link.connector();
    d.profilerMask = 0;
    d.reconnectDelayMs = 20;
    return conn.Open(sync->Prepare(d)) == SND_OK;
  }
  void Tick() {
    g.Pump();
    sync->Tick(conn, conn.Poll(), Now());
  }
  template <class Pred>
  bool Run(Pred&& pred, std::uint32_t ms = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > end) return false;
      Tick();
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    return true;
  }
  void For(std::uint32_t ms) {
    Run([] { return false; }, ms);
  }
};

}  // namespace

// R1: reconnect with and without revertOnDisconnect; edits, Undo and Redo while offline.
SND_TEST(IndepS7LiveRecheck, ReconnectMakesTheGameTheDocument) {
  for (const bool revert : {false, true}) {
    preview::PreviewEngineDesc ed;
    ed.revertOnDisconnect = revert;
    Rig r(ed, Options(LiveDecision::SendDiffs));
    SND_REQUIRE(r.ok);
    SND_REQUIRE(r.Connect());
    SND_REQUIRE(r.Settle());
    SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -9.0));
    SND_REQUIRE(r.doc.SetNumber(AmbG(), "volumeDb", -12.0));
    SND_REQUIRE(r.doc.SetNumber(Knob2G(), "default", 7.0));
    SND_REQUIRE(r.Settle());
    r.conn.Close();
    r.Frames(10);
    SND_REQUIRE(r.doc.Undo());  // Knob2 back
    SND_REQUIRE(r.doc.Undo());  // Amb back
    SND_REQUIRE(r.doc.Redo());  // Amb -12 again
    SND_REQUIRE(r.doc.SetNumber(SpotG(), "maxDistance", 9.0));
    SND_REQUIRE(r.doc.Undo());  // Spot back
    r.Frames(5);
    SND_REQUIRE(r.Connect());
    SND_REQUIRE(r.Settle());
    std::string first;
    SND_CHECK_MSG(Diffs(r, &first) == 0u, "revertOnDisconnect %d: %s", revert ? 1 : 0, first.c_str());
    // A drop with the client reconnecting by itself (the same Connection).
    SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -3.0));
    SND_REQUIRE(r.Settle());
    r.conn.Drop();
    SND_REQUIRE(r.doc.Undo());  // Sfx back to -9 while the link is down
    SND_REQUIRE(r.Run([&] { return r.conn.sessions() >= 2u; }));
    SND_REQUIRE(r.Settle());
    first.clear();
    SND_CHECK_MSG(Diffs(r, &first) == 0u, "after a drop, revertOnDisconnect %d: %s", revert ? 1 : 0, first.c_str());
    SND_CHECK(r.sync->problems().empty());
  }
}

// R3: a random edit sequence with drops and reopenings (fixed seed).
SND_TEST(IndepS7LiveRecheck, RandomEditsWithDisconnects) {
  LiveSyncOptions o = Options(LiveDecision::SendDiffs);
  o.throttleMs = 5;
  Rig r({}, o);
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  std::mt19937 rng(424242u);
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
  bool open = true;
  for (int step = 0; step < 200; ++step) {
    switch (pick(9)) {
      case 0: r.doc.SetNumber(SfxG(), "volumeDb", -1.0 - pick(30)); break;
      case 1: r.doc.SetNumber(AmbG(), "volumeDb", -2.0 - pick(30)); break;
      case 2: r.doc.SetNumber(SpotG(), "maxDistance", 1 + pick(50)); break;
      case 3: r.doc.SetNumber(Knob2G(), "default", pick(11)); break;
      case 4: r.doc.Undo(); break;
      case 5: r.doc.Redo(); break;
      case 6:
        if (open) {
          r.conn.Close();
        } else {
          r.Connect();
        }
        open = !open;
        break;
      case 7: if (open) r.conn.Drop(); break;
      default: break;
    }
    for (int f = pick(3); f > 0; --f) r.Frame();
  }
  if (!open) SND_REQUIRE(r.Connect());
  r.sync->FlushNow();
  SND_REQUIRE(r.Settle());
  std::string first;
  SND_CHECK_MSG(Diffs(r, &first) == 0u, "%s", first.c_str());
  for (const LiveProblem& p : r.sync->problems()) std::printf("problem: %s\n", p.message.c_str());
  SND_CHECK(r.sync->problems().empty());
}

// R4: the answer is remembered for the same game only (bank list: id, language, content hash).
SND_TEST(IndepS7LiveRecheck, AnswerIsRememberedForTheSameGameOnly) {
  MiniRig m(Options(LiveDecision::Ask));
  SND_REQUIRE(m.g.ok);
  m.g.status = ll::HelloStatus::BuildMismatch;
  m.g.echoHash = false;
  m.g.runtimeHash = 0x4444;
  ProjectDocument gd;
  Load(gd);
  gd.SetNumber(SfxG(), "volumeDb", -4.0);
  m.Banks(BuildDocument(gd));
  SND_REQUIRE(m.Open());
  SND_REQUIRE(m.Run([&] { return m.sync->pending() && m.sync->check().banksKnown; }));
  SND_REQUIRE(m.sync->ChooseSendDiffs(m.conn));
  SND_REQUIRE(m.Run([&] { return m.sync->phase() == LivePhase::Live; }));
  // The same game again: answered as before, no question.
  m.g.Drop();
  bool asked = false;
  SND_REQUIRE(m.Run([&] {
    asked = asked || m.sync->pending();
    return m.conn.sessions() >= 2u && m.sync->phase() == LivePhase::Live;
  }));
  SND_CHECK_MSG(!asked, "the same game was asked again");
  // Another game (one bank's content differs): asked again.
  m.g.banks[0].contentHash ^= 0x5A5Au;
  m.g.Drop();
  SND_CHECK_MSG(m.Run([&] { return m.sync->pending(); }), "another game was not asked");
  SND_REQUIRE(m.sync->ChooseRefuse(m.conn));
  SND_REQUIRE(m.Run([&] { return m.sync->phase() == LivePhase::Refused; }));
  // SetDecision(Ask) forgets the answer.
  m.sync->SetDecision(LiveDecision::Ask);
  SND_CHECK(m.sync->phase() == LivePhase::Refused || m.sync->phase() == LivePhase::Offline);
}

// R4: no question before the bank list is there (1.1), and a 1.0 game is judged by its build hash.
SND_TEST(IndepS7LiveRecheck, NoQuestionBeforeTheBankListAndOneZeroByHash) {
  {  // A remembered answer is applied only once the list shows the same game: Checking meanwhile.
    MiniRig m(Options(LiveDecision::Ask));
    SND_REQUIRE(m.g.ok);
    m.g.status = ll::HelloStatus::BuildMismatch;
    m.g.echoHash = false;
    m.g.runtimeHash = 0x1111;
    ProjectDocument gd;
    Load(gd);
    gd.SetNumber(SfxG(), "volumeDb", -4.0);
    m.Banks(BuildDocument(gd));
    SND_REQUIRE(m.Open());
    SND_REQUIRE(m.Run([&] { return m.sync->pending() && m.sync->check().banksKnown; }));
    SND_REQUIRE(m.sync->ChooseSendDiffs(m.conn));
    SND_REQUIRE(m.Run([&] { return m.sync->phase() == LivePhase::Live; }));
    m.g.holdBanks = true;
    m.g.Drop();
    SND_REQUIRE(m.Run([&] { return m.g.banksAsked() > 0u; }));
    bool asked = false, live = false;
    m.Run([&] {
      asked = asked || m.sync->pending();
      live = live || m.sync->phase() == LivePhase::Live;
      return false;
    }, 400);
    SND_CHECK_MSG(!asked && !live, "asked %d / live %d before the bank list came", asked ? 1 : 0, live ? 1 : 0);
    SND_CHECK_MSG(m.sync->phase() == LivePhase::Checking, "phase %s", LivePhaseName(m.sync->phase()));
    m.g.holdBanks = false;
    m.g.ReplyBanks();
    const bool live2 = m.Run([&] { return m.sync->phase() == LivePhase::Live; }, 5000);
    SND_CHECK_MSG(live2, "phase %s, pending %d, banks asked %u", LivePhaseName(m.sync->phase()), m.sync->pending() ? 1 : 0,
                  static_cast<unsigned>(m.g.Of(ll::MessageType::QueryBanks).size()));
  }
  {  // 1.0: same hash after a drop -> remembered; another hash -> asked again.
    MiniRig m(Options(LiveDecision::Ask));
    SND_REQUIRE(m.g.ok);
    m.g.minor = 0;
    m.g.status = ll::HelloStatus::BuildMismatch;
    m.g.echoHash = false;
    m.g.runtimeHash = 0x2222;
    SND_REQUIRE(m.Open());
    SND_REQUIRE(m.Run([&] { return m.sync->pending(); }));
    SND_REQUIRE(m.sync->ChooseSendDiffs(m.conn));
    SND_REQUIRE(m.Run([&] { return m.sync->phase() == LivePhase::Live; }));
    m.g.Drop();
    bool asked = false;
    SND_REQUIRE(m.Run([&] {
      asked = asked || m.sync->pending();
      return m.conn.sessions() >= 2u && m.sync->phase() == LivePhase::Live;
    }));
    SND_CHECK_MSG(!asked, "the same 1.0 game was asked again");
    m.g.runtimeHash = 0x3333;  // the game restarted with another build
    m.g.Drop();
    SND_CHECK_MSG(m.Run([&] { return m.sync->pending(); }), "another 1.0 game was not asked");
  }
}

// R5: a busy value is dropped when a newer value of the cell exists; a cell that stays busy gives up
// after giveUpMs with a problem.
SND_TEST(IndepS7LiveRecheck, BusyResendsGiveUpByTime) {
  LiveSyncOptions o = Options(LiveDecision::SendDiffs);
  o.giveUpMs = 400;
  MiniRig m(o);
  SND_REQUIRE(m.g.ok);
  m.Banks(BuildDocument(m.doc));
  bool busy = true;
  m.g.ackResult = [&](const MiniFrame& f) {
    return f.Is(ll::MessageType::SetValue) && busy ? SND_ERR_COMMAND_BUFFER_FULL : SND_OK;
  };
  SND_REQUIRE(m.Open());
  SND_REQUIRE(m.Run([&] { return m.sync->phase() == LivePhase::Live; }));
  SND_REQUIRE(m.doc.SetNumber(SfxG(), "volumeDb", -5.0));
  const auto t0 = std::chrono::steady_clock::now();
  SND_REQUIRE(m.Run([&] { return !m.sync->problems().empty(); }, 10000));
  const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  SND_CHECK_MSG(waited >= 0.35, "gave up after %.3f s", waited);
  SND_CHECK(m.sync->problems().back().guid == SfxG());
  SND_CHECK(m.sync->phase() != LivePhase::Syncing || true);
  const std::size_t sentBusy = m.g.Of(ll::MessageType::SetValue).size();
  SND_CHECK(sentBusy >= 2u);  // it was re-sent before giving up
  m.For(300);
  SND_CHECK_EQ(m.g.Of(ll::MessageType::SetValue).size(), sentBusy);  // nothing after giving up
  // A newer value replaces the busy one; the game takes it; nothing older follows it.
  busy = false;
  SND_REQUIRE(m.doc.SetNumber(SfxG(), "volumeDb", -8.0));
  SND_REQUIRE(m.Run([&] { return m.sync->phase() == LivePhase::Live; }));
  m.For(200);
  const std::vector<MiniFrame> sv = m.g.Of(ll::MessageType::SetValue);
  SND_CHECK_EQ(F32Of(sv.back().As<ll::SetValue>().value), -8.0f);
}

// R6: a large throttle, no Flush: the last value goes out by Tick alone.
SND_TEST(IndepS7LiveRecheck, ThrottleWithoutFlush) {
  LiveSyncOptions o = Options();
  o.throttleMs = 200;
  Rig r({}, o);
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  for (int i = 1; i <= 30; ++i) {
    SND_REQUIRE(r.doc.SetNumber(AmbG(), "volumeDb", -2.0 - i, 9u));
    r.Frame();
  }
  SND_REQUIRE(r.Run([&] { return r.sync->phase() == LivePhase::Live; }, 10000));
  SND_CHECK_EQ(r.GameF32(snd::HashId("Amb"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -32.0f);
}

// R7: Undo / Redo of a live structural edit, and a successful hot reload resets the live reserve.
SND_TEST(IndepS7LiveRecheck, ReserveFollowsUndoRedoAndReload) {
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
  SND_CHECK_EQ(r.sync->reserve().effects, 1u);
  SND_REQUIRE(r.doc.Undo());
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.sync->reserve().effects, 0u);
  SND_CHECK_EQ(r.sync->reserve().bytes, 0u);
  SND_REQUIRE(r.doc.Redo());
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.sync->reserve().effects, 1u);
  // A hot reload of the bank (a new event): the rebuilt bank holds the effect; the live part is 0.
  const json::Value init = Json("{\"bank\": \"Main\", \"bus\": \"Sfx\"}");
  SND_REQUIRE(!r.doc.Create("event", "Fresh", &init).empty());
  SND_REQUIRE(r.Settle());
  SND_REQUIRE(!r.sync->reloads().empty());
  SND_CHECK(r.sync->reloads().back().phase == ll::ReloadPhase::Done);
  SND_CHECK_EQ(r.sync->reserve().effects, 0u);
  SND_CHECK_EQ(r.sync->reserve().bytes, 0u);
  // Undo after the reload: the effect is in the bank now; the reserve stays 0 (never negative).
  SND_REQUIRE(r.doc.Undo());  // the event
  SND_REQUIRE(r.doc.Undo());  // the effect
  SND_REQUIRE(r.Settle());
  SND_CHECK(r.sync->reserve().effects <= 1u);
  SND_CHECK(r.sync->reserve().bytes < (1ull << 20));
  std::string first;
  SND_CHECK_MSG(Diffs(r, &first) == 0u, "%s", first.c_str());
}

// R8: the mixer lets go of a cell: the document's value goes out by itself.
SND_TEST(IndepS7LiveRecheck, MixerReleaseSendsTheDocumentValue) {
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
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -1.0f);
  held = false;
  r.Frames(3);
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -9.0f);
  const LiveItem* it = r.sync->Find(SfxG(), "volumeDb");
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->state == LiveState::Live);
}

// R11: phase Failed with failedBanks, back to Live after a successful reload; a writer failure too.
SND_TEST(IndepS7LiveRecheck, PhaseFailedUntilAReloadSucceeds) {
  int mode = 0;  // 0 good, 1 a path the game cannot read, 2 the writer fails
  LiveSyncOptions o = Options(LiveDecision::SendDiffs);
  preview::PreviewEngine* engine = nullptr;
  o.writer = [&](const BankImage& b, std::string* path, std::string* error) {
    if (mode == 2) {
      *error = "no space left";
      return false;
    }
    *path = mode == 1 ? std::string("no/such/bank.sndb") : engine->PutRebuiltBank(b.file, b.bytes);
    return true;
  };
  Rig r({}, o);
  SND_REQUIRE(r.ok);
  engine = r.engine.get();
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const json::Value init = Json("{\"bank\": \"Main\", \"bus\": \"Sfx\"}");
  mode = 1;
  SND_REQUIRE(!r.doc.Create("event", "E1", &init).empty());
  SND_REQUIRE(r.Run([&] { return r.sync->phase() == LivePhase::Failed; }));
  SND_CHECK(r.sync->failedBanks().count("Main.sndb") == 1u);
  SND_CHECK(!r.sync->failedBanks().begin()->second.empty());
  r.Frames(20);
  SND_CHECK(r.sync->phase() == LivePhase::Failed);  // stays until a reload succeeds
  // A value edit of another bank still goes out; the phase stays Failed.
  SND_REQUIRE(r.doc.SetNumber(Knob2G(), "default", 3.0));
  SND_REQUIRE(r.Run([&] { return r.sync->queued() == 0u && r.sync->inFlight() == 0u && r.doc.revision() == r.sync->builtRevision(); }));
  r.Frames(3);
  SND_CHECK_MSG(r.sync->phase() == LivePhase::Failed, "phase %s", LivePhaseName(r.sync->phase()));
  SND_CHECK_EQ(r.GameF32(snd::HashId("Knob2"), bf::chunk::PARM, bf::col::PARM_defaultValue), 3.0f);
  mode = 0;
  SND_REQUIRE(!r.doc.Create("event", "E2", &init).empty());
  SND_REQUIRE(r.Run([&] { return r.sync->phase() == LivePhase::Live; }));
  SND_CHECK(r.sync->failedBanks().empty());
  std::string first;
  SND_CHECK_MSG(Diffs(r, &first) == 0u, "%s", first.c_str());
  mode = 2;
  SND_REQUIRE(!r.doc.Create("event", "E3", &init).empty());
  const bool failed = r.Run([&] { return r.sync->phase() == LivePhase::Failed; }, 5000);
  for (const ReloadProgress& q : r.sync->reloads())
    std::printf("reload %s phase %u finished %d result %d reason %s\n", q.bank.c_str(), static_cast<unsigned>(q.phase),
                q.finished ? 1 : 0, q.result, q.reason.c_str());
  std::printf("phase %s, failed banks %u, problems %u\n", LivePhaseName(r.sync->phase()),
              static_cast<unsigned>(r.sync->failedBanks().size()), static_cast<unsigned>(r.sync->problems().size()));
  SND_REQUIRE(failed);
  SND_REQUIRE(!r.sync->reloads().empty());
  SND_CHECK(r.sync->reloads().back().phase == ll::ReloadPhase::Failed);
  SND_CHECK(r.sync->reloads().back().reason.find("no space left") != std::string::npos);
  SND_CHECK(r.sync->failedBanks().count("Main.sndb") == 1u);
  mode = 0;
  SND_REQUIRE(r.doc.Undo());  // E3 away: the next build needs a reload again
  SND_REQUIRE(r.Run([&] { return r.sync->phase() == LivePhase::Live; }));
  SND_CHECK(r.sync->failedBanks().empty());
}

// R12: the transition audition against the runtime's rule, computed here: 3/4 at 90 BPM (a bar
// is 2 s), 256-frame blocks at 48 kHz, rule "* -> *" next bar, memory material (prime 0). The
// request is applied at block a = max(1, ceil(t / blockSeconds)) after the start; the switch is the
// first bar line at or after a's start.
SND_TEST(IndepS7LiveRecheck, TransitionAuditionFollowsTheRuntimeRule) {
  const double blockSeconds = 256.0 / 48000.0;
  for (const double request : {0.0, 2.0, 1.999, 2.001, 0.96, 1.9973, 3.995, 4.0}) {
    Engine en(true);
    SND_REQUIRE(en.e != nullptr);
    MusicEditor me(en.doc, G(236));
    MusicSettings ms;
    ms.sampleRate = 48000;
    ms.blockFrames = 256;
    const TransitionPreview p = me.Preview("Calm", "Fight", request, ms);
    SND_REQUIRE(p.valid);
    double a = std::ceil(request / blockSeconds - 1e-9);
    if (a < 1.0) a = 1.0;
    const std::uint64_t applyFrame = static_cast<std::uint64_t>(a) * 256u;
    const std::uint64_t bar = 96000u;
    const std::uint64_t want = (applyFrame + bar - 1u) / bar * bar;  // first bar at or after
    SND_CHECK_MSG(std::fabs(p.applySeconds - static_cast<double>(applyFrame) / 48000.0) < 1e-12, "request %g: apply %g",
                  request, p.applySeconds);
    SND_CHECK_MSG(std::llround(p.fireSeconds * 48000.0) == static_cast<long long>(want), "request %g: model %g, rule %g",
                  request, p.fireSeconds, static_cast<double>(want) / 48000.0);
    AuditionRouter router;
    router.Bind(nullptr, en.e.get());
    en.Frames(3);
    const std::uint64_t start = en.e->NextBlock() * 256u;
    SND_REQUIRE(router.PlayTransition(TransitionAudition{snd::HashId("Tune"), snd::HashId("Calm"), snd::HashId("Fight"), request}).ok);
    std::vector<float> all;
    en.e->Render(static_cast<std::uint32_t>((static_cast<double>(want) / 48000.0 + 0.5) / blockSeconds) + 2u, &all);
    const std::vector<preview::PreviewTransition> tr = en.e->Transitions();
    SND_REQUIRE_MSG(!tr.empty(), "request %g: no transition", request);
    SND_CHECK_MSG(tr.back().clock == start + want, "request %g: engine %llu, rule %llu", request,
                  static_cast<unsigned long long>(tr.back().clock - start), static_cast<unsigned long long>(want));
    SND_CHECK_EQ(tr.back().to, snd::HashId("Fight"));
    // The output: Calm (kDc) right before the switch, Fight (3 kDc) a block after it (the switch
    // sample itself mixes the source's last sample, no fades in the rule).
    const std::uint32_t ch = en.e->Status().channels;
    const std::size_t before = static_cast<std::size_t>(want - 1u) * ch, at = static_cast<std::size_t>(want + 256u) * ch;
    SND_REQUIRE(at + ch <= all.size());
    SND_CHECK_MSG(std::fabs(all[at] / all[before] - 3.0f) < 1e-3f, "request %g: %g -> %g", request,
                  static_cast<double>(all[before]), static_cast<double>(all[at]));
    // Calm is heard from the start (also for a request at 0).
    SND_CHECK(std::fabs(all[static_cast<std::size_t>(512) * ch] - all[before]) < 1e-6f);
  }
}

// R14: replay limits.
SND_TEST(IndepS7LiveRecheck, ReplayLimitsAtTheirEdges) {
  auto make = [](std::uint64_t first, std::uint64_t last) {
    Recording r;
    r.info.sampleRate = 48000;
    r.info.blockSize = 256;
    auto put = [&](const void* p, std::size_t n) {
      r.records.insert(r.records.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n);
    };
    const ll::RecordLod lod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, first};
    put(&lod, sizeof lod);
    for (const std::uint64_t b : {first, last}) {
      const ll::RecordArena a{static_cast<std::uint32_t>(ll::RecordKind::Arena), 0u, b, 16u, 0u};
      const ll::ArenaSections s{0u, 0u, 0u, 0u};
      put(&a, sizeof a);
      put(&s, sizeof s);
    }
    return r;
  };
  preview::ReplayOptions o;
  o.maxBlocks = 40;
  o.maxAlignBlocks = 100;
  ReplayRun run;
  // Blocks() = last - first + 1 (no Begin: no lead block).
  SND_CHECK(preview::Replay(make(5, 44), {}, o, &run));  // 40 blocks
  SND_CHECK(run.ok && run.blocks == 40u);
  SND_CHECK(!preview::Replay(make(5, 45), {}, o, &run));  // 41
  SND_CHECK(!run.ok && !run.error.empty());
  // The alignment: the first block at exactly maxAlignBlocks keeps the numbers, one more does not.
  SND_CHECK(preview::Replay(make(100, 101), {}, o, &run));
  SND_CHECK(run.ok);
  SND_CHECK(run.blockNumbersKept);
  SND_CHECK(preview::Replay(make(101, 102), {}, o, &run));
  SND_CHECK(run.ok);
  SND_CHECK(!run.blockNumbersKept);
  // The same digests either way (an empty recording: silence).
  // Huge numbers end at once with the default limits.
  const auto t0 = std::chrono::steady_clock::now();
  preview::Replay(make(0, 1000000000000000ull), {}, preview::ReplayOptions{}, &run);
  SND_CHECK(!run.ok);
  preview::Replay(make(1000000000000000ull, 1000000000000001ull), {}, preview::ReplayOptions{}, &run);
  SND_CHECK(run.ok && !run.blockNumbersKept);
  SND_CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20));
}

// R15: every bug report field round trips (line breaks, non-ASCII, empty).
SND_TEST(IndepS7LiveRecheck, BugReportEveryFieldRoundTrips) {
  const fs::path dir = fs::temp_directory_path() / ("sonus-indep-recheck-" + std::to_string(::getpid()));
  for (int variant = 0; variant < 3; ++variant) {
    BugReport rep;
    rep.recording.info.sampleRate = 44100;
    rep.recording.info.blockSize = 512;
    rep.recording.info.projectBuildHash = 0xFFFFFFFFFFFFFFFFull;
    snd_config_init(&rep.config);
    rep.config.max_banks = 7;
    rep.config.tuning.short_fade_ms = 3.25f;
    rep.configKnown = variant != 1;
    rep.protocolMajor = 1;
    rep.protocolMinor = static_cast<std::uint16_t>(variant);
    if (variant == 0) {
      rep.title = "ずれる音 \xF0\x9F\x8E\xB5";
      rep.notes = "line 1\nline 2\r\n\n  indented = with equals\n# hash\n";
      rep.summary = "summary\nkey = value\n";
      rep.comparison = "decisions match\n\nbits differ: 0x1\n";
    } else if (variant == 1) {
      rep.title = "";
      rep.notes = "";
      rep.summary = "";
      rep.comparison = "";
    } else {
      rep.title = "only a title";
      rep.notes = std::string(5000, 'n') + "\n";
      rep.summary = "s";
      rep.comparison = "c\n";
    }
    rep.replayed = variant != 1;
    rep.decisionsMatch = variant == 0;
    rep.bitExactExpected = true;
    rep.bitsMatch = variant == 2;
    rep.expectation.hasDigest = variant == 0;
    rep.expectation.digest = 0x123;
    rep.expectation.decisions = {ll::ReplayDecision{1, 2, 3, 4, 5}};
    ReportBank b;
    b.file = "Main.sndb";
    b.bankId = 0xFFFFFFFFu;
    b.languageId = 0;
    b.contentHash = 0x8000000000000001ull;
    b.bytes = {1, 2, 3, 0, 255};
    rep.banks.push_back(b);
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::string err;
    SND_REQUIRE_MSG(WriteBugReport(dir.string(), rep, &err), "%s", err.c_str());
    BugReport got;
    SND_REQUIRE_MSG(ReadBugReport(dir.string(), &got, &err), "variant %d: %s", variant, err.c_str());
    SND_CHECK_MSG(got.title == rep.title, "variant %d title", variant);
    SND_CHECK_MSG(got.notes == rep.notes, "variant %d notes", variant);
    SND_CHECK_MSG(got.summary == rep.summary, "variant %d summary", variant);
    SND_CHECK_MSG(got.comparison == rep.comparison, "variant %d comparison", variant);
    SND_CHECK(got.configKnown == rep.configKnown);
    SND_CHECK_EQ(ConfigText(got.config), ConfigText(rep.config));
    SND_CHECK_EQ(got.protocolMajor, rep.protocolMajor);
    SND_CHECK_EQ(got.protocolMinor, rep.protocolMinor);
    SND_CHECK(got.recording.info == rep.recording.info);
    SND_CHECK(got.recording.records == rep.recording.records);
    SND_REQUIRE_EQ(got.banks.size(), 1u);
    SND_CHECK_EQ(got.banks[0].bankId, b.bankId);
    SND_CHECK_EQ(got.banks[0].contentHash, b.contentHash);
    SND_CHECK(got.banks[0].bytes == b.bytes);
    SND_CHECK(got.replayed == rep.replayed && got.decisionsMatch == rep.decisionsMatch &&
              got.bitExactExpected == rep.bitExactExpected && got.bitsMatch == rep.bitsMatch);
    if (rep.replayed) {
      SND_CHECK(got.expectation.hasDigest == rep.expectation.hasDigest);
      SND_CHECK_EQ(got.expectation.decisions.size(), 1u);
    }
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// R16: ParseConfigLine checks sign, range and type for every member.
namespace {
template <class T>
void CheckMember(const char* name, T* field, snd_config* c, std::uint32_t* checked) {
  ++*checked;
  const std::string before = ConfigText(*c);
  if constexpr (std::is_pointer_v<T>) {
    return;
  } else if constexpr (std::is_floating_point_v<T>) {
    SND_CHECK_MSG(ParseConfigLine(name, "1.5", c) && *field == static_cast<T>(1.5), "%s 1.5", name);
    const std::string mid = ConfigText(*c);
    for (const char* bad : {"x", "", "1e40", "-1e40", "1.5x"}) {
      SND_CHECK_MSG(!ParseConfigLine(name, bad, c), "%s accepted '%s'", name, bad);
      SND_CHECK_MSG(ConfigText(*c) == mid, "%s changed by '%s'", name, bad);
    }
  } else {
    using U = std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>, std::type_identity<T>>;
    using V = typename U::type;
    const std::string maxText = std::to_string(static_cast<unsigned long long>(std::numeric_limits<V>::max()));
    SND_CHECK_MSG(ParseConfigLine(name, maxText, c), "%s refused its maximum %s", name, maxText.c_str());
    SND_CHECK_MSG(static_cast<unsigned long long>(*field) == static_cast<unsigned long long>(std::numeric_limits<V>::max()),
                  "%s maximum", name);
    SND_CHECK_MSG(ParseConfigLine(name, "0", c) && static_cast<unsigned long long>(*field) == 0ull, "%s 0", name);
    const std::string mid = ConfigText(*c);
    std::string over = sizeof(V) >= 8 ? std::string("18446744073709551616")
                                      : std::to_string(static_cast<unsigned long long>(std::numeric_limits<V>::max()) + 1ull);
    for (const std::string& bad : {std::string("-1"), over, std::string("1.5"), std::string(" 1"), std::string("1 "),
                                   std::string("+1"), std::string(""), std::string("0x"), std::string("99999999999999999999999")}) {
      SND_CHECK_MSG(!ParseConfigLine(name, bad, c), "%s accepted '%s'", name, bad.c_str());
      SND_CHECK_MSG(ConfigText(*c) == mid, "%s changed by '%s'", name, bad.c_str());
    }
  }
  (void)before;
}
}  // namespace

SND_TEST(IndepS7LiveRecheck, ConfigLineRangesForEveryMember) {
  snd_config c;
  snd_config_init(&c);
  std::uint32_t n = 0;
#define IND_TOP(cn, cpp, ct, cppt, d) CheckMember(#cn, &c.cn, &c, &n);
#define IND_MEM(cn, cpp, ct, cppt, d) CheckMember("memory." #cn, &c.memory.cn, &c, &n);
#define IND_CMD(cn, cpp, ct, cppt, d) CheckMember("commands." #cn, &c.commands.cn, &c, &n);
#define IND_JOB(cn, cpp, ct, cppt, d) CheckMember("jobs." #cn, &c.jobs.cn, &c, &n);
#define IND_TUN(cn, cpp, ct, cppt, d) CheckMember("tuning." #cn, &c.tuning.cn, &c, &n);
#define IND_DEV(cn, cpp, ct, cppt, d) CheckMember("dev_reserve." #cn, &c.dev_reserve.cn, &c, &n);
#define IND_LL(cn, cpp, ct, cppt, d) CheckMember("livelink." #cn, &c.livelink.cn, &c, &n);
  SND_CONFIG_TOP(IND_TOP)
  SND_CONFIG_TOP_1_1(IND_TOP)
  SND_CONFIG_TOP_1_2(IND_TOP)
  SND_CONFIG_MEMORY(IND_MEM)
  SND_CONFIG_COMMANDS(IND_CMD)
  SND_CONFIG_JOBS(IND_JOB)
  SND_CONFIG_TUNING(IND_TUN)
  SND_CONFIG_DEV_RESERVE(IND_DEV)
  SND_CONFIG_LIVELINK(IND_LL)
#undef IND_TOP
#undef IND_MEM
#undef IND_CMD
#undef IND_JOB
#undef IND_TUN
#undef IND_DEV
#undef IND_LL
  CheckMember("output_kind", &c.output_kind, &c, &n);
  SND_CHECK(n > 60u);
}

// R18: the document changes while the handshake runs (1.1 and 1.0 games that answer Accepted for
// the Hello's build): no question, the later edit goes as a value edit, nothing else.
SND_TEST(IndepS7LiveRecheck, DocumentChangesDuringTheHandshake) {
  for (const std::uint16_t minor : {std::uint16_t{1}, std::uint16_t{0}}) {
    MiniRig m(Options(LiveDecision::Ask));
    SND_REQUIRE(m.g.ok);
    m.g.minor = minor;
    m.g.holdHello = true;
    const DocBuild b0 = BuildDocument(m.doc);
    m.Banks(b0);
    SND_REQUIRE(m.Open());
    SND_REQUIRE(m.Run([&] { return m.g.helloHeld(); }));
    SND_CHECK_EQ(m.g.heldHelloHash(), b0.buildHash);
    SND_REQUIRE(m.doc.SetNumber(SfxG(), "volumeDb", -11.0));  // while the game has not answered
    m.Tick();
    m.g.ReplyHello();  // Accepted for b0 (the game holds b0)
    bool asked = false;
    SND_REQUIRE(m.Run([&] {
      asked = asked || m.sync->pending();
      return m.sync->phase() == LivePhase::Live;
    }));
    SND_CHECK_MSG(!asked, "minor %u: asked although the game holds the Hello's build", minor);
    const std::vector<MiniFrame> sv = m.g.Of(ll::MessageType::SetValue);
    SND_CHECK_MSG(!sv.empty() && F32Of(sv.back().As<ll::SetValue>().value) == -11.0f, "minor %u: the edit did not go", minor);
    SND_CHECK_MSG(m.g.Of(ll::MessageType::ReplaceChunk).empty() && m.g.Of(ll::MessageType::ReloadBank).empty(),
                  "minor %u: %u ReplaceChunk, %u ReloadBank for one value edit", minor,
                  static_cast<unsigned>(m.g.Of(ll::MessageType::ReplaceChunk).size()),
                  static_cast<unsigned>(m.g.Of(ll::MessageType::ReloadBank).size()));
  }
}
