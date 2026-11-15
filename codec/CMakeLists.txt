# codec module (design.md 12.1). Owner: codec/io team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_codec).
snd_add_module(codec
  SOURCES
    src/codec_module.cpp)
