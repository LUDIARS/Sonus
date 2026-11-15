// music module (design.md 12.1): transports, reservations, segments/playlists/stems/states, transition state machine, sync groups (IMusicSystem).
// Dependencies (frozen, root CMakeLists.txt): core (internal IF).
//
// Stage-0 stub. The owner of this module adds its headers under
// music/include/snd/music/ and its sources under music/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_MUSIC_MUSIC_MODULE_HPP
#define SND_MUSIC_MUSIC_MODULE_HPP

namespace snd::music {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::music

#endif  // SND_MUSIC_MUSIC_MODULE_HPP
