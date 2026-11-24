// The material table screen: materials and their numbers (renumbering said), single events,
// footsteps, defaults, the symmetric pair table (a scrolled 8 x 8 window), the resolution of a
// contact, the strength curves and a test input against EvaluateStrength.
#include <cmath>

#include "snd/test/test.hpp"
#include "snd/tool/material_editor.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {

const std::string kPhys = doctest::G(210);

std::map<std::string, std::string> Files(int materials = 2) {
  using doctest::G;
  std::map<std::string, std::string> f = doctest::BaseFiles();
  for (int i = 0; i < 5; ++i) {
    f["events/M" + std::to_string(i) + ".json"] = "{\"type\": \"event\", \"guid\": \"" + G(200 + i) + "\", \"name\": \"SE/M" +
                                                  std::to_string(i) + "\", \"bank\": \"Main\"}";
  }
  std::string mats = "\"metal\", \"wood\"";
  for (int i = 2; i < materials; ++i) mats += ", \"m" + std::to_string(i) + "\"";
  f["materials/Phys.json"] = "{\"type\": \"materialTable\", \"guid\": \"" + G(210) + "\", \"name\": \"Phys/Default\", \"bank\": \"Main\", "
                             "\"materials\": [" + mats + "], \"single\": {\"metal\": {\"impact\": \"SE/M0\"}}}";
  return f;
}

}  // namespace

SND_TEST(ToolUiMaterial, MaterialsSinglesPairsAndResolution) {
  Editor t(ui::Screen::Material, 1280, 800, 1.0f, Files());
  SND_REQUIRE(t.app.view().material.table == kPhys);
  MaterialEditor me(t.doc, kPhys);
  SND_CHECK(t.app.view().material.material == "metal");
  SND_CHECK(t.Label("ma.mat.0").find("0: metal") != std::string::npos);
  SND_CHECK(t.Label("ma.p.single0").find("SE/M0") == 0u);
  // A new material at the end keeps the numbers; moving it up renumbers.
  SND_REQUIRE(t.Click("ma.mat.add"));
  SND_CHECK_EQ(me.Materials().size(), 3u);
  SND_CHECK(t.app.view().material.material == "material1");
  SND_REQUIRE(t.Click("ma.mat.up"));
  SND_CHECK_EQ(me.IndexOf("material1"), 1);
  SND_CHECK(t.Label("ma.renumbered").find("1->2") != std::string::npos);
  // Singles and footsteps of wood.
  SND_REQUIRE(t.Click("ma.mat.2"));
  SND_CHECK(t.app.view().material.material == "wood");
  SND_REQUIRE(t.Pick("ma.p.single1", "SE/M1"));
  SND_CHECK_EQ(me.Single("wood", ContactKind::Slide), doctest::G(201));
  SND_REQUIRE(t.Pick("ma.p.foot", "SE/M3"));
  SND_CHECK_EQ(me.Footstep("wood"), doctest::G(203));
  // The pair cell metal x wood (row 0, column 2), shown on both sides.
  SND_REQUIRE(t.Pick("ma.pair.0.2", "SE/M2"));
  SND_CHECK_EQ(me.Pair("metal", "wood"), doctest::G(202));
  SND_CHECK(t.Label("ma.pair.2.0").find("SE/M2") == 0u);
  // Resolution of metal x wood: the pair.
  SND_REQUIRE(t.Pick("ma.res.b", "2: wood"));
  SND_CHECK(t.Label("ma.res.text").find("pair: SE/M2") != std::string::npos);
  SND_REQUIRE(t.Pick("ma.def3", "SE/M4"));
  SND_CHECK_EQ(me.Default(ContactKind::Break), doctest::G(204));
  SND_CHECK(t.Has("ma.fb.title"));
}

SND_TEST(ToolUiMaterial, StrengthCurvesAndTestInput) {
  Editor t(ui::Screen::Material, 1280, 800, 1.0f, Files());
  MaterialEditor me(t.doc, kPhys);
  SND_REQUIRE(t.Click("ma.p.imp3.inc"));  // full velocity 100 -> 110
  SND_CHECK_NEAR(me.Curves("metal").impulseToVelocity[3], 110.0f, 1e-3f);
  SND_REQUIRE(t.Click("ma.p.ms1.inc"));
  SND_CHECK_NEAR(me.Curves("metal").massToPitchCents[1], 10.0f, 1e-4f);
  SND_REQUIRE(t.Click("ma.t.impulse"));
  t.ClearEntry();
  t.Type("55");
  const StrengthResult r = EvaluateStrength(me.Curves("metal"), 55.0f, 1.0f, 1.0f);
  char b[64];
  std::snprintf(b, sizeof b, "velocity %.3f", r.velocity);
  SND_CHECK(t.Label("ma.t.result").find(b) != std::string::npos);
  SND_CHECK(t.Has("ma.t.plot"));
}

SND_TEST(ToolUiMaterial, ManyMaterialsScrollThePairWindow) {
  Editor t(ui::Screen::Material, 1280, 800, 1.0f, Files(20));
  SND_CHECK(t.Has("ma.pair.7.7"));
  SND_CHECK(!t.Has("ma.pair.8.0"));
  SND_CHECK(t.Label("ma.pair.row.0").find("0 metal") == 0u);
  SND_REQUIRE(t.Click("ma.pair.down"));
  SND_CHECK(t.Label("ma.pair.row.0").find("1 wood") == 0u);
  for (int i = 0; i < 30; ++i) t.Click("ma.pair.right");
  SND_CHECK_EQ(t.app.view().material.pairCol, 12u);  // 20 - 8: clamped
  SND_CHECK(t.app.runtime().elements().size() < ui::kMaxElements);
}
