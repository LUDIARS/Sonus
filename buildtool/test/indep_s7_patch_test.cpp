// Independent tests of group M: patch compilation and the provisional XPCH chunk
// (properties G1..G5 of tool/test/indep_s7_model_properties.md). Written from
// design.md 5.1, project-format.md "patch", bank-format.md §9 and patch.hpp.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "snd/buildtool/json.hpp"
#include "snd/buildtool/patch.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"

namespace json = snd::buildtool::json;
namespace patch = snd::buildtool::patch;

namespace {

struct Rng {
  std::uint64_t s;
  std::uint32_t Below(std::uint32_t n) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return n ? static_cast<std::uint32_t>(s % n) : 0u;
  }
};

struct GNode {
  std::string id, kind;
};
struct GConn {
  std::string from, to, input;
};

std::string Doc(const std::vector<GNode>& nodes, const std::vector<GConn>& conns, const std::string& output = std::string()) {
  std::string t = "{\"type\": \"patch\", \"name\": \"P\", \"nodes\": [";
  for (std::size_t i = 0; i < nodes.size(); ++i)
    t += (i ? ", " : "") + std::string("{\"id\": \"") + nodes[i].id + "\", \"kind\": \"" + nodes[i].kind + "\"}";
  t += "], \"connections\": [";
  for (std::size_t i = 0; i < conns.size(); ++i) {
    t += (i ? ", " : "") + std::string("{\"from\": \"") + conns[i].from + "\", \"to\": \"" + conns[i].to + "\"";
    if (!conns[i].input.empty()) t += ", \"input\": \"" + conns[i].input + "\"";
    t += "}";
  }
  t += "]";
  if (!output.empty()) t += ", \"output\": \"" + output + "\"";
  return t + "}";
}

bool CompileText(const std::string& text, patch::Program* prog, std::vector<patch::Problem>* problems) {
  json::Value v;
  json::ParseError pe;
  if (!json::Parse(text, &v, &pe)) return false;
  return patch::Compile(v, snd::HashId("P"), prog, problems);
}

bool HasError(const std::vector<patch::Problem>& p) {
  for (const auto& x : p)
    if (x.error) return true;
  return false;
}

// A random valid graph: sources (oscillator / noise / adsr / lfo) and processors (amp / filter / mix),
// one output fed by an audio node. Nodes are generated in dependency order and then shuffled in the file.
void RandomGraph(Rng& r, std::uint32_t count, std::vector<GNode>* nodes, std::vector<GConn>* conns) {
  std::vector<GNode> gen;
  std::vector<std::string> audio, control;
  std::vector<GConn> cs;
  for (std::uint32_t i = 0; i + 1 < count; ++i) {
    const std::string id = "n" + std::to_string(i);
    std::uint32_t pick = r.Below(audio.empty() ? 4u : 7u);
    static const char* const kKinds[] = {"oscillator", "noise", "adsr", "lfo", "amp", "filter", "mix"};
    const std::string kind = kKinds[pick];
    gen.push_back({id, kind});
    if (kind == "amp" || kind == "filter") {
      cs.push_back({audio[r.Below(static_cast<std::uint32_t>(audio.size()))], id, "in"});
      if (!control.empty() && r.Below(2)) cs.push_back({control[r.Below(static_cast<std::uint32_t>(control.size()))], id, kind == "amp" ? "gain" : "cutoff"});
    } else if (kind == "mix") {
      cs.push_back({audio[r.Below(static_cast<std::uint32_t>(audio.size()))], id, "a"});
      cs.push_back({audio[r.Below(static_cast<std::uint32_t>(audio.size()))], id, "b"});
    } else if (kind == "oscillator" && !control.empty() && r.Below(2)) {
      cs.push_back({control[r.Below(static_cast<std::uint32_t>(control.size()))], id, "pitch"});
    }
    if (kind == "adsr" || kind == "lfo") control.push_back(id);
    else audio.push_back(id);
  }
  if (audio.empty()) {
    gen.push_back({"src", "oscillator"});
    audio.push_back("src");
  }
  gen.push_back({"out", "output"});
  cs.push_back({audio[r.Below(static_cast<std::uint32_t>(audio.size()))], "out", "in"});
  // Shuffle the file order.
  for (std::size_t i = gen.size(); i > 1; --i) std::swap(gen[i - 1], gen[r.Below(static_cast<std::uint32_t>(i))]);
  *nodes = gen;
  *conns = cs;
}

