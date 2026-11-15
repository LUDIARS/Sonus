# platform module (design.md 12.1). Owner: base/platform/jobs team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_platform).
snd_add_module(platform
  SOURCES
    src/platform_module.cpp
    src/clock_thread.cpp
    src/semaphore.cpp
    src/cpu.cpp
    src/output_device.cpp)
find_package(Threads REQUIRED)
target_link_libraries(sonus_platform PUBLIC Threads::Threads)
