// Independent tests (tool/test/indep_properties.md A, B): value sending against a
// scripted game (indep_scripted_game.hpp) that decides every Ack, and the mixer
// rules (mute, solo, revert, undo) against a real core + live link server whose
// DC output level shows what the game actually applied.
#include <cmath>
#include <set>

#include "indep_scripted_game.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "tool_fixture.hpp"

using namespace snd::tool;
using indep::Drive;
using indep::DriveFor;
using indep::GameFrame;
using indep::ScriptedGame;
using indep::ValueF32;
namespace ll = snd::livelink;
namespace bf = snd::bank_format;

namespace {

struct Rig {
  tooltest::ToolProject tool;
  ScriptedGame g;
  Session s;
  explicit Rig(MismatchPolicy policy = MismatchPolicy::SendDiffs,
               ll::HelloStatus status = ll::HelloStatus::Accepted) {
    SND_REQUIRE(tool.ok && g.ok);
    g.status = status;
    s.UseProject(tool.project);
    ConnectionDesc d = indep::GameConn(g, tool.project.buildHash());
    d.policy = policy;
    SND_REQUIRE_EQ(s.Connect(d), SND_OK);
  }
  std::int32_t B(const char* n) const { return tooltest::Bus(tool.project, n); }
  bool Online() {
    return Drive(s, g, [&] { return s.connection().state() == LinkState::Online; });
  }
  bool Settled() {
    return Drive(s, g, [&] { return s.sync().Count(CellState::Pending) == 0u && s.sync().Count(CellState::Unsent) == 0u; });
  }
  std::vector<ll::SetValue> Sets(std::size_t from = 0) const {
    std::vector<ll::SetValue> out;
    for (const GameFrame& f : g.Of(ll::MessageType::SetValue, from)) out.push_back(f.As<ll::SetValue>());
    return out;
  }
  const CellStatus& Status(Control c, std::int32_t a) const {
    const std::vector<Cell> cells = s.mixer().Cells();
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (cells[i].owner.control == c && cells[i].owner.a == a) return s.sync().cells()[i];
    }
    static CellStatus none;
    return none;
  }
};

bool IsBusVolume(const ll::SetValue& v, const char* bus) {
  return v.chunk == bf::chunk::BUSS && v.column == bf::col::BUSS_volumeDb && v.runtimeId == snd::HashId(bus) &&
         v.valueType == static_cast<std::uint32_t>(ll::ValueType::F32);
}

}  // namespace

// A1: nothing differs and nothing was sent -> nothing is sent; several moves within one
// frame are one message with the last value; an unchanged value is never re-sent.
SND_TEST(IndepToolSync, OnlyChangedValuesAreSentOncePerFrame) {
  Rig t;
  SND_REQUIRE(t.Online());
  DriveFor(t.s, t.g, 100);
  SND_CHECK_EQ(t.Sets().size(), 0u);
  const Target sfx{Control::BusFader, t.B("Sfx"), 0, 0};
  t.s.mixer().Set(sfx, -3.0f);
  t.s.mixer().Set(sfx, -4.0f);
  t.s.mixer().Set(sfx, -6.0f);
  SND_REQUIRE(t.Settled());
  std::vector<ll::SetValue> v = t.Sets();
  SND_REQUIRE_EQ(v.size(), 1u);
  SND_CHECK(IsBusVolume(v[0], "Sfx"));
  SND_CHECK_EQ(ValueF32(v[0]), -6.0f);
  // GUID of the owner (protocol §5): the bus's project GUID.
  const BusDesc& bd = t.tool.project.buses()[static_cast<std::size_t>(t.B("Sfx"))];
  SND_CHECK_EQ(v[0].guidLo, bd.guid.lo);
  SND_CHECK_EQ(v[0].guidHi, bd.guid.hi);
  DriveFor(t.s, t.g, 100);
  SND_CHECK(!t.s.mixer().Set(sfx, -6.0f));
  // Away and back within one frame: nothing to send.
  t.s.mixer().Set(sfx, -5.0f);
  t.s.mixer().Set(sfx, -6.0f);
  DriveFor(t.s, t.g, 100);
  SND_CHECK_EQ(t.Sets().size(), 1u);
  const CellStatus& st = t.Status(Control::BusFader, t.B("Sfx"));
  SND_CHECK(st.state == CellState::Synced);
  SND_CHECK_EQ(st.confirmed, -6.0f);
}

