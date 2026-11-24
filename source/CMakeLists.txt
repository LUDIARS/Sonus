# source module (design.md 12.1): Sample and Stream kinds (stage 2), Callback and
# UserBuffer kinds (api stage, contract 1.2 D-91), Oscillator, Noise and Input
# kinds (stage 7-D, D-190 .. D-195).
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_source).
snd_add_module(source
  SOURCES
    src/source_module.cpp
    src/decode_engine.cpp
    src/pcm_cache.cpp
    src/sample_kind.cpp
    src/stream_kind.cpp
    src/callback_kind.cpp
    src/user_buffer_kind.cpp
    src/oscillator_kind.cpp
    src/noise_kind.cpp
    src/input_kind.cpp
    src/runtime.cpp)
target_sources(sonus_source PRIVATE src/granular_kind.cpp)