// Reference order: nodes that reach the output; Kahn's algorithm, ties by file position.
std::vector<std::uint32_t> ReferenceOrder(const std::vector<GNode>& nodes, const std::vector<GConn>& conns) {
  auto index = [&](const std::string& id) {
    for (std::uint32_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].id == id) return i;
    return 0xFFFFFFFFu;
  };
  const std::uint32_t n = static_cast<std::uint32_t>(nodes.size());
  std::vector<bool> reach(n, false);
  std::vector<std::uint32_t> stack = {index("out")};
  while (!stack.empty()) {
    const std::uint32_t x = stack.back();
    stack.pop_back();
    if (reach[x]) continue;
    reach[x] = true;
    for (const auto& c : conns)
      if (index(c.to) == x) stack.push_back(index(c.from));
  }
  std::vector<std::uint32_t> pending(n, 0);
  for (const auto& c : conns)
    if (reach[index(c.to)]) ++pending[index(c.to)];
  std::vector<bool> done(n, false);
  std::vector<std::uint32_t> order;
  for (;;) {
    std::uint32_t pick = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < n; ++i)
      if (reach[i] && !done[i] && pending[i] == 0) {
        pick = i;
        break;
      }
    if (pick == 0xFFFFFFFFu) break;
    done[pick] = true;
    order.push_back(pick);
    for (const auto& c : conns)
      if (index(c.from) == pick && reach[index(c.to)]) --pending[index(c.to)];
  }
  return order;
}

}  // namespace

SND_TEST(IndepS7Patch, G1_DependencyOrderIsDeterministic) {
  Rng r{424242};
  std::uint32_t checked = 0;
  for (int iter = 0; iter < 300; ++iter) {
    std::vector<GNode> nodes;
    std::vector<GConn> conns;
    RandomGraph(r, 2 + r.Below(31), &nodes, &conns);
    const std::string text = Doc(nodes, conns);
    patch::Program a, b;
    std::vector<patch::Problem> pa, pb;
    const bool okA = CompileText(text, &a, &pa);
    const bool okB = CompileText(text, &b, &pb);
    SND_CHECK_EQ(okA, okB);
    if (!okA) {
      for (const auto& p : pa)
        if (p.error) snd::test::Note("iter %d: %s %s", iter, p.path.c_str(), p.message.c_str());
      SND_CHECK(false);
      continue;
    }
    ++checked;
    SND_CHECK_EQ(a.id, snd::HashId("P"));
    const auto ref = ReferenceOrder(nodes, conns);
    SND_REQUIRE(a.nodes.size() == ref.size());
    for (std::size_t i = 0; i < ref.size(); ++i) {
      SND_CHECK_EQ(a.nodes[i].source, ref[i]);
      SND_CHECK_EQ(a.nodes[i].kind, b.nodes[i].kind);
      for (std::uint32_t k = 0; k < patch::kMaxInputs; ++k) {
        const std::uint32_t in = a.nodes[i].inputs[k];
        SND_CHECK(in == patch::kNoInput || in < i);  // every input comes earlier
        SND_CHECK_EQ(in, b.nodes[i].inputs[k]);
      }
    }
    // The output node is the last.
    std::uint32_t outKind = 0;
    SND_REQUIRE(patch::FindNodeKind("output", &outKind) != nullptr);
    SND_CHECK_EQ(a.nodes.back().kind, outKind);
    // Unreached nodes: a warning each, no error.
    const std::size_t dropped = nodes.size() - ref.size();
    std::size_t warnings = 0;
    for (const auto& p : pa) warnings += p.error ? 0u : 1u;
    SND_CHECK(warnings >= dropped);
  }
  SND_CHECK(checked > 250u);
}

