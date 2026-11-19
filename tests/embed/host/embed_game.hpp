// The game side of the embedding test: a Sonus system made through the C API only
// (snd/sonus.h) on a silent device the host pumps. Plain C++17, no Sonus header here.
#ifndef SONUS_EMBED_GAME_HPP
#define SONUS_EMBED_GAME_HPP

#include <cstdint>

namespace embed {

struct Game;

// livelinkPort 0: no live link. Returns the snd_result of snd_create (0 = SND_OK).
int StartGame(std::uint16_t livelinkPort, Game** out);
// One snd_update and one device period per step until `blocks` blocks have been rendered
// (or 10 s). Returns the rendered block count (snd_stats.block_index).
std::uint64_t RunGame(Game* game, std::uint64_t blocks);
// true if every sample the device received so far was 0 (nothing plays) and the callback
// always overwrote the device's buffer.
bool OutputWasSilent(const Game* game);
std::uint64_t PeriodsPumped(const Game* game);
void StopGame(Game* game);

}  // namespace embed

#endif  // SONUS_EMBED_GAME_HPP
