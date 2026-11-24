snd_add_test(source_test
  SOURCES source_smoke_test.cpp
  LINK sonus_source sonus_base sonus_mocks)

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
snd_add_test(indep_fix7_test SOURCES indep_fix7_test.cpp LINK ${_source_test_link})
target_include_directories(indep_fix7_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_fix7_test PROPERTIES TIMEOUT 900)

# Stage 7-D kinds (D-190 .. D-195) driven directly (synth_driver.hpp): Oscillator, Noise.
snd_add_test(source_osc_test SOURCES source_osc_test.cpp LINK sonus_source sonus_base sonus_mocks)
set_tests_properties(source_osc_test PROPERTIES TIMEOUT 600)
snd_add_test(source_noise_test SOURCES source_noise_test.cpp LINK sonus_source sonus_base sonus_mocks)
target_include_directories(source_noise_test PRIVATE ${PROJECT_SOURCE_DIR}/source/src)
set_tests_properties(source_noise_test PROPERTIES TIMEOUT 900)
snd_add_test(source_input_test SOURCES source_input_test.cpp LINK sonus_source sonus_base sonus_mocks)
set_tests_properties(source_input_test PROPERTIES TIMEOUT 600)
# Voice limit: RT scope, no allocation after Init, time per block (printed).
snd_add_test(source_synth_load_test SOURCES source_synth_load_test.cpp LINK sonus_source sonus_base sonus_mocks)
set_tests_properties(source_synth_load_test PROPERTIES TIMEOUT 900)
# Through core: SYNP banks built by buildtool, the oscillator following a Parameter,
# noise colours, a wavetable, an input playback fed through the Runtime's port.
snd_add_test(source_synth_e2e_test SOURCES source_synth_e2e_test.cpp LINK ${_source_test_link})
target_include_directories(source_synth_e2e_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(source_synth_e2e_test PROPERTIES TIMEOUT 900)
# snd_write_input through the C API (MANUAL output device; the driver's audio thread reads).
snd_add_test(source_input_api_test SOURCES source_input_api_test.cpp LINK sonus sonus_source sonus_base sonus_mocks)
set_tests_properties(source_input_api_test PROPERTIES TIMEOUT 600)

# Independent tests of the stage 7-D kinds, group 1 (Oscillator, Noise, Input; D-190 .. D-196),
# written from the specification by a separate tester. indep_defects_src1_test holds the
# Defect_ tests (they fail until the defects are fixed).
#   ctest --repeat until-fail:20 -R 'indep_input_test|indep_src1_core_test'   (TSan -j1 and Release)
snd_add_test(indep_osc_test SOURCES indep_osc_test.cpp LINK sonus_source sonus_base sonus_mocks)
set_tests_properties(indep_osc_test PROPERTIES TIMEOUT 900)
snd_add_test(indep_noise_test SOURCES indep_noise_test.cpp LINK sonus_source sonus_base sonus_mocks)
set_tests_properties(indep_noise_test PROPERTIES TIMEOUT 900)
snd_add_test(indep_input_test SOURCES indep_input_test.cpp LINK sonus_source sonus_base sonus_mocks)
set_tests_properties(indep_input_test PROPERTIES TIMEOUT 900)
snd_add_test(indep_src1_core_test SOURCES indep_src1_core_test.cpp LINK ${_source_test_link})
target_include_directories(indep_src1_core_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(indep_src1_core_test PROPERTIES TIMEOUT 1200)
snd_add_test(indep_src1_load_test SOURCES indep_src1_load_test.cpp LINK sonus_source sonus_base sonus_mocks)
set_tests_properties(indep_src1_load_test PROPERTIES TIMEOUT 900)
# snd_write_input / snd_play_input through the C API (the api test rig: null output device, audio thread).
snd_add_test(indep_src1_api_test SOURCES indep_src1_api_test.cpp
  LINK sonus sonus_core sonus_music sonus_source sonus_livelink sonus_platform sonus_io sonus_jobs sonus_buildtool
       sonus_codec sonus_mocks)
target_include_directories(indep_src1_api_test PRIVATE ${PROJECT_SOURCE_DIR}/api/test ${PROJECT_SOURCE_DIR}/api/compose)
set_tests_properties(indep_src1_api_test PROPERTIES TIMEOUT 900)
snd_add_test(source_input_floor_test SOURCES source_input_floor_test.cpp LINK sonus_source sonus_base sonus_mocks)