SND_TEST(IndepS7Patch, G1_InputsWiredToTheRightNodes) {
  // osc -> filt.in, lfo -> filt.cutoff, filt -> out
  const std::vector<GNode> nodes = {{"out", "output"}, {"filt", "filter"}, {"lfo", "lfo"}, {"osc", "oscillator"}};
  const std::vector<GConn> conns = {{"osc", "filt", "in"}, {"lfo", "filt", "cutoff"}, {"filt", "out", ""}};
  patch::Program p;
  std::vector<patch::Problem> pr;
  SND_REQUIRE(CompileText(Doc(nodes, conns, "out"), &p, &pr));
  SND_REQUIRE(p.nodes.size() == 4u);
  // Ready at first: lfo (file 2), osc (file 3) -> lfo first.
  SND_CHECK_EQ(p.nodes[0].source, 2u);
  SND_CHECK_EQ(p.nodes[1].source, 3u);
  SND_CHECK_EQ(p.nodes[2].source, 1u);
  SND_CHECK_EQ(p.nodes[3].source, 0u);
  std::uint32_t filterKind = 0;
  const patch::NodeKindInfo* fk = patch::FindNodeKind("filter", &filterKind);
  SND_REQUIRE(fk != nullptr);
  std::uint32_t inIdx = 0, cutIdx = 0;
  SND_REQUIRE(patch::FindInput(*fk, "in", &inIdx) != nullptr);
  SND_REQUIRE(patch::FindInput(*fk, "cutoff", &cutIdx) != nullptr);
  SND_CHECK_EQ(p.nodes[2].inputs[inIdx], 1u);
  SND_CHECK_EQ(p.nodes[2].inputs[cutIdx], 0u);
  // Defaults filled in for every parameter.
  for (std::uint32_t k = 0; k < fk->paramCount; ++k) {
    if (fk->params[k].choices != nullptr) continue;
    SND_CHECK_EQ(p.nodes[2].params[k], fk->params[k].defaultValue);
  }
  // An omitted input name is the first input.
  std::uint32_t outKind = 0;
  const patch::NodeKindInfo* ok = patch::FindNodeKind("output", &outKind);
  SND_REQUIRE(ok != nullptr && ok->inputCount >= 1);
  SND_CHECK_EQ(p.nodes[3].inputs[0], 2u);
}

