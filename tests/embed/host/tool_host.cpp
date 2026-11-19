// The engine's editor: the tool's core (Sonus::tool_core, no Tela) connected over TCP
// to a game made through the C API in the same process. The editor polls the Session
// from its own loop and reads what the game streams.
static_assert(__cplusplus >= 202002L, "Sonus::tool_core must carry cxx_std_20 to its users");

#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

#include "embed_game.hpp"
#include "snd/tool/session.hpp"

int main() {
  // Offline first: the models work without a game.
  snd::tool::Session offline;
  offline.Tick();
  if (offline.connection().state() != snd::tool::LinkState::Offline || offline.StartRecording()) return 1;

  // The game, with its live link on a free port near a per-process base.
  embed::Game* game = nullptr;
  std::uint16_t port = 0;
  for (int i = 0; i < 32 && game == nullptr; ++i) {
    port = static_cast<std::uint16_t>(30000 + (::getpid() * 7 + i * 101) % 20000);
    if (embed::StartGame(port, &game) != 0) game = nullptr;
  }
  if (game == nullptr) {
    std::printf("could not start a game with a live link\n");
    return 1;
  }

  snd::tool::Session s;
  snd::tool::ConnectionDesc d;
  d.address = "127.0.0.1";
  d.port = port;
  d.name = "embed-editor";
  d.reconnectDelayMs = 20;
  bool ok = s.Connect(d) == 0;
  // The editor's loop and the game's loop (here one thread drives both, the network runs
  // on the client's and the server's threads).
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  bool online = false, timings = false;
  std::uint64_t blocks = 0;
  while (ok && !(online && timings) && std::chrono::steady_clock::now() < end) {
    blocks = embed::RunGame(game, blocks + 1u);
    s.Tick();
    online = online || s.connection().state() == snd::tool::LinkState::Online;
    timings = s.profiler().Summary().samples > 0u;
  }
  const std::uint32_t rate = s.connection().hello().sampleRate;
  const std::uint32_t samples = s.profiler().Summary().samples;
  s.Disconnect();
  embed::StopGame(game);
  std::printf("port %u, online %d, timing samples %u, sample rate %u, blocks %llu\n", static_cast<unsigned>(port),
              online ? 1 : 0, samples, rate, static_cast<unsigned long long>(blocks));
  if (!ok || !online || !timings || rate != 48000u) return 1;
  std::printf("embed_tool_host: OK\n");
  return 0;
}
