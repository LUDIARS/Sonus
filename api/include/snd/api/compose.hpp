// Composition root of the `sonus` library (api/compose/compose.cpp, CONTRACTS.md D-03).
#ifndef SND_API_COMPOSE_HPP
#define SND_API_COMPOSE_HPP

namespace snd::compose {

// Names of the runtime modules linked into `sonus`, in dependency order.
const char* const* LinkedModules(unsigned* count) noexcept;

}  // namespace snd::compose

#endif  // SND_API_COMPOSE_HPP
