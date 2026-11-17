// The UI thread (App frames, gestures, edits, recording) against a game whose
// audio and live link threads run freely: the client's network thread fills
// its buffers while the UI copies them. Meant for -DSND_SANITIZE=thread; in
// every build it checks that the last value the UI set is what the game plays.
#include <atomic>
#include <cmath>
#include <thread>

#include "snd/test/test.hpp"
#include "ui_helpers.hpp"

using namespace snd::tool;
using namespace uitest;

SND_TEST(ToolUiConcurrency, UiThreadAgainstRunningGameAndNetworkThreads) {
  tooltest::LiveRig rig;
  SND_REQUIRE(rig.ok);
  tooltest::ToolProject tool;
  SND_REQUIRE(tool.ok);
  rig.Play("Tone");
  const float base = rig.Level();
  Session s;
  s.UseProject(tool.project);
  ui::AppDesc d;
  d.connection = rig.Conn(tool.project.buildHash());
  ui::App app(s, d);
  SND_REQUIRE_EQ(s.Connect(d.connection), SND_OK);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> blocks{0};
  std::thread audio([&] {
    while (!stop.load(std::memory_order_acquire)) {
      rig.f.Step();
      if (rig.f.all.size() > (1u << 16)) rig.f.all.clear();
      blocks.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::microseconds(150));
    }
  });

  Pointer ptr(app);
  const std::int32_t sfx = tooltest::Bus(tool.project, "Sfx");
  const std::string hit = ui::BusId(snd::HashId("Sfx")) + ".fader.hit";
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  std::uint32_t frame = 0, drags = 0;
  bool online = false;
  while (std::chrono::steady_clock::now() < end || !online) {
    app.Frame(tooltest::Now());
    online = online || s.connection().state() == LinkState::Online;
    ++frame;
    if (frame % 7 == 0) {  // a short fader drag
      const tela::PlacedElement* p = Find(app.runtime(), hit);
      if (p != nullptr) {
        const float x = p->bounds.x + p->bounds.width * 0.5f;
        const float y = p->bounds.y + ui::FaderGeometry{p->bounds.height, 18.0f}.CapCenterY(
                                          ui::FaderDbToPos(s.mixer().buses()[static_cast<std::size_t>(sfx)].faderDb));
        ptr.Advance(1000);
        ptr.Down(x, y);
        app.Frame(tooltest::Now());
        ptr.Move(x, y + static_cast<float>((frame / 7) % 5) * 3.0f - 6.0f);
        app.Frame(tooltest::Now());
        ptr.Up(x, y);
        ++drags;
      }
    }
    if (frame % 23 == 0) app.OnToggle(Target{Control::BusMute, tooltest::Bus(tool.project, "Music"), 0, 0});
    if (frame % 41 == 0) app.OnScreen(frame % 82 == 0 ? ui::Screen::Mixer : ui::Screen::Profiler);
    if (frame % 57 == 0) app.OnRecordToggle();
    if (frame % 97 == 0) s.connection().Drop();  // the client reconnects by itself
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  // Let everything settle with the threads still running.
  s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -9.0f);
  const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool synced = false;
  while (std::chrono::steady_clock::now() < settle && !synced) {
    app.Frame(tooltest::Now());
    synced = s.connection().state() == LinkState::Online && s.sync().Count(CellState::Pending) == 0u &&
             s.sync().Count(CellState::Unsent) == 0u && s.sync().Count(CellState::Failed) == 0u;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const std::uint64_t at = blocks.load();
  while (blocks.load() < at + 32u) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  stop.store(true, std::memory_order_release);
  audio.join();
  SND_CHECK(synced);
  SND_CHECK(drags > 10u);
  SND_CHECK(s.connection().sessions() >= 2u);
  SND_CHECK_NEAR(static_cast<double>(rig.Level()) / base, tooltest::Ratio(-9.0f), 1e-4);
  s.Disconnect();
}
