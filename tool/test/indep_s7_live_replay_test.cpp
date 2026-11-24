// Independent tests of group P: recording and replay through the tool (design 2.5, 11.3;
// tool/test/indep_s7_live_properties.md E, F3). The expected values are the replays themselves
// (two replays agree), analytic file sizes, and round trips; broken input must be refused without
// a crash (ASan / UBSan).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include <unistd.h>

#include "indep_s7_live_fixture.hpp"
#include "snd/internal/commands.hpp"
#include "snd/livelink/runtime.hpp"
#include "snd/tool/preview/replay_host.hpp"
#include "snd/tool/profiler.hpp"
#include "snd/tool/recording_report.hpp"

using namespace snd::tool;
using namespace snd::tool::preview;
using namespace ilive;
namespace fs = std::filesystem;

namespace {

struct Rec {
  bool ok = false;
  Recording rec;
  std::vector<ReportBank> banks;
  snd_config config{};
};

// Records on the preview engine through the live link (the game's path): plays, a live edit, a
// live audition at a position, a parameter override, a stop.
Rec RecordOnce() {
  Rec out;
  Rig r;
  if (!r.ok || !r.Connect() || !r.Settle()) return out;
  ll::Client* c = r.conn.client();
  Profiler prof;
  if (!prof.StartRecording(c)) return out;
  if (!r.Run([&] {
        prof.Poll(c, Now());
        return prof.recordingState() == RecordingState::Recording;
      })) {
    return out;
  }
  r.Frames(4);
  const std::uint64_t tone = r.engine->Play(snd::HashId("Tone"), nullptr);
  r.Frames(6);
  r.doc.SetNumber(SfxG(), "volumeDb", -4.0);
  r.Settle();
  const float pos[3] = {1.0f, 0.0f, 2.0f};
  c->Audition(snd::HashId("Spot"), pos, false, true);
  c->OverrideParam(snd::HashId("Knob"), 0.75f, true, true);
  r.Frames(8);
  r.engine->Play(snd::HashId("Hum"), nullptr);
  r.Frames(5);
  r.engine->Stop(tone);
  r.Frames(8);
  if (!prof.StopRecording(c)) return out;
  if (!r.Run([&] {
        prof.Poll(c, Now());
        return prof.recordingState() == RecordingState::Ready;
      })) {
    return out;
  }
  RecordingInfo info;
  info.sampleRate = r.conn.hello().sampleRate;
  info.blockSize = r.conn.hello().blockSize;
  info.platform = r.conn.hello().platform;
  info.contractVersion = r.conn.hello().contractVersion;
  info.projectBuildHash = r.gameBuild.buildHash;
  info.runtimeBuildHash = r.conn.hello().runtimeBuildHash;
  if (!prof.TakeRecording(c, info, &out.rec)) return out;
  out.banks = BanksOf(r.gameBuild);
  out.config = r.engine->config();
  out.ok = true;
  return out;
}

const Rec& Recorded() {
  static const Rec r = RecordOnce();
  return r;
}

ReplayOptions WithConfig(const Rec& r) {
  ReplayOptions o;
  o.haveConfig = true;
  o.config = r.config;
  return o;
}

std::string TempDir(const char* name) {
  const fs::path p = fs::temp_directory_path() / ("sonus-indep-replay-" + std::to_string(::getpid()) + "-" + name);
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p.string();
}

bool WriteFile(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << text;
  return static_cast<bool>(f);
}

std::uint32_t U32At(const std::vector<std::uint8_t>& b, std::size_t at) {
  std::uint32_t v = 0;
  std::memcpy(&v, b.data() + at, 4);
  return v;
}

}  // namespace

