// sonus-replay: replays recordings without a UI (design.md 11.3; group P).
//
//   sonus-replay <bug-report-dir>
//       replays the report twice and against its expectation; prints the comparison.
//   sonus-replay --recording <file.snrec> --banks <dir> [--expect <file>] [--write-expect <file>]
//                [--wav <out.wav>] [--twice] [--report <dir> [--title <text>]]
//       replays a recording with the banks of <dir> (*.sndb); --expect checks the decision log
//       and digest, --write-expect writes them, --wav writes the output, --twice compares two
//       replays, --report writes a bug report folder.
// Exit status: 0 ok, 1 mismatch, 2 usage / input error.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "snd/tool/preview/replay_host.hpp"

namespace {

int Usage() {
  std::fprintf(stderr,
               "usage: sonus-replay <bug-report-dir>\n"
               "       sonus-replay --recording <file> --banks <dir> [--expect <file>] [--write-expect <file>]\n"
               "                    [--wav <out.wav>] [--twice] [--report <dir> [--title <text>]]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace snd::tool;
  using namespace snd::tool::preview;
  std::string recording, banks, expect, writeExpect, wav, report, title = "sonus-replay";
  std::string reportIn;
  bool twice = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string* v) {
      if (i + 1 >= argc) return false;
      *v = argv[++i];
      return true;
    };
    if (a == "--recording") {
      if (!next(&recording)) return Usage();
    } else if (a == "--banks") {
      if (!next(&banks)) return Usage();
    } else if (a == "--expect") {
      if (!next(&expect)) return Usage();
    } else if (a == "--write-expect") {
      if (!next(&writeExpect)) return Usage();
    } else if (a == "--wav") {
      if (!next(&wav)) return Usage();
    } else if (a == "--report") {
      if (!next(&report)) return Usage();
    } else if (a == "--title") {
      if (!next(&title)) return Usage();
    } else if (a == "--twice") {
      twice = true;
    } else if (!a.empty() && a[0] != '-' && reportIn.empty()) {
      reportIn = a;
    } else {
      return Usage();
    }
  }
  std::string err;
  if (!reportIn.empty()) {
    BugReport rep;
    if (!ReadBugReport(reportIn, &rep, &err)) {
      std::fprintf(stderr, "sonus-replay: %s\n", err.c_str());
      return 2;
    }
    ExpectationResult e;
    const ReplayComparison c = ReplayBugReport(rep, &e);
    std::printf("%s", c.Text().c_str());
    for (const std::string& m : e.messages) std::printf("expectation: %s\n", m.c_str());
    return c.ok && e.ok ? 0 : 1;
  }
  if (recording.empty() || banks.empty()) return Usage();
  Recording rec;
  if (!LoadRecording(recording, &rec, &err)) {
    std::fprintf(stderr, "sonus-replay: %s\n", err.c_str());
    return 2;
  }
  std::vector<ReportBank> bankFiles;
  if (!ReadBankDirectory(banks, &bankFiles, &err)) {
    std::fprintf(stderr, "sonus-replay: %s\n", err.c_str());
    return 2;
  }
  ReplayOptions o;
  o.keepOutput = !wav.empty();
  ReplayRun run;
  bool ok = Replay(rec, bankFiles, o, &run);
  if (!ok) {
    std::fprintf(stderr, "sonus-replay: %s\n", run.error.c_str());
    return 2;
  }
  std::printf("%s", DescribeRecording(rec).Text().c_str());
  std::printf("replay: %llu blocks, %zu decisions, digest %016llx, bit identity %s\n",
              static_cast<unsigned long long>(run.blocks), run.decisions.size(),
              static_cast<unsigned long long>(run.digest), run.bitExactExpected ? "expected" : "not expected");
  if (twice) {
    const ReplayComparison c = ReplayTwice(rec, bankFiles, o);
    std::printf("%s", c.Text().c_str());
    ok = ok && c.ok;
  }
  if (!expect.empty()) {
    ReplayExpectation e;
    if (!LoadExpectation(expect, &e, &err)) {
      std::fprintf(stderr, "sonus-replay: %s\n", err.c_str());
      return 2;
    }
    const ExpectationResult r = CheckExpectation(run, e);
    for (const std::string& m : r.messages) std::printf("expectation: %s\n", m.c_str());
    std::printf("expectation: %s\n", r.ok ? "OK" : "MISMATCH");
    ok = ok && r.ok;
  }
  if (!writeExpect.empty() && !SaveExpectation(writeExpect, ExpectationOf(run, run.bitExactExpected), &err)) {
    std::fprintf(stderr, "sonus-replay: %s\n", err.c_str());
    return 2;
  }
  if (!wav.empty() && !WriteWav(wav, run.output, run.channels, run.sampleRate, &err)) {
    std::fprintf(stderr, "sonus-replay: %s\n", err.c_str());
    return 2;
  }
  if (!report.empty()) {
    const BugReport rep = MakeBugReport(rec, bankFiles, nullptr, title, std::string(), true);
    if (!WriteBugReport(report, rep, &err)) {
      std::fprintf(stderr, "sonus-replay: %s\n", err.c_str());
      return 2;
    }
  }
  return ok ? 0 : 1;
}
