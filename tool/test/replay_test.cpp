// Recording and replay through the tool (group P, design.md 2.5, 11.3): record on the preview
// engine over the live link (the game's path), save, load, replay twice in fresh runtimes
// (decisions equal, bits equal under 2.5's conditions), compare with the recorded run's event log,
// check against an expectation file, write and read a bug report (and leave one for the
// sonus-replay test), write a WAV; broken input never crashes.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "live_fixture.hpp"
#include "snd/internal/commands.hpp"
#include "snd/livelink/runtime.hpp"
#include "snd/tool/preview/replay_host.hpp"
#include "snd/tool/profiler.hpp"
#include "snd/tool/profiler_streams.hpp"
#include "snd/tool/recording_report.hpp"

using namespace snd::tool;
using namespace snd::tool::preview;
using livetest::Rig;
namespace ll = snd::livelink;
namespace fs = std::filesystem;

namespace {

struct Recorded {
  Recording rec;
  RecordedRunLog log;
  std::vector<ReportBank> banks;
  snd_config config{};
  bool ok = false;
};

// Records a short session on the preview engine: sounds start and stop, a live edit, a one-shot.
void RecordSession(Recorded* out) {
  Rig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  ll::Client* c = r.conn.client();
  Profiler prof;
  ProfilerStreams ps;
  ps.SetViews(kViewEventLog | kViewLod);
  auto poll = [&] {
    prof.Poll(c, livetest::Now());
    ps.Poll(c, livetest::Now());
  };
  SND_REQUIRE(r.Run([&] {
    poll();
    return ps.subscribedMask() != 0u;
  }));
  SND_REQUIRE(prof.StartRecording(c));
  SND_REQUIRE(r.Run([&] {
    poll();
    return prof.recordingState() == RecordingState::Recording;
  }));
  // The Ack comes from the live link thread; the audio thread starts recording at the next block
  // boundary. A few blocks first, so that nothing plays when the recording starts.
  r.Frames(4);
  poll();
  const std::uint64_t firstLogged = ps.log().size();
  const std::uint64_t tone = r.engine->Play(snd::HashId("Tone"), nullptr);
  r.Frames(10);
  SND_REQUIRE(r.doc.SetNumber(livetest::SfxGuid(), "volumeDb", -6.0));  // a live edit, recorded
  SND_REQUIRE(r.Settled());
  r.engine->Play(snd::HashId("Pad"), nullptr);
  r.Frames(10);
  r.engine->Stop(tone);
  r.Frames(10);
  SND_REQUIRE(prof.StopRecording(c));
  SND_REQUIRE(r.Run([&] {
    poll();
    return prof.recordingState() == RecordingState::Ready;
  }));
  r.Frames(4);
  poll();
  RecordingInfo info;
  info.sampleRate = r.conn.hello().sampleRate;
  info.blockSize = r.conn.hello().blockSize;
  info.platform = r.conn.hello().platform;
  info.contractVersion = r.conn.hello().contractVersion;
  info.projectBuildHash = r.gameBuild.buildHash;
  info.runtimeBuildHash = r.conn.hello().runtimeBuildHash;
  SND_REQUIRE(prof.TakeRecording(c, info, &out->rec));
  // The recorded run's log: the commands the audio thread took while recording.
  const std::uint32_t playKind = ll::kLogCommandBit | static_cast<std::uint32_t>(snd::cmd::CommandType::Play);
  for (std::size_t i = firstLogged; i < ps.log().size(); ++i) {
    if (ps.log()[i].entry.kind == playKind) out->log.log.push_back(ps.log()[i].entry);
  }
  out->banks = BanksOf(r.gameBuild);
  out->config = r.engine->config();
  out->ok = true;
}

}  // namespace