// A3: SND_ERR_COMMAND_BUFFER_FULL is not a refusal: the same value goes again.
SND_TEST(IndepToolSync, CommandBufferFullResendsTheSameValue) {
  Rig t;
  int n = 0;
  t.g.ackResult = [&](const GameFrame& f) {
    if (!f.Is(ll::MessageType::SetValue)) return static_cast<std::int32_t>(SND_OK);
    return static_cast<std::int32_t>(++n <= 2 ? SND_ERR_COMMAND_BUFFER_FULL : SND_OK);
  };
  SND_REQUIRE(t.Online());
  t.s.mixer().Set(Target{Control::BusFader, t.B("Sfx"), 0, 0}, -6.0f);
  SND_REQUIRE(t.Settled());
  const std::vector<ll::SetValue> v = t.Sets();
  SND_CHECK_EQ(v.size(), 3u);
  for (const ll::SetValue& s : v) SND_CHECK(IsBusVolume(s, "Sfx") && ValueF32(s) == -6.0f);
  SND_CHECK_EQ(t.s.sync().stats().refused, 0u);
  SND_CHECK_EQ(t.s.sync().Count(CellState::Failed), 0u);
  SND_CHECK_EQ(t.Status(Control::BusFader, t.B("Sfx")).confirmed, -6.0f);
}

