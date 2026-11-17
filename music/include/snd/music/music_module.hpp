// music module (design.md 12.1): transports, reservations, segments/playlists/stems/states, transition state machine, sync groups (IMusicSystem).
// Dependencies (frozen, root CMakeLists.txt): core (internal IF).
//
// The music system itself: snd/music/music_system.hpp (snd::music::MusicSystem).
// Only types from the contract (contract/), base and platform appear in headers here.
#ifndef SND_MUSIC_MUSIC_MODULE_HPP
#define SND_MUSIC_MUSIC_MODULE_HPP

namespace snd::music {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::music

#endif  // SND_MUSIC_MUSIC_MODULE_HPP