SND_TEST(IndepS7Patch, G2_InvalidGraphsAreErrors) {
  patch::Program p;
  std::vector<patch::Problem> pr;
  auto bad = [&](const std::vector<GNode>& n, const std::vector<GConn>& c, const char* what) {
    pr.clear();
    const bool ok = CompileText(Doc(n, c), &p, &pr);
    if (ok || !HasError(pr)) {
      snd::test::Note("accepted: %s", what);
      SND_CHECK(false);
    }
  };
  // Cycle through two amps.
  bad({{"a", "amp"}, {"b", "amp"}, {"out", "output"}}, {{"a", "b", "in"}, {"b", "a", "in"}, {"b", "out", "in"}}, "cycle");
  bad({{"a", "amp"}, {"out", "output"}}, {{"a", "a", "in"}, {"a", "out", "in"}}, "self loop");
  bad({{"o", "oscillator"}}, {}, "no output node");
  bad({{"o", "oscillator"}, {"x", "output"}, {"y", "output"}}, {{"o", "x", "in"}, {"o", "y", "in"}}, "two outputs");
  bad({{"e", "adsr"}, {"out", "output"}}, {{"e", "out", "in"}}, "control into audio");
  bad({{"o", "oscillator"}, {"o2", "oscillator"}, {"out", "output"}}, {{"o", "o2", "pitch"}, {"o2", "out", "in"}}, "audio into control");
  bad({{"o", "oscillator"}, {"n", "noise"}, {"out", "output"}}, {{"o", "out", "in"}, {"n", "out", "in"}}, "two into one input");
  bad({{"o", "oscillator"}, {"o", "noise"}, {"out", "output"}}, {{"o", "out", "in"}}, "duplicate id");
  bad({{"o", "oscillator"}, {"out", "output"}}, {{"o", "out", "nope"}}, "unknown input");
  bad({{"o", "oscillator"}, {"out", "output"}}, {{"ghost", "out", "in"}}, "unknown node");
  bad({{"o", "bogus"}, {"out", "output"}}, {{"o", "out", "in"}}, "unknown kind");
  bad({{"f", "filter"}, {"out", "output"}}, {{"f", "out", "in"}}, "required input missing");
  bad({{"out", "output"}}, {}, "output without input");
  bad({}, {}, "no nodes");
  // 33 nodes.
  std::vector<GNode> many = {{"out", "output"}, {"o", "oscillator"}};
  for (int i = 0; i < 31; ++i) many.push_back({"l" + std::to_string(i), "lfo"});
  bad(many, {{"o", "out", "in"}}, "33 nodes");
  // 32 is fine.
  many.pop_back();
  pr.clear();
  SND_CHECK(CompileText(Doc(many, {{"o", "out", "in"}}), &p, &pr));
  SND_CHECK_EQ(p.nodes.size(), 2u);  // the lfos do not reach the output
  // "output" naming a non-output node.
  pr.clear();
  SND_CHECK(!CompileText(Doc({{"o", "oscillator"}, {"out", "output"}}, {{"o", "out", "in"}}, "o"), &p, &pr));
}

SND_TEST(IndepS7Patch, G2_ParameterRangesAndChoices) {
  const std::string text = R"({"type": "patch", "name": "P", "nodes": [
    {"id": "o", "kind": "oscillator", "params": {"waveform": "square", "frequencyHz": 1e12}},
    {"id": "f", "kind": "filter", "params": {"mode": "bandpass", "q": -5}},
    {"id": "c", "kind": "parameterCurve", "params": {"parameter": "Speed", "points": [[0, 0], [1, 1], [2, 0.5]]}},
    {"id": "out", "kind": "output"}],
    "connections": [{"from": "o", "to": "f", "input": "in"}, {"from": "c", "to": "f", "input": "cutoff"}, {"from": "f", "to": "out"}]})";
  patch::Program p;
  std::vector<patch::Problem> pr;
  SND_REQUIRE(CompileText(text, &p, &pr));
  bool warned = false;
  for (const auto& x : pr) warned |= !x.error && x.path.find("frequencyHz") != std::string::npos;
  SND_CHECK(warned);
  std::uint32_t oscKind = 0;
  const patch::NodeKindInfo* ok = patch::FindNodeKind("oscillator", &oscKind);
  std::uint32_t fi = 0, wi = 0;
  SND_REQUIRE(patch::FindParam(*ok, "frequencyHz", &fi) != nullptr);
  SND_REQUIRE(patch::FindParam(*ok, "waveform", &wi) != nullptr);
  for (const auto& n : p.nodes) {
    if (n.kind != oscKind) continue;
    SND_CHECK_EQ(n.params[fi], ok->params[fi].maxValue);  // clamped
    SND_CHECK_EQ(n.params[wi], 2.0f);                     // sine, saw, square -> 2
  }
  for (const auto& n : p.nodes) {
    if (n.pointCount == 0) continue;
    SND_CHECK_EQ(n.parameter, snd::HashId("Speed"));
    SND_CHECK_EQ(n.pointCount, 3u);
    SND_CHECK_EQ(n.points[2][0], 2.0f);
  }
  // Bad choices, unsorted / too many curve points, unknown parameter: errors.
  for (const char* bad : {R"({"type": "patch", "name": "P", "nodes": [{"id": "o", "kind": "oscillator", "params": {"waveform": "zigzag"}}, {"id": "out", "kind": "output"}], "connections": [{"from": "o", "to": "out"}]})",
                          R"({"type": "patch", "name": "P", "nodes": [{"id": "o", "kind": "oscillator"}, {"id": "c", "kind": "parameterCurve", "params": {"parameter": "S", "points": [[1, 0], [0, 1]]}}, {"id": "out", "kind": "output"}], "connections": [{"from": "o", "to": "out"}, {"from": "c", "to": "o", "input": "pitch"}]})",
                          R"({"type": "patch", "name": "P", "nodes": [{"id": "o", "kind": "oscillator"}, {"id": "c", "kind": "parameterCurve", "params": {"parameter": "S", "points": [[0,0],[1,0],[2,0],[3,0],[4,0],[5,0],[6,0],[7,0],[8,0]]}}, {"id": "out", "kind": "output"}], "connections": [{"from": "o", "to": "out"}, {"from": "c", "to": "o", "input": "pitch"}]})",
                          R"({"type": "patch", "name": "P", "nodes": [{"id": "o", "kind": "oscillator"}, {"id": "c", "kind": "parameterCurve", "params": {"parameter": "S", "points": [[0,0]]}}, {"id": "out", "kind": "output"}], "connections": [{"from": "o", "to": "out"}, {"from": "c", "to": "o", "input": "pitch"}]})"}) {
    pr.clear();
    const bool ok2 = CompileText(bad, &p, &pr);
    if (ok2 || !HasError(pr)) {
      snd::test::Note("accepted: %s", bad);
      SND_CHECK(false);
    }
  }
}