SND_TEST(ToolReplay, RecordSaveLoadReplayTwiceAndReport) {
  Recorded rd;
  RecordSession(&rd);
  SND_REQUIRE(rd.ok);
  SND_REQUIRE(!rd.rec.records.empty());
  SND_CHECK_EQ(rd.log.log.size(), 2u);  // Tone and Pad
  // Save and load (recording_file.hpp).
  const fs::path dir = fs::temp_directory_path() / ("sonus-replay-test-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  std::string err;
  SND_REQUIRE(SaveRecording((dir / "r.snrec").string(), rd.rec, &err));
  Recording back;
  SND_REQUIRE_MSG(LoadRecording((dir / "r.snrec").string(), &back, &err), "%s", err.c_str());
  SND_CHECK(back.info == rd.rec.info);
  SND_CHECK(back.records == rd.rec.records);
  // What it holds (1.1 records included).
  const RecordingDescription d = DescribeRecording(back);
  SND_CHECK(d.summary.valid);
  SND_CHECK(d.hasBegin);
  SND_CHECK(d.summary.arenas > 0u);
  SND_CHECK(d.commands > 0u);
  SND_CHECK_EQ(d.commandsByType[static_cast<std::size_t>(snd::cmd::CommandType::Play)], 2u);
  SND_CHECK(d.commandsByType[static_cast<std::size_t>(snd::cmd::CommandType::LiveSetValue)] >= 1u);
  SND_CHECK(!d.banks.empty());
  SND_CHECK(!d.Text().empty());
  // Replay twice in fresh runtimes with the engine's Config.
  ReplayOptions o;
  o.haveConfig = true;
  o.config = rd.config;
  o.keepOutput = true;
  ReplayRun a, b;
  const ReplayComparison c = ReplayTwice(back, rd.banks, o, &a, &b);
  SND_REQUIRE_MSG(a.ok && b.ok, "%s / %s", a.error.c_str(), b.error.c_str());
  SND_CHECK(a.finished);
  SND_CHECK_MSG(c.decisionsMatch, "%s", c.Text().c_str());
  SND_CHECK_MSG(c.bitExactExpected, "%s", c.Text().c_str());
  SND_CHECK_MSG(c.bitsMatch, "%s", c.Text().c_str());
  SND_CHECK(c.ok);
  SND_CHECK(!a.decisions.empty());
  SND_CHECK(a.samples > 0u);
  SND_CHECK_EQ(a.output.size(), a.samples);
  // The replay heard the tone (and its live edit): not silence.
  float peak = 0.0f;
  for (float s : a.output) peak = std::max(peak, std::fabs(s));
  SND_CHECK(peak > 0.05f);
  // Compared with the recorded run as the tool saw it (the event log's Play commands).
  const RecordedRunComparison rc = CompareWithRecordedRun(a, d, rd.log);
  SND_CHECK(rc.compared);
  SND_CHECK_MSG(rc.logMatches, "%s", rc.Text().c_str());
  // Expectations (tests): what the first replay did, checked by a new replay.
  const ReplayExpectation e = ExpectationOf(a, true);
  SND_REQUIRE(SaveExpectation((dir / "expected.txt").string(), e, &err));
  SND_REQUIRE(fs::create_directories(dir / "banks") || fs::exists(dir / "banks"));
  for (const ReportBank& bank : rd.banks) {
    std::ofstream f(dir / "banks" / bank.file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bank.bytes.data()), static_cast<std::streamsize>(bank.bytes.size()));
  }
  ReplayOptions o2 = o;
  o2.keepOutput = false;
  const ExpectationResult er = CheckRecordingFiles((dir / "r.snrec").string(), (dir / "banks").string(),
                                                   (dir / "expected.txt").string(), o2);
  SND_CHECK_MSG(er.ok, "%s", er.messages.empty() ? "" : er.messages[0].c_str());
  ReplayExpectation wrong = e;
  SND_REQUIRE(!wrong.decisions.empty());
  wrong.decisions[0].id ^= 1u;
  wrong.digest ^= 1u;
  const ExpectationResult bad = CheckExpectation(a, wrong);
  SND_CHECK(!bad.ok);
  SND_CHECK(bad.messages.size() >= 2u);  // the decision and the digest
  // WAV of the replayed output.
  SND_REQUIRE(WriteWav((dir / "replay.wav").string(), a.output, a.channels, a.sampleRate, &err));
  SND_CHECK(fs::file_size(dir / "replay.wav") > 44u + a.output.size() * 4u - 1u);
  // A bug report: written, read back, replayed again.
  const BugReport rep = MakeBugReport(back, rd.banks, &rd.config, "tone too quiet", "steps: play Tone, lower Sfx", true,
                                      &rd.log);
  SND_CHECK(rep.replayed && rep.decisionsMatch && rep.bitsMatch);
  const fs::path rdir = dir / "report";
  SND_REQUIRE_MSG(WriteBugReport(rdir.string(), rep, &err), "%s", err.c_str());
  BugReport got;
  SND_REQUIRE_MSG(ReadBugReport(rdir.string(), &got, &err), "%s", err.c_str());
  SND_CHECK(got.title == "tone too quiet");
  SND_CHECK(got.configKnown);
  SND_CHECK_EQ(got.config.max_playbacks, rd.config.max_playbacks);
  SND_CHECK_EQ(got.config.block_size, rd.config.block_size);
  SND_CHECK(got.recording.records == back.records);
  SND_REQUIRE_EQ(got.banks.size(), rd.banks.size());
  SND_CHECK(got.banks[0].bytes == rd.banks[0].bytes);
  SND_CHECK_EQ(got.banks[0].contentHash, rd.banks[0].contentHash);
  SND_CHECK(got.summary.find("tone too quiet") != std::string::npos);
  ExpectationResult ge;
  const ReplayComparison gc = ReplayBugReport(got, &ge);
  SND_CHECK_MSG(gc.ok, "%s", gc.Text().c_str());
  SND_CHECK_MSG(ge.ok, "%s", ge.messages.empty() ? "" : ge.messages[0].c_str());
  // Leave a report for the sonus-replay test (tool_replay_cli).
  if (const char* where = std::getenv("SND_TOOL_REPLAY_REPORT_DIR")) {
    std::error_code ec;
    fs::remove_all(where, ec);
    SND_CHECK(WriteBugReport(where, rep, &err));
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

SND_TEST(ToolReplay, BrokenInputIsRefusedWithoutCrashing) {
  std::string err;
  // A recording whose records are garbage: Decode refuses it, a replay refuses it.
  Recording junk;
  junk.info.sampleRate = 48000;
  junk.info.blockSize = 256;
  for (std::uint32_t i = 0; i < 200; ++i) junk.records.push_back(static_cast<std::uint8_t>(i * 37u + 11u));
  const std::vector<std::uint8_t> bytes = EncodeRecording(junk);
  Recording out;
  SND_CHECK(!DecodeRecording(bytes.data(), bytes.size(), &out, &err));
  ReplayRun run;
  SND_CHECK(!Replay(junk, {}, ReplayOptions{}, &run));
  SND_CHECK(!run.ok);
  SND_CHECK(!run.error.empty());
  const RecordingDescription d = DescribeRecording(junk);
  SND_CHECK(!d.summary.valid);
  SND_CHECK(!d.Text().empty());
  // Truncated and bit-flipped files.
  for (std::size_t cut = 0; cut < bytes.size(); cut += 17) {
    SND_CHECK(!DecodeRecording(bytes.data(), cut, &out, &err));
  }
  // Missing report folder, broken report text, broken expectation text.
  BugReport rep;
  SND_CHECK(!ReadBugReport("/nonexistent/sonus/report", &rep, &err));
  const fs::path dir = fs::temp_directory_path() / ("sonus-replay-broken-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  {
    std::ofstream f(dir / "report.txt");
    f << "format = sonus-bug-report 1\nbanks = 2\nbank.7.file = ../x\n";
  }
  SND_CHECK(!ReadBugReport(dir.string(), &rep, &err));
  ReplayExpectation e;
  SND_CHECK(!DecodeExpectation("not one", &e, &err));
  SND_CHECK(!DecodeExpectation("sonus-replay-expectation 1\ndecisions 2\n1 2 3 4 5\n", &e, &err));
  SND_CHECK(!DecodeExpectation("sonus-replay-expectation 1\n1 2 3 4 5\n", &e, &err));
  SND_CHECK(DecodeExpectation("sonus-replay-expectation 1\ndigest 00000000000000ff\ndecisions 1\n1 2 3 4 5\n", &e, &err));
  SND_CHECK(e.hasDigest && e.digest == 0xFFu && e.decisions.size() == 1u && e.decisions[0].detail == 5u);
  // Config text round trip.
  snd_config c;
  snd_config_init(&c);
  c.max_playbacks = 77;
  c.tuning.short_fade_ms = 7.5f;
  c.dev_reserve.bytes = 12345;
  snd_config r2;
  snd_config_init(&r2);
  const std::string text = ConfigText(c);
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t nl = text.find('\n', at);
    const std::string line = text.substr(at, nl - at);
    at = nl + 1;
    const std::size_t eq = line.find(" = ");
    SND_REQUIRE(eq != std::string::npos);
    SND_CHECK(ParseConfigLine(line.substr(0, eq), line.substr(eq + 3), &r2));
  }
  SND_CHECK_EQ(r2.max_playbacks, 77u);
  SND_CHECK_EQ(r2.tuning.short_fade_ms, 7.5f);
  SND_CHECK_EQ(r2.dev_reserve.bytes, 12345u);
  SND_CHECK(!ParseConfigLine("no_such", "1", &r2));
  SND_CHECK(!ParseConfigLine("max_playbacks", "x", &r2));
  std::error_code ec;
  fs::remove_all(dir, ec);
}

SND_TEST(ToolReplay, OutputPlayerPlaysEverySample) {
  // Listening to a replay: the samples go to an output device in real time. A silent timer
  // device stands in for the platform's (none in the test machine).
  std::vector<float> samples(2u * 256u * 8u, 0.25f);
  OutputPlayer p;
  std::string err;
  SND_REQUIRE_MSG(p.Start(samples, 2, 48000, true, &err), "%s", err.c_str());
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!p.Done() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  SND_CHECK(p.Done());
  SND_CHECK_EQ(p.FramesPlayed(), samples.size() / 2u);
  p.Stop();
}
