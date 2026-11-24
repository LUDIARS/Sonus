// Music editor model: tempo map (analytic), tap tempo and the grid, segment / playlist /
// music event editing with the rules, the transition rule table (match, unreachable rows
// against the build's warning, auto sort), minimum lookahead, the transition preview,
// undo / redo, save / load, the build output. The comparison with the music module's
// time line and rule lookup is in runtime_compare_test.cpp.
#include <cmath>

#include "doc_fixture.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/music_editor.hpp"

using namespace snd::tool;
using namespace doctest;
namespace bf = snd::bank_format;

namespace {

constexpr std::uint32_t kRate = 12000;

// Music project: assets a (8 s, stream, preload 0), b (8 s, memory), br (2 s bridge);
// segments SegA (a, 4 bars 120 BPM 4/4, markers), SegB (b, 2 bars 60 BPM 3/4),
// Bridge (br, 1 bar 120 BPM); playlists PA, PB; music event Music (Explore -> PA,
// Battle -> PB) with three rules and a stinger.
std::map<std::string, std::string> MusicFiles() {
  std::map<std::string, std::string> f = BaseFiles();
  f["assets/a.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(80) + "\", \"name\": \"a\", \"file\": \"audio/a.wav\", "
                       "\"loadForm\": \"stream\", \"preloadMs\": 0}";
  f["assets/b.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(81) + "\", \"name\": \"b\", \"file\": \"audio/b.wav\"}";
  f["assets/br.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(82) + "\", \"name\": \"br\", \"file\": \"audio/br.wav\", "
                        "\"loadForm\": \"stream\"}";
  f["segments/SegA.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(83) + "\", \"name\": \"SegA\", \"asset\": \"a\", \"bars\": 4, "
                            "\"tempo\": [{\"tick\": 0, \"bpm\": 120, \"beatsPerBar\": 4, \"beatUnit\": 4}], "
                            "\"markers\": [{\"name\": \"Drop\", \"frame\": 30000}, {\"name\": \"Out\", \"frame\": 90000, \"kind\": \"exit\"}]}";
  f["segments/SegB.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(84) + "\", \"name\": \"SegB\", \"asset\": \"b\", \"bars\": 2, "
                            "\"tempo\": [{\"tick\": 0, \"bpm\": 60, \"beatsPerBar\": 3, \"beatUnit\": 4}], "
                            "\"markers\": [{\"name\": \"Hit\", \"frame\": 24000}]}";
  f["segments/Bridge.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(85) + "\", \"name\": \"Bridge\", \"asset\": \"br\", \"bars\": 1}";
  f["playlists/PA.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(86) + "\", \"name\": \"PA\", \"entries\": [{\"segment\": \"SegA\"}]}";
  f["playlists/PB.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(87) + "\", \"name\": \"PB\", \"entries\": [{\"segment\": \"SegB\"}]}";
  f["events/Music.json"] =
      "{\"type\": \"event\", \"guid\": \"" + G(88) + "\", \"name\": \"Music\", \"bank\": \"Main\", \"music\": {"
      "\"initialState\": \"Explore\", \"states\": [{\"name\": \"Explore\", \"playlist\": \"PA\"}, {\"name\": \"Battle\", \"playlist\": \"PB\"}],"
      "\"transitions\": [{\"from\": \"*\", \"to\": \"*\", \"timing\": \"nextBar\"},"
      "                  {\"from\": \"Explore\", \"to\": \"Battle\", \"timing\": \"nextBeat\", \"fadeOutBeats\": 0.5, \"bridge\": \"Bridge\"},"
      "                  {\"from\": \"*\", \"to\": \"Battle\", \"timing\": \"segmentEnd\"}],"
      "\"stingers\": [{\"name\": \"Hit\", \"event\": \"Shot\"}]}}";
  return f;
}

void Load(ProjectDocument& doc) {
  LoadBase(doc, MusicFiles());
  doc.SetBinaryFile("audio/a.wav", ToneWav(8 * kRate, kRate, 2));
  doc.SetBinaryFile("audio/b.wav", ToneWav(8 * kRate, kRate, 2));
  doc.SetBinaryFile("audio/br.wav", ToneWav(2 * kRate, kRate, 2));
}

}  // namespace

SND_TEST(MusicEditor, TempoMapAnalytic) {
  // 120 BPM 4/4, from bar 2 (tick 7680) 90 BPM 3/4, from tick 7680 + 3 * 2880 6/8 at 60 BPM.
  const TempoMap m({{0, 120, 4, 4}, {7680, 90, 3, 4}, {7680 + 3 * 2880, 60, 6, 8}});
  SND_CHECK_NEAR(m.SecondsAtTick(960), 0.5, 1e-12);
  SND_CHECK_NEAR(m.SecondsAtTick(7680), 4.0, 1e-12);
  SND_CHECK_NEAR(m.SecondsAtTick(7680 + 960), 4.0 + 60.0 / 90.0, 1e-12);
  SND_CHECK_NEAR(m.TickAtSeconds(4.0 + 60.0 / 90.0), 7680 + 960, 1e-9);
  SND_CHECK_NEAR(m.BarPosition(7680), 2.0, 1e-12);
  SND_CHECK_NEAR(m.BarPosition(7680 + 2880), 3.0, 1e-12);
  const MusicalPosition p = m.Musical(7680 + 2880 + 960 + 100);
  SND_CHECK(p.bar == 3 && p.beat == 1 && p.tick == 100);
  SND_CHECK_NEAR(m.TickOfMusical(p), 7680 + 2880 + 960 + 100, 1e-9);
  // 6/8: a beat is an eighth (480 ticks), a bar 2880.
  const double t68 = 7680 + 3 * 2880;
  SND_CHECK_NEAR(m.BeatTicks(m.PointAt(t68)), 480.0, 1e-12);
  SND_CHECK_NEAR(m.NextBeat(t68 + 1), t68 + 480, 1e-9);
  // On a line: fires there (>=).
  SND_CHECK_NEAR(m.NextBar(7680), 7680, 1e-9);
  SND_CHECK_NEAR(m.NextBar(7681), 7680 + 2880, 1e-9);
  SND_CHECK_NEAR(m.BarsAfter(100, 2), 7680, 1e-9);
  SND_CHECK_NEAR(m.NextGrid(10, 16), 240, 1e-9);
  SND_CHECK_NEAR(m.NextGrid(7680 + 2880 + 1, 8), 7680 + 2880 + 480, 1e-9);
  // Frames.
  SND_CHECK_NEAR(m.FrameAtTick(960, 48000), 24000, 1e-9);
  SND_CHECK_NEAR(m.TickAtFrame(24000, 48000), 960, 1e-9);
}

SND_TEST(MusicEditor, TapTempoAndGrid) {
  TapTempo t;
  // Taps at 100 BPM (0.6 s) starting at 0.25 s with +-5 ms jitter.
  const double jitter[] = {0.004, -0.003, 0.005, -0.004, 0.0, 0.002, -0.005, 0.003};
  for (int i = 0; i < 8; ++i) t.Tap(0.25 + 0.6 * i + jitter[i]);
  SND_CHECK_NEAR(t.Bpm(), 100.0, 0.5);
  SND_CHECK_NEAR(t.OffsetSeconds(), 0.25, 0.01);
  const std::vector<GridLine> g = BeatGrid(100, 0.25, 4, 4, 3.0, 48000);
  SND_REQUIRE_EQ(g.size(), std::size_t{5});  // 0.25, 0.85, 1.45, 2.05, 2.65
  SND_CHECK(g[0].barLine && !g[1].barLine && g[4].barLine && g[4].bar == 1u);
  SND_CHECK_NEAR(g[1].frame, 0.85 * 48000, 1e-6);
  // 6/8 at 90: an eighth is 1/3 s.
  const std::vector<GridLine> g8 = BeatGrid(90, 0, 6, 8, 1.0, 48000);
  SND_CHECK_NEAR(g8[1].seconds, 1.0 / 3.0, 1e-12);
  // Apply to a segment: 8 s of material at 120 BPM 4/4 from frame 6000: 3.5 s left -> 1 bar? (2 s per bar) 3 bars? floor(7.5 / 2) = 3.
  ProjectDocument doc;
  Load(doc);
  SegmentEditor s(doc, G(83));
  SND_REQUIRE(s.ApplyGrid(120, 6000, 4, 4));
  SND_CHECK_EQ(doc.UndoCount(), std::size_t{1});
  SND_CHECK_EQ(NumberAt(doc.Find(G(83))->doc, "entryFrame", 0), 6000.0);
  SND_CHECK_EQ(NumberAt(doc.Find(G(83))->doc, "bars", 0), 3.0);
  SND_CHECK(!s.ApplyGrid(0, 0, 4, 4));
  SND_CHECK(!s.ApplyGrid(120, 0, 4, 3));
}

SND_TEST(MusicEditor, SegmentPlaylistEditingAndRules) {
  ProjectDocument doc;
  Load(doc);
  SegmentEditor s(doc, G(83));
  SND_REQUIRE(s.valid());
  SND_CHECK_EQ(s.SampleRate(), static_cast<double>(kRate));
  SND_CHECK(s.AddTempo({7680, 90, 3, 4}));
  SND_CHECK(!s.AddTempo({7680, 100, 3, 4}));  // the same tick
  SND_CHECK(!s.AddTempo({100, 0, 4, 4}));     // bpm
  SND_CHECK(!s.AddTempo({100, 120, 4, 3}));   // beat unit
  SND_CHECK(s.SetTempo(1, {3840, 90, 3, 4}));
  SND_CHECK_EQ(s.Tempo().size(), std::size_t{2});
  SND_CHECK(s.RemoveTempo(1));
  SND_CHECK(!s.AddMarker({"Drop", 1, MarkerKind::User}));        // name taken
  SND_CHECK(!s.AddMarker({"Late", 10 * kRate, MarkerKind::User}));  // outside the material
  SND_CHECK(s.AddMarker({"Pick", 100, MarkerKind::Pickup}));
  SND_CHECK(s.SetMarker(2, {"Pick2", 200, MarkerKind::Entry}));
  SND_CHECK(s.RemoveMarker(2));
  SND_CHECK(!s.SetEntryFrame(8 * kRate));
  SND_CHECK(s.SetExitFrame(7 * kRate));
  SND_CHECK(!s.SetEntryFrame(7 * kRate));
  SND_CHECK(s.SetEntryFrame(1000));
  SND_CHECK(!s.SetBars(0));
  SND_CHECK(s.SetBars(3));
  SND_CHECK(s.SetEntryPreload(true));
  SND_CHECK(!s.AddStem({1, 2, ""}));  // channels 1..2 of a stereo file
  SND_CHECK(s.AddStem({0, 2, ""}));
  SND_CHECK(s.AddStem({0, 1, G(81)}));
  SND_CHECK(s.SetStem(1, {1, 1, G(81)}));
  SND_CHECK(s.RemoveStem(0));
  PlaylistEditor pl(doc, G(86));
  SND_CHECK(pl.SetMode(PlaylistMode::Shuffle));
  SND_CHECK(pl.SetLoopCount(3));
  SND_CHECK(pl.AddEntry(G(84), 2.0, 2));
  SND_CHECK(!pl.AddEntry(G(84), -1, 1));
  SND_CHECK(!pl.AddEntry(G(84), 1, 0));
  SND_CHECK(!pl.AddEntry(G(80), 1, 1));  // an asset
  SND_CHECK(pl.MoveEntry(1, 0));
  SND_CHECK(pl.SetEntry(0, G(84), 3.0, 1));
  SND_CHECK_EQ(pl.EntryCount(), 2u);
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  const std::vector<CellRef> w = LocateField(b, doc, G(86), "entries[0].weight");
  SND_REQUIRE_EQ(w.size(), std::size_t{1});
  SND_CHECK_EQ(BitsToFloat(w[0].bits), 3.0f);
  SND_CHECK_EQ(LocateField(b, doc, G(86), "mode")[0].bits, 2u);
  // Everything undoes.
  while (doc.Undo()) {
  }
  SND_CHECK(doc.PendingChanges().empty());
}

SND_TEST(MusicEditor, StatesRulesAndStingers) {
  TempDir dir("music");
  WriteAll(dir, MusicFiles());
  dir.WriteBytes("audio/a.wav", ToneWav(8 * kRate, kRate, 2));
  dir.WriteBytes("audio/b.wav", ToneWav(8 * kRate, kRate, 2));
  dir.WriteBytes("audio/br.wav", ToneWav(2 * kRate, kRate, 2));
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  MusicEditor m(doc, G(88));
  SND_REQUIRE(m.valid());
  // The build warns about rows 1 and 2 (covered by "* -> *"); Unreachable says the same.
  std::vector<DocIssue> issues = CheckDocument(doc);
  SND_CHECK(HasIssue(issues, "can never match", false));
  const auto un = m.Unreachable();
  SND_REQUIRE_EQ(un.size(), std::size_t{2});
  SND_CHECK(un[0].first == 1u && un[0].second == 0u && un[1].first == 2u);
  SND_CHECK_EQ(m.Match("Explore", "Battle"), 0);
  // Auto sort is refused while rows are unreachable (D-179e); moving "* -> *" last fixes the table,
  // and then auto sort keeps it (specific rows first, then "* -> Battle", then "* -> *").
  SND_CHECK(!m.AutoSort());
  SND_REQUIRE(m.MoveRule(0, 2));
  SND_REQUIRE(m.AutoSort());
  std::vector<TransitionRule> rs = m.Rules();
  SND_CHECK(rs[0].from == "Explore" && rs[1].from == "*" && rs[1].to == "Battle" && rs[2].to == "*");
  SND_CHECK(m.Unreachable().empty());
  SND_CHECK(!HasIssue(CheckDocument(doc), "can never match", false));
  SND_CHECK_EQ(m.Match("Explore", "Battle"), 0);
  SND_CHECK_EQ(m.Match("Battle", "Battle"), 1);
  SND_CHECK_EQ(m.Match("Battle", "Explore"), 2);
  SND_CHECK_EQ(m.Match("Nope", "Explore"), 2);
  // Rules: states must exist, counts for barsAfter / grid, a marker for start=marker.
  TransitionRule r;
  r.from = "Calm";
  SND_CHECK(!m.AddRule(r, 0));
  r.from = "Battle";
  r.timing = Timing::BarsAfter;
  SND_CHECK(!m.AddRule(r, 0));
  r.timingCount = 2;
  r.start = StartPosition::Marker;
  SND_CHECK(!m.AddRule(r, 0));
  r.startMarker = "Drop";
  r.late = LateRule::StartLate;
  r.prepare = true;
  SND_CHECK(m.AddRule(r, 0));
  SND_CHECK_EQ(m.Match("Battle", "Explore"), 0);
  SND_CHECK(m.MoveRule(0, 3));
  SND_CHECK_EQ(m.Unreachable().size(), std::size_t{1});  // now covered by * -> *
  r.fadeInBeats = 2;
  SND_CHECK(m.SetRule(3, r));
  SND_CHECK(m.RemoveRule(3));
  // States.
  SND_CHECK(!m.AddState("*", G(86)));
  SND_CHECK(!m.AddState("Explore", G(86)));
  SND_CHECK(m.AddState("Boss", G(87)));
  SND_CHECK(!m.RemoveState("Battle"));  // used by a rule
  SND_CHECK(m.RenameState("Battle", "Fight"));
  rs = m.Rules();
  SND_CHECK(rs[0].to == "Fight" && rs[1].to == "Fight");
  SND_CHECK(m.SetInitialState("Boss"));
  SND_CHECK(!m.RemoveState("Boss"));  // the initial state
  SND_CHECK(m.SetInitialState("Explore"));
  SND_CHECK(m.SetStatePlaylist("Boss", G(86)));
  SND_CHECK(m.RemoveState("Boss"));
  // Stingers.
  SND_CHECK(!m.AddStinger({"Hit", G(8), Timing::NextBar, 0, false}));  // name taken
  SND_CHECK(m.AddStinger({"Accent", G(8), Timing::Grid, 8, true}));
  SND_CHECK(m.SetStinger(1, {"Accent", G(8), Timing::NextBar, 0, false}));
  SND_CHECK(!m.AddStinger({"Bad", G(83), Timing::NextBar, 0, false}));  // a segment
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  const ParsedBank* v = b.FindBank("Main")->view();
  SND_REQUIRE(v != nullptr);
  SND_CHECK_EQ(v->Rows(bf::chunk::MTRN), 3u);
  SND_CHECK_EQ(v->U32(bf::chunk::MTRN, bf::col::MTRN_toState, 0), snd::HashId("Fight"));
  SND_CHECK_EQ(v->Rows(bf::chunk::MSTG), 2u);
  // Save and reload; undo back to the file.
  SND_REQUIRE(doc.Save());
  ProjectDocument again;
  SND_REQUIRE(again.Load(dir.path().string()));
  SND_CHECK(JsonEqual(again.Find(G(88))->doc, doc.Find(G(88))->doc));
  while (doc.Undo()) {
  }
  ProjectDocument original;
  original.LoadFiles(MusicFiles());
  SND_CHECK(JsonEqual(doc.Find(G(88))->doc, original.Find(G(88))->doc));
}

SND_TEST(MusicEditor, MinimumLookaheadAndTransitionPreview) {
  ProjectDocument doc;
  Load(doc);
  MusicEditor m(doc, G(88));
  MusicSettings s;
  s.sampleRate = 48000;
  s.blockFrames = 480;  // 10 ms
  s.streamPrimeMs = 40;
  const std::vector<LookaheadEntry> la = m.MinimumLookahead(s);
  auto find = [&](const char* kind, const char* name) -> const LookaheadEntry* {
    for (const LookaheadEntry& e : la) {
      if (e.kind == kind && e.name == name) return &e;
    }
    return nullptr;
  };
  const LookaheadEntry* explore = find("state", "Explore");  // stream without preload
  const LookaheadEntry* battle = find("state", "Battle");    // memory
  const LookaheadEntry* bridge = find("bridge", "Bridge");    // stream with preload, head
  const LookaheadEntry* hit = find("stinger", "Hit");        // Shot's tone: memory
  SND_REQUIRE(explore && battle && bridge && hit);
  SND_CHECK(explore->reason == "stream" && explore->lookaheadMs == 50.0);
  SND_CHECK(battle->reason == "memory" && battle->lookaheadMs == 10.0);
  SND_CHECK(bridge->reason == "preload" && bridge->lookaheadMs == 10.0);
  SND_CHECK(hit->reason == "memory" && hit->lookaheadMs == 10.0);
  // "prepare" removes the prime time.
  TransitionRule toExplore;
  toExplore.to = "Explore";
  toExplore.prepare = true;
  SND_REQUIRE(m.AddRule(toExplore, 0));
  // Worst case over the rules leading to Explore: "* -> *" (not prepared) still needs it.
  SND_CHECK_EQ(m.MinimumLookahead(s)[0].primeMs, 40.0);
  SND_REQUIRE(doc.Undo());

  // Preview Explore -> Battle at 1.1 s (SegA 120 BPM: beat 0.5 s): rule 1? The table starts
  // with "* -> *" (rule 0) which covers it: next bar = 2.0 s.
  TransitionPreview p = m.Preview("Explore", "Battle", 1.1, s);
  SND_REQUIRE(p.valid);
  SND_CHECK_EQ(p.rule, 0);
  SND_CHECK_NEAR(p.fireSeconds, 2.0, 1e-9);
  SND_CHECK(p.fireAt.bar == 1 && p.fireAt.beat == 0);
  SND_CHECK(!p.lateApplied);
  // Sorted table: Explore -> Battle, next beat, 0.5 beat fade out, bridge (1 bar at 120 = 2 s).
  SND_REQUIRE(m.MoveRule(0, 2));
  SND_REQUIRE(m.AutoSort());
  p = m.Preview("Explore", "Battle", 1.1, s);
  SND_CHECK_EQ(p.rule, 0);
  SND_CHECK_NEAR(p.fireSeconds, 1.5, 1e-9);
  SND_CHECK_NEAR(p.fadeOutSeconds, 0.25, 1e-12);
  SND_CHECK(p.bridge == G(85));
  SND_CHECK_NEAR(p.bridgeSeconds, 2.0, 1e-9);
  SND_CHECK_NEAR(p.targetStartSeconds, 3.5, 1e-9);
  // Too close: at 1.495 s the next beat (1.5 s) is 5 ms away, SegB is in memory: 10 ms needed ->
  // late rule nextGrid: the following beat (2.0 s).
  p = m.Preview("Explore", "Battle", 1.495, s);
  // D-184 (the runtime's rules, music transition.cpp): taken at the first block start at or after
  // the request (10 ms blocks: block 150 = 1.5 s); the beat at that block start counts; the first
  // material (the bridge's preloaded stream) needs no prime and there is no extra lead block -> not
  // late, 1.5 s.
  SND_CHECK(!p.lateApplied);
  SND_CHECK_NEAR(p.fireSeconds, 1.5, 1e-9);
  // Battle -> Explore: "* -> *", next bar on SegB (60 BPM 3/4: a bar is 3 s), the fade of
  // the built-in default does not apply (a row matched): 0.
  p = m.Preview("Battle", "Explore", 0.2, s);
  SND_CHECK_EQ(p.rule, 2);
  SND_CHECK_NEAR(p.fireSeconds, 3.0, 1e-9);
  SND_CHECK_EQ(p.fadeOutSeconds, 0.0);
  // * -> Battle at segment end: SegA's exit (4 bars = 8 s); from Battle (the rule "* -> Battle").
  TransitionRule end;
  end.to = "Battle";
  end.timing = Timing::NextMarker;
  end.timingCount = snd::HashId("Drop");  // frame 30000 at 12 kHz = 2.5 s
  end.start = StartPosition::SameBar;
  end.fadeInBeats = 1;
  SND_REQUIRE(m.SetRule(1, end));
  SND_REQUIRE(m.RemoveRule(0));  // Explore -> Battle gone: "* -> Battle" is used
  p = m.Preview("Explore", "Battle", 0.3, s);
  SND_CHECK_EQ(p.rule, 0);
  SND_CHECK_NEAR(p.fireSeconds, 2.5, 1e-9);
  // Same bar: the source bar (1) modulo SegB's 2 bars = 1; 4/4 vs 3/4: beat not kept.
  SND_CHECK_NEAR(p.targetStartTick, 2880.0, 1e-9);
  SND_CHECK_NEAR(p.fadeInSeconds, 1.0, 1e-12);  // one beat at 60 BPM
  // Unknown states: invalid.
  SND_CHECK(!m.Preview("Nope", "Battle", 0, s).valid);
}
