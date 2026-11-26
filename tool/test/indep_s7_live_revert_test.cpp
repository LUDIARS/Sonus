// Independent tests of group P: item revert and read-back through the Session (protocol 1.1
// §8.2), and the detail profiler's CPU numbers (D-174) against the real runtime (preview engine).
#include <cmath>

#include "indep_s7_live_fixture.hpp"
#include "snd/tool/session.hpp"

using namespace snd::tool;
using namespace ilive;

namespace {
struct SessionRig {
  ProjectDocument doc;
  DocBuild build;
  std::unique_ptr<preview::PreviewEngine> engine;
  Project project;
  Session s;
  bool ok = false;
  SessionRig() {
    Load(doc, false);
    build = BuildDocument(doc);
    std::string err;
    engine = preview::PreviewEngine::Create({}, &err);
    if (!build.ok || engine == nullptr || !engine->LoadBuild(build, &err) || !project.Load(doc)) return;
    s.UseProject(project);
    ConnectionDesc d;
    d.connector = engine->connector();
    d.reconnectDelayMs = 20;
    d.projectBuildHash = build.buildHash;
    ok = s.Connect(d) == SND_OK;
  }
  ~SessionRig() { s.Disconnect(); }
  template <class Pred>
  bool Run(Pred&& pred, std::uint32_t ms = 20000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > end) return false;
      engine->Render(1, nullptr);
      s.Tick();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
  }
  bool Synced() {
    s.Tick();
    return Run([&] { return s.sync().Count(CellState::Pending) == 0u && s.sync().Count(CellState::Unsent) == 0u; });
  }
  bool Read(ll::CellValue* v) {
    ll::SetValue cell{};
    cell.runtimeId = snd::HashId("Sfx");
    cell.chunk = bf::chunk::BUSS;
    cell.column = bf::col::BUSS_volumeDb;
    ll::Client* c = s.connection().client();
    const std::uint32_t seq = c->ReadCell(cell);
    return seq != 0u && Run([&] { return c->TakeCell(seq, v); });
  }
};
}  // namespace

// RevertOnly: the game forgets the override (RevertValue: not overridden, back to the bank value);
// Undo sends the value again (an ordinary SetValue: overridden); the read-back fills every cell.
SND_TEST(IndepS7LiveRevert, RevertOnlyForgetsTheOverrideAndUndoSetsItAgain) {
  SessionRig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Run([&] { return r.s.connection().state() == LinkState::Online; }));
  const Target fader{Control::BusFader, r.project.FindBus(snd::HashId("Sfx")), 0, 0};
  SND_REQUIRE(r.s.mixer().Set(fader, -12.0f));
  SND_REQUIRE(r.Synced());
  ll::CellValue v{};
  SND_REQUIRE(r.Read(&v));
  SND_CHECK_EQ(F32Of(v.current), -12.0f);
  SND_CHECK_EQ(F32Of(v.original), -1.0f);
  SND_CHECK((v.flags & ll::kCellOverridden) != 0u);
  SND_REQUIRE(r.s.RevertOnly(fader));
  SND_REQUIRE(r.Synced());
  SND_REQUIRE(r.Read(&v));
  SND_CHECK_EQ(F32Of(v.current), -1.0f);
  SND_CHECK_MSG((v.flags & ll::kCellOverridden) == 0u, "flags %u: still overridden after the item revert", v.flags);
  SND_CHECK(r.s.sync().reverts() >= 1u);
  SND_CHECK(!r.s.RevertOnly(fader));  // nothing left to revert
  SND_REQUIRE(r.s.mixer().Undo());
  SND_REQUIRE(r.Synced());
  SND_REQUIRE(r.Read(&v));
  SND_CHECK_EQ(F32Of(v.current), -12.0f);
  SND_CHECK((v.flags & ll::kCellOverridden) != 0u);
  // Read-back of every mixer cell.
  SND_REQUIRE(r.s.ReadBack());
  SND_REQUIRE(r.Run([&] { return r.s.sync().ReadsOutstanding() == 0u; }));
  const std::int32_t i = r.s.sync().Find(CellAddress{bf::chunk::BUSS, snd::HashId("Sfx"), bf::col::BUSS_volumeDb, 0, {}});
  SND_REQUIRE(i >= 0);
  const CellStatus& cs = r.s.sync().cells()[static_cast<std::size_t>(i)];
  SND_CHECK(cs.readState == CellStatus::Read::Done);
  SND_CHECK_EQ(cs.readCurrent, -12.0f);
  SND_CHECK_EQ(cs.readOriginal, -1.0f);
  SND_CHECK(cs.readOverridden);
  for (const CellStatus& c : r.s.sync().cells()) SND_CHECK(c.readState == CellStatus::Read::Done);
}

// D-174: budgetMicroseconds = 1e6 x block / rate x load_target_percent / 100; load = (logic + render) / budget.
SND_TEST(IndepS7LiveRevert, ProfilerCpuBudgetAndLoad) {
  SessionRig r;
  SND_REQUIRE(r.ok);
  r.s.EnableProfilerStreams(kViewCpu | kViewCpuDetail | kViewMemory);
  r.engine->Play(snd::HashId("Tone"), nullptr);
  SND_REQUIRE(r.Run([&] { return r.s.streams().cpu().size() >= 3u && !r.s.streams().cpuDetail().empty() && r.s.streams().hasMemory(); }));
  const snd_config& c = r.engine->config();
  const float budget = static_cast<float>(1e6 * c.block_size / c.sample_rate * c.tuning.load_target_percent / 100.0);
  for (const CpuPoint& p : r.s.streams().cpu()) {
    SND_CHECK_NEAR(p.timing.budgetMicroseconds, budget, budget * 1e-6f);
    SND_CHECK_NEAR(p.load, (p.timing.logicMicroseconds + p.timing.renderMicroseconds) / p.timing.budgetMicroseconds, 1e-6);
  }
  for (const CpuDetailPoint& p : r.s.streams().cpuDetail()) SND_CHECK_NEAR(p.detail.budgetMicroseconds, budget, budget * 1e-6f);
  std::string why;
  SND_CHECK(r.s.streams().Available(kViewCpuDetail, &why));
  SND_CHECK_EQ(r.s.streams().cpuCount().missed, 0u);
  SND_CHECK(r.s.streams().totalUsed() > 0u);
}
