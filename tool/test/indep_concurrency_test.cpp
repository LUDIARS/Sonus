// Independent tests (tool/test/indep_properties.md G, A5): the UI thread edits, drags,
// records, drops and reopens the connection while a scripted game on its own thread
// restarts at random moments and answers some values with COMMAND_BUFFER_FULL, and the
// client's network thread runs between them. Run under TSan. After the chaos the game's
// state (reset at every restart, as a restarted game holds the bank) must equal the
// tool's cells: every value differing from the bank or ever sent reached the new session.
#include <atomic>
#include <map>
#include <thread>

#include "indep_scripted_game.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "tool_fixture.hpp"
#include "ui_helpers.hpp"

using namespace snd::tool;
using indep::GameFrame;
using indep::ScriptedGame;
namespace ll = snd::livelink;

namespace {

struct Rng {
  std::uint64_t s;
  std::uint32_t Below(std::uint32_t n) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>((s >> 33) % n);
  }
};

struct Key {
  std::uint32_t chunk, id, column, index;
  bool operator<(const Key& o) const {
    if (chunk != o.chunk) return chunk < o.chunk;
    if (id != o.id) return id < o.id;
    if (column != o.column) return column < o.column;
    return index < o.index;
  }
};

void RunChaos(std::uint64_t seed, std::uint32_t chaosMs) {
  tooltest::ToolProject tool;
  ScriptedGame g;
  SND_REQUIRE(tool.ok && g.ok);
  std::atomic<bool> chaos{true}, stop{false};
  std::atomic<std::uint32_t> restarts{0};
  // Results the game gave per frame seq of each session (game thread only until joined).
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::int32_t> results;
  Rng grng{seed ^ 0xA5A5u};
  g.ackResult = [&](const GameFrame& f) {
    std::int32_t r = SND_OK;
    if (chaos.load(std::memory_order_relaxed) && f.Is(ll::MessageType::SetValue) && grng.Below(8) == 0) {
      r = SND_ERR_COMMAND_BUFFER_FULL;
    }
    results[{f.session, f.h.seq}] = r;
    return r;
  };
  std::thread game([&] {
    while (!stop.load(std::memory_order_acquire)) {
      g.Pump();
      if (chaos.load(std::memory_order_relaxed) && g.connected() && grng.Below(400) == 0) {
        g.Drop();  // the game restarts
        restarts.fetch_add(1, std::memory_order_relaxed);
      }
      std::this_thread::sleep_for(std::chrono::microseconds(150));
    }
  });

  Session s;
  s.UseProject(tool.project);
  ui::App app(s, ui::AppDesc{});
  uitest::Pointer ptr(app);
  const ConnectionDesc desc = indep::GameConn(g, tool.project.buildHash());
  SND_REQUIRE_EQ(s.Connect(desc), SND_OK);
  Rng rng{seed};
  const std::int32_t buses = static_cast<std::int32_t>(tool.project.buses().size());
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(chaosMs);
  std::uint32_t frames = 0;
  while (std::chrono::steady_clock::now() < end) {
    ++frames;
    const std::int32_t b = static_cast<std::int32_t>(rng.Below(static_cast<std::uint32_t>(buses)));
    switch (rng.Below(14)) {
      case 0: case 1: case 2:
        s.mixer().Set(Target{Control::BusFader, b, 0, 0}, -static_cast<float>(rng.Below(60)));
        break;
      case 3: s.mixer().Set(Target{Control::BusMute, b, 0, 0}, static_cast<float>(rng.Below(2))); break;
      case 4: s.mixer().Set(Target{Control::BusSolo, b, 0, 0}, static_cast<float>(rng.Below(2))); break;
      case 5: s.mixer().Set(Target{Control::EffectParam, tooltest::Bus(tool.project, "Sfx"), 0, 2},
                            -static_cast<float>(rng.Below(96))); break;
      case 6: s.mixer().Undo(); break;
      case 7: s.mixer().Redo(); break;
      case 8: {  // a drag on a fader through the UI
        const tela::PlacedElement* f = uitest::Find(app.runtime(), ui::BusId(tool.project.buses()[static_cast<std::size_t>(b)].id) + ".fader.hit");
        if (f != nullptr) {
          const tela::Point c = uitest::Center(f);
          ptr.Down(std::round(c.x), std::round(c.y));
          ptr.Move(std::round(c.x), std::round(c.y) + static_cast<float>(rng.Below(40)) - 20.0f);
          ptr.Up(std::round(c.x), std::round(c.y));
          ptr.Advance(1000);
        }
        break;
      }
      case 9: if (rng.Below(20) == 0) s.connection().Drop(); break;
      case 10: if (rng.Below(40) == 0) { s.Disconnect(); s.Connect(desc); } break;
      case 11: app.OnRecordToggle(); break;
      case 12: s.mixer().Set(Target{Control::ParamOverrideOn, 0, 0, 0}, static_cast<float>(rng.Below(2))); break;
      case 13: if (rng.Below(10) == 0) s.mixer().RevertAll(); break;
    }
    app.Frame(indep::NowNs());
    std::this_thread::sleep_for(std::chrono::microseconds(300));
  }
  if (s.profiler().recordingState() == RecordingState::Recording) app.OnRecordToggle();
  chaos.store(false, std::memory_order_relaxed);
  // Converge: online, nothing pending or unsent, for a few frames in a row.
  std::uint32_t quiet = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (quiet < 20u && std::chrono::steady_clock::now() < deadline) {
    app.Frame(indep::NowNs());
    const bool done = s.connection().state() == LinkState::Online && s.sync().Count(CellState::Pending) == 0u &&
                      s.sync().Count(CellState::Unsent) == 0u;
    quiet = done ? quiet + 1u : 0u;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  SND_CHECK_EQ(quiet, 20u);
  const std::uint32_t session = g.accepts;  // stable now: no more drops
  stop.store(true, std::memory_order_release);
  game.join();
  SND_CHECK_EQ(s.sync().Count(CellState::Failed), 0u);
  // The game's state in its current session: the bank, then each SetValue it accepted.
  std::map<Key, float> applied;
  for (const GameFrame& f : g.frames) {
    if (f.session != session || !f.Is(ll::MessageType::SetValue)) continue;
    const auto it = results.find({f.session, f.h.seq});
    if (it == results.end() || it->second != SND_OK) continue;
    const ll::SetValue v = f.As<ll::SetValue>();
    applied[Key{v.chunk, v.runtimeId, v.column, v.index}] = indep::ValueF32(v);
  }
  for (const Cell& c : s.mixer().Cells()) {
    const auto it = applied.find(Key{c.address.chunk, c.address.runtimeId, c.address.column, c.address.index});
    const float gameValue = it == applied.end() ? c.bankValue : it->second;
    SND_CHECK_MSG(gameValue == c.value, "cell %08x/%u/%u: game %g tool %g", c.address.runtimeId, c.address.column,
                  c.address.index, static_cast<double>(gameValue), static_cast<double>(c.value));
  }
  std::printf("chaos seed %llu: %u frames, %u game restarts, %u sessions seen by the tool\n",
              static_cast<unsigned long long>(seed), frames, restarts.load(), s.connection().sessions());
  s.Disconnect();
}

}  // namespace

SND_TEST(IndepToolConcurrency, ChaosConvergesToTheToolsState) {
  for (std::uint64_t seed : {1ull, 2ull, 3ull}) RunChaos(seed, 700);
}
