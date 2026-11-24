// Independent tests of group P: Defect_* tests. Each fails on purpose and documents a defect of
// the implementation (tool/test/indep_s7_live_properties.md; the notes in
// spec/handoff/parallel/C-notes/live-indep.md give the reproduction, the expectation and its
// basis). One CTest test per defect (tool/test/CMakeLists.txt).
#include <cstdio>
#include <filesystem>
#include <unistd.h>

#include "indep_s7_live_fixture.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/recording_report.hpp"

using namespace snd::tool;
using namespace ilive;

namespace {

std::string TempDir(const char* name) {
  const std::filesystem::path p =
      std::filesystem::temp_directory_path() / ("sonus-indep-live-" + std::to_string(::getpid()) + "-" + name);
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  return p.string();
}

}  // namespace

// Design 11.2 "切断時: ツールが切断しても、上書きした値はそのまま残る". The game keeps the values the
// tool wrote; a value the tool put back to the bank value while offline (Undo) must reach the game
// when it reconnects (live_sync.hpp 1: "the document's unsaved state is sent again"; P-impl 1
// 再接続). LiveSync assumes the overrides are gone and compares the game's bank file content only,
// so the cell keeps the old override.
SND_TEST(IndepS7LiveDefects, Defect_ReconnectKeepsCellsTheToolPutBackWhileOffline) {
  Rig r({}, Options(LiveDecision::SendDiffs));  // revertOnDisconnect off: the 11.2 default
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  SND_REQUIRE(r.doc.SetNumber(SfxG(), "volumeDb", -9.0));
  SND_REQUIRE(r.Settle());
  SND_REQUIRE_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -9.0f);
  r.conn.Close();
  r.Frames(20);
  SND_REQUIRE(r.doc.Undo());  // the document is the bank's build again
  r.Frames(5);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  SND_CHECK_EQ(r.GameF32(snd::HashId("Sfx"), bf::chunk::BUSS, bf::col::BUSS_volumeDb), -1.0f);
}

// D-86 / protocol §5: COMMAND_BUFFER_FULL is answered at receipt and the tool re-sends. A value
// refused as busy must not overtake a newer value of the same cell that the game already took
// (P-impl 2 "最後の値は必ず届く", the order of edits). LiveSync puts the refused older value back at
// the front of its queue and sends it again after the newer one was applied.
SND_TEST(IndepS7LiveDefects, Defect_ResentBusyValueOvertakesANewerValue) {
  ProjectDocument doc;
  Load(doc);
  LiveSync sync(doc, Options(LiveDecision::SendDiffs));
  MiniGame g;
  SND_REQUIRE(g.ok);
  const DocBuild b = BuildDocument(doc);
  for (const BankImage& bank : b.banks) {
    ll::BankEntry e{};
    e.bankId = bank.bankId;
    e.projectBuildHash = b.buildHash;
    e.contentHash = bank.contentHash;
    e.writable = 1;
    g.banks.push_back(e);
  }
  std::uint32_t setValues = 0;
  g.ackResult = [&](const MiniFrame& f) {
    if (!f.Is(ll::MessageType::SetValue)) return SND_OK;
    return ++setValues == 1u ? SND_ERR_COMMAND_BUFFER_FULL : SND_OK;
  };
  Connection conn;
  ConnectionDesc d;
  d.connector = &g.link.connector();
  d.profilerMask = 0;
  SND_REQUIRE_EQ(conn.Open(sync.Prepare(d)), SND_OK);
  auto tick = [&] {
    g.Pump();
    sync.Tick(conn, conn.Poll(), Now());
  };
  auto run = [&](auto pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > end) return false;
      tick();
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    return true;
  };
  SND_REQUIRE(run([&] { return sync.phase() == LivePhase::Live; }));
  g.holdAcks = true;  // the answers are slow on the wire
  SND_REQUIRE(doc.SetNumber(SfxG(), "volumeDb", -5.0));
  SND_REQUIRE(run([&] { return g.Of(ll::MessageType::SetValue).size() == 1u; }));
  SND_REQUIRE(doc.SetNumber(SfxG(), "volumeDb", -8.0));
  SND_REQUIRE(run([&] { return g.Of(ll::MessageType::SetValue).size() == 2u; }));
  g.holdAcks = false;
  g.Release();  // -5: busy (at receipt), -8: OK (applied)
  SND_REQUIRE(run([&] { return sync.phase() == LivePhase::Live; }));
  for (int i = 0; i < 20; ++i) tick();
  const std::vector<MiniFrame> sv = g.Of(ll::MessageType::SetValue);
  SND_REQUIRE(!sv.empty());
  // What the game holds is the last SetValue it applied (every one after the first was OK).
  SND_CHECK_EQ(F32Of(sv.back().As<ll::SetValue>().value), -8.0f);
  conn.Close();
}

