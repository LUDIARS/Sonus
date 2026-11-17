# livelink module (design.md 12.1): protocol, override layer, profiler stream, recording.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_livelink).
snd_add_module(livelink
  SOURCES
    src/livelink_module.cpp
    src/wire.cpp
    src/frame_stream.cpp
    src/rings.cpp
    src/socket.cpp
    src/tcp.cpp
    src/memory_transport.cpp
    src/record_format.cpp
    src/value_applier.cpp
    src/runtime.cpp
    src/server.cpp
    src/client.cpp
    src/replayer.cpp)
# platform (threads, clock, semaphore) is a foundation module (D-02: visible to every
# module); core does not re-export it because it reaches platform only through jobs / dsp.
target_link_libraries(sonus_livelink PUBLIC sonus_platform)
if(WIN32)
  target_link_libraries(sonus_livelink PUBLIC ws2_32)
endif()
