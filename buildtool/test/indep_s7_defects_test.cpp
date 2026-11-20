// Defects found by the independent tests of group M in the build tool (patch.hpp). FAILS ON
// PURPOSE (see spec/handoff/parallel/C-notes/model-indep.md); each test is its own CTest test.
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
std::vector<std::uint8_t> TwoPrograms() {
  json::Value v;
  json::ParseError pe;
  json::Parse(R"({"type": "patch", "name": "P", "nodes": [{"id": "o", "kind": "oscillator"}, {"id": "a", "kind": "amp"}, {"id": "out", "kind": "output"}],
                  "connections": [{"from": "o", "to": "a", "input": "in"}, {"from": "a", "to": "out"}]})",
              &v, &pe);
  patch::Program p;
  std::vector<patch::Problem> pr;
  patch::Compile(v, snd::HashId("P"), &p, &pr);
  return patch::WritePatchChunk({p, p}, {{0, 1}});
}
}  // namespace

// bank-format.md §9: "inputs[i] は同じプログラムの中のノード番号（0 から）、無ければ 0xFFFFFFFF" and the
// link table names a program of the chunk. ReadPatchChunk returns true for a node input that
// points past its program and for a link to a program that does not exist, so a reader trusting
// its result indexes out of range.
SND_TEST(IndepS7Defect, Defect_ReadPatchChunkAcceptsOutOfRangeIndices) {
  const auto good = TwoPrograms();
  std::vector<patch::Program> back;
  std::vector<patch::Link> links;
  SND_REQUIRE(patch::ReadPatchChunk(good.data(), good.size(), &back, &links));
  auto input = good;
  const std::uint32_t bad = 1000;
  std::memcpy(input.data() + 16 + 16 * 2 + 16, &bad, 4);  // program 0, node 0, inputs[0]
  SND_CHECK(!patch::ReadPatchChunk(input.data(), input.size(), &back, &links));
  // A node reading a later node (not dependency order).
  auto later = good;
  const std::uint32_t two = 2;
  std::memcpy(later.data() + 16 + 16 * 2 + 16, &two, 4);
  SND_CHECK(!patch::ReadPatchChunk(later.data(), later.size(), &back, &links));
  auto link = good;
  SND_REQUIRE(link.size() >= 24u);
  const std::uint32_t seven = 7;
  const std::size_t at = link.size() - 4u;  // the last link's program
  for (int k = 0; k < 4; ++k) link[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(seven >> (8 * k));  // program 7 of 2
  SND_CHECK(!patch::ReadPatchChunk(link.data(), link.size(), &back, &links));
}
