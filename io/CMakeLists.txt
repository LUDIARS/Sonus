# io module (design.md 12.1). Owner: codec/io team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_io).
snd_add_module(io
  SOURCES
    src/io_module.cpp
    src/stream_pool.cpp
    src/io_scheduler.cpp
    src/default_file_system.cpp
    src/native_file_posix.cpp
    src/native_file_win32.cpp
    src/native_file_none.cpp)
