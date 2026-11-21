# physics module (design.md 9, 12.1). Owner: stage 4 team (session A).
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_physics): the public
# API only. The C API functions it calls (snd_play, ...) live in the composition root
# `sonus`; a game links sonus_physics together with sonus.
snd_add_module(physics
  SOURCES
    src/physics_module.cpp src/physics.cpp)
