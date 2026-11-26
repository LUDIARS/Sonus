// The build tool's in-memory project (ProjectSource) and in-memory output, and patches
// (patch.hpp): checks, dependency order, the provisional XPCH chunk.
#include <cstring>
#include <filesystem>
#include <map>

#include "mini_project.hpp"
#include "snd/buildtool/patch.hpp"
#include "snd/buildtool/source.hpp"
#include "snd/test/test.hpp"

namespace bt = snd::buildtool;
namespace pt = snd::buildtool::patch;
namespace fs = std::filesystem;
using mini::Project;

namespace {

// Every file of a directory, held in memory.
class MemorySource final : public bt::ProjectSource {
 public:
  explicit MemorySource(const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec)) files[fs::relative(it->path(), dir, ec).generic_string()] = mini::ReadBytes(it->path());
    }
  }
  void ListObjectFiles(std::vector<std::string>* out) const override {
    out->clear();
    for (const auto& [f, b] : files) {
      if (f.size() > 5 && f.compare(f.size() - 5, 5, ".json") == 0) out->push_back(f);
    }
  }
  bool ReadFile(const std::string& path, std::vector<std::uint8_t>* out) const override {
    auto it = files.find(path);
    if (it == files.end()) return false;
    *out = it->second;
    return true;
  }
  std::map<std::string, std::vector<std::uint8_t>> files;
};

bool Compile(const std::string& text, pt::Program* prog, std::vector<pt::Problem>* problems) {
  bt::json::Value v;
  bt::json::ParseError e;
  if (!bt::json::Parse(text, &v, &e)) return false;
  return pt::Compile(v, 7, prog, problems);
}

