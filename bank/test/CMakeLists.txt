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

# --- Independent tests (test owner, not the implementer) -----------------------------
# Structure-aware fuzz: re-signed boundary mutations of every structural field,
# invariants re-checked against the spec, every accessor walked (run under ASan/UBSan).
snd_add_test(bank_struct_fuzz_test
  SOURCES struct_fuzz_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_buildtool sonus_mocks)
# Differential check against buildtool's generated self-check (buildtool/src/self_check.hpp).
target_include_directories(bank_struct_fuzz_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/src)
set_tests_properties(bank_struct_fuzz_test PROPERTIES TIMEOUT 1200)

# Manager edge cases (cancel at every step, alloc failure at every n, queue,
# dependencies, languages). Tests named Defect_* fail on purpose (see file).
snd_add_test(bank_manager_edge_test
  SOURCES manager_edge_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)

# Header bytes [52,128) are outside every checksum (format defect): tests named
# SpecDefect_* fail on purpose.
snd_add_test(bank_header_gap_test
  SOURCES header_gap_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)

# Dev override layer (11.2).
snd_add_test(bank_overlay_test
  SOURCES overlay_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)

# Real worker threads (thread-pool IJobSystem) + IO / core / game threads: run under TSan.
snd_add_test(bank_manager_thread_test
  SOURCES manager_thread_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)
find_package(Threads REQUIRED)
target_link_libraries(bank_manager_thread_test PRIVATE Threads::Threads)

# buildtool -> bank read-back: every documented project field comes back from
# the columns, streams / seek / preload, release banks, built-bank fuzz.
# Tests named Defect_* fail on purpose.
snd_add_test(bank_build_readback_test
  SOURCES build_readback_test.cpp
  LINK sonus_bank sonus_buildtool sonus_mocks)
target_include_directories(bank_build_readback_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(bank_build_readback_test PROPERTIES TIMEOUT 600)

# ParseBank targeted corner cases (chunk layout, columns, versions, cycles).
snd_add_test(bank_validate_edge_test
  SOURCES validate_edge_test.cpp fixture_bank.cpp
  LINK sonus_bank sonus_mocks)
