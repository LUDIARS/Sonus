// The mixer model against a real game: core System + live link server over the
// in-memory transport (tool_fixture.hpp). Analytic checks on the output: the
// DC signal of "Tone" (bus Sfx, category SE) and "Pad" (bus Music) scales
// exactly with the volumes the tool sends.
#include <cmath>

#include "snd/test/test.hpp"
#include "snd/tool/session.hpp"
#include "tool_fixture.hpp"

using namespace snd::tool;
using tooltest::Bus;
using tooltest::Cat;
using tooltest::LiveRig;
using tooltest::Ratio;
using tooltest::ToolProject;

namespace {

constexpr double kWet = 1.5848931924611107e-05;  // 10^(-96/20): the delay's wet path

struct Setup {
  LiveRig rig;
  ToolProject tool;
  Session s;
  float base = 0.0f;  // Tone alone
  explicit Setup(const tooltest::MixVariant& toolVariant = {}, snd::livelink::ServerDesc sd = {},
                 MismatchPolicy policy = MismatchPolicy::SendDiffs)
      : rig(tooltest::MixVariant{}, sd), tool(toolVariant) {
    SND_REQUIRE(rig.ok && tool.ok);
    s.UseProject(tool.project);
    rig.Play("Tone");
    base = rig.Level();
    SND_REQUIRE(base > 0.1f);
    ConnectionDesc d = rig.Conn(tool.project.buildHash());
    d.policy = policy;
    SND_REQUIRE_EQ(s.Connect(d), SND_OK);
  }
  bool Online() {
    return rig.Run(s, [&] { return s.connection().state() == LinkState::Online && s.connection().sessions() > 0u; });
  }
  // Ticks until every cell is confirmed, then lets the level settle.
  bool Synced() {
    const bool ok = rig.Run(s, [&] {
      return s.sync().Count(CellState::Pending) == 0u && s.sync().Count(CellState::Unsent) == 0u;
    });
    rig.Settle(s, 8);
    return ok;
  }
  double Rel() const { return static_cast<double>(rig.Level()) / static_cast<double>(base); }
};

}  // namespace

SND_TEST(ToolLiveMixer, FaderSetsTheBusVolumeAndTheAckConfirmsIt) {
  Setup t;
  SND_REQUIRE(t.Online());
  SND_CHECK(!t.s.connection().mismatch());
  const std::int32_t sfx = Bus(t.tool.project, "Sfx");
  SND_REQUIRE(t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -6.0f));
  t.s.Tick(tooltest::Now());
  SND_CHECK_EQ(t.s.sync().Count(CellState::Pending), 1u);  // sent, not yet confirmed
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), Ratio(-6.0f), 1e-4);
  bool confirmed = false;
  for (std::size_t i = 0; i < t.s.sync().cells().size(); ++i) {
    const CellStatus& st = t.s.sync().cells()[i];
    if (st.everSent) confirmed = st.confirmed == -6.0f && st.state == CellState::Synced;
  }
  SND_CHECK(confirmed);
  SND_CHECK_EQ(t.s.sync().stats().acked, 1u);
  // +12 dB (the top of the range).
  t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, 99.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), Ratio(12.0f), 1e-3);
  // The master fader scales everything.
  t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, 0.0f);
  t.s.mixer().Set(Target{Control::BusFader, Bus(t.tool.project, "Master"), 0, 0}, -20.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), Ratio(-20.0f), 1e-4);
}

SND_TEST(ToolLiveMixer, MuteIsSilenceAndUnmuteRestoresTheFader) {
  Setup t;
  SND_REQUIRE(t.Online());
  const std::int32_t sfx = Bus(t.tool.project, "Sfx");
  t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -10.0f);
  t.s.mixer().Set(Target{Control::BusMute, sfx, 0, 0}, 1.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_EQ(t.rig.Level(), 0.0f);  // gain(-144 dB) = 0 exactly
  for (float v : t.rig.f.out) SND_CHECK_EQ(v, 0.0f);
  t.s.mixer().Set(Target{Control::BusMute, sfx, 0, 0}, 0.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), Ratio(-10.0f), 1e-4);
}