// Protocol §8.4: RemoveEffect of the last effect the tool added live returns the Add's cost
// (effects, bytes). When the document's Undo removes the effect (LiveSync sends RemoveEffect),
// reserve() must fall back to zero (live_sync.hpp 3 "reserve(): what the live structural edits
// take"); it keeps counting the AddEffect.
SND_TEST(IndepS7LiveDefects, Defect_ReserveStaysTakenAfterDocumentUndoOfALiveEffect) {
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
  SND_REQUIRE_EQ(r.sync->reserve().effects, 1u);
  SND_REQUIRE(r.doc.Undo());
  SND_REQUIRE(r.Settle());
  // (RemoveEffect, or a reload: either way the game no longer holds the live effect)
  std::printf("editor applied %u, reloads %u\n", editor.applied, static_cast<unsigned>(r.sync->reloads().size()));
  SND_CHECK_EQ(r.sync->reserve().effects, 0u);
  SND_CHECK_EQ(r.sync->reserve().bytes, 0u);
}

// live_sync.hpp: LivePhase::Live = "the game holds the document's build". After a hot reload that
// failed in the game (the bank is detached there, reloads() says so), the phase is still Live.
SND_TEST(IndepS7LiveDefects, Defect_PhaseIsLiveAfterAFailedReload) {
  LiveSyncOptions o = Options(LiveDecision::SendDiffs);
  o.writer = [](const BankImage&, std::string* path, std::string*) {
    *path = "no/such/bank.sndb";
    return true;
  };
  Rig r({}, o);
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const json::Value init = Json("{\"bank\": \"Main\", \"bus\": \"Sfx\"}");
  SND_REQUIRE(!r.doc.Create("event", "Fresh", &init).empty());
  SND_REQUIRE(r.Run([&] { return !r.sync->reloads().empty() && r.sync->reloads().back().finished; }));
  SND_REQUIRE(r.sync->reloads().back().detached);
  r.Frames(10);
  SND_CHECK_MSG(r.sync->phase() != LivePhase::Live, "phase %s", LivePhaseName(r.sync->phase()));
}

// live_sync.hpp 3: reloads() shows the phases Unloading, Loading, Done / Failed. When the rebuilt
// bank cannot be written (the writer fails), the reload is finished with SND_ERR_IO but its phase
// stays Unloading (as if the game were unloading the bank), and its reason is the escalation's,
// not the failure.
SND_TEST(IndepS7LiveDefects, Defect_ReloadWriterFailureIsNotAFailedPhase) {
  LiveSyncOptions o = Options(LiveDecision::SendDiffs);
  o.writer = [](const BankImage&, std::string*, std::string* error) {
    *error = "disk full";
    return false;
  };
  Rig r({}, o);
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settle());
  const json::Value init = Json("{\"bank\": \"Main\", \"bus\": \"Sfx\"}");
  SND_REQUIRE(!r.doc.Create("event", "Fresh", &init).empty());
  SND_REQUIRE(r.Run([&] { return !r.sync->reloads().empty() && r.sync->reloads().back().finished; }));
  const ReloadProgress& p = r.sync->reloads().back();
  SND_CHECK_EQ(p.result, SND_ERR_IO);
  SND_CHECK(!p.detached);  // the game was never asked: the bank is still loaded
  SND_CHECK_MSG(p.phase == ll::ReloadPhase::Failed, "phase %u", static_cast<unsigned>(p.phase));
  SND_CHECK_MSG(p.reason.find("disk full") != std::string::npos, "reason: %s", p.reason.c_str());
}