// An unknown parameter is an unknown member: a warning (project-format.md §1), not an error.
SND_TEST(IndepS7Patch, G2_UnknownParameterWarns) {
  patch::Program p;
  std::vector<patch::Problem> pr;
  SND_CHECK(CompileText(R"({"type": "patch", "name": "P", "nodes": [{"id": "o", "kind": "oscillator", "params": {"nope": 1}}, {"id": "out", "kind": "output"}], "connections": [{"from": "o", "to": "out"}]})",
                        &p, &pr));
  bool warned = false;
  for (const auto& x : pr) warned |= !x.error && x.path.find("nope") != std::string::npos;
  SND_CHECK(warned);
}

SND_TEST(IndepS7Patch, G4_ChunkRoundTrip) {
  Rng r{77};
  std::vector<patch::Program> progs;
  for (int k = 0; k < 5; ++k) {
    std::vector<GNode> nodes;
    std::vector<GConn> conns;
    RandomGraph(r, 2 + r.Below(31), &nodes, &conns);
    patch::Program p;
    std::vector<patch::Problem> pr;
    SND_REQUIRE(CompileText(Doc(nodes, conns), &p, &pr));
    p.id = snd::HashId("P" + std::to_string(k));
    p.nodes[0].parameter = 0x12345678u;
    p.nodes[0].pointCount = 2;
    p.nodes[0].points[1][0] = 3.5f;
    p.nodes[0].points[1][1] = -1.25f;
    progs.push_back(p);
  }
  const std::vector<patch::Link> links = {{0, 0}, {5, 4}, {7, 2}};
  const auto bytes = patch::WritePatchChunk(progs, links);
  // Layout of §9: 16 B header, 16 B per program, 128 B per node, 8 B per link.
  std::size_t nodes = 0;
  for (const auto& p : progs) nodes += p.nodes.size();
  SND_CHECK_EQ(bytes.size(), 16u + 16u * progs.size() + 128u * nodes + 8u * links.size());
  std::uint32_t hdr[4];
  std::memcpy(hdr, bytes.data(), 16);
  SND_CHECK_EQ(hdr[0], 1u);
  SND_CHECK_EQ(hdr[1], 5u);
  SND_CHECK_EQ(hdr[2], 3u);
  SND_CHECK_EQ(hdr[3], 128u);
  std::vector<patch::Program> back;
  std::vector<patch::Link> lb;
  SND_REQUIRE(patch::ReadPatchChunk(bytes.data(), bytes.size(), &back, &lb));
  SND_REQUIRE(back.size() == progs.size());
  for (std::size_t i = 0; i < progs.size(); ++i) {
    SND_CHECK_EQ(back[i].id, progs[i].id);
    SND_REQUIRE(back[i].nodes.size() == progs[i].nodes.size());
    for (std::size_t n = 0; n < progs[i].nodes.size(); ++n) {
      const auto& a = progs[i].nodes[n];
      const auto& b = back[i].nodes[n];
      SND_CHECK_EQ(a.kind, b.kind);
      SND_CHECK_EQ(a.parameter, b.parameter);
      SND_CHECK_EQ(a.pointCount, b.pointCount);
      SND_CHECK(std::memcmp(a.inputs, b.inputs, sizeof a.inputs) == 0);
      SND_CHECK(std::memcmp(a.params, b.params, sizeof a.params) == 0);
      SND_CHECK(std::memcmp(a.points, b.points, sizeof a.points) == 0);
    }
  }
  SND_REQUIRE(lb.size() == links.size());
  for (std::size_t i = 0; i < links.size(); ++i) SND_CHECK(lb[i].synpRow == links[i].synpRow && lb[i].program == links[i].program);
  // Empty.
  const auto empty = patch::WritePatchChunk({}, {});
  SND_CHECK(patch::ReadPatchChunk(empty.data(), empty.size(), &back, &lb));
  SND_CHECK(back.empty() && lb.empty());
}

