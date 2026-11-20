set(_ll_test_link sonus_livelink sonus_core sonus_bank sonus_dsp sonus_jobs sonus_io sonus_codec sonus_platform
    sonus_base sonus_mocks)
snd_add_test(livelink_test SOURCES livelink_unit_test.cpp LINK ${_ll_test_link})
# Fuzz (design.md 14). Scale with SND_FUZZ_ITERATIONS; run under SND_SANITIZE=address,undefined.
snd_add_test(livelink_fuzz_test SOURCES livelink_fuzz_test.cpp LINK ${_ll_test_link})
set_tests_properties(livelink_fuzz_test PROPERTIES TIMEOUT 600)
# Independent tests (written from the headers by a separate tester).
snd_add_test(livelink_indep_unit_test SOURCES indep_livelink_unit_test.cpp LINK ${_ll_test_link})
set_tests_properties(livelink_indep_unit_test PROPERTIES TIMEOUT 600)
# Protocol 1.1 (组 L): unit tests, end to end with a real core (the integration fixture of
# tests/integration, included unchanged), recording / replay, fuzz of the new messages.
snd_add_test(livelink_v11_unit_test SOURCES livelink_v11_unit_test.cpp LINK ${_ll_test_link})
set(_ll_v11_link ${_ll_test_link} sonus_buildtool)
foreach(_t livelink_v11_live_test livelink_v11_record_test livelink_v11_fuzz_test)
  snd_add_test(${_t} SOURCES ${_t}.cpp LINK ${_ll_v11_link})
  target_include_directories(${_t} PRIVATE ${PROJECT_SOURCE_DIR}/core/test ${PROJECT_SOURCE_DIR}/tests/integration)
  set_tests_properties(${_t} PROPERTIES TIMEOUT 600)
endforeach()
