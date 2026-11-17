snd_add_test(source_test
  SOURCES source_smoke_test.cpp
  LINK sonus_source sonus_mocks)

# Sample / Stream kinds through core: banks built by buildtool, IO through the
# IO scheduler over the memory FS (source_harness.hpp).
set(_source_test_link sonus_source sonus_core sonus_buildtool sonus_bank sonus_dsp sonus_jobs sonus_io sonus_codec
    sonus_mocks)
snd_add_test(source_sample_test SOURCES source_sample_test.cpp LINK ${_source_test_link})
target_include_directories(source_sample_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(source_sample_test PROPERTIES TIMEOUT 600)
snd_add_test(source_stream_test SOURCES source_stream_test.cpp LINK ${_source_test_link})
target_include_directories(source_stream_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(source_stream_test PROPERTIES TIMEOUT 600)
snd_add_test(source_kind_test SOURCES source_kind_test.cpp LINK ${_source_test_link})
target_include_directories(source_kind_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(source_kind_test PROPERTIES TIMEOUT 600)
