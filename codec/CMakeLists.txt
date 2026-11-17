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
    src/opus_decoder.cpp
    src/registry.cpp
    src/decoder_pool.cpp
    src/encode.cpp
    src/encode_opus.cpp)

# Opus (CONTRACTS.md D-56): the bundled libopus (third_party/). With
# SND_CODEC_OPUS=OFF the Opus files compile to stubs and libopus is not built.
# opus_decoder.cpp is runtime, encode_opus.cpp build-time only (like encode.cpp).
if(SND_CODEC_OPUS)
  target_link_libraries(sonus_codec PRIVATE sonus_opus)
  target_compile_definitions(sonus_codec PRIVATE SND_CODEC_OPUS_ENABLED=1)
else()
  target_compile_definitions(sonus_codec PRIVATE SND_CODEC_OPUS_ENABLED=0)
endif()
