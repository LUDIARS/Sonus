snd_add_test(codec_test
  SOURCES
    codec_smoke_test.cpp
    decode_test.cpp
    encode_test.cpp
    pool_test.cpp
    registry_test.cpp
  LINK sonus_codec sonus_mocks)

# Opus (D-56..D-60): built in every configuration; with SND_CODEC_OPUS=OFF it
# only checks that Opus is unavailable. Analytic checks need an optimised
# libopus to stay fast, hence the longer timeout for Debug / sanitizer builds.
snd_add_test(codec_opus_test SOURCES opus_test.cpp LINK sonus_codec)
set_tests_properties(codec_opus_test PROPERTIES TIMEOUT 900)

# Runtime-only user of the module: must not pull in the encoder (snd/codec/encode.hpp).
snd_add_test(codec_runtime_test SOURCES runtime_link_test.cpp LINK sonus_codec)
if(CMAKE_NM)
  add_test(NAME codec_runtime_has_no_encoder
    COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DEXE=$<TARGET_FILE:codec_runtime_test>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_encoder.cmake)
endif()

# Independent tests (written by a tester from the specification, not by the implementer).
snd_add_test(codec_indep_decode_test SOURCES indep_decode_test.cpp LINK sonus_codec)
snd_add_test(codec_indep_encode_test SOURCES indep_encode_test.cpp LINK sonus_codec)
snd_add_test(codec_indep_pool_test SOURCES indep_pool_registry_test.cpp LINK sonus_codec)
snd_add_test(codec_indep_fuzz_test SOURCES indep_fuzz_test.cpp LINK sonus_codec)
set_tests_properties(codec_indep_decode_test codec_indep_encode_test codec_indep_fuzz_test PROPERTIES TIMEOUT 900)

# Independent Opus tests (written from bank-format.md §6.5 / D-56..D-60 and the RFCs).
foreach(_t layout decode robust recheck)
  snd_add_test(codec_indep_opus_${_t}_test SOURCES indep_opus_${_t}_test.cpp LINK sonus_codec)
  target_compile_definitions(codec_indep_opus_${_t}_test PRIVATE SND_INDEP_OPUS_EXPECTED=$<BOOL:${SND_CODEC_OPUS}>)
  set_tests_properties(codec_indep_opus_${_t}_test PROPERTIES TIMEOUT 900)
endforeach()
