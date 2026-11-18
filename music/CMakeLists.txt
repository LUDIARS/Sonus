# music module (design.md 8, 12.1). Owner: stage 4 team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_music).
snd_add_module(music
  SOURCES
    src/music_module.cpp
    src/bank_tables.cpp
    src/timeline.cpp
    src/segment.cpp
    src/rules.cpp
    src/playlist.cpp
    src/music_common.cpp
    src/music_system.cpp
    src/transport_update.cpp
    src/transition.cpp
    src/music_logic.cpp)
