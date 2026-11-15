snd_add_test(buildtool_test
  SOURCES buildtool_smoke_test.cpp buildtool_test.cpp
  LINK sonus_buildtool sonus_mocks)
target_compile_definitions(buildtool_test PRIVATE
  SND_BUILDTOOL_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
