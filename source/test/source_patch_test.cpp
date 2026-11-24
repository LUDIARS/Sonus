#include "snd/source/patch_program.hpp"
#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using namespace synthtest;

SND_TEST(SourcePatch, SineTimesConstantAndProgramValidation) {
  src::PatchNode nodes[4]{};
  nodes[0].op = src::PatchOp::Oscillator;
  nodes[0].arg[0] = 750;
  nodes[1].arg[0] = 0.5f;
  nodes[2] = {src::PatchOp::Multiply, 0, 1};
  nodes[3] = {src::PatchOp::Output, 2};
  std::uint8_t program[src::PatchProgramBytes(4)]{};
  const auto bytes = src::BuildPatchProgram(nodes, 4, program, sizeof(program));
  SND_REQUIRE_EQ(bytes, sizeof(program));
  for (unsigned i = 0; i < bytes; ++i) SND_CHECK(!src::ValidatePatchProgram(program, i));
  src::PatchParams p{program, static_cast<unsigned>(bytes), 0};
  Driver d(snd::SourceKind::Patch, {});
  SND_REQUIRE(d.Ok());
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  program[12 + 20 * 2 + 1] = 2;  // self-edge, start already owns a validated copy
  SND_CHECK(!src::ValidatePatchProgram(program, bytes));
  d.Render(v, 512, {}, 37);
  for (unsigned i = 0; i < 512; ++i) {
    SND_CHECK_NEAR(v.out[0][i], 0.5 * std::sin(i * 6.283185307179586 / 64), 1e-6);
    SND_CHECK_EQ(v.out[0][i], v.out[1][i]);
  }
}
