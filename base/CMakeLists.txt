# base module (design.md 12.1). Owner: base/platform/jobs team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_base).
snd_add_module(base
  SOURCES
    src/base_module.cpp
    src/allocator.cpp
    src/log.cpp
    src/notify.cpp)
