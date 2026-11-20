// Independent re-check of D-180c: ReadPatchChunk checks every number. Mutation fuzz of valid
// XPCH chunks (run under ASan / UBSan): when the reader accepts, what it returns must satisfy
// bank-format.md §9 (inputs name earlier nodes of the same program, links name existing programs,
// kinds exist, point counts fit).
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

patch::Program Compile(const char* text) {
  json::Value v;
  json::ParseError pe;
  json::Parse(text, &v, &pe);
  patch::Program p;
  std::vector<patch::Problem> pr;
  patch::Compile(v, snd::HashId("P"), &p, &pr);
  return p;
}

bool Valid(const std::vector<patch::Program>& progs, const std::vector<patch::Link>& links) {
  std::uint32_t kinds = 0;
  patch::NodeKinds(&kinds);
  for (const auto& p : progs) {
    if (p.nodes.empty() || p.nodes.size() > patch::kMaxNodes) return false;
    for (std::size_t n = 0; n < p.nodes.size(); ++n) {
      const auto& node = p.nodes[n];
      if (node.kind >= kinds) return false;
      if (node.pointCount > patch::kMaxCurvePoints) return false;
      for (std::uint32_t k = 0; k < patch::kMaxInputs; ++k)
        if (node.inputs[k] != patch::kNoInput && node.inputs[k] >= n) return false;
    }
  }
  for (const auto& l : links)
    if (l.program >= progs.size()) return false;
  return true;
}
}  // namespace

SND_TEST(IndepS7PatchRecheck, D180c_AcceptedChunksAreConsistent) {
  const patch::Program a = Compile(R"({"type": "patch", "name": "P", "nodes": [{"id": "o", "kind": "oscillator"}, {"id": "l", "kind": "lfo"}, {"id": "f", "kind": "filter"}, {"id": "out", "kind": "output"}],
    "connections": [{"from": "o", "to": "f", "input": "in"}, {"from": "l", "to": "f", "input": "cutoff"}, {"from": "f", "to": "out"}]})");
  const patch::Program c = Compile(R"({"type": "patch", "name": "P", "nodes": [{"id": "c", "kind": "parameterCurve", "params": {"parameter": "S", "points": [[0, 0], [1, 1]]}}, {"id": "o", "kind": "oscillator"}, {"id": "out", "kind": "output"}],
    "connections": [{"from": "c", "to": "o", "input": "pitch"}, {"from": "o", "to": "out"}]})");
  SND_REQUIRE(!a.nodes.empty() && !c.nodes.empty());
  const auto good = patch::WritePatchChunk({a, c, a}, {{0, 0}, {3, 1}, {9, 2}});
  std::vector<patch::Program> progs;
  std::vector<patch::Link> links;
  SND_REQUIRE(patch::ReadPatchChunk(good.data(), good.size(), &progs, &links));
  SND_CHECK(Valid(progs, links));
  Rng r{0xC0FFEEu};
  std::uint32_t accepted = 0, refused = 0, bad = 0;
  for (int i = 0; i < 20000; ++i) {
    auto b = good;
    const std::uint32_t edits = 1 + r.Below(3);
    for (std::uint32_t e = 0; e < edits; ++e) {
      const std::uint32_t at = r.Below(static_cast<std::uint32_t>(b.size()) / 4) * 4;
      std::uint32_t v;
      switch (r.Below(5)) {
        case 0: v = r.Below(4); break;
        case 1: v = r.Below(64); break;
        case 2: v = 0xFFFFFFFFu - r.Below(4); break;
        case 3: v = 0x7FFFFFFFu; break;
        default: v = static_cast<std::uint32_t>(r.Below(0xFFFFFFFFu)); break;
      }
      std::memcpy(b.data() + at, &v, 4);
    }
    if (r.Below(10) == 0) b.resize(r.Below(static_cast<std::uint32_t>(b.size()) + 1));
    progs.clear();
    links.clear();
    if (patch::ReadPatchChunk(b.empty() ? nullptr : b.data(), b.size(), &progs, &links)) {
      ++accepted;
      if (!Valid(progs, links)) {
        ++bad;
        if (bad < 5) snd::test::Note("iteration %d: accepted an inconsistent chunk", i);
      }
    } else {
      ++refused;
    }
  }
  snd::test::Note("accepted %u, refused %u", accepted, refused);
  SND_CHECK_EQ(bad, 0u);
  SND_CHECK(refused > 1000u);
  // Null with a non-zero size, and an empty chunk.
  SND_CHECK(!patch::ReadPatchChunk(nullptr, 64, &progs, &links));
  SND_CHECK(!patch::ReadPatchChunk(good.data(), 0, &progs, &links));
}
