// Patch editor model: nodes, parameters, connections and their rules, the output node, the
// node limit, the build's checks, the dependency order, undo / redo, save / load, and the
// provisional XPCH chunk of the built bank.
#include <cmath>
#include <memory>

#include "doc_fixture.hpp"
#include "snd/buildtool/patch.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/patch_editor.hpp"

using namespace snd::tool;
using namespace doctest;
namespace bf = snd::bank_format;
namespace pt = snd::buildtool::patch;

namespace {

// osc -> filter -> amp -> out, lfo -> osc.pitch, adsr -> amp.gain, Speed curve -> filter.cutoff.
std::string MakePatch(ProjectDocument& doc, PatchEditor** out, std::unique_ptr<PatchEditor>& holder) {
  const std::string g = doc.Create("patch", "Patch/Laser");
  holder = std::make_unique<PatchEditor>(doc, g);
  PatchEditor& p = *holder;
  *out = &p;
  p.AddNode("output", 400, 0);
  p.AddNode("amp", 300, 0);
  p.AddNode("filter", 200, 0);
  p.AddNode("oscillator", 100, 0, "osc");
  p.AddNode("lfo", 0, 100);
  p.AddNode("adsr", 200, 100);
  p.AddNode("parameterCurve", 100, 100, "speed");
  return g;
}

}  // namespace

SND_TEST(PatchEditor, BuildAGraphWithTheRules) {
  ProjectDocument doc;
  LoadBase(doc);
  PatchEditor* pp = nullptr;
  std::unique_ptr<PatchEditor> holder;
  const std::string g = MakePatch(doc, &pp, holder);
  PatchEditor& p = *pp;
  SND_REQUIRE(p.valid());
  SND_CHECK_EQ(p.Nodes().size(), std::size_t{7});
  SND_CHECK_EQ(p.OutputNode(), std::string("output1"));
  SND_CHECK(p.AddNode("output", 0, 0).empty());     // one output node
  SND_CHECK(p.AddNode("wobble", 0, 0).empty());     // unknown kind
  SND_CHECK(p.AddNode("amp", 0, 0, "osc").empty()); // id taken
  // Connections.
  SND_CHECK(p.Connect("osc", "filter1"));             // "in" by default
  SND_CHECK(p.Connect("filter1", "amp1", "in"));
  SND_CHECK(p.Connect("amp1", "output1"));
  SND_CHECK(p.Connect("lfo1", "osc", "pitch"));
  SND_CHECK(p.Connect("adsr1", "amp1", "gain"));
  SND_CHECK(!p.Connect("adsr1", "filter1", "in"));    // control -> audio input
  SND_CHECK(!p.Connect("osc", "amp1", "gain"));       // audio -> control input
  SND_CHECK(!p.Connect("lfo1", "osc", "pitch"));      // input already connected
  SND_CHECK(!p.Connect("amp1", "filter1", "cutoff")); // audio into control (and a cycle)
  SND_CHECK(!p.Connect("output1", "amp1", "in"));     // the output node has no output
  SND_CHECK(!p.Connect("osc", "osc", "amp"));         // itself
  SND_CHECK(!p.Connect("osc", "filter1", "nope"));    // unknown input
  // Parameters.
  SND_CHECK(p.SetParam("filter1", "cutoffHz", 1200));
  SND_CHECK(!p.SetParam("filter1", "cutoffHz", 1e6));  // outside the range
  SND_CHECK(!p.SetParam("filter1", "gainDb", 1));      // not a filter parameter
  SND_CHECK(!p.SetParam("filter1", "mode", 1));        // an enumeration: by name
  SND_CHECK(p.SetChoice("filter1", "mode", "highpass"));
  SND_CHECK(!p.SetChoice("filter1", "mode", "notch"));
  SND_CHECK_EQ(p.Param("filter1", "mode"), 1.0);
  SND_CHECK_EQ(p.Param("filter1", "q"), static_cast<double>(0.707f));
  SND_CHECK(p.SetCurve("speed", G(6), {{0, 0}, {10, 1}}));
  SND_CHECK(!p.SetCurve("speed", G(6), {{0, 0}, {0, 1}}));  // x ascending
  SND_CHECK(!p.SetCurve("osc", G(6), {{0, 0}, {1, 1}}));    // not a parameterCurve
  SND_CHECK(p.Connect("speed", "filter1", "cutoff"));
  SND_CHECK(p.MoveNode("osc", 120, 10));
  SND_CHECK_EQ(p.Nodes()[3].x, 120.0f);
  SND_CHECK(p.Check().empty());
  // The dependency order: the output last; every node after its inputs.
  pt::Program prog;
  SND_REQUIRE(p.Compiled(&prog));
  SND_REQUIRE_EQ(prog.nodes.size(), std::size_t{7});
  std::uint32_t outKind = 0;
  pt::FindNodeKind("output", &outKind);
  SND_CHECK_EQ(prog.nodes.back().kind, outKind);
  for (std::size_t i = 0; i < prog.nodes.size(); ++i) {
    for (std::uint32_t in : prog.nodes[i].inputs) SND_CHECK(in == pt::kNoInput || in < i);
  }
  // Rename follows into the connections; removal takes the node's connections.
  SND_CHECK(p.RenameNode("osc", "osc2"));
  SND_CHECK_EQ(p.Connections()[0].from, std::string("osc2"));
  SND_CHECK(p.RemoveNode("lfo1"));
  SND_CHECK_EQ(p.Connections().size(), std::size_t{5});
  // The build's checks: an unconnected required input.
  SND_CHECK(p.Disconnect("amp1", "in"));
  bool found = false;
  for (const pt::Problem& pr : p.Check()) found = found || (pr.error && pr.message.find("'in'") != std::string::npos);
  SND_CHECK(found);
  // Everything undoes to nothing.
  while (doc.Undo()) {
  }
  SND_CHECK(doc.Find(g) == nullptr);
  SND_CHECK(doc.PendingChanges().empty());
}

