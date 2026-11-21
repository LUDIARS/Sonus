// Item revert and read-back (group P, protocol 1.1 §8.2) through the tool's Session against a real
// core and live link server (tool_fixture.hpp). The DC level of "Tone" (bus Sfx) shows what the
// game holds; ReadCell shows the override layer.
#include <cmath>

#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/session.hpp"
#include "tool_fixture.hpp"

using namespace snd::tool;
using tooltest::Bus;
using tooltest::LiveRig;
using tooltest::Ratio;
using tooltest::ToolProject;

namespace {

struct Setup {
  LiveRig rig;
  ToolProject tool;
  Session s;
  float base = 0.0f;
  Setup() : rig(tooltest::MixVariant{}), tool(tooltest::MixVariant{}) {
    SND_REQUIRE(rig.ok && tool.ok);
    s.UseProject(tool.project);
    rig.Play("Tone");
    base = rig.Level();
    SND_REQUIRE(base > 0.1f);
    SND_REQUIRE_EQ(s.Connect(rig.Conn(tool.project.buildHash())), SND_OK);
  }
  bool Online() {
    return rig.Run(s, [&] { return s.connection().state() == LinkState::Online && s.connection().sessions() > 0u; });
  }
  bool Synced() {
    const bool ok = rig.Run(s, [&] {
      return s.sync().Count(CellState::Pending) == 0u && s.sync().Count(CellState::Unsent) == 0u;
    });
    rig.Settle(s, 8);
    return ok;
  }
  bool ReadBack() {
    if (!s.ReadBack()) return false;
    return rig.Run(s, [&] { return s.sync().ReadsOutstanding() == 0u; });
  }
  const CellStatus& SfxCell() {
    CellAddress a;
    a.chunk = snd::bank_format::chunk::BUSS;
    a.runtimeId = snd::HashId("Sfx");
    a.column = snd::bank_format::col::BUSS_volumeDb;
    return s.sync().cells()[static_cast<std::size_t>(s.sync().Find(a))];
  }
  double Rel() const { return static_cast<double>(rig.Level()) / static_cast<double>(base); }
};

}  // namespace

SND_TEST(ToolLiveRevert, ItemRevertSendsRevertValueAndUndoSendsTheValueAgain) {
  Setup t;
  SND_REQUIRE(t.Online());
  const Target f{Control::BusFader, Bus(t.tool.project, "Sfx"), 0, 0};
  SND_REQUIRE(t.s.mixer().Set(f, -6.0f));
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), Ratio(-6.0f), 1e-4);
  // Read back: the game's override layer holds -6 over the bank's 0.
  SND_REQUIRE(t.ReadBack());
  SND_CHECK(t.SfxCell().readState == CellStatus::Read::Done);
  SND_CHECK(t.SfxCell().readOverridden);
  SND_CHECK_NEAR(t.SfxCell().readCurrent, -6.0, 1e-6);
  SND_CHECK_NEAR(t.SfxCell().readOriginal, 0.0, 1e-6);
  SND_CHECK_EQ(t.SfxCell().readCells, 1u);
  // Revert the item: one undo step on the mixer, RevertValue to the game.
  const std::size_t steps = t.s.mixer().history().UndoCount();
  std::vector<OverrideItem> items = t.s.mixer().Overrides();
  SND_REQUIRE_EQ(items.size(), 1u);
  SND_REQUIRE(t.s.RevertItem(items[0]));
  SND_CHECK_EQ(t.s.mixer().history().UndoCount(), steps + 1u);
  SND_REQUIRE(t.Synced());
  SND_CHECK_EQ(t.s.sync().reverts(), 1u);
  SND_CHECK(t.SfxCell().revertedLive);
  SND_CHECK_NEAR(t.Rel(), 1.0, 1e-5);
  SND_REQUIRE(t.ReadBack());
  SND_CHECK(!t.SfxCell().readOverridden);  // the game forgot the cell
  SND_CHECK_NEAR(t.SfxCell().readCurrent, 0.0, 1e-6);
  // Undo: the value goes again as an ordinary SetValue.
  SND_REQUIRE(t.s.mixer().Undo());
  SND_REQUIRE(t.Synced());
  SND_CHECK_EQ(t.s.sync().reverts(), 1u);
  SND_CHECK(!t.SfxCell().revertedLive);
  SND_CHECK_NEAR(t.Rel(), Ratio(-6.0f), 1e-4);
  SND_REQUIRE(t.ReadBack());
  SND_CHECK(t.SfxCell().readOverridden);
}

SND_TEST(ToolLiveRevert, RevertOnlyLeavesTheOtherControls) {
  Setup t;
  SND_REQUIRE(t.Online());
  const std::int32_t sfx = Bus(t.tool.project, "Sfx");
  SND_REQUIRE(t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -6.0f));
  SND_REQUIRE(t.s.mixer().Set(Target{Control::BusMute, sfx, 0, 0}, 1.0f));
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), 0.0, 1e-6);
  // Only the mute goes back: the fader's -6 dB stays (Revert(item) would clear both).
  SND_REQUIRE(t.s.RevertOnly(Target{Control::BusMute, sfx, 0, 0}));
  SND_REQUIRE(t.Synced());
  SND_CHECK_EQ(t.s.mixer().Get(Target{Control::BusMute, sfx, 0, 0}), 0.0f);
  SND_CHECK_EQ(t.s.mixer().Get(Target{Control::BusFader, sfx, 0, 0}), -6.0f);
  SND_CHECK_NEAR(t.Rel(), Ratio(-6.0f), 1e-4);
  // The cell is not at the bank value: SetValue (no revert).
  SND_CHECK_EQ(t.s.sync().reverts(), 0u);
  // Now the fader alone: back to the bank value, by RevertValue.
  SND_REQUIRE(t.s.RevertOnly(Target{Control::BusFader, sfx, 0, 0}));
  SND_REQUIRE(t.Synced());
  SND_CHECK_EQ(t.s.sync().reverts(), 1u);
  SND_CHECK_NEAR(t.Rel(), 1.0, 1e-5);
  SND_CHECK(!t.s.RevertOnly(Target{Control::BusFader, sfx, 0, 0}));  // nothing to revert
}
