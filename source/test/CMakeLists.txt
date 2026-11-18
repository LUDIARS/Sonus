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
# Real IO thread + worker pool + RT workers; load (32 streams + 128 samples).
#   ctest --repeat until-fail:20 -R source_parallel_test   (TSan and Release)
snd_add_test(source_parallel_test SOURCES source_parallel_test.cpp LINK ${_source_test_link})
target_include_directories(source_parallel_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(source_parallel_test PROPERTIES TIMEOUT 600)

# Independent tests (indep_*): written from the spec by a separate tester.
snd_add_test(indep_sample_test SOURCES indep_sample_test.cpp LINK ${_source_test_link})
target_include_directories(indep_sample_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_sample_test PROPERTIES TIMEOUT 600)
snd_add_test(indep_stream_test SOURCES indep_stream_test.cpp LINK ${_source_test_link})
target_include_directories(indep_stream_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_stream_test PROPERTIES TIMEOUT 600)
# D-43 detectors (ThreadSanitizer): RenderBatch of a late chunk against ScheduleBackground.
snd_add_test(indep_late_chunk_test SOURCES indep_late_chunk_test.cpp LINK ${_source_test_link})
target_include_directories(indep_late_chunk_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
snd_add_test(indep_fix_sample_test SOURCES indep_fix_sample_test.cpp LINK ${_source_test_link})
target_include_directories(indep_fix_sample_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_fix_sample_test PROPERTIES TIMEOUT 600)
snd_add_test(indep_fix_stream_test SOURCES indep_fix_stream_test.cpp LINK ${_source_test_link})
target_include_directories(indep_fix_stream_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_fix_stream_test PROPERTIES TIMEOUT 600)
snd_add_test(indep_fix2_test SOURCES indep_fix2_test.cpp LINK ${_source_test_link})
target_include_directories(indep_fix2_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_fix2_test PROPERTIES TIMEOUT 900)
snd_add_test(indep_fix3_test SOURCES indep_fix3_test.cpp LINK ${_source_test_link})
target_include_directories(indep_fix3_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_fix3_test PROPERTIES TIMEOUT 900)
# Stream loops of any length (loop replay from the PCM ring vs IO wraps).
snd_add_test(source_stream_loop_test SOURCES source_stream_loop_test.cpp LINK ${_source_test_link})
target_include_directories(source_stream_loop_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(source_stream_loop_test PROPERTIES TIMEOUT 900)
snd_add_test(indep_fix4_test SOURCES indep_fix4_test.cpp LINK ${_source_test_link})
target_include_directories(indep_fix4_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_fix4_test PROPERTIES TIMEOUT 900)
snd_add_test(indep_fix5_test SOURCES indep_fix5_test.cpp LINK ${_source_test_link})
target_include_directories(indep_fix5_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_fix5_test PROPERTIES TIMEOUT 1200)
# Stream IO errors: what is in memory plays out, then the voice ends.
snd_add_test(source_stream_ioerror_test SOURCES source_stream_ioerror_test.cpp LINK ${_source_test_link})
target_include_directories(source_stream_ioerror_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(source_stream_ioerror_test PROPERTIES TIMEOUT 900)
snd_add_test(indep_fix6_test SOURCES indep_fix6_test.cpp LINK ${_source_test_link})
target_include_directories(indep_fix6_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_fix6_test PROPERTIES TIMEOUT 1200)
