snd_add_test(bank_test
  SOURCES bank_smoke_test.cpp validate_test.cpp manager_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)

# Fuzz: valid banks with random (fixed-seed) mutations; run under ASan/UBSan too.
snd_add_test(bank_fuzz_test
  SOURCES fuzz_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)
set_tests_properties(bank_fuzz_test PROPERTIES TIMEOUT 600)

# Round trip with buildtool's output (buildtool must not depend on bank, so the
# check lives here; the test executable links both).
snd_add_test(bank_roundtrip_test
  SOURCES roundtrip_test.cpp
  LINK sonus_bank sonus_buildtool sonus_mocks)
target_include_directories(bank_roundtrip_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
target_compile_definitions(bank_roundtrip_test PRIVATE
  SND_BUILDTOOL_TEST_DATA="${PROJECT_SOURCE_DIR}/buildtool/test/data")
