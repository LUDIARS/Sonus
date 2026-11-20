snd_add_test(buildtool_test
  SOURCES buildtool_smoke_test.cpp buildtool_test.cpp
  LINK sonus_buildtool sonus_mocks)
target_compile_definitions(buildtool_test PRIVATE
  SND_BUILDTOOL_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Self-check of written banks: the same corruption cases as the runtime's
# ParseBank (bank/test/corruption_cases.hpp, built from bank/test fixtures).
snd_add_test(buildtool_self_check_test
  SOURCES self_check_test.cpp ${PROJECT_SOURCE_DIR}/bank/test/fixture_bank.cpp
  LINK sonus_buildtool sonus_mocks)
target_include_directories(buildtool_self_check_test PRIVATE
  ${PROJECT_SOURCE_DIR}/bank/test ${PROJECT_SOURCE_DIR}/buildtool/src)

# D-104: names hashing to the runtime's reserved ids.
snd_add_test(buildtool_reserved_id_test
  SOURCES reserved_id_test.cpp
  LINK sonus_buildtool sonus_mocks)

# --- Independent tests (test owner, not the implementer) -----------------------------
# JSON (RFC 8259) and WAV readers: boundaries, error positions, fixed-seed fuzz.
# Tests named Defect_* fail on purpose (see the file).
snd_add_test(buildtool_json_wav_edge_test
  SOURCES json_wav_edge_test.cpp
  LINK sonus_buildtool sonus_mocks)

# Build edge cases: generated header compiled with the real compiler, real ID
# collisions, references / cycles, transition warnings, determinism, cache,
# output atomicity, CLI exit codes. Tests named Defect_* fail on purpose.
snd_add_test(buildtool_build_edge_test
  SOURCES build_edge_test.cpp
  LINK sonus_buildtool sonus_mocks)
target_compile_definitions(buildtool_build_edge_test PRIVATE
  SND_TEST_CXX="${CMAKE_CXX_COMPILER}"
  SND_TEST_PUBLIC_INCLUDE="${PROJECT_SOURCE_DIR}/contract/public/include"
  SND_BUILD_EXE="$<TARGET_FILE:sonus-build>")
add_dependencies(buildtool_build_edge_test sonus-build)
set_tests_properties(buildtool_build_edge_test PROPERTIES TIMEOUT 600)

# In-memory project / output (ProjectSource, memoryOutput) and patches (patch.hpp).
snd_add_test(buildtool_source_patch_test
  SOURCES source_patch_test.cpp
  LINK sonus_buildtool sonus_mocks)