SND_TEST(IndepS7Patch, G5_BrokenChunksAreRefused) {
  Rng r{5};
  std::vector<GNode> nodes;
  std::vector<GConn> conns;
  RandomGraph(r, 8, &nodes, &conns);
  patch::Program p;
  std::vector<patch::Problem> pr;
  SND_REQUIRE(CompileText(Doc(nodes, conns), &p, &pr));
  const auto good = patch::WritePatchChunk({p, p}, {{0, 1}});
  std::vector<patch::Program> back;
  std::vector<patch::Link> lb;
  // Every truncation.
  for (std::size_t n = 0; n < good.size(); ++n) {
    std::vector<std::uint8_t> t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
    SND_CHECK(!patch::ReadPatchChunk(t.empty() ? nullptr : t.data(), t.size(), &back, &lb));
  }
  // Huge counts, bad version / node size, node ranges, input / link indices beyond the program.
  auto patched = [&](std::size_t off, std::uint32_t v) {
    auto b = good;
    std::memcpy(b.data() + off, &v, 4);
    return b;
  };
  const std::vector<std::pair<std::size_t, std::uint32_t>> edits = {
      {0, 2}, {4, 0xFFFFFFFFu}, {8, 0xFFFFFFFFu}, {12, 64}, {12, 0}, {16 + 4, 0x7FFFFFFFu}, {16 + 8, 0xFFFFFFFFu},
      {16 + 16 + 8, 0xFFFFFFF0u}};
  for (const auto& [off, v] : edits) {
    auto b = patched(off, v);
    SND_CHECK(!patch::ReadPatchChunk(b.data(), b.size(), &back, &lb));
  }
  // Random bytes: no crash.
  for (int i = 0; i < 2000; ++i) {
    auto b = good;
    for (int k = 0; k < 4; ++k) b[r.Below(static_cast<std::uint32_t>(b.size()))] = static_cast<std::uint8_t>(r.Below(256));
    (void)patch::ReadPatchChunk(b.data(), b.size(), &back, &lb);
  }
}