SND_TEST(ToolLiveMixer, SoloMutesTheOtherBranches) {
  Setup t;
  SND_REQUIRE(t.Online());
  t.rig.Play("Pad");  // Music: same level as Tone
  const double both = t.Rel();
  SND_CHECK_NEAR(both, 2.0 / (1.0 + kWet), 1e-4);  // Pad has no delay on its bus
  const std::int32_t sfx = Bus(t.tool.project, "Sfx"), music = Bus(t.tool.project, "Music");
  t.s.mixer().Set(Target{Control::BusSolo, sfx, 0, 0}, 1.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), 1.0, 1e-4);  // Tone alone
  t.s.mixer().Set(Target{Control::BusSolo, music, 0, 0}, 1.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), both, 1e-4);
  t.s.mixer().Set(Target{Control::BusSolo, sfx, 0, 0}, 0.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), both - 1.0, 1e-4);  // Pad alone
  // Soloing the master keeps everything (every bus is below it).
  t.s.mixer().Set(Target{Control::BusSolo, music, 0, 0}, 0.0f);
  t.s.mixer().Set(Target{Control::BusSolo, Bus(t.tool.project, "Master"), 0, 0}, 1.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), both, 1e-4);
}

SND_TEST(ToolLiveMixer, UndoAndRedoReachTheGame) {
  Setup t;
  SND_REQUIRE(t.Online());
  const Target f{Control::BusFader, Bus(t.tool.project, "Sfx"), 0, 0};
  t.s.mixer().BeginGroup();  // a drag
  for (int i = 1; i <= 12; ++i) {
    t.s.mixer().Set(f, -static_cast<float>(i));
    t.s.Tick(tooltest::Now());  // sent while dragging
    t.rig.f.Step();
  }
  t.s.mixer().EndGroup();
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), Ratio(-12.0f), 1e-4);
  SND_CHECK(t.s.mixer().Undo());
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), 1.0, 1e-5);
  SND_CHECK(t.s.mixer().Redo());
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), Ratio(-12.0f), 1e-4);
  SND_CHECK(t.s.mixer().RevertAll());
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), 1.0, 1e-5);
  SND_CHECK(t.s.mixer().Overrides().empty());
}

SND_TEST(ToolLiveMixer, EffectParameterAndCategoryVolume) {
  Setup t;
  SND_REQUIRE(t.Online());
  t.rig.Play("Pad");
  const double both = t.Rel();
  const std::int32_t sfx = Bus(t.tool.project, "Sfx");
  // Delay DryDb (parameter 2): Tone becomes kDc * (g(dry) + g(wet)).
  SND_REQUIRE(t.s.mixer().Set(Target{Control::EffectParam, sfx, 0, 2}, -6.0f));
  SND_REQUIRE(t.Synced());
  const double tone = (Ratio(-6.0f) + kWet) / (1.0 + kWet);
  SND_CHECK_NEAR(t.Rel(), both - 1.0 + tone, 2e-4);
  // Category SE (Tone only) muted: Pad alone.
  SND_REQUIRE(t.s.mixer().Set(Target{Control::CategoryMute, Cat(t.tool.project, "SE"), 0, 0}, 1.0f));
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), both - 1.0, 1e-4);
  // Category fader -6 dB (unmuted).
  t.s.mixer().Set(Target{Control::CategoryMute, Cat(t.tool.project, "SE"), 0, 0}, 0.0f);
  t.s.mixer().Set(Target{Control::CategoryFader, Cat(t.tool.project, "SE"), 0, 0}, -6.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), both - 1.0 + tone * Ratio(-6.0f), 2e-4);
}