SND_TEST(PatchEditor, NodeLimit) {
  ProjectDocument doc;
  LoadBase(doc);
  PatchEditor p(doc, doc.Create("patch", "Big"));
  for (std::uint32_t i = 0; i < pt::kMaxNodes; ++i) SND_CHECK(!p.AddNode("noise", 0, 0).empty());
  SND_CHECK(p.AddNode("noise", 0, 0).empty());
  SND_CHECK_EQ(p.Nodes().size(), std::size_t{pt::kMaxNodes});
}

SND_TEST(PatchEditor, BuildsIntoTheProvisionalChunk) {
  TempDir dir("patch");
  WriteAll(dir, BaseFiles());
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  doc.SetRandom(Counter{});
  PatchEditor* pp = nullptr;
  std::unique_ptr<PatchEditor> holder;
  const std::string g = MakePatch(doc, &pp, holder);
  PatchEditor& p = *pp;
  SND_REQUIRE(p.Connect("osc", "filter1"));
  SND_REQUIRE(p.Connect("filter1", "amp1"));
  SND_REQUIRE(p.Connect("amp1", "output1"));
  SND_REQUIRE(p.Connect("adsr1", "amp1", "gain"));
  SND_REQUIRE(p.SetCurve("speed", G(6), {{0, 100}, {10, 8000}}));
  SND_REQUIRE(p.Connect("speed", "filter1", "cutoff"));
  SND_REQUIRE(p.SetParam("osc", "frequencyHz", 330));
  // lfo1 is left unconnected: a warning, left out of the program.
  bool warned = false;
  for (const pt::Problem& pr : p.Check()) warned = warned || (!pr.error && pr.path == "nodes[4]");
  SND_CHECK(warned);
  EventEditor e(doc, G(8));
  SynthDesc s;
  s.kind = "patch";
  s.patch = g;
  SND_REQUIRE(e.AddSynthClip(0, s, 0));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  SND_CHECK(HasIssue(b.issues, "does not reach the output", false));
  const ParsedBank* v = b.FindBank("Main")->view();
  std::uint64_t bytes = 0;
  const std::uint8_t* chunk = v->Chunk(pt::kChunkFourcc, &bytes);
  SND_REQUIRE(chunk != nullptr);
  std::vector<pt::Program> programs;
  std::vector<pt::Link> links;
  SND_REQUIRE(pt::ReadPatchChunk(chunk, bytes, &programs, &links));
  SND_REQUIRE(programs.size() == 1u && links.size() == 1u);
  SND_CHECK_EQ(programs[0].id, snd::HashId("Patch/Laser"));
  SND_CHECK_EQ(programs[0].nodes.size(), std::size_t{6});
  SND_CHECK_EQ(v->U32(bf::chunk::SYNP, bf::col::SYNP_kind, links[0].synpRow), static_cast<std::uint32_t>(bf::SynthKind::Patch));
  pt::Program local;
  SND_REQUIRE(p.Compiled(&local));
  SND_CHECK_EQ(local.nodes.size(), programs[0].nodes.size());
  for (std::size_t i = 0; i < local.nodes.size(); ++i) {
    SND_CHECK(local.nodes[i].kind == programs[0].nodes[i].kind && local.nodes[i].params[1] == programs[0].nodes[i].params[1]);
  }
  bool curve = false;
  for (const pt::CompiledNode& n : programs[0].nodes) curve = curve || (n.parameter == snd::HashId("Speed") && n.pointCount == 2);
  SND_CHECK(curve);
  // Deterministic: building again gives the same bank.
  SND_CHECK(BuildDocument(doc).FindBank("Main")->bytes == b.FindBank("Main")->bytes);
  // Positions do not reach the bank; save / load keeps them.
  ProjectDocument before = doc;
  SND_REQUIRE(p.MoveNode("lfo1", 5, 6));
  SND_CHECK(ClassifyEdit(before, doc) == EditClass::None);
  SND_REQUIRE(doc.Save());
  ProjectDocument again;
  SND_REQUIRE(again.Load(dir.path().string()));
  SND_CHECK(JsonEqual(again.Find(g)->doc, doc.Find(g)->doc));
  SND_CHECK_EQ(again.Find(g)->file, std::string("patches/Patch/Laser.json"));
}