// A4: any other error leaves the cell refused and it is not sent again until it changes.
SND_TEST(IndepToolSync, OtherErrorsAreRefusedUntilTheValueChanges) {
  Rig t;
  t.g.ackResult = [](const GameFrame& f) {
    if (f.Is(ll::MessageType::SetValue) && ValueF32(f.As<ll::SetValue>()) == -6.0f) {
      return static_cast<std::int32_t>(SND_ERR_NOT_FOUND);
    }
    return static_cast<std::int32_t>(SND_OK);
  };
  SND_REQUIRE(t.Online());
  const Target sfx{Control::BusFader, t.B("Sfx"), 0, 0};
  t.s.mixer().Set(sfx, -6.0f);
  SND_REQUIRE(Drive(t.s, t.g, [&] { return t.s.sync().Count(CellState::Failed) == 1u; }));
  SND_CHECK_EQ(t.Status(Control::BusFader, t.B("Sfx")).error, SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(t.s.sync().stats().refused, 1u);
  DriveFor(t.s, t.g, 150);
  SND_CHECK_EQ(t.Sets().size(), 1u);  // not re-sent
  t.s.mixer().Set(sfx, -7.0f);
  SND_REQUIRE(t.Settled());
  SND_CHECK_EQ(t.Sets().size(), 2u);
  SND_CHECK_EQ(t.s.sync().Count(CellState::Failed), 0u);
  SND_CHECK_EQ(t.Status(Control::BusFader, t.B("Sfx")).confirmed, -7.0f);
}

// A10: an Ack for a superseded send confirms nothing and refuses nothing.
SND_TEST(IndepToolSync, AckOfAnOlderSendDoesNotConfirmTheNewValue) {
  Rig t;
  SND_REQUIRE(t.Online());
  t.g.holdAcks = true;
  t.g.ackResult = [](const GameFrame& f) {
    return static_cast<std::int32_t>(ValueF32(f.As<ll::SetValue>()) == -6.0f ? SND_ERR_INVALID_ARGUMENT : SND_OK);
  };
  const Target sfx{Control::BusFader, t.B("Sfx"), 0, 0};
  t.s.mixer().Set(sfx, -6.0f);
  SND_REQUIRE(Drive(t.s, t.g, [&] { return t.g.heldAcks.size() == 1u; }));
  t.s.mixer().Set(sfx, -7.0f);
  SND_REQUIRE(Drive(t.s, t.g, [&] { return t.g.heldAcks.size() == 2u; }));
  const ScriptedGame::HeldAck second = t.g.heldAcks[1];
  t.g.heldAcks.pop_back();
  t.g.ReleaseAcks();  // only the first (an error for -6)
  DriveFor(t.s, t.g, 100);
  SND_CHECK(t.Status(Control::BusFader, t.B("Sfx")).state == CellState::Pending);
  SND_CHECK_EQ(t.s.sync().Count(CellState::Failed), 0u);
  t.g.heldAcks.push_back(second);
  t.g.ReleaseAcks();
  t.g.holdAcks = false;
  SND_REQUIRE(t.Settled());
  SND_CHECK_EQ(t.Status(Control::BusFader, t.B("Sfx")).confirmed, -7.0f);
  SND_CHECK_EQ(t.s.sync().Count(CellState::Failed), 0u);
}

// A5 / A6 / G2: each new session gets every value that differs from the bank and every
// value ever sent (also when it is back at the bank value), and nothing else; edits made
// while disconnected wait and go on the next connection.
SND_TEST(IndepToolSync, NewSessionResendsDifferingAndEverSentValuesOnly) {
  Rig t;
  SND_REQUIRE(t.Online());
  t.s.mixer().Set(Target{Control::BusFader, t.B("Sfx"), 0, 0}, -6.0f);
  SND_REQUIRE(t.Settled());
  t.s.mixer().Set(Target{Control::BusFader, t.B("Music"), 0, 0}, -3.0f);
  SND_REQUIRE(t.Settled());
  t.s.mixer().Set(Target{Control::BusFader, t.B("Music"), 0, 0}, 0.0f);  // back to the bank value
  SND_REQUIRE(t.Settled());
  SND_CHECK_EQ(t.Sets().size(), 3u);
  // The game restarts: the connection drops and nobody listens for a while.
  t.g.autoAccept = false;
  t.g.Drop();
  SND_REQUIRE(Drive(t.s, t.g, [&] { return t.s.connection().state() != LinkState::Online; }));
  const std::size_t mark = t.g.frames.size();
  t.s.mixer().Set(Target{Control::BusFader, t.B("Master"), 0, 0}, -1.0f);  // offline edit
  DriveFor(t.s, t.g, 100);
  SND_CHECK_EQ(t.g.frames.size(), mark);
  SND_CHECK(t.Status(Control::BusFader, t.B("Master")).state == CellState::Unsent);
  t.g.autoAccept = true;
  SND_REQUIRE(Drive(t.s, t.g, [&] { return t.s.connection().sessions() == 2u; }));
  SND_REQUIRE(t.Settled());
  std::set<std::uint32_t> ids;
  for (const ll::SetValue& v : t.Sets(mark)) {
    ids.insert(v.runtimeId);
    if (IsBusVolume(v, "Sfx")) SND_CHECK_EQ(ValueF32(v), -6.0f);
    if (IsBusVolume(v, "Music")) SND_CHECK_EQ(ValueF32(v), 0.0f);
    if (IsBusVolume(v, "Master")) SND_CHECK_EQ(ValueF32(v), -1.0f);
    SND_CHECK(v.chunk == bf::chunk::BUSS);  // no effect parameter / category was ever touched
  }
  SND_CHECK(ids == (std::set<std::uint32_t>{snd::HashId("Sfx"), snd::HashId("Music"), snd::HashId("Master")}));
  SND_CHECK_EQ(t.Sets(mark).size(), 3u);
}

// A7: a build mismatch is answered with "send diffs" and then every managed value is sent.
SND_TEST(IndepToolSync, BuildMismatchSendsEveryManagedValue) {
  Rig t(MismatchPolicy::SendDiffs, ll::HelloStatus::BuildMismatch);
  SND_REQUIRE(t.Online());
  SND_REQUIRE(t.Settled());
  const std::vector<GameFrame> sd = t.g.Of(ll::MessageType::SyncDecision);
  SND_REQUIRE_EQ(sd.size(), 1u);
  SND_CHECK_EQ(sd[0].As<ll::SyncDecision>().mode, static_cast<std::uint32_t>(ll::SyncMode::SendDiffs));
  SND_CHECK(t.s.connection().mismatch());
  const std::vector<Cell> cells = t.s.mixer().Cells();
  const std::vector<ll::SetValue> v = t.Sets();
  SND_CHECK_EQ(v.size(), cells.size());
  for (const Cell& c : cells) {
    bool found = false;
    for (const ll::SetValue& s : v) {
      found = found || (s.chunk == c.address.chunk && s.runtimeId == c.address.runtimeId &&
                        s.column == c.address.column && s.index == c.address.index && ValueF32(s) == c.value);
    }
    SND_CHECK(found);
  }
}

// A7: --refuse-mismatch answers "disconnect" and does not try again.
SND_TEST(IndepToolSync, RefusedMismatchDisconnectsAndDoesNotRetry) {
  Rig t(MismatchPolicy::Refuse, ll::HelloStatus::BuildMismatch);
  SND_REQUIRE(Drive(t.s, t.g, [&] { return t.s.connection().state() == LinkState::Rejected; }));
  const std::vector<GameFrame> sd = t.g.Of(ll::MessageType::SyncDecision);
  SND_REQUIRE_EQ(sd.size(), 1u);
  SND_CHECK_EQ(sd[0].As<ll::SyncDecision>().mode, static_cast<std::uint32_t>(ll::SyncMode::Disconnect));
  t.s.mixer().Set(Target{Control::BusFader, t.B("Sfx"), 0, 0}, -6.0f);
  DriveFor(t.s, t.g, 400);  // well past reconnectDelayMs (20 ms)
  SND_CHECK_EQ(t.g.hellos, 1u);
  SND_CHECK_EQ(t.Sets().size(), 0u);
  SND_CHECK(t.s.connection().state() == LinkState::Rejected);
  SND_CHECK_EQ(t.s.connection().sessions(), 0u);
}

// B2: mute sends exactly -144 dB and keeps the fader; unmute sends the fader again.
SND_TEST(IndepToolSync, MuteSendsMinus144AndKeepsTheFader) {
  Rig t;
  SND_REQUIRE(t.Online());
  const std::int32_t sfx = t.B("Sfx");
  t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -10.0f);
  SND_REQUIRE(t.Settled());
  t.s.mixer().Set(Target{Control::BusMute, sfx, 0, 0}, 1.0f);
  SND_REQUIRE(t.Settled());
  std::vector<ll::SetValue> v = t.Sets();
  SND_REQUIRE(!v.empty());
  SND_CHECK(IsBusVolume(v.back(), "Sfx"));
  SND_CHECK_EQ(ValueF32(v.back()), -144.0f);
  SND_CHECK_EQ(t.s.mixer().buses()[static_cast<std::size_t>(sfx)].faderDb, -10.0f);
  // Moving the fader while muted sends nothing (the game keeps -144).
  t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -8.0f);
  DriveFor(t.s, t.g, 80);
  SND_CHECK_EQ(t.Sets().size(), v.size());
  t.s.mixer().Set(Target{Control::BusMute, sfx, 0, 0}, 0.0f);
  SND_REQUIRE(t.Settled());
  v = t.Sets();
  SND_CHECK_EQ(ValueF32(v.back()), -8.0f);
  // Category mute: CATG volume -144.
  const std::int32_t se = tooltest::Cat(t.tool.project, "SE");
  t.s.mixer().Set(Target{Control::CategoryMute, se, 0, 0}, 1.0f);
  SND_REQUIRE(t.Settled());
  v = t.Sets();
  SND_CHECK(v.back().chunk == bf::chunk::CATG && v.back().runtimeId == snd::HashId("SE"));
  SND_CHECK_EQ(ValueF32(v.back()), -144.0f);
}