SND_TEST(ToolLiveMixer, StateSurvivesDisconnectAndReconnect) {
  snd::livelink::ServerDesc sd;
  sd.revertOnDisconnect = true;  // the game puts its bank values back when the tool leaves
  Setup t(tooltest::MixVariant{}, sd);
  SND_REQUIRE(t.Online());
  const Target f{Control::BusFader, Bus(t.tool.project, "Sfx"), 0, 0};
  t.s.mixer().Set(f, -6.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), Ratio(-6.0f), 1e-4);
  // Drop: the game reverts; the tool reconnects and sends its state again.
  t.s.connection().Drop();
  SND_REQUIRE(t.rig.Run(t.s, [&] { return t.s.connection().sessions() >= 2u; }));
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), Ratio(-6.0f), 1e-4);
  // Closed: edits wait (Unsent) and go out with the next connection.
  t.s.Disconnect();
  t.s.Tick(tooltest::Now());
  t.s.mixer().Set(f, -12.0f);
  t.s.mixer().Set(Target{Control::BusMute, Bus(t.tool.project, "Music"), 0, 0}, 1.0f);
  t.s.Tick(tooltest::Now());
  SND_CHECK_EQ(t.s.sync().Count(CellState::Unsent), 2u);
  t.rig.Settle(t.s, 16);
  SND_CHECK_NEAR(t.Rel(), 1.0, 1e-5);  // reverted by the game while the tool is away
  SND_REQUIRE_EQ(t.s.Connect(t.rig.Conn(t.tool.project.buildHash())), SND_OK);
  SND_REQUIRE(t.Online());
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.Rel(), Ratio(-12.0f), 1e-4);
}

SND_TEST(ToolLiveMixer, BuildMismatchSendsTheToolsValuesOrRefuses) {
  tooltest::MixVariant v;
  v.sfxDb = -3.0f;   // the tool's project differs from the game's bank
  v.extraBus = true; // and has a bus the game does not know
  {
    Setup t(v);
    SND_REQUIRE(t.Online());
    SND_CHECK(t.s.connection().mismatch());
    SND_REQUIRE(t.Synced());
    SND_CHECK_NEAR(t.Rel(), Ratio(-3.0f), 1e-4);  // the tool's Sfx volume reached the game
    SND_CHECK_EQ(t.s.sync().Count(CellState::Failed), 1u);  // "Extra": refused by the game
    SND_CHECK(t.s.sync().stats().lastError == SND_ERR_NOT_FOUND);
    bool reported = false;
    for (const std::string& m : t.s.messages()) reported = reported || m.find("refused") != std::string::npos;
    SND_CHECK(reported);
  }
  {
    Setup t(v, {}, MismatchPolicy::Refuse);
    SND_CHECK(t.rig.Run(t.s, [&] { return t.s.connection().state() == LinkState::Rejected; }));
    SND_CHECK(t.s.connection().mismatch());
    t.rig.Settle(t.s, 8);
    SND_CHECK_NEAR(t.Rel(), 1.0, 1e-6);  // nothing was sent
  }
}

SND_TEST(ToolLiveMixer, ParameterOverrideIsSentAndReleased) {
  Setup t;
  SND_REQUIRE(t.Online());
  const Target on{Control::ParamOverrideOn, 0, 0, 0}, val{Control::ParamOverride, 0, 0, 0};
  t.s.mixer().Set(val, 0.75f);
  t.s.mixer().Set(on, 1.0f);
  SND_REQUIRE(t.Synced());
  SND_REQUIRE_EQ(t.s.sync().params().size(), 1u);
  SND_CHECK(t.s.sync().params()[0].enabled);
  SND_CHECK_EQ(t.s.sync().params()[0].confirmed, 0.75f);
  SND_CHECK_EQ(t.s.sync().Count(CellState::Failed), 0u);
  t.s.mixer().Set(on, 0.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK(!t.s.sync().params()[0].enabled);
}
