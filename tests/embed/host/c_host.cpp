// The engine with Sonus' C API only: make a system on a silent device, render a few
// blocks, check they were silent, destroy it.
#include <cstdio>
#include <stdexcept>

#include "embed_game.hpp"

int main() {
  // The parent keeps its exceptions (Sonus' -fno-exceptions is on Sonus' targets only).
  try {
    throw std::runtime_error("exceptions work in the parent");
  } catch (const std::exception&) {
  }
  embed::Game* game = nullptr;
  const int r = embed::StartGame(0, &game);
  if (r != 0 || game == nullptr) {
    std::printf("snd_create failed: %d\n", r);
    return 1;
  }
  const std::uint64_t blocks = embed::RunGame(game, 8);
  const bool silent = embed::OutputWasSilent(game);
  const std::uint64_t periods = embed::PeriodsPumped(game);
  embed::StopGame(game);
  std::printf("blocks %llu, periods %llu, silent %d\n", static_cast<unsigned long long>(blocks),
              static_cast<unsigned long long>(periods), silent ? 1 : 0);
  if (blocks < 8u || periods == 0u || !silent) return 1;
  std::printf("embed_c_host: OK\n");
  return 0;
}
