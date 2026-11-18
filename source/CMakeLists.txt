# source module (design.md 12.1): Sample and Stream kinds (stage 2).
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_source).
snd_add_module(source
  SOURCES
    src/source_module.cpp
    src/decode_engine.cpp
    src/pcm_cache.cpp
    src/sample_kind.cpp
    src/stream_kind.cpp
    src/runtime.cpp)