// recording_report.hpp: "It reads back into the same BugReport". The notes and the comparison
// text written to summary.txt do not come back (notes empty, comparison empty) and the summary
// read back is the whole summary.txt, not the DescribeRecording text that was written.
SND_TEST(IndepS7LiveDefects, Defect_BugReportRoundTripLosesNotesAndComparison) {
  BugReport rep;
  rep.recording.info.sampleRate = 48000;
  rep.recording.info.blockSize = 256;
  snd_config_init(&rep.config);
  rep.configKnown = true;
  rep.title = "a title";
  rep.notes = "steps: press A, then B";
  rep.summary = DescribeRecording(rep.recording).Text();
  rep.comparison = "decisions match";
  const std::string dir = TempDir("report-notes");
  std::string err;
  SND_REQUIRE_MSG(WriteBugReport(dir, rep, &err), "%s", err.c_str());
  BugReport back;
  SND_REQUIRE_MSG(ReadBugReport(dir, &back, &err), "%s", err.c_str());
  SND_CHECK_EQ(back.title, rep.title);
  SND_CHECK_EQ(back.notes, rep.notes);
  SND_CHECK_EQ(back.comparison, rep.comparison);
  SND_CHECK_EQ(back.summary, rep.summary);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// recording_report.hpp: ParseConfigLine "false: unknown name or a malformed value". A negative
// number and a number that does not fit the member's type are malformed for an unsigned 32-bit
// member; they are taken (wrapped / truncated: max_banks becomes 4294967295 or 0).
SND_TEST(IndepS7LiveDefects, Defect_ConfigLineAcceptsNegativeAndOutOfRangeIntegers) {
  snd_config c;
  snd_config_init(&c);
  const std::uint32_t before = c.max_banks;
  SND_CHECK(!ParseConfigLine("max_banks", "-1", &c));
  SND_CHECK(!ParseConfigLine("max_banks", "4294967296", &c));
  SND_CHECK(!ParseConfigLine("max_banks", "99999999999999999999999", &c));
  SND_CHECK_EQ(c.max_banks, before);
}

namespace {
// Plays Tune from Calm and asks for Fight at `request` s through the router; returns the
// transitions and the output, and what MusicEditor::Preview computes.
struct TransitionRun {
  bool ok = false;
  TransitionPreview p;
  std::uint64_t startClock = 0;
  std::vector<preview::PreviewTransition> tr;
  std::vector<float> out;
  std::uint32_t channels = 2;
};
TransitionRun RunTransition(double request) {
  TransitionRun t;
  Engine en(true);
  if (en.e == nullptr) return t;
  MusicEditor me(en.doc, G(236));
  MusicSettings ms;
  ms.sampleRate = en.e->Status().sampleRate;
  ms.blockFrames = en.e->Status().blockFrames;
  t.p = me.Preview("Calm", "Fight", request, ms);
  AuditionRouter router;
  router.Bind(nullptr, en.e.get());
  en.Frames(2);
  t.startClock = en.e->NextBlock() * ms.blockFrames;
  if (!router.PlayTransition(TransitionAudition{snd::HashId("Tune"), snd::HashId("Calm"), snd::HashId("Fight"), request}).ok) return t;
  en.e->Render(static_cast<std::uint32_t>((t.p.fireSeconds + 1.0) * ms.sampleRate / ms.blockFrames), &t.out);
  t.tr = en.e->Transitions();
  t.channels = en.e->Status().channels;
  t.ok = t.p.valid;
  return t;
}
}  // namespace

// P-impl 4: the transition audition plays From and switches to To where the rule table says
// (MusicEditor::Preview, D-179: the tool's computation equals the runtime's). A request exactly on a
// bar line (2.0 s, 3/4 at 90 BPM, a block boundary too): MusicEditor::Preview says the next bar (4.0 s);
// the preview engine switches at once (2.0 s). One of the two is wrong (the router's request
// block, or the model's "next bar" at a bar line, group M); the audition does not do what the
// tool shows. The same 1 ms before a bar line (3.999 s): the model adds its lookahead (one block)
// and says the bar after; the engine switches at the near bar (it applies the request at the start
// of the block that contains the request time, 4.3 ms early).
SND_TEST(IndepS7LiveDefects, Defect_TransitionAuditionNearABarLineDisagreesWithThePreview) {
  for (const double request : {2.0, 3.999}) {
    const TransitionRun t = RunTransition(request);
    SND_REQUIRE(t.ok);
    SND_REQUIRE(!t.tr.empty());
    const std::uint64_t want = t.startClock + static_cast<std::uint64_t>(std::llround(t.p.fireSeconds * 48000.0));
    SND_CHECK_MSG(t.tr.back().clock == want, "request %.3f s: switch at %.4f s, MusicEditor::Preview %.4f s (lookahead %.2f ms)",
                  request, static_cast<double>(t.tr.back().clock - t.startClock) / 48000.0, t.p.fireSeconds, t.p.lookaheadMs);
  }
}

// P-impl 4 "From の State で鳴らし、要求の時刻に To へ切り替える". A request at 0 s: MusicEditor::Preview
// says Calm plays and Fight starts at the first bar (2.0 s); the preview engine starts in Fight
// directly (no transition, Calm is never heard): the state request lands before the music starts.
SND_TEST(IndepS7LiveDefects, Defect_TransitionAuditionAtTimeZeroSkipsTheFromState) {
  const TransitionRun t = RunTransition(0.0);
  SND_REQUIRE(t.ok);
  SND_CHECK_MSG(!t.tr.empty(), "no transition (MusicEditor::Preview: at %.4f s)", t.p.fireSeconds);
  // 0.5 s after the start the From state (Calm, DC kDc) must be playing, not Fight (3 kDc).
  const std::size_t at = static_cast<std::size_t>(24000) * t.channels;
  SND_REQUIRE(at < t.out.size());
  const float early = t.out[at], late = t.out[t.out.size() - t.channels];
  SND_CHECK_MSG(std::fabs(late / early - 3.0f) < 1e-3f, "level at 0.5 s %g, at the end %g", static_cast<double>(early),
                static_cast<double>(late));
}