bool HasProblem(const std::vector<pt::Problem>& p, const std::string& needle, bool error = true) {
  for (const pt::Problem& x : p) {
    if (x.error == error && x.message.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

SND_TEST(BuildtoolSource, MemoryBuildEqualsTheDirectoryBuild) {
  Project p("memsrc");
  p.Minimal();
  p.Add("events/F.json", "event", "F", "\"bank\": \"Main\", \"prority\": 3");  // a warning
  bt::Diagnostics dd;
  bt::BuildResult dr;
  SND_REQUIRE(p.Build(&dd, &dr));
  MemorySource src(p.dir());
  bt::BuildOptions o;
  o.source = &src;
  o.projectDir = "memory";  // only a name
  std::vector<bt::OutputFile> out;
  o.memoryOutput = &out;
  bt::Diagnostics md;
  bt::BuildResult mr;
  SND_REQUIRE(bt::Build(o, md, &mr));
  SND_CHECK_EQ(mr.projectBuildHash, dr.projectBuildHash);
  SND_REQUIRE_EQ(md.items().size(), dd.items().size());
  for (std::size_t i = 0; i < md.items().size(); ++i) {
    SND_CHECK(bt::Diagnostics::Format(md.items()[i]) == bt::Diagnostics::Format(dd.items()[i]));
  }
  // Every output in memory equals the file on disk; nothing was written for the memory build.
  SND_CHECK(!out.empty());
  for (const bt::OutputFile& f : out) SND_CHECK_MSG(mini::ReadBytes(p.out() / f.path) == f.bytes, "%s", f.path.c_str());
  SND_CHECK_EQ(mr.filesWritten, static_cast<std::uint32_t>(out.size()));
  // A missing file through the source is the same error as on disk.
  src.files.erase("audio/a.wav");
  out.clear();
  bt::Diagnostics md2;
  SND_CHECK(!bt::Build(o, md2, nullptr));
  SND_CHECK(mini::HasDiag(md2, bt::Severity::Error, "cannot read"));
  // Required: a source or a project directory, an output list or directory.
  bt::BuildOptions none;
  bt::Diagnostics nd;
  SND_CHECK(!bt::Build(none, nd, nullptr));
}

SND_TEST(BuildtoolPatch, CompileChecksAndOrder) {
  pt::Program prog;
  std::vector<pt::Problem> pr;
  // Nodes listed out of order: the program is in dependency order, ties in file order.
  SND_REQUIRE(Compile("{\"nodes\": [{\"id\": \"out\", \"kind\": \"output\"}, {\"id\": \"mix\", \"kind\": \"mix\"},"
                      " {\"id\": \"n\", \"kind\": \"noise\"}, {\"id\": \"o\", \"kind\": \"oscillator\", \"params\": {\"waveform\": \"saw\"}}],"
                      " \"connections\": [{\"from\": \"mix\", \"to\": \"out\"}, {\"from\": \"o\", \"to\": \"mix\", \"input\": \"a\"},"
                      " {\"from\": \"n\", \"to\": \"mix\", \"input\": \"b\"}], \"output\": \"out\"}",
                      &prog, &pr));
  SND_REQUIRE_EQ(prog.nodes.size(), std::size_t{4});
  SND_CHECK_EQ(prog.nodes[0].source, 2u);  // noise (index 2 before 3)
  SND_CHECK_EQ(prog.nodes[1].source, 3u);
  SND_CHECK_EQ(prog.nodes[2].source, 1u);  // mix
  SND_CHECK_EQ(prog.nodes[3].source, 0u);  // out
  SND_CHECK(prog.nodes[2].inputs[0] == 1u && prog.nodes[2].inputs[1] == 0u);
  SND_CHECK_EQ(prog.nodes[1].params[0], 1.0f);  // saw
  SND_CHECK_EQ(prog.nodes[1].params[1], 440.0f);  // default frequency
  // Errors.
  struct Case {
    const char* text;
    const char* needle;
  } cases[] = {
      {"{}", "needs \"nodes\""},
      {"{\"nodes\": [{\"id\": \"a\", \"kind\": \"noise\"}]}", "needs an output node"},
      {"{\"nodes\": [{\"id\": \"a\", \"kind\": \"output\"}, {\"id\": \"b\", \"kind\": \"output\"}]}", "exactly one output node"},
      {"{\"nodes\": [{\"id\": \"a\", \"kind\": \"output\"}]}", "is not connected"},
      {"{\"nodes\": [{\"id\": \"a\", \"kind\": \"zap\"}]}", "must be one of"},
      {"{\"nodes\": [{\"id\": \"a\", \"kind\": \"output\"}, {\"id\": \"a\", \"kind\": \"noise\"}]}", "duplicate node id"},
      {"{\"nodes\": [{\"id\": \"o\", \"kind\": \"output\"}, {\"id\": \"l\", \"kind\": \"lfo\"}], \"connections\": [{\"from\": \"l\", \"to\": \"o\"}]}",
       "cannot connect a control output"},
      {"{\"nodes\": [{\"id\": \"o\", \"kind\": \"output\"}, {\"id\": \"f\", \"kind\": \"filter\"}, {\"id\": \"g\", \"kind\": \"filter\"}],"
       " \"connections\": [{\"from\": \"f\", \"to\": \"g\"}, {\"from\": \"g\", \"to\": \"f\"}, {\"from\": \"g\", \"to\": \"o\"}]}",
       "cycle"},
      {"{\"nodes\": [{\"id\": \"o\", \"kind\": \"output\"}, {\"id\": \"n\", \"kind\": \"noise\"}, {\"id\": \"m\", \"kind\": \"noise\"}],"
       " \"connections\": [{\"from\": \"n\", \"to\": \"o\"}, {\"from\": \"m\", \"to\": \"o\"}]}",
       "already connected"},
      {"{\"nodes\": [{\"id\": \"o\", \"kind\": \"output\"}, {\"id\": \"p\", \"kind\": \"parameterCurve\", \"params\": {\"parameter\": \"P\","
       " \"points\": [[0, 1]]}}]}",
       "\"points\" must hold"},
      {"{\"nodes\": [{\"id\": \"o\", \"kind\": \"oscillator\", \"params\": {\"waveform\": \"wobble\"}}, {\"id\": \"x\", \"kind\": \"output\"}],"
       " \"connections\": [{\"from\": \"o\", \"to\": \"x\"}]}",
       "must be one of: sine"},
  };
  for (const Case& c : cases) {
    std::vector<pt::Problem> p;
    pt::Program q;
    SND_CHECK_MSG(!Compile(c.text, &q, &p) && HasProblem(p, c.needle), "%s: expected '%s'", c.text, c.needle);
  }
  // Too many nodes.
  std::string big = "{\"nodes\": [";
  for (std::uint32_t i = 0; i <= pt::kMaxNodes; ++i) big += (i ? ", " : "") + std::string("{\"id\": \"n") + std::to_string(i) + "\", \"kind\": \"noise\"}";
  big += "]}";
  std::vector<pt::Problem> p2;
  SND_CHECK(!Compile(big, &prog, &p2) && HasProblem(p2, "at most"));
  // Warnings: out of range (clamped), unknown parameter, a node that does not reach the output.
  std::vector<pt::Problem> p3;
  SND_CHECK(Compile("{\"nodes\": [{\"id\": \"o\", \"kind\": \"output\"}, {\"id\": \"n\", \"kind\": \"oscillator\", \"params\": "
                    "{\"frequencyHz\": 1e9, \"bogus\": 1}}, {\"id\": \"l\", \"kind\": \"lfo\"}], \"connections\": [{\"from\": \"n\", \"to\": \"o\"}]}",
                    &prog, &p3));
  SND_CHECK(HasProblem(p3, "clamped", false) && HasProblem(p3, "unknown parameter", false) &&
            HasProblem(p3, "does not reach the output", false));
  SND_CHECK_EQ(prog.nodes.size(), std::size_t{2});
  SND_CHECK_EQ(prog.nodes[0].params[1], 20000.0f);
}

SND_TEST(BuildtoolPatch, ChunkRoundTripAndBuild) {
  pt::Program a;
  a.id = 99;
  pt::CompiledNode n;
  n.kind = 1;
  n.parameter = 5;
  n.pointCount = 2;
  n.points[1][0] = 3.0f;
  n.params[2] = 0.25f;
  for (std::uint32_t& in : n.inputs) in = pt::kNoInput;
  a.nodes = {n, n};
  a.nodes[1].inputs[0] = 0;
  const std::vector<std::uint8_t> bytes = pt::WritePatchChunk({a}, {pt::Link{4, 0}});
  std::vector<pt::Program> ps;
  std::vector<pt::Link> ls;
  SND_REQUIRE(pt::ReadPatchChunk(bytes.data(), bytes.size(), &ps, &ls));
  SND_CHECK(ps.size() == 1u && ps[0].id == 99u && ps[0].nodes.size() == 2u && ps[0].nodes[1].inputs[0] == 0u &&
            ps[0].nodes[0].points[1][0] == 3.0f && ps[0].nodes[0].params[2] == 0.25f);
  SND_CHECK(ls.size() == 1u && ls[0].synpRow == 4u);
  for (std::size_t cut = 0; cut < bytes.size(); cut += 7) SND_CHECK(!pt::ReadPatchChunk(bytes.data(), cut, &ps, &ls));
  // D-180c: indices out of range (an input not earlier in its program, a link to no program).
  pt::Program bad = a;
  bad.nodes[0].inputs[1] = 1;  // node 0 reads node 1: not dependency order
  std::vector<std::uint8_t> b2 = pt::WritePatchChunk({bad}, {pt::Link{4, 0}});
  SND_CHECK(!pt::ReadPatchChunk(b2.data(), b2.size(), &ps, &ls));
  b2 = pt::WritePatchChunk({a}, {pt::Link{4, 1}});  // program 1 of 1
  SND_CHECK(!pt::ReadPatchChunk(b2.data(), b2.size(), &ps, &ls));
  // A project: the patch object, a clip using it; an unknown patch reference is an error.
  Project p("patchbuild");
  p.Minimal();
  p.Add("patches/Beep.json", "patch", "Beep",
        "\"nodes\": [{\"id\": \"o\", \"kind\": \"oscillator\", \"position\": [10, 20]}, {\"id\": \"out\", \"kind\": \"output\"}],"
        " \"connections\": [{\"from\": \"o\", \"to\": \"out\"}]");
  p.Add("events/S.json", "event", "S", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"synth\": {\"kind\": \"patch\", \"patch\": \"Beep\"}}]}]");
  bt::Diagnostics d;
  const bool built = p.Build(&d);
  SND_CHECK_MSG(built, "%s", mini::Dump(d).c_str());
  SND_REQUIRE(built);
  const std::string ids = mini::ReadText(p.out() / "ids.json");
  SND_CHECK(ids.find("\"Beep\"") != std::string::npos);
  p.Add("events/T.json", "event", "T", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"synth\": {\"kind\": \"patch\", \"patch\": \"Nope\"}}]}]");
  bt::Diagnostics d2;
  SND_CHECK(!p.Build(&d2));
  SND_CHECK(mini::HasDiag(d2, bt::Severity::Error, "unknown patch 'Nope'"));
}
