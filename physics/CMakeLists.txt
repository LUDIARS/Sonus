# physics module (design.md 12.1). Owner: stage 4 team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_physics).
snd_add_module(physics
  SOURCES
    src/physics_module.cpp src/physics_stub.cpp)
