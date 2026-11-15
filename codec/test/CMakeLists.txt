snd_add_test(codec_test
  SOURCES
    codec_smoke_test.cpp
    decode_test.cpp
    encode_test.cpp
    pool_test.cpp
    registry_test.cpp
  LINK sonus_codec sonus_mocks)

# Runtime-only user of the module: must not pull in the encoder (snd/codec/encode.hpp).
snd_add_test(codec_runtime_test SOURCES runtime_link_test.cpp LINK sonus_codec)
if(CMAKE_NM)
  add_test(NAME codec_runtime_has_no_encoder
    COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DEXE=$<TARGET_FILE:codec_runtime_test>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_encoder.cmake)
endif()