// E1: save / load round trip, two replays agree on decisions and (2.5) on every bit.
SND_TEST(IndepS7LiveReplay, RecordSaveLoadReplayTwiceAgree) {
  const Rec& rd = Recorded();
  SND_REQUIRE(rd.ok);
  const std::string dir = TempDir("e1");
  std::string err;
  SND_REQUIRE(SaveRecording(dir + "/r.snrec", rd.rec, &err));
  Recording back;
  SND_REQUIRE_MSG(LoadRecording(dir + "/r.snrec", &back, &err), "%s", err.c_str());
  SND_CHECK(back.info == rd.rec.info);
  SND_CHECK(back.records == rd.rec.records);
  const RecordingDescription d = DescribeRecording(back);
  SND_CHECK(d.summary.valid);
  SND_CHECK(d.hasBegin);
  SND_CHECK_EQ(d.commandsByType[static_cast<std::size_t>(snd::cmd::CommandType::Play)], 2u);
  SND_CHECK(d.commandsByType[static_cast<std::size_t>(snd::cmd::CommandType::LiveAudition)] >= 1u);
  SND_CHECK(d.summary.firstBlock <= d.summary.lastBlock);
  ReplayOptions o = WithConfig(rd);
  o.keepOutput = true;
  ReplayRun a, b;
  const ReplayComparison c = ReplayTwice(back, rd.banks, o, &a, &b);
  SND_REQUIRE_MSG(a.ok && b.ok, "%s / %s", a.error.c_str(), b.error.c_str());
  SND_CHECK(a.finished && b.finished);
  SND_CHECK_MSG(c.ok && c.decisionsMatch && c.bitExactExpected && c.bitsMatch, "%s", c.Text().c_str());
  SND_CHECK_EQ(a.decisions.size(), b.decisions.size());
  SND_CHECK_EQ(a.digest, b.digest);
  // The digest covers the recorded blocks: blocks x frames x channels samples (lead blocks excluded).
  const std::uint64_t recorded = d.summary.lastBlock - d.summary.firstBlock + 1u;
  SND_CHECK_EQ(a.samples, recorded * a.channels * rd.config.block_size);
  SND_CHECK_EQ(a.output.size(), a.samples);
  // Two plays and the audition are logic decisions of both replays.
  std::uint32_t plays = 0;
  for (const ll::ReplayDecision& x : a.decisions) {
    plays += x.kind == (ll::kLogCommandBit | static_cast<std::uint32_t>(snd::cmd::CommandType::Play)) ? 1u : 0u;
  }
  SND_CHECK_EQ(plays, 2u);
  // A third replay with extra blocks keeps the same digest (it covers the recorded blocks only).
  ReplayOptions o3 = WithConfig(rd);
  o3.extraBlocks = 16;
  ReplayRun e;
  SND_REQUIRE(Replay(back, rd.banks, o3, &e));
  SND_CHECK_EQ(e.digest, a.digest);
  SND_CHECK_EQ(e.blocks, a.blocks + 16u);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// E2 / F3: replays on two threads at once give the same result as one alone.
SND_TEST(IndepS7LiveReplay, ParallelReplaysAgree) {
  const Rec& rd = Recorded();
  SND_REQUIRE(rd.ok);
  ReplayRun one;
  SND_REQUIRE(Replay(rd.rec, rd.banks, WithConfig(rd), &one));
  ReplayRun runs[3];
  std::thread t[3];
  for (int i = 0; i < 3; ++i) t[i] = std::thread([&, i] { Replay(rd.rec, rd.banks, WithConfig(rd), &runs[i]); });
  for (std::thread& x : t) x.join();
  for (const ReplayRun& r : runs) {
    SND_CHECK(r.ok);
    SND_CHECK_EQ(r.digest, one.digest);
    SND_CHECK(CompareRuns(one, r).decisionsMatch);
  }
}

// E3: CompareRuns on runs made by hand.
SND_TEST(IndepS7LiveReplay, CompareRunsByHand) {
  ReplayRun a;
  a.ok = true;
  a.bitExactExpected = true;
  a.digest = 7;
  a.samples = 100;
  a.decisions = {ll::ReplayDecision{1, 2, 3, 4, 5}, ll::ReplayDecision{2, 2, 3, 4, 5}};
  ReplayRun b = a;
  ReplayComparison c = CompareRuns(a, b);
  SND_CHECK(c.ok && c.decisionsMatch && c.bitsMatch && c.firstDecisionDiff == -1);
  b.decisions[1].detail = 6;
  c = CompareRuns(a, b);
  SND_CHECK(!c.ok);
  SND_CHECK_EQ(c.firstDecisionDiff, 1);
  b = a;
  b.decisions.pop_back();  // a prefix
  c = CompareRuns(a, b);
  SND_CHECK(!c.decisionsMatch);
  SND_CHECK_EQ(c.firstDecisionDiff, 1);
  b = a;
  b.digest = 8;
  c = CompareRuns(a, b);
  SND_CHECK(!c.ok && c.decisionsMatch && !c.bitsMatch && !c.reasons.empty());
  // Outside 2.5: bits are not asked for, and the comparison says why.
  b.bitExactExpected = false;
  b.info.conditions = ll::kReplayLodRecorded;
  c = CompareRuns(a, b);
  SND_CHECK(c.ok);
  SND_CHECK(!c.bitExactExpected);
  SND_CHECK(!c.reasons.empty());
  SND_CHECK(!ConditionReasons(ll::kReplayLodRecorded).empty());
  SND_CHECK(ConditionReasons(0).empty());
  // A failed replay is never ok, even with equal (empty) logs.
  ReplayRun f1, f2;
  c = CompareRuns(f1, f2);
  SND_CHECK(!c.ok);
  SND_CHECK(!c.Text().empty());
}

// E4: expectation files.
SND_TEST(IndepS7LiveReplay, ExpectationRoundTripAndMismatch) {
  ReplayExpectation e;
  e.hasDigest = true;
  e.digest = 0xFEDCBA9876543210ull;
  e.decisions = {ll::ReplayDecision{0, 0x8001u, 0xFFFFFFFFu, 0, 0xFFFFFFFFu}, ll::ReplayDecision{~0ull, 1, 2, 3, 4}};
  ReplayExpectation back;
  std::string err;
  SND_REQUIRE_MSG(DecodeExpectation(EncodeExpectation(e), &back, &err), "%s", err.c_str());
  SND_CHECK(back.hasDigest && back.digest == e.digest);
  SND_REQUIRE_EQ(back.decisions.size(), 2u);
  SND_CHECK_EQ(back.decisions[0].id, 0xFFFFFFFFu);
  SND_CHECK_EQ(back.decisions[1].block, ~0ull);
  // Empty decisions round trip.
  ReplayExpectation empty;
  SND_CHECK(DecodeExpectation(EncodeExpectation(empty), &back, &err));
  SND_CHECK(back.decisions.empty() && !back.hasDigest);
  // Malformed.
  for (const char* t : {"", "sonus-replay-expectation 2\ndecisions 0\n", "sonus-replay-expectation 1\n",
                        "sonus-replay-expectation 1\ndecisions 1\n1 2 3 4\n",
                        "sonus-replay-expectation 1\ndecisions 1\n1 2 3 4 99999999999\n",
                        "sonus-replay-expectation 1\ndecisions 99999999999999999999\n",
                        "sonus-replay-expectation 1\ndigest zz\ndecisions 0\n",
                        "sonus-replay-expectation 1\ndecisions 0\n1 2 3 4 5\n"}) {
    SND_CHECK_MSG(!DecodeExpectation(t, &back, &err), "accepted: %s", t);
  }
  // CheckExpectation names what differs.
  ReplayRun run;
  run.ok = true;
  run.bitExactExpected = true;
  run.digest = e.digest;
  run.decisions = e.decisions;
  SND_CHECK(CheckExpectation(run, e).ok);
  run.decisions[1].id = 9;
  ExpectationResult r = CheckExpectation(run, e);
  SND_CHECK(!r.ok && !r.messages.empty());
  run.decisions = e.decisions;
  run.bitExactExpected = false;  // a digest is expected but the replay is outside 2.5
  r = CheckExpectation(run, e);
  SND_CHECK(!r.ok);
  ReplayRun failed;
  SND_CHECK(!CheckExpectation(failed, ReplayExpectation{}).ok);
}

// E5: every numeric member of snd_config survives ConfigText -> ParseConfigLine.
SND_TEST(IndepS7LiveReplay, ConfigTextRoundTripsEveryMember) {
  snd_config c;
  snd_config_init(&c);
  // Change every member through the text itself: n-th line gets a value derived from n.
  const std::string text = ConfigText(c);
  snd_config d;
  snd_config_init(&d);
  std::size_t at = 0;
  std::uint32_t n = 0;
  std::string changed;
  while (at < text.size()) {
    const std::size_t nl = text.find('\n', at);
    const std::string line = text.substr(at, nl - at);
    at = nl + 1;
    const std::size_t eq = line.find(" = ");
    SND_REQUIRE(eq != std::string::npos);
    const std::string name = line.substr(0, eq), value = line.substr(eq + 3);
    ++n;
    const bool isFloat = value.find_first_of(".e") != std::string::npos ||
                         name.find("_ms") != std::string::npos || name.find("percent") != std::string::npos;
    const std::string v = isFloat ? std::to_string(n) + ".5" : std::to_string(n % 2 == 0 ? n : 1u);
    if (!ParseConfigLine(name, v, &d)) {
      // integer members do not take "n.5": try an integer
      SND_CHECK_MSG(ParseConfigLine(name, std::to_string(n), &d), "member %s", name.c_str());
    }
    changed += name + "\n";
  }
  SND_CHECK(n > 20u);
  // The changed Config prints and parses back to itself.
  const std::string t2 = ConfigText(d);
  snd_config e;
  snd_config_init(&e);
  at = 0;
  while (at < t2.size()) {
    const std::size_t nl = t2.find('\n', at);
    const std::string line = t2.substr(at, nl - at);
    at = nl + 1;
    const std::size_t eq = line.find(" = ");
    SND_CHECK(ParseConfigLine(line.substr(0, eq), line.substr(eq + 3), &e));
  }
  SND_CHECK_EQ(ConfigText(e), t2);
  SND_CHECK(t2 != text);
  SND_CHECK(!ParseConfigLine("", "1", &e));
  SND_CHECK(!ParseConfigLine("max_banks", "", &e));
  SND_CHECK(!ParseConfigLine("max_banks", "1 2", &e));
  SND_CHECK(!ParseConfigLine("max_banks", "0x", &e));
}

// E6: a bug report made from the recording, written, read back and replayed.
SND_TEST(IndepS7LiveReplay, BugReportRoundTripReplays) {
  const Rec& rd = Recorded();
  SND_REQUIRE(rd.ok);
  const BugReport rep = MakeBugReport(rd.rec, rd.banks, &rd.config, "one line", "", true);
  SND_REQUIRE(rep.replayed);
  SND_CHECK(rep.decisionsMatch && rep.bitExactExpected && rep.bitsMatch);
  SND_CHECK(!rep.summary.empty());
  SND_CHECK(!rep.comparison.empty());
  const std::string dir = TempDir("report");
  std::string err;
  SND_REQUIRE_MSG(WriteBugReport(dir, rep, &err), "%s", err.c_str());
  BugReport got;
  SND_REQUIRE_MSG(ReadBugReport(dir, &got, &err), "%s", err.c_str());
  SND_CHECK_EQ(got.title, rep.title);
  SND_CHECK(got.configKnown);
  SND_CHECK_EQ(ConfigText(got.config), ConfigText(rep.config));
  SND_CHECK(got.recording.info == rep.recording.info);
  SND_CHECK(got.recording.records == rep.recording.records);
  SND_REQUIRE_EQ(got.banks.size(), rep.banks.size());
  for (std::size_t i = 0; i < got.banks.size(); ++i) {
    SND_CHECK_EQ(got.banks[i].file, rep.banks[i].file);
    SND_CHECK_EQ(got.banks[i].bankId, rep.banks[i].bankId);
    SND_CHECK_EQ(got.banks[i].languageId, rep.banks[i].languageId);
    SND_CHECK_EQ(got.banks[i].contentHash, rep.banks[i].contentHash);
    SND_CHECK(got.banks[i].bytes == rep.banks[i].bytes);
  }
  SND_CHECK_EQ(got.protocolMajor, rep.protocolMajor);
  SND_CHECK_EQ(got.protocolMinor, rep.protocolMinor);
  SND_CHECK(got.replayed && got.decisionsMatch == rep.decisionsMatch && got.bitsMatch == rep.bitsMatch);
  SND_CHECK_EQ(got.expectation.decisions.size(), rep.expectation.decisions.size());
  SND_CHECK_EQ(got.expectation.digest, rep.expectation.digest);
  ExpectationResult er;
  const ReplayComparison c = ReplayBugReport(got, &er);
  SND_CHECK_MSG(c.ok, "%s", c.Text().c_str());
  SND_CHECK(er.ok);
  // A title with a line break keeps the first line only (one line, by the header).
  BugReport two = rep;
  two.title = "first\nsecond";
  SND_REQUIRE(WriteBugReport(dir, two, &err));
  SND_REQUIRE(ReadBugReport(dir, &got, &err));
  SND_CHECK_EQ(got.title, std::string("first"));
  // Bank names that leave the folder are refused when writing.
  BugReport evil = rep;
  evil.banks[0].file = "../evil.sndb";
  SND_CHECK(!WriteBugReport(TempDir("evil"), evil, &err));
  // Leave reports for the sonus-replay exit code tests (tool/test/CMakeLists.txt).
  if (const char* where = std::getenv("SND_INDEP_LIVE_REPORTS")) {
    SND_CHECK(WriteBugReport(std::string(where) + "/ok", rep, &err));
    BugReport bad = rep;
    if (!bad.expectation.decisions.empty()) bad.expectation.decisions[0].id ^= 1u;
    bad.expectation.digest ^= 1u;
    SND_CHECK(WriteBugReport(std::string(where) + "/mismatch", bad, &err));
    fs::create_directories(std::string(where) + "/broken");
    WriteFile(std::string(where) + "/broken/report.txt", "format = sonus-bug-report 1\nbanks = 1\n");
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// E7: broken files: truncated at every length, lying lengths, huge numbers, bad report folders.
SND_TEST(IndepS7LiveReplay, BrokenFilesAreRefused) {
  const Rec& rd = Recorded();
  SND_REQUIRE(rd.ok);
  const std::vector<std::uint8_t> file = EncodeRecording(rd.rec);
  Recording out;
  std::string err;
  SND_REQUIRE(DecodeRecording(file.data(), file.size(), &out, &err));
  for (std::size_t cut = 0; cut < file.size(); cut += (cut < 256u ? 1u : 61u)) {
    SND_CHECK_MSG(!DecodeRecording(file.data(), cut, &out, &err), "cut %zu", cut);
  }
  // recordBytes (offset 48) lies: larger, smaller, huge.
  for (const std::uint64_t lie : {std::uint64_t{file.size()} * 2u, std::uint64_t{8}, ~std::uint64_t{0}, ~std::uint64_t{0} - 63u}) {
    std::vector<std::uint8_t> f = file;
    std::memcpy(f.data() + 48, &lie, 8);
    SND_CHECK(!DecodeRecording(f.data(), f.size(), &out, &err));
  }
  // headerBytes (offset 8) lies.
  for (const std::uint32_t hb : {0u, 63u, 65u, 0xFFFFFFFFu}) {
    std::vector<std::uint8_t> f = file;
    std::memcpy(f.data() + 8, &hb, 4);
    SND_CHECK(!DecodeRecording(f.data(), f.size(), &out, &err));
  }
  // Bit flips anywhere: the checksum refuses them.
  std::mt19937 rng(7u);
  for (int i = 0; i < 200; ++i) {
    std::vector<std::uint8_t> f = file;
    f[rng() % f.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8u));
    SND_CHECK(!DecodeRecording(f.data(), f.size(), &out, &err));
  }
  // Report folders.
  const std::string dir = TempDir("broken");
  BugReport rep;
  const std::vector<std::string> reports = {
      "",
      "format = sonus-bug-report 2\n",
      "format = sonus-bug-report 1\n",  // no recording
      "format = sonus-bug-report 1\nbanks = 18446744073709551615\n",
      "format = sonus-bug-report 1\nbanks = 1\nbank.1.file = a.sndb\n",
      "format = sonus-bug-report 1\nbanks = 1\nbank.0.file = ..\n",
      "format = sonus-bug-report 1\nbanks = 1\nbank.0.file = a/b.sndb\n",
      "format = sonus-bug-report 1\nbanks = 1\nbank.0.bankId = zz\n",
      "format = sonus-bug-report 1\nconfig.max_banks = x\n",
      "format = sonus-bug-report 1\nbank.0.file = a.sndb\n",
  };
  for (const std::string& t : reports) {
    SND_REQUIRE(WriteFile(dir + "/report.txt", t));
    SND_CHECK_MSG(!ReadBugReport(dir, &rep, &err), "accepted: %s", t.c_str());
  }
  // A good report whose bank file is gone, or whose recording is cut.
  const BugReport good = MakeBugReport(rd.rec, rd.banks, &rd.config, "t", "", false);
  SND_REQUIRE(WriteBugReport(dir, good, &err));
  SND_REQUIRE(ReadBugReport(dir, &rep, &err));
  fs::resize_file(dir + "/recording.snrec", 100);
  SND_CHECK(!ReadBugReport(dir, &rep, &err));
  SND_REQUIRE(WriteBugReport(dir, good, &err));
  fs::remove(dir + "/banks/" + good.banks[0].file);
  SND_CHECK(!ReadBugReport(dir, &rep, &err));
  // A bank directory with a file too short to be a bank.
  const std::string bd = TempDir("bankdir");
  SND_REQUIRE(WriteFile(bd + "/tiny.sndb", "abc"));
  std::vector<ReportBank> banks;
  SND_CHECK(!ReadBankDirectory(bd, &banks, &err));
  SND_CHECK(!ReadBankDirectory(bd + "/none", &banks, &err));
  // A replay with banks that do not belong to the recording, or none: no crash, a result.
  ReplayRun run;
  Replay(rd.rec, {}, WithConfig(rd), &run);
  std::vector<ReportBank> garbage = rd.banks;
  for (ReportBank& b : garbage) std::fill(b.bytes.begin() + 64, b.bytes.end(), std::uint8_t{0x5A});
  Replay(rd.rec, garbage, WithConfig(rd), &run);
  SND_CHECK(!run.ok || run.decisions.size() < 1000000u);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(bd, ec);
}

// E8: DescribeRecording / Summarize on arbitrary bytes (fixed seed) and on mutated records.
SND_TEST(IndepS7LiveReplay, DescribeArbitraryBytes) {
  const Rec& rd = Recorded();
  SND_REQUIRE(rd.ok);
  std::mt19937 rng(11u);
  for (int i = 0; i < 300; ++i) {
    std::vector<std::uint8_t> b = rd.rec.records;
    const int flips = 1 + static_cast<int>(rng() % 8u);
    for (int k = 0; k < flips; ++k) b[rng() % b.size()] = static_cast<std::uint8_t>(rng());
    if ((i % 5) == 0) b.resize(rng() % b.size());
    const RecordingDescription d = DescribeRecording(b.data(), b.size());
    (void)d.Text();
    (void)Summarize(b.data(), b.size());
  }
  for (int i = 0; i < 100; ++i) {
    std::vector<std::uint8_t> b(static_cast<std::size_t>(rng() % 512u));
    for (std::uint8_t& x : b) x = static_cast<std::uint8_t>(rng());
    const RecordingDescription d = DescribeRecording(b.data(), b.size());
    (void)d.Text();
  }
  SND_CHECK(!DescribeRecording(nullptr, 0).Text().empty());
}

// E9: WriteWav: a 32-bit float WAV of exactly the samples given.
SND_TEST(IndepS7LiveReplay, WavFileIsExact) {
  const std::string dir = TempDir("wav");
  std::vector<float> s(2u * 1000u);
  for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<float>(i) / 2000.0f;
  std::string err;
  SND_REQUIRE(WriteWav(dir + "/a.wav", s, 2, 44100, &err));
  std::ifstream in(dir + "/a.wav", std::ios::binary);
  std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  SND_REQUIRE(b.size() >= 44u);
  SND_CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
  SND_CHECK(std::memcmp(b.data() + 8, "WAVE", 4) == 0);
  SND_CHECK_EQ(U32At(b, 4), static_cast<std::uint32_t>(b.size() - 8u));
  // Find fmt and data.
  std::size_t at = 12;
  std::uint32_t format = 0, channels = 0, rate = 0, bits = 0, dataBytes = 0;
  std::size_t dataAt = 0;
  while (at + 8u <= b.size()) {
    const std::uint32_t size = U32At(b, at + 4);
    if (std::memcmp(b.data() + at, "fmt ", 4) == 0) {
      format = U32At(b, at + 8) & 0xFFFFu;
      channels = U32At(b, at + 8) >> 16;
      rate = U32At(b, at + 12);
      bits = U32At(b, at + 22) & 0xFFFFu;
    } else if (std::memcmp(b.data() + at, "data", 4) == 0) {
      dataBytes = size;
      dataAt = at + 8;
    }
    at += 8u + size + (size & 1u);
  }
  SND_CHECK(format == 3u || format == 0xFFFEu);  // IEEE float (or extensible)
  SND_CHECK_EQ(channels, 2u);
  SND_CHECK_EQ(rate, 44100u);
  SND_CHECK_EQ(bits, 32u);
  SND_REQUIRE_EQ(dataBytes, s.size() * 4u);
  SND_CHECK(std::memcmp(b.data() + dataAt, s.data(), dataBytes) == 0);
  // An odd sample count drops the incomplete frame; no channels / rate is refused.
  std::vector<float> odd(5, 0.5f);
  SND_REQUIRE(WriteWav(dir + "/b.wav", odd, 2, 48000, &err));
  SND_CHECK_EQ(fs::file_size(dir + "/b.wav"), fs::file_size(dir + "/a.wav") - (s.size() - 4u) * 4u);
  SND_CHECK(!WriteWav(dir + "/c.wav", odd, 0, 48000, &err));
  SND_CHECK(!WriteWav(dir + "/c.wav", odd, 2, 0, &err));
  SND_CHECK(!WriteWav(dir + "/no/such/dir/c.wav", odd, 2, 48000, &err));
  std::error_code ec;
  fs::remove_all(dir, ec);
}
