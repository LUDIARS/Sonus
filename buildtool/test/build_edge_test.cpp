// sonus-build edge cases (independent tester): generated header, ID
// collisions, references and cycles, transition rule warnings, determinism,
// differential cache, output atomicity, CLI. Tests named Defect_* fail on the
// current implementation on purpose (see the comment above each).
#include <algorithm>
#include <clocale>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mini_project.hpp"
#include "snd/buildtool/json.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"

#ifndef SND_TEST_CXX
#error "SND_TEST_CXX (C++ compiler) must be defined"
#endif
#ifndef SND_TEST_PUBLIC_INCLUDE
#error "SND_TEST_PUBLIC_INCLUDE must be defined"
#endif
#ifndef SND_BUILD_EXE
#error "SND_BUILD_EXE must be defined"
#endif

#define REQUIRE_MSG(cond, ...)       \
  do {                               \
    const bool ok_ = (cond);         \
    SND_CHECK_MSG(ok_, __VA_ARGS__); \
    if (!ok_) return;                \
  } while (0)

namespace fs = std::filesystem;
namespace bt = snd::buildtool;
using mini::HasDiag;
using mini::Project;
using snd::HashId;

namespace {

// Compiles `header` alone (with the public include dir) and returns the
// compiler's exit status; `log` gets its output.
int CompileHeader(const fs::path& header, std::string* log) {
  const fs::path src = header.parent_path() / "use_ids.cpp";
  mini::WriteBytes(src, [&] {
    const std::string t = "#include \"" + header.filename().string() + "\"\nint main() { return 0; }\n";
    return std::vector<std::uint8_t>(t.begin(), t.end());
  }());
  const fs::path out = header.parent_path() / "compile.log";
  const std::string cmd = std::string("\"") + SND_TEST_CXX + "\" -std=c++20 -fsyntax-only -Werror -I\"" +
                          SND_TEST_PUBLIC_INCLUDE + "\" -I\"" + header.parent_path().string() + "\" \"" +
                          src.string() + "\" > \"" + out.string() + "\" 2>&1";
  const int rc = std::system(cmd.c_str());
  *log = mini::ReadText(out);
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : 255;
}

int RunCli(const std::string& args, std::string* log) {
  const std::string logFile = (fs::temp_directory_path() / ("sonus-cli-" + std::to_string(::getpid()) + ".log")).string();
  const std::string cmd = std::string("\"") + SND_BUILD_EXE + "\" " + args + " > \"" + logFile + "\" 2>&1";
  const int rc = std::system(cmd.c_str());
  *log = mini::ReadText(logFile);
  std::error_code ec;
  fs::remove(logFile, ec);
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : 255;
}

std::vector<std::string> ListFiles(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return out;
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    out.push_back(fs::relative(it->path(), dir, ec).generic_string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Two different ASCII strings with the same FNV-1a 32 hash (birthday search).
std::pair<std::string, std::string> FindCollision(const std::string& prefix) {
  std::unordered_map<std::uint32_t, std::uint32_t> seen;
  for (std::uint32_t i = 0;; ++i) {
    const std::string s = prefix + std::to_string(i);
    auto [it, fresh] = seen.emplace(HashId(s), i);
    if (!fresh) return {prefix + std::to_string(it->second), s};
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Generated header
// ---------------------------------------------------------------------------
SND_TEST(BuildEdge, IdsHeaderCompilesAndMatchesHashId) {
  Project p("hdr-ok");
  p.Minimal();
  p.Add("events/A.json", "event", "SE/A", "\"bank\": \"Main\", \"aliases\": [\"SE/OldA\"]");
  p.Add("events/B.json", "event", "SE_A", "\"bank\": \"Main\"");  // same identifier as SE/A after sanitising
  p.Add("events/C.json", "event", "1up", "\"bank\": \"Main\"");
  p.Add("events/D.json", "event", "q\\\"uote\\\\back", "\"bank\": \"Main\"");
  p.Add("events/J.json", "event", "\xE7\x88\x86\xE7\x99\xBA", "\"bank\": \"Main\"");
  p.Add("buses/X.json", "bus", "SE/A", "\"parent\": \"Master\"");  // same name, other kind: allowed
  bt::Diagnostics d;
  REQUIRE_MSG(p.Build(&d), "%s", mini::Dump(d).c_str());
  std::string log;
  const int rc = CompileHeader(p.out() / "sonus_ids.h", &log);
  SND_CHECK_MSG(rc == 0, "generated header does not compile:\n%s", log.c_str());
  // ids.json agrees with snd::HashId for every entry.
  bt::json::Value ids;
  bt::json::ParseError pe;
  SND_REQUIRE(bt::json::Parse(mini::ReadText(p.out() / "ids.json"), &ids, &pe));
  const bt::json::Value* list = ids.Find("ids");
  SND_REQUIRE(list != nullptr);
  bool sawAlias = false;
  for (const bt::json::Value& e : list->items) {
    const std::string name = e.Find("name")->text;
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", HashId(name));
    SND_CHECK_MSG(e.Find("id")->text == hex, "%s: %s vs %s", name.c_str(), e.Find("id")->text.c_str(), hex);
    if (e.Find("aliasOf") != nullptr) {
      sawAlias = true;
      SND_CHECK(e.Find("aliasOf")->text == "SE/A");
    }
  }
  SND_CHECK(sawAlias);
}

// Defect: names are turned into C++ identifiers by replacing non-identifier
// characters, but C++ keywords are not escaped: an event called "new" (or
// "class", "delete", "default", "switch", ...) makes sonus_ids.h fail to
// compile. Code: build.cpp Sanitize() / WriteIdOutputs().
SND_TEST(BuildEdge, Defect_IdsHeaderWithKeywordNamesDoesNotCompile) {
  Project p("hdr-kw");
  p.Minimal();
  p.Add("events/K1.json", "event", "new", "\"bank\": \"Main\"");
  p.Add("events/K2.json", "event", "default", "\"bank\": \"Main\"");
  p.Add("parameters/K3.json", "parameter", "switch");
  bt::Diagnostics d;
  const bool ok = p.Build(&d);
  REQUIRE_MSG(ok, "%s", mini::Dump(d).c_str());
  std::string log;
  const int rc = CompileHeader(p.out() / "sonus_ids.h", &log);
  SND_CHECK_MSG(rc == 0, "sonus_ids.h with keyword names fails to compile:\n%.600s", log.c_str());
}

// Defect: a name containing U+0000 (legal JSON "\u0000") is accepted; its id
// is hashed over the whole string, but the header's static_assert literal and
// the NUL-terminated STRS entry stop at the NUL, so sonus_ids.h does not
// compile (and NAME maps the id to "A"). Names should reject control characters.
SND_TEST(BuildEdge, Defect_NameWithEmbeddedNulBreaksTheHeader) {
  Project p("hdr-nul");
  p.Minimal();
  p.Add("events/N.json", "event", "A\\u0000B", "\"bank\": \"Main\"");
  bt::Diagnostics d;
  const bool ok = p.Build(&d);
  std::string log;
  const int rc = ok ? CompileHeader(p.out() / "sonus_ids.h", &log) : -1;
  snd::test::Note("name \"A\\u0000B\": build %s, header compile rc=%d %s", ok ? "accepted" : "rejected", rc,
                  rc > 0 ? "(static_assert: HashId(\"A\\u0000B\") stops at the NUL)" : "");
  SND_CHECK_MSG(!ok || rc == 0, "a name the build accepted produced a header that does not compile");
}

// ---------------------------------------------------------------------------
// IDs (10.1)
// ---------------------------------------------------------------------------
SND_TEST(BuildEdge, RealCollisionsAcrossKindsAndNameSpaces) {
  const auto [a, b] = FindCollision("Col/");
  SND_REQUIRE(a != b && HashId(a) == HashId(b));
  snd::test::Note("collision found: '%s' / '%s' -> 0x%08X", a.c_str(), b.c_str(), HashId(a));
  // Each case: two places that both register a runtime id.
  const std::string ev = "\"bank\": \"Main\"";
  std::vector<std::pair<std::string, std::function<void(Project&)>>> cases = {
      {"event vs event", [&](Project& p) {
         p.Add("events/X1.json", "event", a, ev);
         p.Add("events/X2.json", "event", b, ev);
       }},
      {"event vs parameter", [&](Project& p) {
         p.Add("events/X1.json", "event", a, ev);
         p.Add("parameters/X2.json", "parameter", b);
       }},
      {"name vs alias", [&](Project& p) {
         p.Add("events/X1.json", "event", a, ev);
         p.Add("events/X2.json", "event", "Other", ev + ", \"aliases\": [\"" + b + "\"]");
       }},
      {"music state vs marker", [&](Project& p) {
         p.Wav("audio/s.wav", mini::Tone(2, 48000, 4800));
         p.Add("assets/s.json", "asset", "s", "\"file\": \"audio/s.wav\"");
         p.Add("music/seg.json", "segment", "Seg", "\"asset\": \"s\", \"bars\": 1, \"markers\": [{\"name\": \"" + b +
                                                      "\", \"frame\": 0}]");
         p.Add("music/pl.json", "playlist", "PL", "\"entries\": [{\"segment\": \"Seg\"}]");
         p.Add("events/M.json", "event", "Music", ev + ", \"music\": {\"initialState\": \"" + a +
                                                      "\", \"states\": [{\"name\": \"" + a + "\", \"playlist\": \"PL\"}]}");
       }},
      {"switch value vs effect type", [&](Project& p) {
         p.Add("events/X1.json", "event", "Sw", ev + ", \"tracks\": [{\"clips\": [{\"asset\": \"a\", \"switchValue\": \"" + a + "\"}]}]");
         p.Add("buses/X2.json", "bus", "Fx", "\"parent\": \"Master\", \"effects\": [{\"type\": \"" + b + "\"}]");
       }},
      {"language vs bank name", [&](Project& p) {
         p.Add("project.json", "project", "P", "\"languages\": [\"" + a + "\"]");
         p.Add("banks/X.json", "bank", b);
       }},
  };
  for (auto& [what, add] : cases) {
    Project p("collide");
    p.Minimal();
    add(p);
    bt::Diagnostics d;
    const bool ok = p.Build(&d);
    SND_CHECK_MSG(!ok && HasDiag(d, bt::Severity::Error, "id collision"), "%s: not reported\n%s", what.c_str(),
                  mini::Dump(d).c_str());
    SND_CHECK_MSG(!fs::exists(p.out()), "%s: output written despite the error", what.c_str());
  }
  // Same string used by different kinds is not a collision.
  Project p("collide-ok");
  p.Minimal();
  p.Add("parameters/E.json", "parameter", "E");
  bt::Diagnostics d;
  SND_CHECK_MSG(p.Build(&d), "%s", mini::Dump(d).c_str());
}

// ---------------------------------------------------------------------------
// References and graph structure
// ---------------------------------------------------------------------------
SND_TEST(BuildEdge, ReferencesCyclesAndBusGraph) {
  struct Case {
    const char* what;
    std::function<void(Project&)> add;
    const char* needle;
  };
  const std::string ev = "\"bank\": \"Main\"";
  std::vector<Case> cases = {
      {"self nested", [&](Project& p) {
         p.Add("events/S.json", "event", "S", ev + ", \"tracks\": [{\"clips\": [{\"event\": \"S\"}]}]");
       }, "nested event cycle"},
      {"3-step nested", [&](Project& p) {
         p.Add("events/X.json", "event", "X", ev + ", \"tracks\": [{\"clips\": [{\"event\": \"Y\"}]}]");
         p.Add("events/Y.json", "event", "Y", ev + ", \"tracks\": [{\"clips\": [{\"event\": \"Z\"}]}]");
         p.Add("events/Z.json", "event", "Z", ev + ", \"tracks\": [{\"clips\": [{\"event\": \"X\"}]}]");
       }, "nested event cycle"},
      {"parent cycle", [&](Project& p) {
         p.Add("buses/B1.json", "bus", "B1", "\"parent\": \"B2\"");
         p.Add("buses/B2.json", "bus", "B2", "\"parent\": \"B1\"");
         p.Add("events/U.json", "event", "U", ev + ", \"bus\": \"B1\"");
       }, "cycle"},
      {"send cycle", [&](Project& p) {
         p.Add("buses/B1.json", "bus", "B1", "\"parent\": \"Master\", \"sends\": [{\"bus\": \"B2\"}]");
         p.Add("buses/B2.json", "bus", "B2", "\"parent\": \"Master\", \"sends\": [{\"bus\": \"B1\"}]");
         p.Add("events/U.json", "event", "U", ev + ", \"bus\": \"B1\"");
       }, "cycle"},
      {"self send", [&](Project& p) {
         p.Add("buses/B1.json", "bus", "B1", "\"parent\": \"Master\", \"sends\": [{\"bus\": \"B1\"}]");
         p.Add("events/U.json", "event", "U", ev + ", \"bus\": \"B1\"");
       }, "cycle"},
      {"two masters", [&](Project& p) {
         p.Add("buses/M2.json", "bus", "Master2", "\"master\": true");
       }, "master"},
      {"wrong kind by guid", [&](Project& p) {
         const std::string g = p.Add("parameters/P.json", "parameter", "P");
         p.Add("events/U.json", "event", "U", ev + ", \"bus\": \"" + g + "\"");
       }, "is a parameter, expected a bus"},
      {"wrong kind by name", [&](Project& p) {
         p.Add("events/U.json", "event", "U", ev + ", \"bus\": \"E\"");
       }, "unknown bus 'E'"},
      {"bank dependency cycle", [&](Project& p) {
         p.Add("banks/B1.json", "bank", "B1", "\"dependsOn\": [\"B2\"]");
         p.Add("banks/B2.json", "bank", "B2", "\"dependsOn\": [\"B1\"]");
       }, "bank dependency cycle"},
      {"bank self dependency", [&](Project& p) {
         p.Add("banks/B1.json", "bank", "B1", "\"dependsOn\": [\"B1\"]");
       }, "itself"},
      {"stinger in another bank", [&](Project& p) {
         p.Add("banks/Other.json", "bank", "Other", "\"dependsOn\": [\"Main\"]");
         p.Add("events/St.json", "event", "St", ev + ", \"stinger\": true");
         p.Add("events/M.json", "event", "M", "\"bank\": \"Other\", \"music\": {\"stingers\": [{\"name\": \"S1\", \"event\": \"St\"}]}");
       }, ""},
      {"nested event in another bank", [&](Project& p) {
         p.Add("banks/Other.json", "bank", "Other", "\"dependsOn\": [\"Main\"]");
         p.Add("events/N.json", "event", "N", "\"bank\": \"Other\", \"tracks\": [{\"clips\": [{\"event\": \"E\"}]}]");
       }, ""},
      {"duplicate name same kind", [&](Project& p) {
         p.Add("events/E2.json", "event", "E", ev);
       }, "duplicate event name"},
      {"bad guid", [&](Project& p) {
         p.Raw("events/G.json", "{\"type\": \"event\", \"guid\": \"7e570000-0000-4000-8000-00000000000g\", \"name\": \"G\"}");
       }, "guid"},
      {"unknown type", [&](Project& p) {
         p.Add("events/T.json", "evnt", "T");
       }, "unknown \"type\""},
      {"missing wav", [&](Project& p) {
         p.Add("assets/m.json", "asset", "m", "\"file\": \"audio/missing.wav\"");
         p.Add("events/U.json", "event", "U", ev + ", \"tracks\": [{\"clips\": [{\"asset\": \"m\"}]}]");
       }, "cannot read"},
      {"clip with two sources", [&](Project& p) {
         p.Add("events/U.json", "event", "U", ev + ", \"tracks\": [{\"clips\": [{\"asset\": \"a\", \"event\": \"E\"}]}]");
       }, ""},
      {"patch synth before stage 6", [&](Project& p) {
         p.Add("events/U.json", "event", "U", ev + ", \"tracks\": [{\"clips\": [{\"synth\": {\"kind\": \"patch\"}}]}]");
       }, ""},
      {"curve points not ascending", [&](Project& p) {
         p.Add("events/U.json", "event", "U", ev + ", \"distanceCurves\": {\"volume\": {\"points\": [[10, 0], [0, -6]]}}");
       }, ""},
      {"priority out of range", [&](Project& p) {
         p.Add("events/U.json", "event", "U", ev + ", \"priority\": 256");
       }, ""},
  };
  for (Case& c : cases) {
    Project p("refs");
    p.Minimal();
    c.add(p);
    bt::Diagnostics d;
    const bool ok = p.Build(&d);
    const bool found = c.needle[0] == '\0' || HasDiag(d, bt::Severity::Error, c.needle);
    SND_CHECK_MSG(!ok && found, "%s: build %s, expected error '%s'\n%s", c.what, ok ? "succeeded" : "failed", c.needle,
                  mini::Dump(d).c_str());
    SND_CHECK_MSG(ok || !fs::exists(p.out()), "%s: files written despite errors", c.what);
  }
}

// A non-master bus without a parent: a second root of the bus graph (4.6
// "multiple roots"). Reported: does the build accept a second, unrouted root?
SND_TEST(BuildEdge, BusWithoutParentThatIsNotTheMaster) {
  Project p("orphan-bus");
  p.Minimal();
  p.Add("buses/Orphan.json", "bus", "Orphan");
  p.Add("events/U.json", "event", "U", "\"bank\": \"Main\", \"bus\": \"Orphan\"");
  bt::Diagnostics d;
  const bool ok = p.Build(&d);
  snd::test::Note("bus without parent and without \"master\": build %s%s", ok ? "accepted" : "rejected: ",
                  ok ? "" : mini::Dump(d).c_str());
}

// ---------------------------------------------------------------------------
// Transition rule table (8.5): rows that can never match are warned about.
// ---------------------------------------------------------------------------
SND_TEST(BuildEdge, UnreachableTransitionRows) {
  auto build = [](const std::string& transitions, bt::Diagnostics* d) {
    Project p("mtrn");
    p.Minimal();
    p.Wav("audio/s.wav", mini::Tone(2, 48000, 4800));
    p.Add("assets/s.json", "asset", "s", "\"file\": \"audio/s.wav\"");
    p.Add("music/seg.json", "segment", "Seg", "\"asset\": \"s\", \"bars\": 1");
    p.Add("music/pl.json", "playlist", "PL", "\"entries\": [{\"segment\": \"Seg\"}]");
    p.Add("events/M.json", "event", "Music",
          "\"bank\": \"Main\", \"music\": {\"initialState\": \"A\", \"states\": [{\"name\": \"A\", \"playlist\": \"PL\"},"
          " {\"name\": \"B\", \"playlist\": \"PL\"}], \"transitions\": [" + transitions + "]}");
    return p.Build(d);
  };
  struct Case {
    const char* rows;
    bool expectWarning;
  } cases[] = {
      {R"({"from": "A", "to": "B"}, {"from": "*", "to": "*"})", false},
      {R"({"from": "*", "to": "*"}, {"from": "A", "to": "B"})", true},
      {R"({"from": "A", "to": "*"}, {"from": "A", "to": "B"})", true},
      {R"({"from": "*", "to": "B"}, {"from": "A", "to": "B"})", true},
      {R"({"from": "A", "to": "B"}, {"from": "A", "to": "B"})", true},
      {R"({"from": "A", "to": "B"}, {"from": "B", "to": "A"})", false},
      {R"({"from": "A", "to": "*"}, {"from": "*", "to": "B"})", false},
  };
  for (const Case& c : cases) {
    bt::Diagnostics d;
    const bool ok = build(c.rows, &d);
    SND_CHECK_MSG(ok, "%s\n%s", c.rows, mini::Dump(d).c_str());
    bool warned = false;
    for (const bt::Diagnostic& x : d.items()) {
      warned = warned || (x.severity == bt::Severity::Warning &&
                          (x.message.find("match") != std::string::npos || x.message.find("reach") != std::string::npos));
    }
    SND_CHECK_MSG(warned == c.expectWarning, "%s: warning %s\n%s", c.rows, warned ? "given" : "missing",
                  mini::Dump(d).c_str());
  }
  // Unknown state name in a rule: an error (otherwise the row silently never matches).
  bt::Diagnostics d;
  const bool ok = build(R"({"from": "A", "to": "Battel"})", &d);
  snd::test::Note("transition to an undeclared state 'Battel': %s", ok ? "accepted silently" : "error");
}

// ---------------------------------------------------------------------------
// Unknown members (project-format.md §1: unknown members are warned about so
// that typos do not silently fall back to defaults).
// Defect: only top-level members are checked; a typo inside a nested object
// (sends[], effects[], tracks[].clips[], clip.random, cone, music.transitions[],
// segment.tempo[], ...) is ignored without a diagnostic and the default is used.
// Code: project.cpp Project::Load checks o.doc.members only.
// ---------------------------------------------------------------------------
SND_TEST(BuildEdge, Defect_TyposInNestedObjectsAreSilentlyIgnored) {
  struct Case {
    const char* what;
    std::function<void(Project&)> add;
  };
  const std::string ev = "\"bank\": \"Main\"";
  std::vector<Case> cases = {
      {"sends[].levelDB", [&](Project& p) {
         p.Add("buses/S.json", "bus", "S", "\"parent\": \"Master\", \"sends\": [{\"bus\": \"Master\", \"levelDB\": -6}]");
         p.Add("events/U.json", "event", "U", ev + ", \"bus\": \"S\"");
       }},
      {"clip.volumedb", [&](Project& p) {
         p.Add("events/U.json", "event", "U", ev + ", \"tracks\": [{\"clips\": [{\"asset\": \"a\", \"volumedb\": -12}]}]");
       }},
      {"clip.random.pitch", [&](Project& p) {
         p.Add("events/U.json", "event", "U", ev + ", \"tracks\": [{\"clips\": [{\"asset\": \"a\", \"random\": {\"pitch\": 100}}]}]");
       }},
      {"cone.innerDegrees", [&](Project& p) {
         p.Add("events/U.json", "event", "U", ev + ", \"cone\": {\"innerDegrees\": 30}");
       }},
      {"tracks[].clip (singular)", [&](Project& p) {
         p.Add("events/U.json", "event", "U", ev + ", \"tracks\": [{\"clip\": [{\"asset\": \"a\"}]}]");
       }},
      {"asset.convert.default.samplerate", [&](Project& p) {
         p.Add("assets/b.json", "asset", "b", "\"file\": \"audio/a.wav\", \"convert\": {\"default\": {\"samplerate\": 24000}}");
         p.Add("events/U.json", "event", "U", ev + ", \"tracks\": [{\"clips\": [{\"asset\": \"b\"}]}]");
       }},
      {"asset.convert.LINUX", [&](Project& p) {
         p.Add("assets/b.json", "asset", "b", "\"file\": \"audio/a.wav\", \"convert\": {\"LINUX\": {\"sampleRate\": 24000}}");
         p.Add("events/U.json", "event", "U", ev + ", \"tracks\": [{\"clips\": [{\"asset\": \"b\"}]}]");
       }},
      {"project.platforms.defaults", [&](Project& p) {
         p.Add("project.json", "project", "P", "\"platforms\": {\"defaults\": {\"codec\": \"PCMF\"}}");
       }},
  };
  int silent = 0;
  std::string list;
  for (Case& c : cases) {
    Project p("typo");
    p.Minimal();
    c.add(p);
    bt::Diagnostics d;
    (void)p.Build(&d);
    if (d.warnings() == 0 && d.errors() == 0) {
      ++silent;
      list += std::string("  ") + c.what + "\n";
    }
  }
  SND_CHECK_MSG(silent == 0, "%d typo(s) in nested objects produced no diagnostic:\n%s", silent, list.c_str());
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------
namespace {
void SampleLike(Project& p, bool reverse) {
  // Same objects, created in opposite orders (file system enumeration order
  // and mtimes differ); the project directory path differs too.
  std::vector<std::function<void()>> steps = {
      [&] { p.Add("banks/Main.json", "bank", "Main"); },
      [&] { p.Add("buses/Master.json", "bus", "Master", "\"master\": true"); },
      [&] { p.Add("buses/Z.json", "bus", "Z", "\"parent\": \"Master\", \"sends\": [{\"bus\": \"Y\"}]"); },
      [&] { p.Add("buses/Y.json", "bus", "Y", "\"parent\": \"Master\""); },
      [&] { p.Wav("audio/a.wav", mini::Tone(2, 44100, 3000)); },
      [&] { p.Add("assets/a.json", "asset", "a", "\"file\": \"audio/a.wav\", \"loadForm\": \"stream\""); },
      [&] { p.Add("events/E.json", "event", "E", "\"bank\": \"Main\", \"bus\": \"Z\", \"tracks\": [{\"clips\": [{\"asset\": \"a\"}]}]"); },
      [&] { p.Add("events/F.json", "event", "F", "\"bank\": \"Main\", \"bus\": \"Y\""); },
  };
  if (reverse) std::reverse(steps.begin(), steps.end());
  for (auto& s : steps) s();
}
}  // namespace

SND_TEST(BuildEdge, SameBytesRegardlessOfCreationOrderAndPath) {
  // GUIDs are assigned in creation order by Project::Add, so write both with
  // the same GUID sequence: build the forward project, then a reverse one
  // whose files carry the same content (copy) but are created in reverse.
  Project a("det-a");
  SampleLike(a, false);
  Project b("det-b-longer-path");
  std::vector<fs::path> files;
  for (fs::recursive_directory_iterator it(a.dir()), end; it != end; ++it) {
    if (it->is_regular_file()) files.push_back(fs::relative(it->path(), a.dir()));
  }
  std::sort(files.rbegin(), files.rend());
  for (const fs::path& f : files) mini::WriteBytes(b.dir() / f, mini::ReadBytes(a.dir() / f));
  bt::Diagnostics d1, d2;
  REQUIRE_MSG(a.Build(&d1), "%s", mini::Dump(d1).c_str());
  REQUIRE_MSG(b.Build(&d2), "%s", mini::Dump(d2).c_str());
  const std::vector<std::string> la = ListFiles(a.out()), lb = ListFiles(b.out());
  SND_CHECK(la == lb);
  for (const std::string& f : la) {
    if (fs::is_directory(a.out() / f)) continue;
    SND_CHECK_MSG(mini::ReadBytes(a.out() / f) == mini::ReadBytes(b.out() / f), "%s differs", f.c_str());
  }
}

// Defect (follows from JsonEdge.Defect_AsDoubleDependsOnTheLocale): under a
// comma-decimal LC_NUMERIC the same project no longer builds (or builds
// differently): every fractional number is rejected as "must be a number".
SND_TEST(BuildEdge, Defect_BuildDependsOnTheLocale) {
  Project p("locale");
  p.Minimal();
  p.Add("buses/S.json", "bus", "S", "\"parent\": \"Master\", \"volumeDb\": -3.5");
  bt::Diagnostics d1;
  REQUIRE_MSG(p.Build(&d1), "%s", mini::Dump(d1).c_str());
  const std::vector<std::uint8_t> ref = mini::ReadBytes(p.out() / "Main.sndb");
  bool haveLocale = std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr;
  if (!haveLocale) {
    const std::string dir = "/tmp/sonus-test-locales-" + std::to_string(::getpid());
    if (std::system(("mkdir -p " + dir + " && localedef -i de_DE -f UTF-8 " + dir + "/de_DE.UTF-8 >/dev/null 2>&1").c_str()) == 0) {
      ::setenv("LOCPATH", dir.c_str(), 1);
      haveLocale = std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr;
    }
  }
  if (!haveLocale) {
    snd::test::Note("de_DE locale not available; skipped");
    return;
  }
  std::error_code ec;
  fs::remove_all(p.out(), ec);
  bt::Diagnostics d2;
  const bool ok = p.Build(&d2);
  const std::vector<std::uint8_t> got = ok ? mini::ReadBytes(p.out() / "Main.sndb") : std::vector<std::uint8_t>{};
  std::setlocale(LC_NUMERIC, "C");
  SND_CHECK_MSG(ok && got == ref, "under de_DE: build %s\n%s", ok ? "differs" : "failed", mini::Dump(d2).c_str());
}

// ---------------------------------------------------------------------------
// Differential build cache
// ---------------------------------------------------------------------------
SND_TEST(BuildEdge, CacheInvalidation) {
  Project p("cache");
  p.Minimal();
  bt::BuildOptions o = p.Options();
  o.cacheDir = (p.root() / "cache").string();
  bt::Diagnostics d;
  bt::BuildResult r;
  REQUIRE_MSG(p.Build(o, &d, &r), "%s", mini::Dump(d).c_str());
  SND_CHECK_EQ(r.assetsEncoded, 1u);
  SND_REQUIRE(p.Build(o, &d, &r));
  SND_CHECK_EQ(r.assetsEncoded, 0u);
  SND_CHECK_EQ(r.assetsFromCache, 1u);
  // Each change invalidates the entry.
  struct Change {
    const char* what;
    std::function<void()> apply;
  } changes[] = {
      {"wav bytes", [&] { p.Wav("audio/a.wav", mini::Tone(1, 48000, 480, 441)); }},
      {"codec", [&] { p.Raw("assets/a.json", "{\"type\": \"asset\", \"guid\": \"" + Project::Guid(3) +
                                                "\", \"name\": \"a\", \"file\": \"audio/a.wav\", \"convert\": {\"default\": {\"codec\": \"PCMF\"}}}"); }},
      {"sample rate", [&] { p.Raw("assets/a.json", "{\"type\": \"asset\", \"guid\": \"" + Project::Guid(3) +
                                                      "\", \"name\": \"a\", \"file\": \"audio/a.wav\", \"convert\": {\"default\": {\"codec\": \"PCMF\", \"sampleRate\": 24000}}}"); }},
      {"loop", [&] { p.Raw("assets/a.json", "{\"type\": \"asset\", \"guid\": \"" + Project::Guid(3) +
                                               "\", \"name\": \"a\", \"file\": \"audio/a.wav\", \"loop\": {\"start\": 10, \"end\": 400}, \"convert\": {\"default\": {\"codec\": \"PCMF\", \"sampleRate\": 24000}}}"); }},
      {"project platform default", [&] { p.Add("project.json", "project", "P", "\"platforms\": {\"default\": {\"channels\": 2}}"); }},
  };
  for (Change& c : changes) {
    c.apply();
    REQUIRE_MSG(p.Build(o, &d, &r), "%s: %s", c.what, mini::Dump(d).c_str());
    SND_CHECK_MSG(r.assetsEncoded == 1u, "%s: not re-encoded (encoded %u, cached %u)", c.what, r.assetsEncoded,
                  r.assetsFromCache);
  }
  // Encoder version.
  bt::EncoderRegistry reg;
  const bt::Encoder* builtin = reg.Find(snd::Fourcc('P', 'C', 'M', 'F'));
  SND_REQUIRE(builtin != nullptr);
  bt::Encoder newer = *builtin;
  newer.version = builtin->version + 1;
  reg.Register(newer);
  o.encoders = &reg;
  SND_REQUIRE(p.Build(o, &d, &r));
  SND_CHECK_MSG(r.assetsEncoded == 1u, "encoder version change not re-encoded");
}

// The cache stores encoded samples without any integrity check: flipping a
// byte of a cache file's sample data changes the built bank silently.
// Defect (task property "キャッシュが壊れているときに気づくか"): convert.cpp
// LoadCached validates magic/version/key and the structure only.
SND_TEST(BuildEdge, Defect_CorruptCacheEntryIsNotDetected) {
  Project p("cache-corrupt");
  p.Minimal();
  bt::BuildOptions o = p.Options();
  o.cacheDir = (p.root() / "cache").string();
  bt::Diagnostics d;
  bt::BuildResult r;
  REQUIRE_MSG(p.Build(o, &d, &r), "%s", mini::Dump(d).c_str());
  const std::vector<std::uint8_t> clean = mini::ReadBytes(p.out() / "Main.sndb");
  std::vector<fs::path> entries;
  for (fs::recursive_directory_iterator it(o.cacheDir), end; it != end; ++it) {
    if (it->is_regular_file()) entries.push_back(it->path());
  }
  SND_REQUIRE_EQ(entries.size(), 1u);
  std::vector<std::uint8_t> c = mini::ReadBytes(entries[0]);
  SND_REQUIRE(c.size() > 400);
  c[c.size() / 2] ^= 0x40;  // inside the encoded samples
  mini::WriteBytes(entries[0], c);
  std::error_code ec;
  fs::remove_all(p.out(), ec);
  SND_REQUIRE(p.Build(o, &d, &r));
  const std::vector<std::uint8_t> again = mini::ReadBytes(p.out() / "Main.sndb");
  SND_CHECK_MSG(again == clean || d.warnings() + d.errors() > 0,
                "corrupt cache entry used silently (cached %u, encoded %u): bank differs from a clean build",
                r.assetsFromCache, r.assetsEncoded);
  // Truncated entry: must fall back to encoding.
  c.resize(c.size() / 3);
  mini::WriteBytes(entries[0], c);
  fs::remove_all(p.out(), ec);
  SND_REQUIRE(p.Build(o, &d, &r));
  SND_CHECK_EQ(r.assetsEncoded, 1u);
  SND_CHECK(mini::ReadBytes(p.out() / "Main.sndb") == clean);
}

// ---------------------------------------------------------------------------
// Output: nothing written on error, nothing half-written
// ---------------------------------------------------------------------------
SND_TEST(BuildEdge, ErrorsWriteNothingAndKeepOldOutputs) {
  Project p("noerr");
  p.Minimal();
  bt::Diagnostics d;
  SND_REQUIRE(p.Build(&d));
  const std::vector<std::string> before = ListFiles(p.out());
  const std::vector<std::uint8_t> bank = mini::ReadBytes(p.out() / "Main.sndb");
  p.Add("events/Bad.json", "event", "Bad", "\"bank\": \"Main\", \"bus\": \"Nope\"");
  p.Add("events/F.json", "event", "F", "\"bank\": \"Main\", \"priority\": 7");  // would change Main.sndb
  bt::Diagnostics d2;
  SND_CHECK(!p.Build(&d2));
  SND_CHECK(ListFiles(p.out()) == before);
  SND_CHECK(mini::ReadBytes(p.out() / "Main.sndb") == bank);
}

// Defect: an output path that exists as a directory (here <out>/ids.json)
// makes sonus-build abort: Commit() reads the old file with an ifstream +
// istreambuf_iterator, which throws std::ios_base::failure ("Is a
// directory") through -fno-exceptions code -> std::terminate. Banks committed
// before it stay replaced (partial output), exit status is SIGABRT, not 1.
// Code: build.cpp ReadBinary() / Builder::Commit().
SND_TEST(BuildEdge, Defect_OutputPathIsADirectoryAbortsWithPartialOutput) {
  Project p("partial");
  p.Minimal();
  bt::Diagnostics d;
  REQUIRE_MSG(p.Build(&d), "%s", mini::Dump(d).c_str());
  const std::vector<std::uint8_t> oldBank = mini::ReadBytes(p.out() / "Main.sndb");
  std::error_code ec;
  fs::remove(p.out() / "ids.json", ec);
  fs::create_directories(p.out() / "ids.json" / "blocker", ec);
  p.Add("events/F.json", "event", "F", "\"bank\": \"Main\", \"priority\": 7");
  std::string log;
  const int rc = RunCli("--project \"" + p.dir().string() + "\" --out \"" + p.out().string() + "\"", &log);
  SND_CHECK_MSG(rc == 1, "exit status %d (expected 1)\n%.400s", rc, log.c_str());
  SND_CHECK_MSG(mini::ReadBytes(p.out() / "Main.sndb") == oldBank,
                "Main.sndb was replaced although the build failed while writing");
  bool tmp = false;
  for (const std::string& f : ListFiles(p.out())) tmp = tmp || f.find(".tmp") != std::string::npos;
  SND_CHECK_MSG(!tmp, "a .tmp file was left in the output directory");
}

// Defect: the same crash for inputs: an asset "file" (or effect "blob") that
// names a directory aborts sonus-build instead of reporting
// "cannot read". Code: build.cpp ReadBinary().
SND_TEST(BuildEdge, Defect_InputPathIsADirectoryAborts) {
  Project p("indir");
  p.Minimal();
  std::error_code ec;
  fs::create_directories(p.dir() / "audio" / "dir.wav", ec);
  p.Add("assets/d.json", "asset", "d", "\"file\": \"audio/dir.wav\"");
  p.Add("events/U.json", "event", "U", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"d\"}]}]");
  std::string log;
  const int rc = RunCli("--project \"" + p.dir().string() + "\" --out \"" + p.out().string() + "\"", &log);
  SND_CHECK_MSG(rc == 1, "exit status %d (expected 1)\n%.400s", rc, log.c_str());
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
SND_TEST(BuildEdge, CliExitCodesAndMessages) {
  Project p("cli");
  p.Minimal();
  const std::string proj = "--project \"" + p.dir().string() + "\"";
  const std::string out = "--out \"" + p.out().string() + "\"";
  std::string log;
  SND_CHECK_EQ(RunCli(proj + " " + out, &log), 0);
  SND_CHECK_EQ(RunCli("--help", &log), 0);
  SND_CHECK_EQ(RunCli("", &log), 2);
  SND_CHECK_EQ(RunCli(proj, &log), 2);
  SND_CHECK_EQ(RunCli(out, &log), 2);
  SND_CHECK_EQ(RunCli(proj + " " + out + " --bogus", &log), 2);
  SND_CHECK(log.find("unknown option") != std::string::npos);
  SND_CHECK_EQ(RunCli(proj + " " + out + " --platform", &log), 2);
  SND_CHECK_EQ(RunCli(proj + " " + out + " --platform XBOX", &log), 2);
  SND_CHECK(log.find("unknown platform") != std::string::npos);
  SND_CHECK_EQ(RunCli("--project /nonexistent/sonus/project " + out, &log), 1);
  SND_CHECK(log.find("project directory not found") != std::string::npos);
  // Output path is a regular file: cannot write.
  const fs::path file = p.root() / "afile";
  mini::WriteBytes(file, {1});
  SND_CHECK_EQ(RunCli(proj + " --out \"" + file.string() + "\"", &log), 1);
  SND_CHECK(log.find("error") != std::string::npos);
  // Read-only output directory (skip when running as root: permissions do not apply).
  if (::geteuid() != 0) {
    const fs::path ro = p.root() / "ro";
    fs::create_directories(ro);
    ::chmod(ro.c_str(), 0555);
    SND_CHECK_EQ(RunCli(proj + " --out \"" + ro.string() + "\"", &log), 1);
    ::chmod(ro.c_str(), 0755);
  }
  // Same option twice: the last wins (report).
  const int rc = RunCli(proj + " " + out + " --platform LNX_ --platform WIN_ --quiet", &log);
  snd::test::Note("repeated --platform: exit %d", rc);
  // A build error: exit 1 and the diagnostic format file:line:col: error:
  p.Raw("events/Bad.json", "{\n  \"type\": \"event\",\n  \"guid\": \"7e570000-0000-4000-8000-0000000000ff\",\n"
                           "  \"name\": \"Bad\",\n  \"bank\": \"Main\",\n  \"bus\": \"Nope\"\n}\n");
  SND_CHECK_EQ(RunCli(proj + " " + out, &log), 1);
  SND_CHECK_MSG(log.find("events/Bad.json:6:10: error: unknown bus 'Nope'") != std::string::npos, "%s", log.c_str());
  SND_CHECK(log.find("nothing written") != std::string::npos);
}
