# codec module (design.md 12.1). Owner: codec/io team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_codec).
#
# Runtime objects: codec_module, header, decoders, registry, decoder_pool.
# Build-time object: encode (snd/codec/encode.hpp). sonus_codec is static, so a
# runtime binary that does not call the encoder API does not link encode.cpp.o.
snd_add_module(codec
  SOURCES
    src/codec_module.cpp
    src/header.cpp
    src/decoders.cpp
    src/registry.cpp
    src/decoder_pool.cpp
    src/encode.cpp)