// --- real game: what the core applies (DC levels) ------------------------------------------

namespace {

constexpr double kWet = 1.5848931924611107e-05;  // the Sfx delay's wet path, 10^(-96/20)

struct Live {
  tooltest::LiveRig rig;
  tooltest::ToolProject tool;
  Session s;
  double tone = 0.0, pad = 0.0;  // levels of Tone (Sfx) and Pad (Music) alone at 0 dB
  Live() {
    SND_REQUIRE(rig.ok && tool.ok);
    s.UseProject(tool.project);
    rig.Play("Tone");
    tone = rig.Level();
    pad = tone / (1.0 + kWet);  // the same DC without the delay's wet path
    rig.Play("Pad");
    SND_REQUIRE_EQ(s.Connect(rig.Conn(tool.project.buildHash())), SND_OK);
    SND_REQUIRE(rig.Run(s, [&] { return s.connection().state() == LinkState::Online; }));
  }
  std::int32_t B(const char* n) const { return tooltest::Bus(tool.project, n); }
  void Set(Control c, const char* bus, float v) { s.mixer().Set(Target{c, B(bus), 0, 0}, v); }
  bool Synced() {
    const bool ok = rig.Run(s, [&] {
      return s.sync().Count(CellState::Pending) == 0u && s.sync().Count(CellState::Unsent) == 0u;
    });
    rig.Settle(s, 8);
    return ok;
  }
  double L() const { return static_cast<double>(rig.Level()); }
};

}  // namespace

