# Tests of the test kit and the mocks.
set(SND_GOLDEN_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/golden_scratch")
file(MAKE_DIRECTORY "${SND_GOLDEN_SCRATCH}")

snd_add_test(testkit_selftest
  SOURCES runner_selftest.cpp golden_selftest.cpp rt_check_selftest.cpp
  LINK sonus_mocks)
target_compile_definitions(testkit_selftest PRIVATE SND_GOLDEN_SCRATCH_DIR="${SND_GOLDEN_SCRATCH}")

snd_add_test(mocks_test SOURCES mocks_test.cpp LINK sonus_mocks)

# An unexpected RT violation must make the test executable fail.
snd_add_test(rt_violation_fails_test SOURCES rt_violation_fails_test.cpp)
set_tests_properties(rt_violation_fails_test PROPERTIES WILL_FAIL TRUE)