// B3 / B4: solo + mute combinations and their release, as heard in the game.
SND_TEST(IndepToolLive, SoloAndMuteCombinationsAndRelease) {
  Live t;
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.L(), t.tone + t.pad, 1e-5);
  t.Set(Control::BusFader, "Sfx", -6.0f);
  t.Set(Control::BusFader, "Music", -3.0f);
  t.Set(Control::BusSolo, "Sfx", 1.0f);  // Music is solo-muted
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.L(), t.tone * tooltest::Ratio(-6.0f), 1e-5);
  t.Set(Control::BusMute, "Sfx", 1.0f);  // soloed and muted: silence
  SND_REQUIRE(t.Synced());
  SND_CHECK_EQ(t.L(), 0.0);
  t.Set(Control::BusSolo, "Music", 1.0f);  // two solos: Music audible, Sfx still muted
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.L(), t.pad * tooltest::Ratio(-3.0f), 1e-5);
  t.Set(Control::BusSolo, "Sfx", 0.0f);
  t.Set(Control::BusSolo, "Music", 0.0f);
  t.Set(Control::BusMute, "Sfx", 0.0f);
  SND_REQUIRE(t.Synced());  // every fader back as it was
  SND_CHECK_NEAR(t.L(), t.tone * tooltest::Ratio(-6.0f) + t.pad * tooltest::Ratio(-3.0f), 1e-5);
  // Soloing the master silences nothing (everything is below it).
  t.Set(Control::BusSolo, "Master", 1.0f);
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.L(), t.tone * tooltest::Ratio(-6.0f) + t.pad * tooltest::Ratio(-3.0f), 1e-5);
}

// B8 / B9 / C3 / C5: Revert, Revert all and their undo reach the game.
SND_TEST(IndepToolLive, RevertRevertAllAndUndoReachTheGame) {
  Live t;
  SND_REQUIRE(t.Synced());
  t.Set(Control::BusFader, "Sfx", -6.0f);
  t.Set(Control::BusSolo, "Music", 1.0f);  // Sfx solo-muted
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.L(), t.pad, 1e-5);
  std::vector<OverrideItem> before = t.s.mixer().Overrides();
  SND_CHECK(t.s.mixer().RevertAll());
  SND_CHECK(t.s.mixer().Overrides().empty());
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.L(), t.tone + t.pad, 1e-5);
  SND_CHECK(t.s.mixer().Undo());  // one step back to the solo and the -6 dB fader
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.L(), t.pad, 1e-5);
  const std::vector<OverrideItem> after = t.s.mixer().Overrides();
  SND_REQUIRE_EQ(after.size(), before.size());
  for (std::size_t i = 0; i < after.size(); ++i) {
    SND_CHECK(after[i].key == before[i].key);
    SND_CHECK_EQ(after[i].value, before[i].value);
  }
  SND_CHECK(t.s.mixer().Redo());
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.L(), t.tone + t.pad, 1e-5);
  SND_CHECK(t.s.mixer().Undo());
  // Revert of the solo-muted Sfx: its fader back to 0 dB and every solo off.
  bool reverted = false;
  for (const OverrideItem& it : t.s.mixer().Overrides()) {
    if (it.target.control == Control::BusFader && it.target.a == t.B("Sfx")) {
      SND_CHECK(it.muted);
      reverted = t.s.mixer().Revert(it);
    }
  }
  SND_CHECK(reverted);
  SND_CHECK(!t.s.mixer().AnySolo());
  SND_CHECK(t.s.mixer().Overrides().empty());
  SND_REQUIRE(t.Synced());
  SND_CHECK_NEAR(t.L(), t.tone + t.pad, 1e-5);
}
